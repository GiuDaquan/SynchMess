#pragma once

#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#define DEVICES_HOME_DIR "/dev/synch/\0"

struct group {
	rwlock_t messages_rwlock;
	struct mutex delayed_messages_mutex;
	spinlock_t sleeping_threads_spinlock;
	spinlock_t size_spinlock;
	wait_queue_head_t wait_queue;
	struct list_head sleeping_threads;
	struct list_head messages;
	struct list_head delayed_messages;
	atomic_t current_size;
	unsigned descriptor;
	unsigned delay_ms;
};

struct message {
	struct list_head list;
	size_t buf_size;
	char *buffer;
};

struct delayed_message {
	struct delayed_work delayed_work;
	struct list_head list;
	struct group *group;
	struct message *message;
};

struct sleeping_thread {
	struct list_head list;
	bool awake;
};