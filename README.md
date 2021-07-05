Advanced Operating Systems and Virtualization - A.A. 2019/2020

Final Project Report

**Student Full Name**: Giuseppe D’Aquanno

**Student ID**: 1712078
# Introduction
**SynchMess** is a loadable kernel module aiming to provide a quick message exchange subsystem alongside with some support for thread synchronization.

Threads belonging to any process can start exchanging messages by installing new groups or using existing ones. A group is a character device located under **/dev/synch**. Any thread interacting with a group device is a group member.

Group members can communicate by issuing I/O requests on the group devices directly:

a **write** request to a device will enqueue a message into the group queue, whereas a **read** request will dequeue the message from it.

The module entry point is the master device, created under **/dev/synch/synchmess** upon module loading. Direct I/O operations on the master device allow calling any initialization routine. A correct **ioctl** call on the master device is responsible for the creation of new groups.

SynchMess supports multiple write operating modes. Group members can set a timeout value for the group. Readers from this group will be able to retrieve messages only after the timeout elapsed. Any delayed message can be made immediately available to readers with a **fsynch** system call on the appropriate device.

In addition to this, the module provides a simple sleep/awake synchronization facility. Group members can start sleeping on the group barrier, causing the kernel to avoid scheduling them until some other group member deliberately wakes them up.








# User Space Library
SynchMess does not provide a user-space library. Users can interact with the module via direct I/O on the group devices. The interface header file **synchmess\_interface.h** specifies ioctl parameters. Users only need to include the header to use the module.

The interface file also specifies a **struct group\_t**, which user-space threads can use to install new groups or to get access to existing ones.


|**struct** **group\_t** {<br>`    `**unsigned** descriptor;<br>`    `**char** device\_path[256];<br>};|
| :- |

The following code snippet shows a simple SynchMess usage pattern:


|#**include** <errno.h><br>#**include** <fcntl.h><br>#**include** <stdio.h><br>#**include** <string.h><br>#**include** <sys/ioctl.h><br>#**include** <unistd.h><br><br>#**include** "synchmess\_interface.h"<br><br>**int** **main**(**int** argc, **char** \*\*argv)<br>{<br>`    `**int** ret;<br>`    `**int** master\_dev\_fd;<br>`    `**int** fd;<br>`    `**unsigned** delay\_ms;<br>`    `**struct** **group\_t** **group**;<br>`    `**char** message[1024] = "ABADCAFE\0";<br>`    `**char** delivery[1024] = { 0 };<br>    <br>`    `// Open master device<br>`    `master\_dev\_fd = open("/dev/synch/synchmess", O\_RDONLY);<br>    	<br>`    `// Install a new group identified by descriptor 1<br>`    `group.descriptor = 1;<br>`    `ret = ioctl(master\_dev\_fd, IOCTL\_INSTALL\_GROUP, &group);<br>    <br><br>`    `// Open group device<br>`    `**while** (access(group.device\_path, R\_OK | W\_OK)) {<br>`   	 `**continue**;<br>`    `}<br>`    `fd = open(group.device\_path, O\_RDWR);<br>`    `// Send a message<br>`    `ret = write(fd, message, strlen(message));<br><br>`    `// Read a message<br>`    `ret = read(fd, &delivery, 1024);<br><br>`    `// Set the group delay to 5 seconds<br>`    `delay\_ms = 5;<br>`    `ret = ioctl(fd, IOCTL\_SET\_SEND\_DELAY, &delay\_ms);<br><br>`    `// Send a delayed message<br>`    `ret = write(fd, message, strlen(message));<br>	<br>`    `// Revoke delayed messages<br>`    `ret = ioctl(fd, IOCTL\_REVOKE\_DELAYED\_MESSAGES);<br><br>`    `// Send another delayed message<br>`    `ret = write(fd, message, strlen(message));<br>    <br>`    `// Flush messages<br>`    `ret = fsync(fd);<br>    <br>`    `ret = read(fd, &delivery, 1024);<br>`    `delay\_ms = 0;<br>`    `ret = ioctl(fd, IOCTL\_SET\_SEND\_DELAY, &delay\_ms);<br><br>`    `**return** 0;<br>}|
| :- |

















# Kernel-Level Data Structures
The module keeps track of groups using the **groups** global array:


|**static** **struct** **group** \***groups**[**MAX\_NUM\_GROUPS**] = { NULL };|
| :- |

The **struct group** data structure identifies a group in the subsystem:


|**struct** **group** {<br>`    `**rwlock\_t** messages\_rwlock;<br>`    `**struct** **mutex** **delayed\_messages\_mutex**;<br>`    `**spinlock\_t** sleeping\_threads\_spinlock;<br>`    `**spinlock\_t** size\_spinlock;<br>`    `**wait\_queue\_head\_t** wait\_queue;<br>`    `**struct** **list\_head** **sleeping\_threads**;<br>`    `**struct** **list\_head** **messages**;<br>`    `**struct** **list\_head** **delayed\_messages**;<br>`    `**atomic\_t** current\_size;<br>`    `**unsigned** descriptor;<br>`    `**unsigned** delay\_ms;<br>};|
| :- |

- **unsigned descriptor**: An integer number identifying a group, descriptor 0 identifies the master device.
- **unsigned current\_size**: Cumulative group size in bytes.
- **struct list\_head messages**: The main message queue for the group,

writers enqueue messages while readers dequeue them.

- **wait\_queue\_head\_t wait\_queue**: The Linux wait queue for the group. Threads can sleep in this queue until a member of the group wakes them up.
- **struct list\_head sleeping\_threads:** The module uses this list to notify sleeping threads about awake requests.
- **unsigned delay\_ms**: Timeout before write operations have any effect.
- **struct list\_head delayed\_messages**: If the group delay is greater than 0, the module stores messages in this queue for further processing.

Threads communicate by enqueuing and dequeuing messages from the group queue. A **struct message** represents a message:


|**struct** **message** {<br>`    `**struct** **list\_head** **list**;<br>`    `**atomic\_t** delivered;<br>`    `**size\_t** buf\_size;<br>`    `**char** \*buffer;<br>};|
| :- |

- **char \*buffer**: Message body.
- **atomic\_t delivered**: Flag showing whether a reader successfully retrieved a message from the queue.  Delivered messages logically disappear from it.

Whenever a group member sets a timeout for a group, the module needs to save the received messages and make them available to readers after the timeout elapses. The **struct delayed\_message** is part of the implementation for this requirement:


|**struct** **delayed\_message** {<br>`    `**struct** **delayed\_work** **delayed\_work**;<br>`    `**struct** **list\_head** **list**;<br>`    `**struct** **group** \***group**;<br>`    `**struct** **message** \***message**;<br>};|
| :- |

- **struct delayed\_work delayed\_work**: The Linux Kernel uses this struct for the work queues implementation.
- **struct group \*group**: Reference to the group targeted by the write request.
- **struct message \*message**: Reference to the actual message to post after the timeout.

SynchMess tackles the delayed work problem using a global Linux **work\_queue**.


|**static** **struct** **workqueue\_struct** \***work\_queue**;|
| :- |

Sleeping threads assert a logical condition before waking up. The wakener thread writes the **struct sleeping\_thread** to notify sleeping threads about the condition change.


|**struct** **sleeping\_thread** {<br>`    `**struct** **list\_head** **list**;<br>`    `**bool** awake;<br>};|
| :- |

- **bool awake**: Sleeping threads will check this flag upon waking up.
#

# Kernel-Level Subsystem Implementation
## Initialization (synchmess\_init)
SynchMess naturally performs initialization upon module loading. **synchmess\_init** is the function responsible for:

- The allocation of a major number for the SynchMess devices: 
- The registration of a class for the devices.
- The installation of the master device.
- The installation of the global work queue.

Upon returning from the function, the Linux system correctly shows the master device under **/dev/synch/synchmess**, and the module is ready to serve requests.

## Groups installation (synchmess\_install\_group)
User-space threads can install new groups by issuing **IOCTL\_INSTALL\_GROUP** requests to the master device. They only need to provide a **struct group\_t** indicating the group descriptor. The module installs the group device if it does not already exist, or it just writes back into the **struct group\_t** the path for the group device otherwise.

It is worth mentioning that SynchMess associates descriptors to devices minor numbers. In this way, it is easy to understand which group we need to carry out any operation on. The master device is the only device not corresponding to a group.

The installation of a group ultimately boils down to the allocation of a **struct group**. 

As soon as some thread completes this operation, it updates the global **groups** array with a pointer to the freshly allocated data structure.

Synchemss handles race conditions on the array by using an allocation bitmap. Threads atomically set a bit in the bitmap before accessing the critical allocation section. Upon successful installation, the one thread responsible for the allocation will update the array, consequently letting the racing threads continue the execution.


|**static** **DECLARE\_BITMAP**(groups\_bitmap, MAX\_NUM\_GROUPS);<br><br>**int** old\_bit = 1;<br>**struct** **group\_t** **group\_desc**;<br><br>**while** (!groups[group\_desc.descriptor - 1]) {<br>`    `old\_bit = test\_and\_set\_bit(group\_desc.descriptor - 1, groups\_bitmap);<br>`    `**if** (!old\_bit)<br>`        `**break**;<br>`    `**else**<br>`        `yield();<br>}|
| :- |
||
Installed devices populate the **/dev/synch/** directory.

## Immediate writes (synchmess\_write, publish\_message)
Synchmess maintains a per-group **messages** queue, **write** requests to any group trigger insertions into it.

After the message allocation completes, the writer thread checks the group current size and, possibly, it acquires the rw\_lock on the message queue before enqueueing the message.


|**static** ssize\_t **publish\_message**(struct group \*group, <br>`                               `struct message \*message)<br>{<br>`    `**int** ret;<br><br>`    `spin\_lock(&group->size\_spinlock);<br>`    `**if** (atomic\_read(&group->current\_size) + message->buf\_size ><br>`            `MAX\_STORAGE\_SIZE) {<br>`   	  `ret = -EPERM;<br>`   	  `**goto** unlock\_spinlock;<br>`    `}<br>`    `atomic\_add(message->buf\_size, &group->current\_size);<br>`    `spin\_unlock(&group->size\_spinlock);<br><br>`    `write\_lock(&group->messages\_rwlock);<br>`    `list\_add\_tail(&message->list, &group->messages);<br>`    `write\_unlock(&group->messages\_rwlock);<br><br>`    `**return** message->buf\_size;<br><br><br>unlock\_spinlock:<br>`    `spin\_unlock(&group->size\_spinlock);<br>`    `free\_message(message);<br>`    `**return** ret;<br>}|
| :- |
##
## Reads (synchmess\_read)
Reader threads concurrently access the group **messages** queue via **read** requests. As soon as one of them finds an undelivered message, it attempts to write it back to the user-space buffer.

As this operation may fail, threads need to wait before flagging the message as delivered. This case scenario creates a race condition for messages delivery.

Threads will race to atomically flag the message as delivered. In case more than one manages to write back to user-space, losers invalidate the user buffer and continue scanning the queue.






|**static** ssize\_t **synchmess\_read**(struct file \*filp, **char** \*buf, <br>`                              `**size\_t** count, **loff\_t** \*offp)<br>{<br>`    `**int** ret;<br>`    `**size\_t** readable\_bytes = 0;<br>`    `**unsigned** descriptor = MINOR(filp->f\_path.dentry->d\_inode->i\_rdev);<br>`    `**struct** **group** \***group** = **groups**[**descriptor** - 1];<br>`    `**struct** **message** \***message**, \***delivered\_message** = **NULL**;<br><br>`    `read\_lock(&group->messages\_rwlock);<br>`    `list\_for\_each\_entry(message, &group->messages, list) {<br>`        `**if** (!atomic\_read(&message->delivered)) {<br>`            `readable\_bytes = message->buf\_size <= count ? <br>`                             `message->buf\_size : count;<br>`   	      `ret = copy\_to\_user(buf, message->buffer, readable\_bytes);<br>`   	      `**if** (ret > 0) {<br>`   	          `printk(KERN\_ERR "%s: read error: copy\_to\_user failed\n", <br>`                       `KBUILD\_MODNAME);<br>`                `read\_unlock(&group->messages\_rwlock);<br>`                `**return** -EFAULT;<br>`   	      `}<br>`            `**if** (!atomic\_xchg(&message->delivered, true)) {<br>`   	          `delivered\_message = message;<br>`   	          `**break**;<br>`   	      `}<br>`            `memset(buf, 0, readable\_bytes);<br>`        `}<br>`    `}<br>`    `read\_unlock(&group->messages\_rwlock);<br><br>`    `**if** (delivered\_message) {<br>`        `write\_lock(&group->messages\_rwlock);<br>`   	  `list\_del(&delivered\_message->list);<br>`   	  `write\_unlock(&group->messages\_rwlock);<br><br>`   	  `atomic\_sub(delivered\_message->buf\_size, &group->current\_size);<br>`   	  `readable\_bytes = delivered\_message->buf\_size;<br>`   	  `free\_message(delivered\_message);<br>`     `}<br><br>`    `**return** readable\_bytes;<br>}|
| :- |
##
## Delay definition (synchmess\_set\_send\_delay)
Synchmess clients can set the delay for any group by issuing an **IOCTL\_SET\_SEND\_DELAY** request to the appropriate group device. The module will simply update the **delay** attribute. 

To preserve FIFO order delivery, threads cannot change the delay value for a group until there are no pending delayed messages in the **delayed\_messages** queue.

## Delayed writes (synchmess\_write, publisher\_work, publish\_message)
Upon setting the delay for any group, **write** operations will affect the message queue only after the delay time elapsed.

Synchmess allocates a **struct delayed\_message** and sets up a **struct delayed\_work**. 

The SynchMess **work\_queue** maintains a list of **delayed\_work** structs and triggers the associated callback functions as soon as the specified timer expires.

On the other hand, every group keeps track of received delayed messages using the **delayed\_messages** queue, which stores multiple **struct delayed\_message**.


|delayed\_message = kmalloc(**sizeof**(struct delayed\_message), GFP\_KERNEL);<br>**if** (!delayed\_message) {<br>`    `printk(KERN\_ERR "%s: delayed write error: kmalloc failed on delayed "               <br>`           `"message allocation\n", KBUILD\_MODNAME);<br>`    `ret = -ENOMEM;<br>`    `**goto** free\_message\_buffer;<br>}<br>INIT\_DELAYED\_WORK(&delayed\_message->delayed\_work, &publisher\_work);<br>delayed\_message->group = group;<br>delayed\_message->message = message;<br><br>mutex\_lock(&group->delayed\_messages\_mutex);<br>list\_add\_tail(&delayed\_message->list, &group->delayed\_messages);<br>queue\_delayed\_work(work\_queue, &delayed\_message->delayed\_work,<br>`   		       `msecs\_to\_jiffies(group->delay\_ms));<br>mutex\_unlock(&group->delayed\_messages\_mutex);<br><br>writable\_bytes = count;<br>**return** writable\_bytes;|
| :- |

Dedicated Linux kernel worker-threads will periodically wake up and execute callbacks registered with the multiple **struct delayed\_work** stored in the **work\_queue**.


|// Kernel threads waking up from the work\_queue execute this code<br>**static** **void** **publisher\_work**(struct work\_struct \*work)<br>{<br>`    `**struct** **delayed\_message** \***delayed\_message**;<br>`    `delayed\_message = container\_of((struct delayed\_work \*) work,<br>`   	                             `struct delayed\_message, delayed\_work);<br>`    `publish\_message(delayed\_message->group, delayed\_message->message);<br>    <br>`    `mutex\_lock(&delayed\_message->group->delayed\_messages\_mutex);<br>`    `list\_del(&delayed\_message->list);<br>`    `mutex\_unlock(&delayed\_message->group->delayed\_messages\_mutex);<br><br>`    `kfree(delayed\_message);<br>}|
| :- |
## Delayed messages revocation (synchmess\_revoke\_delayed\_messages)
Group Members can revoke any pending delayed message invoking an **IOCTL\_REVOKE\_DELAYED\_MESSAGES** on a group a device.

The module starts canceling the pending delayed work, also acquiring the mutex on the **delayed\_messages** queue.

Any kernel worker-thread concurrently running alongside with a revocation request that does not complete before the delayed work cancellation will safely complete the execution, thus inserting the message into the group message queue.


|/\*<br>` `\* Cancel the delayed\_work set up so far, revoked messages are never   <br>` `\* going to be delivered to readers, we need to destroy them here.<br>` `\* If some worker-thread is concurrently running, we let it continue the <br>` `\* execution. It will cleanup the delayed\_mesages queue on its own and it   <br>` `\* will correctly insert the delayed message into the messages queue).<br>` `\*/<br>**static** **long** **synchmess\_revoke\_delayed\_messages**(struct file \*filp)<br>{<br>`    `**int** ret;<br>`    `**unsigned** descriptor = MINOR(filp->f\_path.dentry->d\_inode->i\_rdev);<br>`    `**struct** **group** \***group** = **groups**[**descriptor** - 1];<br>`    `**struct** **delayed\_message** \***delayed\_message**, \***dummy**;<br><br>`    `mutex\_lock(&group->delayed\_messages\_mutex);<br>`    `list\_for\_each\_entry\_safe(delayed\_message, dummy,<br>`                             `&group->delayed\_messages, list) {<br>`        `ret = cancel\_delayed\_work(&delayed\_message->delayed\_work);<br>`   	  `**if** (ret) {<br>`   	      `free\_message(delayed\_message->message);<br>`   	      `list\_del(&delayed\_message->list);<br>`   	      `kfree(delayed\_message);<br>`        `}<br>`    `}<br>`    `mutex\_unlock(&group->delayed\_messages\_mutex);<br><br>`    `**return** 0;<br>}|
| :- |
##







## Flushing (synchmess\_fsync)
User-space threads can flush group devices using the **fsynch** system call.

Synchmess cancels all pending delayed\_work this time also, but it does not destroy it. Instead, it empties the group **delayed\_messages** queue and directly publishes every delayed message into the **messages** queue.


|/\* <br>` `\* Whenever we receive a flush request we cancel the delayed work set up  <br>` `\* so far and we directly publish the delayed messages into the group <br>` `\* message queue.<br>` `\*/<br>**static** **int** **synchmess\_fsync**(struct file \*filp, **loff\_t** offp1, **loff\_t** offp2,<br>`                           `**int** datasync)<br>{<br>`    `**int** ret;<br>`    `**unsigned** descriptor = MINOR(filp->f\_path.dentry->d\_inode->i\_rdev);<br>`    `**struct** **group** \***group** = **groups**[**descriptor** - 1];<br>`    `**struct** **delayed\_message** \***delayed\_message**, \***dummy**;<br><br>`    `mutex\_lock(&group->delayed\_messages\_mutex);<br>`    `list\_for\_each\_entry\_safe(delayed\_message, dummy,<br>`                             `&group->delayed\_messages, list) {<br>`   	 `ret = cancel\_delayed\_work(&delayed\_message->delayed\_work);<br>`   	 `**if** (ret) {<br>`   	      `publish\_message(delayed\_message->group, <br>`                            `delayed\_message->message);<br>`   	      `list\_del(&delayed\_message->list);<br>`   	      `kfree(delayed\_message);<br>`        `}<br>`    `}<br>`    `mutex\_unlock(&group->delayed\_messages\_mutex);<br><br>`    `**return** 0;<br>}|
| :- |



##
##
##





## Sleeping and waking up (synchmess\_sleep, sychmess\_awake)
Group Members can synchronize each other by sleeping on the group barrier using **IOCT\_SLEEP**. Every group maintains a Linux wait queue for this purpose.

Upon receiving a sleep request, SynchMess locally allocates a **struct sleeping\_thread** and inserts it into the **sleeping\_threads** group list. Finally, it preempts the **current** thread.

As soon as someone wakes them up, sleeping threads read the **awake** flag and decide whether to wake up or not.


|**static** **long** **synchmess\_sleep**(struct file \*filp)<br>{<br>`    `**struct** **sleeping\_thread** **sleeping\_thread** = { 0 };<br>`    `**unsigned** descriptor = MINOR(filp->f\_path.dentry->d\_inode->i\_rdev);<br>`    `**struct** **group** \***group** = **groups**[**descriptor** - 1];<br><br>`    `spin\_lock(&group->sleeping\_threads\_spinlock);<br>`    `list\_add(&sleeping\_thread.list, &group->sleeping\_threads);<br>`    `spin\_unlock(&group->sleeping\_threads\_spinlock);<br>`    `wait\_event\_interruptible(group->wait\_queue, <br>`                             `sleeping\_thread.awake == true);<br><br>`    `**return** 0;<br>}|
| :- |

Conversely, user-space threads can wake up group members using **IOCTL\_AWAKE.**

The module scans the **sleeping\_threads** list, sets the **awake** flag for each entry, and finally, it will wake up the group **wait\_queue**.


|**static** **long** **synchmess\_awake**(struct file \*filp)<br>{<br>`    `**unsigned** num\_awakened = 0; <br>`    `**unsigned** descriptor = MINOR(filp->f\_path.dentry->d\_inode->i\_rdev);<br>`    `**struct** **group** \***group** = **groups**[**descriptor** - 1];<br>`    `**struct** **sleeping\_thread** \***sleeping\_thread**, \***dummy**;<br><br>`    `spin\_lock(&group->sleeping\_threads\_spinlock);<br>`    `list\_for\_each\_entry\_safe(sleeping\_thread, dummy,<br>`                             `&group->sleeping\_threads, list) {<br>`   	 `list\_del(&sleeping\_thread->list);<br>`   	 `sleeping\_thread->awake = true;<br>`   	 `num\_awakened++;<br>`    `}<br>`    `wake\_up\_all(&group->wait\_queue);<br>`    `spin\_unlock(&group->sleeping\_threads\_spinlock);<br><br>`    `**return** num\_awakened;<br>}|
| :- |
## Locking and concurrency management
The **struct group** contains multiple Linux locks implementations, namely **mutexes**, **rwlocks,** and **spinlocks**.
### Spinlock
Spinlocks are the simplest kind of lock and guard the access to **current\_size** and **sleeping\_threads**.

The lock on the **current\_size** attribute is required as writer threads need to check the group size before inserting a message into the queue, the lock ensures only one thread at a time reads the value.


|// publish\_message function <br>spin\_lock(&group->size\_spinlock);<br>**if** (atomic\_read(&group->current\_size) + message->buf\_size ><br>`        `MAX\_STORAGE\_SIZE) {<br>`    `ret = -EPERM;<br>`    `**goto** unlock\_spinlock;<br>}<br>atomic\_add(message->buf\_size, &group->current\_size);<br>spin\_unlock(&group->size\_spinlock);|
| :- |

The lock on **sleeping\_threads** is required to make the list thread-safe, thus allowing it to receive concurrent requests of insertion and deletion.

In both cases, spinlocks are the preferred lock implementation since the critical section execution time should be extremely short.
### Mutex
One mutex per group guards the access to the **delayed\_messages** queue. Delayed **write** requests insert delayed messages into this queue, whereas the kernel worker-threads remove its entries whenever the callbacks inside the **work\_queue** execute.


|// Kernel worker-thread<br>mutex\_lock(&delayed\_message->group->delayed\_messages\_mutex);<br>list\_del(&delayed\_message->list);<br>mutex\_unlock(&delayed\_message->group->delayed\_messages\_mutex);<br><br>// Write syscall<br>mutex\_lock(&group->delayed\_messages\_mutex);<br>list\_add\_tail(&delayed\_message->list, &group->delayed\_messages);<br>queue\_delayed\_work(work\_queue, &delayed\_message->delayed\_work,<br>`   		       `msecs\_to\_jiffies(group->delay\_ms));<br>mutex\_unlock(&group->delayed\_messages\_mutex);|
| :- |

Upon flushing delayed messages, we need to let all concurrent kernel worker-thread complete the execution. 

|// fsync syscall<br>mutex\_lock(&group->delayed\_messages\_mutex);<br>list\_for\_each\_entry\_safe(delayed\_message, dummy, <br>`                         `&group->delayed\_messages, list) {<br>`    `// ret == 0 if the delayed\_work is currently running on some cpu.<br>`    `ret = cancel\_delayed\_work(&delayed\_message->delayed\_work);<br>`    `**if** (ret) {<br>`        `publish\_message(delayed\_message->group,delayed\_message->message);<br>`        `list\_del(&delayed\_message->list);<br>`   	  `kfree(delayed\_message);<br>`   	  `atomic\_sub(**sizeof**(struct delayed\_message), &allocated\_bytes);<br>`    `}<br>}<br>mutex\_unlock(&group->delayed\_messages\_mutex);|
| :- |

Suppose the Kernel worker-thread retrieves the message ***M1*** from the **delayed\_messages** queue. Also, suppose SynchMess receives a concurrent fsync request and retrieves message ***M2*** stored in the queue.

We have two execution paths:


|**Kernel Worker Thread**|**Flushing Thread** |
| :- | :- |
|mutex\_lock()||
||mutex\_lock()|
|deliver(***M1)***||
|unlock\_mutex()||
||deliver(***M2)***|
||unlock\_mutex()|



|**Kernel Worker Thread**|**Flushing Thread** |
| :- | :- |
||mutex\_lock()|
|mutex\_lock()||
||deliver(***M2)***|
||unlock\_mutex()|
|deliver(***M1)***||
|unlock\_mutex()||

FIFO order delivery thus depends on which thread acquires the lock first. Synchmess enforces the order adhering to this principle.

A mutex, in this case, is the preferred type of lock since the probability of having contention on queue operations (insertions and deletions boil down to simple pointer operations on the queue entries) is very low.

On the other hand, contention during a flush request may cause the contending thread to spin for a long time, thus making going to sleep a preferable option.
### Read-Write Lock
Finally, a Linux **rwlock** protects the **messages** queue. The choice of this kind of lock implementation seems trivial, considered the queue access patterns.

Readers can concurrently read the queue to logically deliver a message, writers operate in isolation instead.


|// Writers<br>write\_lock(&group->messages\_rwlock);<br>list\_add\_tail(&message->list, &group->messages);<br>write\_unlock(&group->messages\_rwlock);<br><br>// Readers<br>read\_lock(&group->messages\_rwlock);<br>list\_for\_each\_entry(message, &group->messages, list) {<br>`    `**if** (!atomic\_read(&message->delivered)) {<br>`        `readable\_bytes = message->buf\_size <= count ?<br>`                         `message->buf\_size : count;<br>`        `ret = copy\_to\_user(buf, message->buffer, readable\_bytes);<br>`        `**if** (!atomic\_xchg(&message->delivered, true)) {<br>`   	      `delivered\_message = message;<br>`   		`**break**;<br>`   	  `}<br>`        `memset(buf, 0, readable\_bytes);<br>`    `}<br>}<br>read\_unlock(&group->messages\_rwlock);|
| :- |

The **rwlock** is preferred to spinlocks as it should enhance the number of concurrent operations in the subsystem.








# Testcase and Benchmark
The benchmark application tries to put SynchMess under a heavy request load.

Eight Producer threads and eight consumer threads register to every SynchMess group and start issuing requests for thirty seconds.

Producers continuously pick a random group and write a message into the queue, whereas readers try to read messages from random groups.

Threads track the execution times for **write** and **read** operations and compute an average when the thirty seconds timeout elapses.


|**while** (!standard\_rw\_time\_elapsed) {<br>`    `r = rand() % NUM\_GROUPS;<br>`    `start = clock();<br>`    `ret = write(fds[r], message, strlen(message));<br>`    `end = clock();<br><br>`    `**if** (ret < 0)<br>`        `fprintf(stderr, "producer %u: failed write, %s\n",<br>`                `thread\_arg->id, strerror(errno));<br><br>`    `**if** (ret == 0)<br>`        `**continue**;<br><br>`    `thread\_arg->avg\_write\_time\_ms += ((**double**) (end - start)) \* 1000 / <br>`                                     `CLOCKS\_PER\_SEC;<br>`    `num\_writes++;<br>}|
| :- |

Upon timer expiration, one producer thread sets a delay value for every group. Later, it synchronizes with other producers. Finally, every producer starts sending delayed messages at random for another thirty seconds.

On the consumers’ side, one consumer thread wakes up the producers and benchmarks the flush operation randomly using the fsync system call, also calculating average execution times once again.

Before exiting, the benchmark application revokes any undelivered delayed messages, thus using every function offered by the module.

To run the benchmark application, please insert the **udev** rule file into your system rules:


|cp 50-synchmess.rules /etc/udev/rules.d/|
| :- |

Restart udev daemon using:


|udevadm control --reload-rules && udevadm trigger|
| :- |



Compile the module (and benchmark) and load it into the kernel:


|make<br>insmod synchmess.ko|
| :- |

Finally. run the benchmark using:


|./benchmark.sh|
| :- |

The following screenshot shows the benchmark results for a virtual Ubuntu 20.04 machine running on top of an Intel Core i5-6600 processor.

![](Image\_0)


