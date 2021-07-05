#include <linux/atomic.h>
#include <linux/bitmap.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include "synchmess.h"
#include "synchmess_interface.h"



static int MAX_MESSAGE_SIZE = 1024;
module_param(MAX_MESSAGE_SIZE, int, 0664);
MODULE_PARM_DESC(MAX_MESSAGE_SIZE, "The maximum size (bytes) currently allowed for sending "
	"messages to a device file");

static int MAX_STORAGE_SIZE = 1048576;
module_param(MAX_STORAGE_SIZE, int, 0664);
MODULE_PARM_DESC(MAX_STORAGE_SIZE, "The maximum number of bytes allowed for keeping messages in a "
	"device file");

static int major;
static struct class *dev_cl = NULL;
static struct device *master_dev = NULL;

static struct group *groups[MAX_NUM_GROUPS] = { NULL };
static DECLARE_BITMAP(groups_bitmap, MAX_NUM_GROUPS);
static struct workqueue_struct *work_queue;

// No operations allowed during module release.
static bool releasing = false;

// Keep track of allocations/deallocations for memory leak debugging.
atomic_t allocated_bytes = ATOMIC_INIT(0);


static void free_message(struct message *message)
{
	kfree(message->buffer);
	atomic_sub(message->buf_size, &allocated_bytes);
	kfree(message);
	atomic_sub(sizeof(struct message), &allocated_bytes);
}


static void free_group(struct group *group)
{
	struct message *message, *m_dummy;
	struct delayed_message *delayed_message, *dm_dummy;

	write_lock(&group->messages_rwlock);
	list_for_each_entry_safe(message, m_dummy, &group->messages, list) {
		list_del(&message->list);
		free_message(message);
	}
	write_unlock(&group->messages_rwlock);

	mutex_lock(&group->delayed_messages_mutex);
	list_for_each_entry_safe(delayed_message, dm_dummy, &group->delayed_messages, list) {
		cancel_delayed_work(&delayed_message->delayed_work);
		free_message(delayed_message->message);
		list_del(&delayed_message->list);
		kfree(delayed_message);
		atomic_sub(sizeof(struct delayed_message), &allocated_bytes);
	}
	mutex_unlock(&group->delayed_messages_mutex);
}


/*
 * Drop the message whenever the publication would cause the device to exceed the maximum storage
 * size.
 */
static ssize_t publish_message(struct group *group, struct message *message)
{
	int ret;

	spin_lock(&group->size_spinlock);
	if (atomic_read(&group->current_size) + message->buf_size > MAX_STORAGE_SIZE) {
		ret = -EPERM;
		goto unlock_spinlock;
	}
	atomic_add(message->buf_size, &group->current_size);
	spin_unlock(&group->size_spinlock);

	write_lock(&group->messages_rwlock);
	list_add_tail(&message->list, &group->messages);
	write_unlock(&group->messages_rwlock);

	return message->buf_size;


unlock_spinlock:
	spin_unlock(&group->size_spinlock);
	free_message(message);
	return ret;
}


// Kernel threads waking up from the work_queue execute this code
static void publisher_work(struct work_struct *work)
{
	struct delayed_message *delayed_message;

	if (releasing)
		return;

	delayed_message = container_of((struct delayed_work *) work,
		struct delayed_message,
		delayed_work);


	mutex_lock(&delayed_message->group->delayed_messages_mutex);
	publish_message(delayed_message->group, delayed_message->message);
	list_del(&delayed_message->list);
	mutex_unlock(&delayed_message->group->delayed_messages_mutex);

	kfree(delayed_message);
	atomic_sub(sizeof(struct delayed_message), &allocated_bytes);
}


/*
 * Race condition whenever multiple threads try to install the same group device. Threads
 * will try to atomically set the bitmap in order to continue execution. As soon as someone
 * correctly installs the device, it updates the groups global array and consequently
 * lets all threads continue the execution.
 */
static long synchmess_install_group(struct file *filp, unsigned long arg)
{
	int ret, old_bit = 1;
	struct group_t group_desc;
	struct group *group;
	struct device *dev;
	char dev_name[64] = {0}, dev_minor[16] = {0};

	ret = copy_from_user(&group_desc, (struct groupt_t *) arg, sizeof(struct group_t));
	if (ret > 0) {
		printk(KERN_ERR "%s: install_group error: copy_from_user failed\n", KBUILD_MODNAME);
		return -EFAULT;
	}

	if (group_desc.descriptor < 1 || group_desc.descriptor > MAX_NUM_GROUPS) {
		printk(KERN_ERR "%s: install_group error: invalid descriptor %d\n", KBUILD_MODNAME,
			group_desc.descriptor);
		return -EBADF;
	}

	sprintf(dev_minor, "%u", group_desc.descriptor);
	strncpy(dev_name, KBUILD_MODNAME, strlen(KBUILD_MODNAME));
	strncat(dev_name, dev_minor, strlen(dev_minor));
	strncpy(group_desc.device_path, DEVICES_HOME_DIR, strlen(DEVICES_HOME_DIR) + 1);
	strncat(group_desc.device_path, dev_name, strlen(dev_name));

	ret = copy_to_user((struct group_t *) arg, &group_desc, sizeof(struct group_t));
	if (ret > 0) {
		printk(KERN_ERR "%s: install_group error: copy_to_user failed\n", KBUILD_MODNAME);
		return -EFAULT;
	}

	while (!groups[group_desc.descriptor - 1]) {
		old_bit = test_and_set_bit(group_desc.descriptor - 1, groups_bitmap);
		if (!old_bit)
			break;
		else
			yield();
	}

	if (!old_bit) {
		memset(dev_name, 0, strlen(dev_name));
		strncpy(dev_name, "synch", strlen("synch"));
		strncat(dev_name, "!", strlen("!"));
		strncat(dev_name, KBUILD_MODNAME, strlen(KBUILD_MODNAME));
		strncat(dev_name, dev_minor, strlen(dev_minor));

		dev = device_create(dev_cl, NULL, MKDEV(major, group_desc.descriptor), NULL,
			dev_name);

		if (IS_ERR(dev)) {
			printk(KERN_ERR "%s: install_group error: device_create failed\n",
				KBUILD_MODNAME);
			ret = PTR_ERR(dev);
			goto invalidate_path;
		}

		group = kzalloc(sizeof(struct group), GFP_KERNEL);
		if (!group) {
			printk(KERN_ERR "%s: install_group error: kzalloc failed on group"
				"allocation\n", KBUILD_MODNAME);
			ret = -ENOMEM;
			goto destroy_device;
		}
		atomic_add(sizeof(struct group), &allocated_bytes);

		rwlock_init(&group->messages_rwlock);
		mutex_init(&group->delayed_messages_mutex);
		spin_lock_init(&group->sleeping_threads_spinlock);
		spin_lock_init(&group->size_spinlock);
		init_waitqueue_head(&group->wait_queue);
		INIT_LIST_HEAD(&group->sleeping_threads);
		INIT_LIST_HEAD(&group->messages);
		INIT_LIST_HEAD(&group->delayed_messages);
		group->descriptor = group_desc.descriptor;

		groups[group_desc.descriptor - 1] = group;
	}

	return 0;


destroy_device:
	device_destroy(dev_cl, MKDEV(major, group_desc.descriptor));
invalidate_path:
	clear_bit(group_desc.descriptor - 1, groups_bitmap);
	memset(((struct group_t *) arg)->device_path, 0, sizeof(group_desc.device_path));
	return ret;
}


static long synchmess_sleep(struct file *filp)
{
	struct sleeping_thread sleeping_thread = { 0 };
	unsigned descriptor = MINOR(filp->f_path.dentry->d_inode->i_rdev);
	struct group *group = groups[descriptor - 1];

	spin_lock(&group->sleeping_threads_spinlock);
	list_add(&sleeping_thread.list, &group->sleeping_threads);
	spin_unlock(&group->sleeping_threads_spinlock);

	wait_event_interruptible(group->wait_queue, sleeping_thread.awake == true);

	return 0;
}


static long synchmess_awake(struct file *filp)
{
	unsigned num_awakened = 0, descriptor = MINOR(filp->f_path.dentry->d_inode->i_rdev);
	struct group *group = groups[descriptor - 1];
	struct sleeping_thread *sleeping_thread, *dummy;

	spin_lock(&group->sleeping_threads_spinlock);
	list_for_each_entry_safe(sleeping_thread, dummy, &group->sleeping_threads, list) {
		list_del(&sleeping_thread->list);
		sleeping_thread->awake = true;
		num_awakened++;
	}
	wake_up_all(&group->wait_queue);
	spin_unlock(&group->sleeping_threads_spinlock);

	return num_awakened;
}


static long synchmess_set_send_delay(struct file *filp, unsigned long arg)
{
	int ret;
	unsigned descriptor = MINOR(filp->f_path.dentry->d_inode->i_rdev);
	struct group *group = groups[descriptor - 1];

	if (!list_empty(&group->delayed_messages)) {
		printk(KERN_ERR "%s: set_send_delay error: received request while work was "
			"pending\n", KBUILD_MODNAME);
		return -EPERM;
	}

	ret = copy_from_user(&group->delay_ms, (unsigned *) arg, sizeof(unsigned));
	if (ret > 0) {
		printk(KERN_ERR "%s: set_send_delay error: copy_from_user failed\n",
			KBUILD_MODNAME);
		return -EFAULT;
	}

	return 0;
}


/*
 * Cancel the delayed_work set up so far, revoked messages are never going to be delivered to
 * readers, we need to destroy them here.
 * If some kernel thread is concurrently running, we let it continue the execution.
 * It will cleanup the delayed_mesages queue on its own and it will correctly insert
 * the delayed message into the messages queue).
 */
static long synchmess_revoke_delayed_messages(struct file *filp)
{
	int ret;
	unsigned descriptor = MINOR(filp->f_path.dentry->d_inode->i_rdev);
	struct group *group = groups[descriptor - 1];
	struct delayed_message *delayed_message, *dummy;

	mutex_lock(&group->delayed_messages_mutex);
	list_for_each_entry_safe(delayed_message, dummy, &group->delayed_messages, list) {
		ret = cancel_delayed_work(&delayed_message->delayed_work);
		if (ret) {
			free_message(delayed_message->message);
			list_del(&delayed_message->list);
			kfree(delayed_message);
			atomic_sub(sizeof(struct delayed_message), &allocated_bytes);
		}
	}
	mutex_unlock(&group->delayed_messages_mutex);

	return 0;
}


// Master device is not writable
static int synchmess_open(struct inode *inode, struct file *filp)
{
	int flags;
	unsigned descriptor = MINOR(inode->i_rdev);

	if (releasing)
		return -EBUSY;

	if (descriptor == 0) {
		flags = filp->f_flags & O_ACCMODE;
		if (flags & (O_WRONLY | O_RDWR))
			return -EPERM;
	}

	return 0;
}


/*
 * Directly insert the new message into the group queue if no member previously set a delay.
 * Otherwise insert deferred work into the work queue. The kthread callback function
 * will execute the delayed work and publish the message after the delay period.
 */
static ssize_t synchmess_write(struct file *filp, const char *buf, size_t count, loff_t *offp)
{
	int ret;
	size_t writable_bytes;
	unsigned descriptor = MINOR(filp->f_path.dentry->d_inode->i_rdev);
	struct group *group = groups[descriptor - 1];
	struct message *message;
	struct delayed_message *delayed_message;

	if (releasing)
		return -EBUSY;

	if (count > MAX_MESSAGE_SIZE) {
		printk(KERN_ERR "%s: write error: message length exceeds size limit\n",
			KBUILD_MODNAME);
		return -EPERM;
	}

	message = kzalloc(sizeof(struct message), GFP_KERNEL);
	if (!message) {
		printk(KERN_ERR "%s: write error: kzalloc failed on message allocation\n",
			KBUILD_MODNAME);
		return -ENOMEM;
	}
	atomic_add(sizeof(struct message), &allocated_bytes);

	message->buffer = kmalloc(sizeof(char) * count, GFP_KERNEL);
	if (!message->buffer) {
		printk(KERN_ERR "%s: write error: kmalloc failed on message buffer allocation\n",
			KBUILD_MODNAME);
		ret = -ENOMEM;
		goto free_message;
	}
	atomic_add(sizeof(char) * count, &allocated_bytes);

	ret = copy_from_user(message->buffer, buf, count);
	if (ret > 0) {
		printk(KERN_ERR "%s: write error: copy_from_user failed\n", KBUILD_MODNAME);
		ret = -EFAULT;
		goto free_message_buffer;
	}

	message->buf_size = count;

	if (group->delay_ms == 0) {
		writable_bytes = publish_message(group, message);
	} else {
		delayed_message = kmalloc(sizeof(struct delayed_message), GFP_KERNEL);
		if (!delayed_message) {
			printk(KERN_ERR "%s: delayed write error: kmalloc failed on delayed message"
				"allocation\n", KBUILD_MODNAME);
			ret = -ENOMEM;
			goto free_message_buffer;
		}
		atomic_add(sizeof(struct delayed_message), &allocated_bytes);

		INIT_DELAYED_WORK(&delayed_message->delayed_work, &publisher_work);
		delayed_message->group = group;
		delayed_message->message = message;

		mutex_lock(&group->delayed_messages_mutex);
		list_add_tail(&delayed_message->list, &group->delayed_messages);
		queue_delayed_work(work_queue, &delayed_message->delayed_work,
			msecs_to_jiffies(group->delay_ms));
		mutex_unlock(&group->delayed_messages_mutex);

		writable_bytes = count;
	}

	return writable_bytes;


free_message_buffer:
	kfree(message->buffer);
	atomic_sub(sizeof(char) * count, &allocated_bytes);
free_message:
	kfree(message);
	atomic_sub(sizeof(struct message), &allocated_bytes);
	return ret;
}


/*
 * Race condition. Threads will try to atomically set the delivered flag. The winner thread reads
 * the message and delivers, losers invalidate the user buffer and continue scanning the list.
 */
static ssize_t synchmess_read(struct file *filp, char *buf, size_t count, loff_t *offp)
{
	int ret;
	size_t readable_bytes = 0;
	unsigned descriptor = MINOR(filp->f_path.dentry->d_inode->i_rdev);
	struct group *group = groups[descriptor - 1];
	struct message *message, *delivered_message = NULL;

	if (releasing)
		return -EBUSY;

	if (descriptor == 0)
		return -EPERM;

	read_lock(&group->messages_rwlock);
	list_for_each_entry(message, &group->messages, list) {
		if (!atomic_read(&message->delivered)) {
			readable_bytes = message->buf_size <= count ? message->buf_size : count;

			ret = copy_to_user(buf, message->buffer, readable_bytes);
			if (ret > 0) {
				printk(KERN_ERR "%s: read error: copy_to_user failed\n",
					KBUILD_MODNAME);
				read_unlock(&group->messages_rwlock);
				return -EFAULT;
			}

			if (!atomic_xchg(&message->delivered, true)) {
				delivered_message = message;
				break;
			}

			memset(buf, 0, readable_bytes);
		}
	}
	read_unlock(&group->messages_rwlock);

	if (delivered_message) {
		write_lock(&group->messages_rwlock);
		list_del(&delivered_message->list);
		write_unlock(&group->messages_rwlock);

		atomic_sub(delivered_message->buf_size, &group->current_size);
		readable_bytes = delivered_message->buf_size;
		free_message(delivered_message);
	}

	return readable_bytes;
}


/*
 * Whenever we receive a flush request we cancel the delayed work set up so far and we directly
 * publish the delayed messages into the group message queue.
 */
static int synchmess_fsync(struct file *filp, loff_t offp1, loff_t offp2, int datasync)
{
	int ret;
	unsigned descriptor = MINOR(filp->f_path.dentry->d_inode->i_rdev);
	struct group *group = groups[descriptor - 1];
	struct delayed_message *delayed_message, *dummy;

	if (releasing)
		return -EBUSY;

	if (descriptor == 0)
		return -EPERM;

	mutex_lock(&group->delayed_messages_mutex);
	list_for_each_entry_safe(delayed_message, dummy, &group->delayed_messages, list) {
		ret = cancel_delayed_work(&delayed_message->delayed_work);
		if (ret) {
			publish_message(delayed_message->group, delayed_message->message);
			list_del(&delayed_message->list);
			kfree(delayed_message);
			atomic_sub(sizeof(struct delayed_message), &allocated_bytes);
		}
	}
	mutex_unlock(&group->delayed_messages_mutex);

	return 0;
}


static long synchmess_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	int ret;
	unsigned descriptor = MINOR(filp->f_path.dentry->d_inode->i_rdev);

	if (releasing)
		return -EBUSY;

	if (descriptor == 0) {
		switch (cmd) {
		case IOCTL_INSTALL_GROUP:
			ret = synchmess_install_group(filp, arg);
			break;
		default:
			printk(KERN_DEBUG "%s: ioctl error: requested unsupported operation to "
				"master device\n", KBUILD_MODNAME);
			ret = -EPERM;
			break;
		}
	} else {
		switch (cmd) {
		case IOCTL_SLEEP:
			ret = synchmess_sleep(filp);
			break;
		case IOCTL_AWAKE:
			ret = synchmess_awake(filp);
			break;
		case IOCTL_SET_SEND_DELAY:
			ret = synchmess_set_send_delay(filp, arg);
			break;
		case IOCTL_REVOKE_DELAYED_MESSAGES:
			ret = synchmess_revoke_delayed_messages(filp);
			break;
		default:
			printk(KERN_DEBUG "%s: ioctl error, requested unsupported operation to "
				"group %d device\n", KBUILD_MODNAME, descriptor);
			ret = -EPERM;
			break;
		}
	}

	return ret;
}


// File operations for the module
struct file_operations fops = {
	open: synchmess_open,
	write: synchmess_write,
	read: synchmess_read,
	fsync: synchmess_fsync,
	unlocked_ioctl: synchmess_ioctl,
	compat_ioctl: synchmess_ioctl
};


/*
 * Create a work_queue for the module, dynamically allocate a major number for the devices, create
 * a class for the devices and finally install the master device.
 * This device is used to install new groups. The only allowed system calls for this device are
 * open and IOCTL_INSTALL. Master device is associated with 0 as the minor number.
 */
static int __init synchmess_init(void)
{
	int ret;

	work_queue = create_workqueue(KBUILD_MODNAME);

	major = register_chrdev(0, KBUILD_MODNAME, &fops);
	if (major < 0) {
		printk(KERN_ERR "%s: failed to register devices major number\n", KBUILD_MODNAME);
		return major;
	}
	printk(KERN_DEBUG "%s: registered devices major number %d\n", KBUILD_MODNAME, major);

	dev_cl = class_create(THIS_MODULE, KBUILD_MODNAME);
	if (IS_ERR(dev_cl)) {
		printk(KERN_ERR "%s: failed to register devices class\n", KBUILD_MODNAME);
		ret = PTR_ERR(dev_cl);
		goto unregister_dev;
	}
	printk(KERN_DEBUG "%s: registered devices class %s\n", KBUILD_MODNAME, dev_cl->name);

	master_dev = device_create(dev_cl, NULL, MKDEV(major, 0), NULL, "synch" "!" KBUILD_MODNAME);
	if (IS_ERR(master_dev)) {
		printk(KERN_ERR "%s: failed to create master device\n", KBUILD_MODNAME);
		ret = PTR_ERR(master_dev);
		goto unregister_class;
	}
	printk(KERN_DEBUG "%s: registered master device (major: %d, minor: %d)\n", KBUILD_MODNAME,
			major, 0);

	printk(KERN_DEBUG "%s: module successfully installed\n", KBUILD_MODNAME);

	return 0;


unregister_class:
	class_unregister(dev_cl);
	class_destroy(dev_cl);
unregister_dev:
	unregister_chrdev(major, KBUILD_MODNAME);
	return ret;
}


static void __exit synchmess_exit(void)
{
	unsigned i;

	releasing = true;

	for (i = 0; i < MAX_NUM_GROUPS; i++) {
		if (groups[i]) {
			free_group(groups[i]);
			kfree(groups[i]);
			atomic_sub(sizeof(struct group), &allocated_bytes);
			device_destroy(dev_cl, MKDEV(major, i + 1));
		}
	}

	destroy_workqueue(work_queue);
	device_destroy(dev_cl, MKDEV(major, 0));
	class_unregister(dev_cl);
	class_destroy(dev_cl);
	unregister_chrdev(major, KBUILD_MODNAME);
	printk(KERN_INFO "%s: module successfully uninstalled, %d bytes were not correctly deallocated\n",
		KBUILD_MODNAME, atomic_read(&allocated_bytes));
}



MODULE_AUTHOR("Giuseppe D'Aquanno <daquanno.1712078@studenti.uniroma1.it>");
MODULE_DESCRIPTION("Thread synchronization and messange exchange service");
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0.0");

module_init(synchmess_init);
module_exit(synchmess_exit);
