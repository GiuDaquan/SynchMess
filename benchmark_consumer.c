#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "benchmark.h"
#include "synchmess_interface.h"



static volatile char standard_rw_time_elapsed = 0;
static volatile char delayed_rw_time_elapsed = 0;
static struct group_t group_descriptors[NUM_GROUPS];
static int fds[NUM_GROUPS];


void *consumer_job(void *arg)
{
	int ret;
	long num_reads = 0, num_flushes = 0;
	unsigned i, r;
	char message[BUFFER_SIZE];
	clock_t start, end;
	struct consumer_arg *thread_arg = (struct consumer_arg *) arg;

	// Start reading messages at random from groups;
	while (!standard_rw_time_elapsed) {
		r = rand() % NUM_GROUPS;
		start = clock();
		ret = read(fds[r], message, BUFFER_SIZE);
		end = clock();

		if (ret < 0)
			fprintf(stderr, "consumer %u: failed read, %s\n",
				thread_arg->id, strerror(errno));

		if (ret == 0)
			continue;

		memset(message, 0, BUFFER_SIZE);
		thread_arg->avg_read_time_ms += ((double) (end - start)) * 1000 / CLOCKS_PER_SEC;
		num_reads++;
	}

	// Wake up threads sleeping on group 1 barrier
	ret = 0;
	if (thread_arg->id == 0) {
		do {
			ret += ioctl(fds[0], IOCTL_AWAKE);
		} while(ret < NUM_PRODUCERS);
	}

	// Flush delayed messsages
	while (!delayed_rw_time_elapsed) {
		r = rand() % NUM_GROUPS;
		if (thread_arg->id == 0) {
			// Flush delayed messsages
			start = clock();
			fsync(fds[r]);
			end = clock();

			thread_arg->avg_flush_time_ms = ((double) (end - start)) * 1000 / CLOCKS_PER_SEC;
			num_flushes++;
		} else {
			read(fds[r], message, BUFFER_SIZE);
		}
	}

	if (num_reads)
		thread_arg->avg_read_time_ms /= num_reads;

	if (num_flushes)
		thread_arg->avg_flush_time_ms /= num_flushes;

	return (void *) num_reads;
}


int main(int argc, char **argv)
{
	int ret;
	int master_dev_fd;
	double tot_read_time = 0.0;
	unsigned i;
	pthread_t threads[NUM_CONSUMERS];
	struct consumer_arg thread_args[NUM_CONSUMERS] = { 0 };

	// Open master device.
	printf("main_consumer: opening master device...\n");
	master_dev_fd = open("/dev/synch/synchmess", O_RDONLY);
	if (master_dev_fd < 0) {
		fprintf(stderr, "main consumer: failed to open master device, %s\n",
			strerror(errno));
		return master_dev_fd;
	}

	// Register the maximum number of synchmess groups.
	printf("main_consumer: installing groups...\n");
	for (i = 0; i < NUM_GROUPS; i++) {
		group_descriptors[i].descriptor = i + 1;
		ret = ioctl(master_dev_fd, IOCTL_INSTALL_GROUP, &group_descriptors[i]);
		if (ret < 0) {
			fprintf(stderr, "main consumer: failed group %d installation, %s\n", i,
				strerror(errno));
			goto failed_install;
		}
	}

	// Open all synchmess devices
	printf("main_consumer: opening all devices...\n");
	for (i = 0; i < NUM_GROUPS; i++) {
		while (access(group_descriptors[i].device_path, R_OK | W_OK)) {
			usleep(10000); 
		}
		fds[i] = open(group_descriptors[i].device_path, O_RDWR);
		if (fds[i] < 0) {
			fprintf(stderr, "main_consumer: failed to open group %d device, %s\n", i,
				strerror(errno));
			ret = fds[i];
			fds[i] = -1;
			goto failed_io;
		}
	}

	// Create consumer threads
	printf("main_consumer: starting consumers...\n");
	for (i = 0; i < NUM_CONSUMERS; i++) {
		thread_args[i].id = i;
		ret = pthread_create(&threads[i], NULL, consumer_job, (void *) &thread_args[i]);
		if (ret < 0) {
			fprintf(stderr, "main consumer: failed to create consumer thread, %s\n",
				strerror(errno));
			goto failed_io;
		}
	}

	printf("main_consumer: sleeping for %u seconds...\n", TEST_DURATION);
	sleep(TEST_DURATION);
	standard_rw_time_elapsed = 1;

	printf("main_consumer: sleeping for %u seconds...\n", TEST_DURATION);
	sleep(TEST_DURATION);
	delayed_rw_time_elapsed = 1;

	// Wait for thread execution
	printf("main_consumer: waiting for consumers to complete execution...\n");
	for (i = 0; i < NUM_CONSUMERS; i++) {
		pthread_join(threads[i], NULL);
	}

	// Show profiling result
	for (i = 0; i < NUM_CONSUMERS; i++) {
		printf("consumer %u: average reading time: %f ms\n", i,
			thread_args[i].avg_read_time_ms);
		tot_read_time += thread_args[i].avg_read_time_ms;
	}

	printf("main consumer: average reading time: %f ms\n", tot_read_time / NUM_CONSUMERS);
	printf("main consumer: average flushing time: %f ms\n", thread_args[0].avg_flush_time_ms);


failed_io:
	for (i = 0; i < NUM_GROUPS; i++) {
		if (fds[i] == -1)
			break;
		ret = close(fds[i]);
		if (ret < 0)
			fprintf(stderr, "main_consumer: failed to close group %d device, %s\n", i,
				strerror(errno));
	}
failed_install:
	close(master_dev_fd);
	return ret;
}
