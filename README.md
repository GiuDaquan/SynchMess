# SynchMess: Linux Kernel Thread Synchronization & Messaging Subsystem

**Advanced Operating Systems and Virtualization (AOSV) - Final Project (2019/2020)** *Sapienza University of Rome*

## Overview
SynchMess is an out-of-tree Linux kernel character driver that implements a robust IPC (Inter-Process Communication) and thread synchronization subsystem. It allows threads from distinct processes to exchange messages with strictly defined delivery semantics and synchronize execution flows via kernel-level barriers. 

The project demonstrates deep integration with Linux kernel internals, including the Virtual File System (VFS), memory management, preemption-safe locking primitives, and deferred work scheduling.

## Core Architecture

### 1. Master/Worker VFS Multiplexing
The subsystem avoids polluting the root `/dev` directory by employing a hierarchical device layout managed through custom `udev` rules.
* **Master Device (`/dev/synch/synchmess`):** Acts as a control-plane endpoint. Threads interact with it exclusively via `ioctl()` to dynamically provision new communication "groups" (device nodes).
* **Worker Devices (`/dev/synch/synchmess[X]`):** Dynamically spawned endpoints mapped to specific minor numbers. These handle the actual data-plane I/O (`read()`, `write()`, `fsync()`).

### 2. Concurrency and Safe Locking
Handling concurrent I/O from multiple multi-threaded processes requires strict adherence to kernel locking rules to avoid race conditions and `BUG: scheduling while atomic` panics.
* **Mutexes:** Device installation (`IOCTL_INSTALL_GROUP`) and delayed message queuing are protected by kernel `mutex` constructs, ensuring deterministic scheduling and preventing priority inversion.
* **Read/Write Spinlocks:** The core message queue per group is protected by `rwlock_t`. To safely deliver data to user-space without holding an atomic lock (which would panic on `copy_to_user` page faults), the driver implements an O(1) *Pop-and-Process* logic. The message is atomically detached from the list under a write-lock, and the copy operation is performed lock-free.
* **Waitqueues:** Thread synchronization (`IOCTL_SLEEP` and `IOCTL_AWAKE`) is implemented natively using Linux `wait_queue_head_t`, putting threads to sleep (`TASK_INTERRUPTIBLE`) with zero CPU overhead until an awake event occurs.

### 3. Asynchronous Execution (Workqueues)
The driver supports delayed message delivery. When `IOCTL_SET_SEND_DELAY` is invoked, `write()` calls return immediately to user-space, but the actual message publication is deferred.
This is achieved by instantiating a custom `workqueue_struct`. Delayed messages are wrapped in `delayed_work` structs and pushed to the kernel's worker threads, simulating asynchronous timed execution entirely within Ring 0.

## Implementation Details & Semantics
* **Delivery:** Exactly-once FIFO delivery. Once a message is read, it is definitively destroyed from the kernel heap.
* **Memory Management:** Robust memory footprint control via `atomic_t` size limits, ensuring the module cannot be used to exhaust kernel heap (DoS).
* **Flush:** Implementation of the `fsync` system call to manually force the execution of pending delayed work.

## Build and Execution

### Prerequisites
* Linux environment (tested on Ubuntu/Debian)
* `build-essential` and `linux-headers` for the active kernel
* `udev` daemon

### Installation
1. Install the `udev` rules to handle dynamic device permissions:
   ```bash
   sudo cp 50-synchmess.rules /etc/udev/rules.d/
   sudo udevadm control --reload-rules
   sudo udevadm trigger