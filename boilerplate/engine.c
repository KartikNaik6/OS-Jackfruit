/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Complete implementation:
 *   - UNIX domain socket control-plane IPC
 *   - container lifecycle with clone() + namespaces
 *   - chroot + /proc mount inside each container
 *   - bounded-buffer producer/consumer logging pipeline
 *   - SIGCHLD / SIGINT / SIGTERM handling
 *   - ps / logs / stop / start / run commands
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

/* ------------------------------------------------------------------ */
/*  Constants                                                          */
/* ------------------------------------------------------------------ */
#define STACK_SIZE          (1024 * 1024)
#define CONTAINER_ID_LEN    32
#define CONTROL_PATH        "/tmp/mini_runtime.sock"
#define LOG_DIR             "logs"
#define CONTROL_MESSAGE_LEN 512
#define CHILD_COMMAND_LEN   256
#define LOG_CHUNK_SIZE      4096
#define LOG_BUFFER_CAPACITY 64
#define DEFAULT_SOFT_LIMIT  (40UL << 20)   /* 40 MiB */
#define DEFAULT_HARD_LIMIT  (64UL << 20)   /* 64 MiB */
#define MAX_CONTAINERS      64
#define MONITOR_DEV         "/dev/container_monitor"

/* ------------------------------------------------------------------ */
/*  Types                                                              */
/* ------------------------------------------------------------------ */
typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED,
    CONTAINER_HARD_LIMIT_KILLED
} container_state_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
    int exit_code;
    int exit_signal;
    int stop_requested;   /* set before sending SIGTERM/SIGKILL from 'stop' */
    char log_path[PATH_MAX];
    struct container_record *next;
} container_record_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
    int is_run;   /* 1 → caller blocks until container exits */
} control_request_t;

typedef struct {
    int  status;        /* 0 = ok, non-zero = error */
    int  exit_code;     /* filled by CMD_RUN when container exits */
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char  id[CONTAINER_ID_LEN];
    char  rootfs[PATH_MAX];
    char  command[CHILD_COMMAND_LEN];
    int   nice_value;
    int   pipe_write_fd;   /* supervisor end gets the read side */
} child_config_t;

/* per-container producer thread arg */
typedef struct {
    int   read_fd;
    char  container_id[CONTAINER_ID_LEN];
    void *buffer;      /* bounded_buffer_t * */
} producer_arg_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    volatile int should_stop;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t  metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

/* global so signal handler can reach it */
static supervisor_ctx_t *g_ctx = NULL;

/* ------------------------------------------------------------------ */
/*  Utilities                                                          */
/* ------------------------------------------------------------------ */
static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage:\n"
        "  %s supervisor <base-rootfs>\n"
        "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
        "  %s run   <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
        "  %s ps\n"
        "  %s logs <id>\n"
        "  %s stop <id>\n",
        prog, prog, prog, prog, prog, prog);
}

static const char *state_to_string(container_state_t s)
{
    switch (s) {
    case CONTAINER_STARTING:          return "starting";
    case CONTAINER_RUNNING:           return "running";
    case CONTAINER_STOPPED:           return "stopped";
    case CONTAINER_KILLED:            return "killed";
    case CONTAINER_EXITED:            return "exited";
    case CONTAINER_HARD_LIMIT_KILLED: return "hard_limit_killed";
    default:                          return "unknown";
    }
}

static int parse_mib_flag(const char *flag,
                          const char *value,
                          unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;
    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno || end == value || *end) {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }
    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s is too large\n", flag);
        return -1;
    }
    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req,
                                int argc, char *argv[], int start)
{
    int i;
    for (i = start; i < argc; i += 2) {
        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for %s\n", argv[i]);
            return -1;
        }
        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i+1], &req->soft_limit_bytes))
                return -1;
        } else if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i+1], &req->hard_limit_bytes))
                return -1;
        } else if (strcmp(argv[i], "--nice") == 0) {
            char *end = NULL;
            long v = strtol(argv[i+1], &end, 10);
            if (end == argv[i+1] || *end || v < -20 || v > 19) {
                fprintf(stderr, "Invalid --nice value: %s\n", argv[i+1]);
                return -1;
            }
            req->nice_value = (int)v;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return -1;
        }
    }
    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "soft limit cannot exceed hard limit\n");
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Bounded Buffer                                                     */
/* ------------------------------------------------------------------ */
static int bounded_buffer_init(bounded_buffer_t *b)
{
    int rc;
    memset(b, 0, sizeof(*b));
    rc = pthread_mutex_init(&b->mutex, NULL);
    if (rc) return rc;
    rc = pthread_cond_init(&b->not_empty, NULL);
    if (rc) { pthread_mutex_destroy(&b->mutex); return rc; }
    rc = pthread_cond_init(&b->not_full, NULL);
    if (rc) {
        pthread_cond_destroy(&b->not_empty);
        pthread_mutex_destroy(&b->mutex);
        return rc;
    }
    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *b)
{
    pthread_cond_destroy(&b->not_full);
    pthread_cond_destroy(&b->not_empty);
    pthread_mutex_destroy(&b->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *b)
{
    pthread_mutex_lock(&b->mutex);
    b->shutting_down = 1;
    pthread_cond_broadcast(&b->not_empty);
    pthread_cond_broadcast(&b->not_full);
    pthread_mutex_unlock(&b->mutex);
}

/*
 * Push a log_item into the buffer.
 * Blocks when full; returns 0 on success, -1 if shutting down.
 *
 * Race-condition rationale: without mutex + cond, a producer could
 * observe count < CAPACITY, get preempted, another thread fills the
 * buffer, and the producer overwrites data → corruption. The mutex
 * serialises the check-and-write; not_full lets producers sleep
 * instead of spinning when the buffer is full.
 */
int bounded_buffer_push(bounded_buffer_t *b, const log_item_t *item)
{
    pthread_mutex_lock(&b->mutex);
    while (b->count == LOG_BUFFER_CAPACITY && !b->shutting_down)
        pthread_cond_wait(&b->not_full, &b->mutex);

    if (b->shutting_down) {
        pthread_mutex_unlock(&b->mutex);
        return -1;
    }

    b->items[b->tail] = *item;
    b->tail = (b->tail + 1) % LOG_BUFFER_CAPACITY;
    b->count++;
    pthread_cond_signal(&b->not_empty);
    pthread_mutex_unlock(&b->mutex);
    return 0;
}

/*
 * Pop a log_item from the buffer.
 * Returns 0 with item filled, 1 when shutdown+empty (consumer should exit).
 */
int bounded_buffer_pop(bounded_buffer_t *b, log_item_t *item)
{
    pthread_mutex_lock(&b->mutex);
    while (b->count == 0 && !b->shutting_down)
        pthread_cond_wait(&b->not_empty, &b->mutex);

    if (b->count == 0 && b->shutting_down) {
        pthread_mutex_unlock(&b->mutex);
        return 1;   /* done */
    }

    *item = b->items[b->head];
    b->head = (b->head + 1) % LOG_BUFFER_CAPACITY;
    b->count--;
    pthread_cond_signal(&b->not_full);
    pthread_mutex_unlock(&b->mutex);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Logger (Consumer) Thread                                           */
/* ------------------------------------------------------------------ */
/*
 * Opens log files on demand (keyed by container_id) and writes chunks.
 * Drains everything before exiting so no log lines are lost.
 */
void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    bounded_buffer_t *buf = &ctx->log_buffer;
    log_item_t item;
    int rc;

    /* simple open-file cache: up to MAX_CONTAINERS open fds */
    struct { char id[CONTAINER_ID_LEN]; int fd; } cache[MAX_CONTAINERS];
    int cache_sz = 0;
    int i;

    memset(cache, 0, sizeof(cache));
    for (i = 0; i < MAX_CONTAINERS; i++) cache[i].fd = -1;

    while (1) {
        rc = bounded_buffer_pop(buf, &item);
        if (rc != 0) break;   /* shutdown + empty */

        /* find or open log fd */
        int fd = -1;
        for (i = 0; i < cache_sz; i++) {
            if (strcmp(cache[i].id, item.container_id) == 0) {
                fd = cache[i].fd;
                break;
            }
        }
        if (fd < 0 && cache_sz < MAX_CONTAINERS) {
            /* look up log path from metadata */
            char log_path[PATH_MAX] = {0};
            pthread_mutex_lock(&ctx->metadata_lock);
            container_record_t *r = ctx->containers;
            while (r) {
                if (strcmp(r->id, item.container_id) == 0) {
                    strncpy(log_path, r->log_path, PATH_MAX - 1);
                    break;
                }
                r = r->next;
            }
            pthread_mutex_unlock(&ctx->metadata_lock);

            if (log_path[0]) {
                fd = open(log_path, O_CREAT|O_WRONLY|O_APPEND, 0644);
                if (fd >= 0) {
                    strncpy(cache[cache_sz].id, item.container_id, CONTAINER_ID_LEN-1);
                    cache[cache_sz].fd = fd;
                    cache_sz++;
                }
            }
        }

        if (fd >= 0 && item.length > 0) {
            ssize_t written = 0;
            while ((size_t)written < item.length) {
                ssize_t n = write(fd, item.data + written, item.length - written);
                if (n <= 0) break;
                written += n;
            }
        }
    }

    /* flush: close all open log fds */
    for (i = 0; i < cache_sz; i++) {
        if (cache[i].fd >= 0) {
            fsync(cache[i].fd);
            close(cache[i].fd);
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Producer Thread (per container)                                    */
/* ------------------------------------------------------------------ */
static void *producer_thread(void *arg)
{
    producer_arg_t *pa = (producer_arg_t *)arg;
    bounded_buffer_t *buf = (bounded_buffer_t *)pa->buffer;
    log_item_t item;

    memset(&item, 0, sizeof(item));
    strncpy(item.container_id, pa->container_id, CONTAINER_ID_LEN-1);

    while (1) {
        ssize_t n = read(pa->read_fd, item.data, LOG_CHUNK_SIZE);
        if (n <= 0) break;   /* container closed its end (exited) */
        item.length = (size_t)n;
        /* push; if buffer is shutting down, we just drop */
        bounded_buffer_push(buf, &item);
    }

    close(pa->read_fd);
    free(pa);
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Container Child Entry Point                                        */
/* ------------------------------------------------------------------ */
int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;

    /* redirect stdout + stderr to the logging pipe */
    if (cfg->pipe_write_fd >= 0) {
        dup2(cfg->pipe_write_fd, STDOUT_FILENO);
        dup2(cfg->pipe_write_fd, STDERR_FILENO);
        close(cfg->pipe_write_fd);
    }

    /* set hostname to container id for UTS namespace demo */
    sethostname(cfg->id, strlen(cfg->id));

    /* mount /proc inside the container rootfs */
    char proc_path[PATH_MAX];
    snprintf(proc_path, sizeof(proc_path), "%s/proc", cfg->rootfs);
    mkdir(proc_path, 0555);
    if (mount("proc", proc_path, "proc", 0, NULL) < 0)
        perror("[container] mount proc");

    /* chroot into the container rootfs */
    if (chroot(cfg->rootfs) < 0) {
        perror("[container] chroot");
        return 1;
    }
    if (chdir("/") < 0) {
        perror("[container] chdir /");
        return 1;
    }

    /* apply nice value */
    if (cfg->nice_value != 0)
        nice(cfg->nice_value);

    /* exec the requested command */
    char *cmd_argv[] = { "/bin/sh", "-c", cfg->command, NULL };

    /* try direct exec first (if command is a path), else use sh -c */
    execv(cfg->command, (char *[]){ cfg->command, NULL });
    /* fallback: shell */
    execv("/bin/sh", cmd_argv);
    perror("[container] exec");
    return 1;
}

/* ------------------------------------------------------------------ */
/*  Kernel Monitor helpers                                             */
/* ------------------------------------------------------------------ */
int register_with_monitor(int fd, const char *id, pid_t pid,
                          unsigned long soft, unsigned long hard)
{
    if (fd < 0) return 0;
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = pid;
    req.soft_limit_bytes = soft;
    req.hard_limit_bytes = hard;
    strncpy(req.container_id, id, sizeof(req.container_id)-1);
    return ioctl(fd, MONITOR_REGISTER, &req) < 0 ? -1 : 0;
}

int unregister_from_monitor(int fd, const char *id, pid_t pid)
{
    if (fd < 0) return 0;
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = pid;
    strncpy(req.container_id, id, sizeof(req.container_id)-1);
    return ioctl(fd, MONITOR_UNREGISTER, &req) < 0 ? -1 : 0;
}

/* ------------------------------------------------------------------ */
/*  Metadata helpers (call with metadata_lock held)                   */
/* ------------------------------------------------------------------ */
static container_record_t *find_container(supervisor_ctx_t *ctx, const char *id)
{
    container_record_t *r = ctx->containers;
    while (r) {
        if (strcmp(r->id, id) == 0) return r;
        r = r->next;
    }
    return NULL;
}

static container_record_t *new_container(supervisor_ctx_t *ctx,
                                         const control_request_t *req)
{
    container_record_t *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    strncpy(r->id, req->container_id, CONTAINER_ID_LEN-1);
    r->state = CONTAINER_STARTING;
    r->started_at = time(NULL);
    r->soft_limit_bytes = req->soft_limit_bytes;
    r->hard_limit_bytes = req->hard_limit_bytes;
    r->nice_value = req->nice_value;
    snprintf(r->log_path, PATH_MAX, "%s/%s.log", LOG_DIR, req->container_id);
    /* prepend */
    r->next = ctx->containers;
    ctx->containers = r;
    return r;
}

/* ------------------------------------------------------------------ */
/*  SIGCHLD handler — reaps children, updates metadata                */
/* ------------------------------------------------------------------ */
static volatile sig_atomic_t g_got_sigchld = 0;
static volatile sig_atomic_t g_got_sigterm = 0;

static void sigchld_handler(int sig) { (void)sig; g_got_sigchld = 1; }
static void sigterm_handler(int sig) { (void)sig; g_got_sigterm = 1; }

static void reap_children(supervisor_ctx_t *ctx)
{
    int wstatus;
    pid_t pid;
    while ((pid = waitpid(-1, &wstatus, WNOHANG)) > 0) {
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *r = ctx->containers;
        while (r) {
            if (r->host_pid == pid) {
                if (WIFEXITED(wstatus)) {
                    r->exit_code = WEXITSTATUS(wstatus);
                    r->state = CONTAINER_EXITED;
                } else if (WIFSIGNALED(wstatus)) {
                    r->exit_signal = WTERMSIG(wstatus);
                    if (r->stop_requested) {
                        r->state = CONTAINER_STOPPED;
                    } else if (r->exit_signal == SIGKILL) {
                        r->state = CONTAINER_HARD_LIMIT_KILLED;
                    } else {
                        r->state = CONTAINER_KILLED;
                    }
                }
                unregister_from_monitor(ctx->monitor_fd, r->id, r->host_pid);
                break;
            }
            r = r->next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
    }
}

/* ------------------------------------------------------------------ */
/*  Launch a container                                                 */
/* ------------------------------------------------------------------ */
static int launch_container(supervisor_ctx_t *ctx,
                            const control_request_t *req,
                            pid_t *out_pid)
{
    /* create log dir */
    mkdir(LOG_DIR, 0755);

    /* pipe: container writes, supervisor reads */
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        perror("pipe");
        return -1;
    }

    /* child config lives on heap so clone child can use it safely */
    child_config_t *cfg = calloc(1, sizeof(*cfg));
    if (!cfg) { close(pipefd[0]); close(pipefd[1]); return -1; }
    strncpy(cfg->id, req->container_id, CONTAINER_ID_LEN-1);
    strncpy(cfg->rootfs, req->rootfs, PATH_MAX-1);
    strncpy(cfg->command, req->command, CHILD_COMMAND_LEN-1);
    cfg->nice_value = req->nice_value;
    cfg->pipe_write_fd = pipefd[1];

    char *stack = malloc(STACK_SIZE);
    if (!stack) { free(cfg); close(pipefd[0]); close(pipefd[1]); return -1; }
    char *stack_top = stack + STACK_SIZE;

    int flags = CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD;
    pid_t pid = clone(child_fn, stack_top, flags, cfg);
    /* parent closes write end */
    close(pipefd[1]);

    if (pid < 0) {
        perror("clone");
        free(stack);
        free(cfg);
        close(pipefd[0]);
        return -1;
    }

    free(stack);
    /* cfg is used by child; child calls exec so memory is replaced, but
       for safety we don't free cfg here — it's a small one-time leak */

    /* record metadata */
    pthread_mutex_lock(&ctx->metadata_lock);
    container_record_t *r = new_container(ctx, req);
    if (r) {
        r->host_pid = pid;
        r->state = CONTAINER_RUNNING;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    /* register with kernel monitor */
    register_with_monitor(ctx->monitor_fd, req->container_id, pid,
                          req->soft_limit_bytes, req->hard_limit_bytes);

    /* start producer thread for this container's log pipe */
    producer_arg_t *pa = malloc(sizeof(*pa));
    if (pa) {
        pa->read_fd = pipefd[0];
        pa->buffer  = &ctx->log_buffer;
        strncpy(pa->container_id, req->container_id, CONTAINER_ID_LEN-1);
        pthread_t pt;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&pt, &attr, producer_thread, pa) != 0)
            free(pa);
        pthread_attr_destroy(&attr);
    }

    if (out_pid) *out_pid = pid;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Handle a single control request from a connected client           */
/* ------------------------------------------------------------------ */
static void handle_request(supervisor_ctx_t *ctx, int client_fd)
{
    control_request_t req;
    control_response_t resp;
    memset(&resp, 0, sizeof(resp));

    ssize_t n = recv(client_fd, &req, sizeof(req), MSG_WAITALL);
    if (n != (ssize_t)sizeof(req)) {
        resp.status = -1;
        snprintf(resp.message, CONTROL_MESSAGE_LEN, "Bad request size");
        send(client_fd, &resp, sizeof(resp), 0);
        return;
    }

    switch (req.kind) {

    case CMD_START: {
        pid_t pid = -1;
        if (launch_container(ctx, &req, &pid) == 0) {
            resp.status = 0;
            snprintf(resp.message, CONTROL_MESSAGE_LEN,
                     "[supervisor] Started container '%s' with PID %d",
                     req.container_id, (int)pid);
        } else {
            resp.status = -1;
            snprintf(resp.message, CONTROL_MESSAGE_LEN,
                     "[supervisor] Failed to start container '%s'",
                     req.container_id);
        }
        send(client_fd, &resp, sizeof(resp), 0);
        break;
    }

    case CMD_RUN: {
        pid_t pid = -1;
        if (launch_container(ctx, &req, &pid) != 0) {
            resp.status = -1;
            snprintf(resp.message, CONTROL_MESSAGE_LEN,
                     "[supervisor] Failed to start container '%s'",
                     req.container_id);
            send(client_fd, &resp, sizeof(resp), 0);
            break;
        }
        /* ACK that it started */
        resp.status = 0;
        snprintf(resp.message, CONTROL_MESSAGE_LEN,
                 "[supervisor] Running container '%s' PID %d (blocking)…",
                 req.container_id, (int)pid);
        send(client_fd, &resp, sizeof(resp), 0);

        /* wait for this specific container to exit */
        int wstatus = 0;
        waitpid(pid, &wstatus, 0);
        reap_children(ctx);   /* update metadata */

        memset(&resp, 0, sizeof(resp));
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *r = find_container(ctx, req.container_id);
        if (r) {
            resp.exit_code = r->exit_code;
            snprintf(resp.message, CONTROL_MESSAGE_LEN,
                     "[supervisor] Container '%s' exited: state=%s exit_code=%d",
                     req.container_id, state_to_string(r->state), r->exit_code);
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
        resp.status = 0;
        send(client_fd, &resp, sizeof(resp), 0);
        break;
    }

    case CMD_PS: {
        char buf[4096] = {0};
        int off = 0;
        off += snprintf(buf + off, sizeof(buf) - off,
            "%-12s %-8s %-22s %-20s %-10s %-10s %-6s\n",
            "ID", "PID", "STARTED", "STATE", "SOFT(MiB)", "HARD(MiB)", "EXIT");
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *r = ctx->containers;
        while (r && off < (int)sizeof(buf) - 200) {
            char tstr[32];
            struct tm *tm = localtime(&r->started_at);
            strftime(tstr, sizeof(tstr), "%Y-%m-%d %H:%M:%S", tm);
            off += snprintf(buf + off, sizeof(buf) - off,
                "%-12s %-8d %-22s %-20s %-10lu %-10lu %-6d\n",
                r->id, (int)r->host_pid, tstr,
                state_to_string(r->state),
                r->soft_limit_bytes >> 20,
                r->hard_limit_bytes >> 20,
                r->exit_code);
            r = r->next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
        resp.status = 0;
        strncpy(resp.message, buf, CONTROL_MESSAGE_LEN - 1);
        send(client_fd, &resp, sizeof(resp), 0);
        break;
    }

    case CMD_LOGS: {
        char log_path[PATH_MAX] = {0};
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *r = find_container(ctx, req.container_id);
        if (r) strncpy(log_path, r->log_path, PATH_MAX - 1);
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (!log_path[0]) {
            resp.status = -1;
            snprintf(resp.message, CONTROL_MESSAGE_LEN,
                     "Container '%s' not found", req.container_id);
            send(client_fd, &resp, sizeof(resp), 0);
            break;
        }

        /* send log file path so client can read it, or stream it */
        resp.status = 0;
        snprintf(resp.message, CONTROL_MESSAGE_LEN,
                 "Log path: %s", log_path);
        send(client_fd, &resp, sizeof(resp), 0);

        /* stream log file content directly */
        int lfd = open(log_path, O_RDONLY);
        if (lfd >= 0) {
            char chunk[4096];
            ssize_t nr;
            while ((nr = read(lfd, chunk, sizeof(chunk))) > 0)
                send(client_fd, chunk, (size_t)nr, 0);
            close(lfd);
        }
        break;
    }

    case CMD_STOP: {
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *r = find_container(ctx, req.container_id);
        if (!r || (r->state != CONTAINER_RUNNING &&
                   r->state != CONTAINER_STARTING)) {
            pthread_mutex_unlock(&ctx->metadata_lock);
            resp.status = -1;
            snprintf(resp.message, CONTROL_MESSAGE_LEN,
                     "Container '%s' not running", req.container_id);
            send(client_fd, &resp, sizeof(resp), 0);
            break;
        }
        r->stop_requested = 1;
        pid_t pid = r->host_pid;
        pthread_mutex_unlock(&ctx->metadata_lock);

        kill(pid, SIGTERM);
        /* give it 3 seconds then SIGKILL */
        int waited = 0;
        while (waited < 30) {
            usleep(100000);
            if (waitpid(pid, NULL, WNOHANG) == pid) break;
            waited++;
        }
        if (waited == 30) kill(pid, SIGKILL);
        reap_children(ctx);

        resp.status = 0;
        snprintf(resp.message, CONTROL_MESSAGE_LEN,
                 "[supervisor] Stopped container '%s'", req.container_id);
        send(client_fd, &resp, sizeof(resp), 0);
        break;
    }

    default:
        resp.status = -1;
        snprintf(resp.message, CONTROL_MESSAGE_LEN, "Unknown command");
        send(client_fd, &resp, sizeof(resp), 0);
    }
}

/* ------------------------------------------------------------------ */
/*  Supervisor Main Loop                                               */
/* ------------------------------------------------------------------ */
static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd  = -1;
    ctx.monitor_fd = -1;
    g_ctx = &ctx;

    /* init synchronisation */
    if (pthread_mutex_init(&ctx.metadata_lock, NULL) != 0 ||
        bounded_buffer_init(&ctx.log_buffer) != 0) {
        perror("init");
        return 1;
    }

    /* open kernel monitor (optional — may not be loaded) */
    ctx.monitor_fd = open(MONITOR_DEV, O_RDWR);
    if (ctx.monitor_fd < 0)
        fprintf(stderr, "[supervisor] Warning: cannot open %s (%s). "
                "Memory monitoring disabled.\n", MONITOR_DEV, strerror(errno));

    /* create log directory */
    mkdir(LOG_DIR, 0755);

    /* set up UNIX domain socket */
    unlink(CONTROL_PATH);
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    if (listen(ctx.server_fd, 16) < 0) {
        perror("listen"); return 1;
    }

    /* signal handlers */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sa.sa_flags   = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    sa.sa_handler = sigterm_handler;
    sa.sa_flags   = SA_RESTART;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);

    /* start logger thread */
    if (pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx) != 0) {
        perror("pthread_create logger");
        return 1;
    }

    /* make server_fd non-blocking so we can check signals */
    int flags_fd = fcntl(ctx.server_fd, F_GETFL, 0);
    fcntl(ctx.server_fd, F_SETFL, flags_fd | O_NONBLOCK);

    printf("[supervisor] ready. base-rootfs=%s socket=%s\n",
           rootfs, CONTROL_PATH);
    fflush(stdout);

    /* event loop */
    while (!g_got_sigterm) {
        if (g_got_sigchld) {
            g_got_sigchld = 0;
            reap_children(&ctx);
        }

        int client_fd = accept(ctx.server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(50000);
                continue;
            }
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }
        handle_request(&ctx, client_fd);
        close(client_fd);
    }

    /* orderly shutdown */
    printf("[supervisor] Shutting down…\n");

    /* stop all running containers */
    pthread_mutex_lock(&ctx.metadata_lock);
    container_record_t *r = ctx.containers;
    while (r) {
        if (r->state == CONTAINER_RUNNING || r->state == CONTAINER_STARTING) {
            r->stop_requested = 1;
            kill(r->host_pid, SIGTERM);
        }
        r = r->next;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    /* wait briefly for children */
    sleep(1);
    reap_children(&ctx);

    /* shutdown logging */
    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);
    bounded_buffer_destroy(&ctx.log_buffer);

    /* free metadata */
    pthread_mutex_lock(&ctx.metadata_lock);
    r = ctx.containers;
    while (r) {
        container_record_t *next = r->next;
        free(r);
        r = next;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);
    pthread_mutex_destroy(&ctx.metadata_lock);

    if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);
    close(ctx.server_fd);
    unlink(CONTROL_PATH);

    printf("[supervisor] All done.\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Client-side: send a request to the supervisor                     */
/* ------------------------------------------------------------------ */
static int send_control_request(const control_request_t *req)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect (is the supervisor running?)");
        close(fd);
        return 1;
    }

    if (send(fd, req, sizeof(*req), 0) != (ssize_t)sizeof(*req)) {
        perror("send");
        close(fd);
        return 1;
    }

    control_response_t resp;
    ssize_t n = recv(fd, &resp, sizeof(resp), MSG_WAITALL);
    if (n > 0)
        printf("%s\n", resp.message);

    /* for CMD_LOGS, continue reading raw log stream */
    if (req->kind == CMD_LOGS && n > 0 && resp.status == 0) {
        char buf[4096];
        ssize_t nr;
        printf("--- log start ---\n");
        while ((nr = recv(fd, buf, sizeof(buf), 0)) > 0)
            fwrite(buf, 1, (size_t)nr, stdout);
        printf("--- log end ---\n");
    }

    /* for CMD_RUN, wait for second response (exit notification) */
    if (req->kind == CMD_RUN && n > 0 && resp.status == 0) {
        memset(&resp, 0, sizeof(resp));
        n = recv(fd, &resp, sizeof(resp), MSG_WAITALL);
        if (n > 0) {
            printf("%s\n", resp.message);
            close(fd);
            return resp.exit_code;
        }
    }

    close(fd);
    return (n > 0) ? (resp.status != 0 ? 1 : 0) : 1;
}

/* ------------------------------------------------------------------ */
/*  CLI Commands                                                       */
/* ------------------------------------------------------------------ */
static int cmd_start(int argc, char *argv[])
{
    if (argc < 5) {
        fprintf(stderr, "Usage: %s start <id> <rootfs> <cmd> [opts]\n", argv[0]);
        return 1;
    }
    control_request_t req = {0};
    req.kind = CMD_START;
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN-1);
    strncpy(req.rootfs,       argv[3], PATH_MAX-1);
    strncpy(req.command,      argv[4], CHILD_COMMAND_LEN-1);
    if (parse_optional_flags(&req, argc, argv, 5)) return 1;
    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    if (argc < 5) {
        fprintf(stderr, "Usage: %s run <id> <rootfs> <cmd> [opts]\n", argv[0]);
        return 1;
    }
    control_request_t req = {0};
    req.kind = CMD_RUN;
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN-1);
    strncpy(req.rootfs,       argv[3], PATH_MAX-1);
    strncpy(req.command,      argv[4], CHILD_COMMAND_LEN-1);
    if (parse_optional_flags(&req, argc, argv, 5)) return 1;
    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req = {0};
    req.kind = CMD_PS;
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }
    control_request_t req = {0};
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN-1);
    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }
    control_request_t req = {0};
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN-1);
    return send_control_request(&req);
}

/* ------------------------------------------------------------------ */
/*  main                                                               */
/* ------------------------------------------------------------------ */
int main(int argc, char *argv[])
{
    if (argc < 2) { usage(argv[0]); return 1; }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }
    if (strcmp(argv[1], "start") == 0) return cmd_start(argc, argv);
    if (strcmp(argv[1], "run")   == 0) return cmd_run(argc, argv);
    if (strcmp(argv[1], "ps")    == 0) return cmd_ps();
    if (strcmp(argv[1], "logs")  == 0) return cmd_logs(argc, argv);
    if (strcmp(argv[1], "stop")  == 0) return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}
