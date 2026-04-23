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
      
 4  Stopping test beta
      <img width="675" height="43" alt="Screenshot from 2026-04-22 06-07-09" src="https://github.com/user-attachments/assets/dd075819-0824-4ad4-85b5-df11c03fc0e1" />


 5 and 6 `dmesg` showing SOFT LIMIT warning for memory test container and `dmesg` showing HARD LIMIT kill and supervisor reflecting `state=killed` 
<img width="1676" height="246" alt="Screenshot from 2026-04-23 07-14-19" src="https://github.com/user-attachments/assets/2c461ef9-2bae-485b-bd36-e14f69c525a3" />

 7  Log comparison showing CPU usage differences                              
      <img width="675" height="369" alt="Screenshot from 2026-04-22 06-20-48" src="https://github.com/user-attachments/assets/e7357939-35c7-4054-bb15-a68f24c1ca1c" />

 8  `dmesg` showing module unloaded and zero zombie processes                 
     <img width="724" height="121" alt="Screenshot from 2026-04-22 06-27-56" src="https://github.com/user-attachments/assets/b2debaa4-fb8e-400d-833b-5991b6636783" />


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
