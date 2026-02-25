# Advanced Operating Systems and Virtualization 2019/2020 Final Project: Thread Synchronization and Messaging Subsystem

## Specification

This project involves the design and development of a subsystem for the Linux kernel which allows threads from (different) processes to exchange units of information (aka messages) and synchronize with each other.

The subsystem works according to the concept of *groups* to orchestrate which threads can synchronize and exchange messages. A group boils down to the creation of a device file in the `/dev/synch/` folder. Threads (from any process) can use a custom-defined `group_t` type to tell what is the group of threads they want to synchronize/exchange messages with.

The subsystem offers five fundamental primitives:
* **Install a group**: by providing a `group_t` descriptor to the subsystem, a new device file is installed only if a corresponding device is not existing. The path to the corresponding device file is returned.
* **Send a message**: a new unit of information is delivered to the subsystem via a `write()` system call.
* **Retrieve a message**: a previously-sent unit of information is delivered to the caller in FIFO order via a `read()` system call.
* **Sleep on barrier**: a thread calling this primitive is descheduled and will not be re-scheduled until it is explicitly woken up.
* **Awake barrier**: all threads sleeping on a barrier are woken up.

Each message posted to the device file is an independent data unit. The message receipt fully invalidates the content of the message to be delivered to the user land buffer (exactly-once delivery semantic). Both `write()` and `read()` operations can be controlled by relying on the `ioctl()` interface:
* `SET_SEND_DELAY`: sets the associated group to a mode that stores messages after a timeout (in milliseconds) without blocking the caller.
* `REVOKE_DELAYED_MESSAGES`: allows retracting all delayed messages for which the delay timer has not expired.

The system call `flush()` must be supported in order to cancel the effect of the delay. If a special device file is flushed, all delayed messages are made immediately available to subsequent `read()` calls.

The kernel exposes via the `/sys` file system the following reconfigurable parameters:
* `max_message_size`: the maximum size (bytes) currently allowed for posting messages to the device file.
* `max_storage_size`: the maximum number of bytes globally allowed for keeping messages in the device file.

---

## Build and Execution Commands

### Prerequisites
* Linux environment
* `build-essential` and `linux-headers-$(uname -r)`
* `udev` daemon

### 1. Installation and Module Injection
First, install the `udev` rules to handle dynamic device permissions properly:
```bash
sudo cp 50-synchmess.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger
```

Build the kernel module and the multi-threaded benchmarking suite:
```bash
make all
```

Insert the compiled module (synchmess.ko) into the running kernel:
```bash
sudo insmod synchmess.ko
```
(Verify the successful installation by checking the kernel logs: dmesg | tail)

### 2. Running the Functional Test
The repository includes a standard C application (test.c) to functionally verify the basic IPC operations (device creation, standard messaging, delayed messaging, and fsync flushing) with error handling.
Compile and run the test application:

```bash
gcc test.c -o test_app
./test_app
```

### 3. Running the Multi-Threaded Benchmark
To perform stress testing on the kernel locks under heavy concurrency, run the benchmarking suite. This will spawn multiple producer and consumer threads to hammer the VFS endpoints.
You can launch the automated bash script:

```bash
chmod +x benchmark.sh
./benchmark.sh
```

### 4. Uninstallation
When finished, cleanly remove the module from the kernel and purge the build artifacts:

```bash
sudo rmmod synchmess
make clean
```
