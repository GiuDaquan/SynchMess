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
#include <linux/version.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include "synchmess.h"
#include "synchmess_interface.h"

static int MAX_MESSAGE_SIZE = 1024;
module_param(MAX_MESSAGE_SIZE, int, 0664);
MODULE_PARM_DESC(MAX_MESSAGE_SIZE, "Maximum size (bytes) for sending messages");

static int MAX_STORAGE_SIZE = 1048576;
module_param(MAX_STORAGE_SIZE, int, 0664);
MODULE_PARM_DESC(MAX_STORAGE_SIZE, "Maximum storage size (bytes) for messages");

static int major;
static struct class *dev_cl = NULL;
static struct device *master_dev = NULL;

static struct group *groups[MAX_NUM_GROUPS] = { NULL };
static DEFINE_MUTEX(install_mutex);
static struct workqueue_struct *work_queue;

static bool releasing = false;

static void free_message(struct message *message)
{
	if (!message) return;
	kfree(message->buffer);
	kfree(message);
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
		cancel_delayed_work_sync(&delayed_message->delayed_work);
		free_message(delayed_message->message);
		list_del(&delayed_message->list);
		kfree(delayed_message);
	}
	mutex_unlock(&group->delayed_messages_mutex);
}

static ssize_t publish_message(struct group *group, struct message *message)
{
	spin_lock(&group->size_spinlock);
	if (atomic_read(&group->current_size) + message->buf_size > MAX_STORAGE_SIZE) {
		spin_unlock(&group->size_spinlock);
		free_message(message);
		return -EPERM;
	}
	atomic_add(message->buf_size, &group->current_size);
	spin_unlock(&group->size_spinlock);

	write_lock(&group->messages_rwlock);
	list_add_tail(&message->list, &group->messages);
	write_unlock(&group->messages_rwlock);

	return message->buf_size;
}

static void publisher_work(struct work_struct *work)
{
	struct delayed_message *delayed_message;

	if (releasing)
		return;

	delayed_message = container_of((struct delayed_work *) work, struct delayed_message, delayed_work);

	mutex_lock(&delayed_message->group->delayed_messages_mutex);
	publish_message(delayed_message->group, delayed_message->message);
	list_del(&delayed_message->list);
	mutex_unlock(&delayed_message->group->delayed_messages_mutex);

	kfree(delayed_message);
}

static long synchmess_install_group(struct file *filp, unsigned long arg)
{
	struct group_t group_desc;
	struct group *group;
	struct device *dev;
	char dev_name[64] = {0}, dev_minor[16] = {0};

	if (copy_from_user(&group_desc, (struct group_t __user *) arg, sizeof(struct group_t)))
		return -EFAULT;

	if (group_desc.descriptor < 1 || group_desc.descriptor > MAX_NUM_GROUPS)
		return -EBADF;

	snprintf(dev_minor, sizeof(dev_minor), "%u", group_desc.descriptor);
	snprintf(dev_name, sizeof(dev_name), "%s%s", KBUILD_MODNAME, dev_minor);
	snprintf(group_desc.device_path, sizeof(group_desc.device_path), "%s%s", DEVICES_HOME_DIR, dev_name);

	if (copy_to_user((struct group_t __user *) arg, &group_desc, sizeof(struct group_t)))
		return -EFAULT;

	mutex_lock(&install_mutex);

	if (!groups[group_desc.descriptor - 1]) {
		snprintf(dev_name, sizeof(dev_name), "synch!%s%s", KBUILD_MODNAME, dev_minor);

		dev = device_create(dev_cl, NULL, MKDEV(major, group_desc.descriptor), NULL, dev_name);
		if (IS_ERR(dev)) {
			mutex_unlock(&install_mutex);
			return PTR_ERR(dev);
		}

		group = kzalloc(sizeof(struct group), GFP_KERNEL);
		if (!group) {
			device_destroy(dev_cl, MKDEV(major, group_desc.descriptor));
			mutex_unlock(&install_mutex);
			return -ENOMEM;
		}

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

	mutex_unlock(&install_mutex);
	return 0;
}

static long synchmess_sleep(struct file *filp)
{
	struct sleeping_thread sleeping_thread = { .awake = false };
	unsigned descriptor = iminor(file_inode(filp));
	struct group *group = groups[descriptor - 1];

	spin_lock(&group->sleeping_threads_spinlock);
	list_add(&sleeping_thread.list, &group->sleeping_threads);
	spin_unlock(&group->sleeping_threads_spinlock);

	wait_event_interruptible(group->wait_queue, sleeping_thread.awake == true);
	return 0;
}

static long synchmess_awake(struct file *filp)
{
	unsigned num_awakened = 0;
	unsigned descriptor = iminor(file_inode(filp));
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
	unsigned descriptor = iminor(file_inode(filp));
	struct group *group = groups[descriptor - 1];

	if (!list_empty(&group->delayed_messages))
		return -EPERM;

	if (copy_from_user(&group->delay_ms, (unsigned __user *) arg, sizeof(unsigned)))
		return -EFAULT;

	return 0;
}

static long synchmess_revoke_delayed_messages(struct file *filp)
{
	unsigned descriptor = iminor(file_inode(filp));
	struct group *group = groups[descriptor - 1];
	struct delayed_message *delayed_message, *dummy;

	mutex_lock(&group->delayed_messages_mutex);
	list_for_each_entry_safe(delayed_message, dummy, &group->delayed_messages, list) {
		if (cancel_delayed_work(&delayed_message->delayed_work)) {
			free_message(delayed_message->message);
			list_del(&delayed_message->list);
			kfree(delayed_message);
		}
	}
	mutex_unlock(&group->delayed_messages_mutex);

	return 0;
}

static int synchmess_open(struct inode *inode, struct file *filp)
{
	unsigned descriptor = iminor(inode);

	if (releasing) return -EBUSY;

	if (descriptor == 0 && (filp->f_flags & (O_WRONLY | O_RDWR)))
		return -EPERM;

	return 0;
}

static ssize_t synchmess_write(struct file *filp, const char __user *buf, size_t count, loff_t *offp)
{
	size_t writable_bytes;
	unsigned descriptor = iminor(file_inode(filp));
	struct group *group = groups[descriptor - 1];
	struct message *message;
	struct delayed_message *delayed_message;

	if (releasing) return -EBUSY;
	if (count > MAX_MESSAGE_SIZE) return -EPERM;

	message = kzalloc(sizeof(struct message), GFP_KERNEL);
	if (!message) return -ENOMEM;

	message->buffer = kmalloc(count, GFP_KERNEL);
	if (!message->buffer) {
		kfree(message);
		return -ENOMEM;
	}

	if (copy_from_user(message->buffer, buf, count)) {
		free_message(message);
		return -EFAULT;
	}

	message->buf_size = count;

	if (group->delay_ms == 0) {
		writable_bytes = publish_message(group, message);
	} else {
		delayed_message = kmalloc(sizeof(struct delayed_message), GFP_KERNEL);
		if (!delayed_message) {
			free_message(message);
			return -ENOMEM;
		}

		INIT_DELAYED_WORK(&delayed_message->delayed_work, publisher_work);
		delayed_message->group = group;
		delayed_message->message = message;

		mutex_lock(&group->delayed_messages_mutex);
		list_add_tail(&delayed_message->list, &group->delayed_messages);
		queue_delayed_work(work_queue, &delayed_message->delayed_work, msecs_to_jiffies(group->delay_ms));
		mutex_unlock(&group->delayed_messages_mutex);

		writable_bytes = count;
	}

	return writable_bytes;
}

static ssize_t synchmess_read(struct file *filp, char __user *buf, size_t count, loff_t *offp)
{
	size_t readable_bytes = 0;
	unsigned descriptor = iminor(file_inode(filp));
	struct group *group;
	struct message *delivered_message = NULL;

	if (releasing) return -EBUSY;
	if (descriptor == 0) return -EPERM;

	group = groups[descriptor - 1];

	write_lock(&group->messages_rwlock);
	if (!list_empty(&group->messages)) {
		delivered_message = list_first_entry(&group->messages, struct message, list);
		list_del(&delivered_message->list);
	}
	write_unlock(&group->messages_rwlock);

	if (delivered_message) {
		readable_bytes = min(delivered_message->buf_size, count);

		if (copy_to_user(buf, delivered_message->buffer, readable_bytes)) {
			free_message(delivered_message);
			return -EFAULT;
		}

		atomic_sub(delivered_message->buf_size, &group->current_size);
		free_message(delivered_message);
		return readable_bytes;
	}

	return 0;
}

static int synchmess_fsync(struct file *filp, loff_t offp1, loff_t offp2, int datasync)
{
	unsigned descriptor = iminor(file_inode(filp));
	struct group *group = groups[descriptor - 1];
	struct delayed_message *delayed_message, *dummy;

	if (releasing) return -EBUSY;
	if (descriptor == 0) return -EPERM;

	mutex_lock(&group->delayed_messages_mutex);
	list_for_each_entry_safe(delayed_message, dummy, &group->delayed_messages, list) {
		if (cancel_delayed_work(&delayed_message->delayed_work)) {
			publish_message(delayed_message->group, delayed_message->message);
			list_del(&delayed_message->list);
			kfree(delayed_message);
		}
	}
	mutex_unlock(&group->delayed_messages_mutex);

	return 0;
}

static long synchmess_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	unsigned descriptor = iminor(file_inode(filp));

	if (releasing) return -EBUSY;

	if (descriptor == 0) {
		if (cmd == IOCTL_INSTALL_GROUP)
			return synchmess_install_group(filp, arg);
		return -EPERM;
	} else {
		switch (cmd) {
		case IOCTL_SLEEP: return synchmess_sleep(filp);
		case IOCTL_AWAKE: return synchmess_awake(filp);
		case IOCTL_SET_SEND_DELAY: return synchmess_set_send_delay(filp, arg);
		case IOCTL_REVOKE_DELAYED_MESSAGES: return synchmess_revoke_delayed_messages(filp);
		default: return -EPERM;
		}
	}
}

struct file_operations fops = {
	.open = synchmess_open,
	.write = synchmess_write,
	.read = synchmess_read,
	.fsync = synchmess_fsync,
	.unlocked_ioctl = synchmess_ioctl,
	.compat_ioctl = synchmess_ioctl
};

static int __init synchmess_init(void)
{
	work_queue = alloc_workqueue(KBUILD_MODNAME, WQ_UNBOUND, 0);

	major = register_chrdev(0, KBUILD_MODNAME, &fops);
	if (major < 0) return major;

	#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
		dev_cl = class_create(KBUILD_MODNAME);
	#else
		dev_cl = class_create(THIS_MODULE, KBUILD_MODNAME);
	#endif

	if (IS_ERR(dev_cl)) {
		unregister_chrdev(major, KBUILD_MODNAME);
		return PTR_ERR(dev_cl);
	}

	master_dev = device_create(dev_cl, NULL, MKDEV(major, 0), NULL, "synch!%s", KBUILD_MODNAME);
	if (IS_ERR(master_dev)) {
		class_destroy(dev_cl);
		unregister_chrdev(major, KBUILD_MODNAME);
		return PTR_ERR(master_dev);
	}

	return 0;
}

static void __exit synchmess_exit(void)
{
	unsigned i;
	releasing = true;

	for (i = 0; i < MAX_NUM_GROUPS; i++) {
		if (groups[i]) {
			free_group(groups[i]);
			kfree(groups[i]);
			device_destroy(dev_cl, MKDEV(major, i + 1));
		}
	}

	destroy_workqueue(work_queue);
	device_destroy(dev_cl, MKDEV(major, 0));
	class_destroy(dev_cl);
	unregister_chrdev(major, KBUILD_MODNAME);
}

MODULE_AUTHOR("Giuseppe D'Aquanno <daquanno.1712078@studenti.uniroma1.it>");
MODULE_DESCRIPTION("Thread synchronization and message exchange service");
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0.0");

module_init(synchmess_init);
module_exit(synchmess_exit);