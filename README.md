# OS-Jackfruit — Supervised Multi-Container Runtime

## 1. Team Information

| Name              | SRN           |
| ----------------- | ------------- |
| Kartik Naik       | PES1UG24CS219 |
| Kushal Jayavarapu | PES1UG24CS246 |


## 2. Build, Load, and Run Instructions

### Version

*  Ubuntu 24.04 in a VM

### Running the Supervisor

* Start supervisor: `sudo ./engine supervisor ./rootfs-base`
* Container operations: `start`, `run`, `ps`, `logs`, `stop`

### Tests and Experiments

* Memory limit tests
* Scheduler experiments (CPU-bound vs I/O-bound, different priorities)

### Clean Shutdown

* Stop supervisor: `Ctrl+C`
* Remove kernel module: `sudo rmmod monitor`

## 3. Demo Screenshots:
 1  Two containers (alpha, beta) running under one supervisor
      <img width="1166" height="57" alt="ss1" src="https://github.com/user-attachments/assets/1e39a119-bdf5-421e-9c1a-8b51605079a0" />

 2  `ps` output showing container metadata (ID, PID, STATE, SOFT/HARD limits) 
      <img width="1214" height="465" alt="ss2" src="https://github.com/user-attachments/assets/0a858966-1d14-4269-8c8a-d3feb037c3d2" />
 
 3  Container logs showing captured output 
      <img width="1214" height="248" alt="ss3" src="https://github.com/user-attachments/assets/a5d1625a-0af0-4e73-9335-e73cb145caa8" />
      
 4  Stopping test alpha
      <img width="1600" height="158" alt="image" src="https://github.com/user-attachments/assets/c18b0b35-5bfe-427a-afa2-fe2419fef822" />

 5  `dmesg` showing SOFT LIMIT warning for memory test container and `dmesg` showing HARD LIMIT kill and supervisor reflecting `state=killed` 
      <img width="1006" height="208" alt="ss5and6" src="https://github.com/user-attachments/assets/987c6f8e-5476-46a3-9540-13cc79666a09" />

 6  Log comparison showing CPU usage differences                              
      <img width="1055" height="678" alt="ss7" src="https://github.com/user-attachments/assets/8b877814-7748-4d30-84c0-ffeef514e733" />

 7  `dmesg` showing module unloaded and zero zombie processes                 
      <img width="1148" height="175" alt="ss8" src="https://github.com/user-attachments/assets/60b70c1b-1957-43d0-a43c-4be2d7fbb778" />


## 4. Engineering Analysis

### 4.1 Isolation Mechanisms

* PID, UTS, and mount namespaces with `chroot` filesystem isolation

### 4.2 Supervisor and Process Lifecycle

* Long-running supervisor reaps children to avoid zombies
* Metadata stored per container

### 4.3 IPC and Synchronization

* Pipe for logs, UNIX socket for CLI
* Mutexes and condition variables for thread safety

### 4.4 Memory Management

* Soft vs hard limits
* Kernel module enforces limits atomically

### 4.5 Scheduling Behavior

* CPU-bound vs I/O-bound containers
* Nice values affect CPU allocation

## 5. Design Decisions and Tradeoffs

* Namespace isolation vs simplicity
* Single-threaded supervisor for correctness
* Pipe-based logging vs shared memory
* Kernel monitor with `mutex_trylock` vs spinlock
* Metrics choice for scheduler experiments

## 6. Scheduler Experiment Results

### Experiment 1: CPU-bound vs CPU-bound, different nice values

* High nice vs low nice accumulator comparison
* Observed CPU share proportional to weight

### Experiment 2: CPU-bound vs I/O-bound, same nice value

* CPU hog vs I/O-bound process behavior
* Demonstrates CFS responsiveness to I/O-bound workloads
