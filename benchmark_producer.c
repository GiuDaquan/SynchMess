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


void *producer_job(void *arg)
{
	int ret;
	long num_writes = 0;
	unsigned i, r, delay_ms;
	char message[BUFFER_SIZE];
	clock_t start, end;
	struct producer_arg *thread_arg = (struct producer_arg *) arg;

	sprintf(message, "SynchMess producer %u message", thread_arg->id);

	// Start writing messages at random to groups;
	while (!standard_rw_time_elapsed) {
		r = rand() % NUM_GROUPS;
		start = clock();
		ret = write(fds[r], message, strlen(message));
		end = clock();

		if (ret < 0)
			fprintf(stderr, "producer %u: failed write, %s\n", thread_arg->id,
				strerror(errno));

		if (ret == 0)
			continue;

		thread_arg->avg_write_time_ms += ((double) (end - start)) * 1000 / CLOCKS_PER_SEC;
		num_writes++;
	}

	// Set groups delay to 1000 ms
	delay_ms = 1000;
	if (thread_arg->id == 0) {
		for (i = 0; i < NUM_GROUPS; i++) {
			ioctl(fds[i], IOCTL_SET_SEND_DELAY, &delay_ms);
		}
	}

	// Sleep on group 1 barrier and synchronize with producers
	ioctl(fds[0], IOCTL_SLEEP);

	// Send delayed messages
	while (!delayed_rw_time_elapsed) {
		r = rand() % NUM_GROUPS;
		ret = write(fds[r], message, strlen(message));
		if (ret < 0)
			fprintf(stderr, "producer %u: failed write, %s\n", thread_arg->id,
				strerror(errno));
	}

	if (num_writes)
		thread_arg->avg_write_time_ms /= num_writes;

	return (void *) num_writes;
}


int main(int argc, char **argv)
{
	int ret;
	int master_dev_fd;
	double tot_write_time = 0.0;
	unsigned i;
	char buffer[BUFFER_SIZE];
	pthread_t threads[NUM_PRODUCERS];
	struct producer_arg thread_args[NUM_PRODUCERS] = {0};

	// Open master device.
	printf("main_producer: opening master device...\n");
	master_dev_fd = open("/dev/synch/synchmess", O_RDONLY);
	if (master_dev_fd < 0) {
		fprintf(stderr, "main producer: failed to open master device, %s\n",
			strerror(errno));
		return master_dev_fd;
	}

	// Register the maximum number of synchmess groups.
	printf("main_producer: installing groups...\n");
	for (i = 0; i < NUM_GROUPS; i++) {
		group_descriptors[i].descriptor = i + 1;
		ret = ioctl(master_dev_fd, IOCTL_INSTALL_GROUP, &group_descriptors[i]);
		if (ret < 0) {
			fprintf(stderr, "main producer: failed group %d installation, %s\n", i,
				strerror(errno));
			goto failed_install;
		}
	}

	// Open all synchmess devices
	printf("main_producer: opening all devices...\n");
	for (i = 0; i < NUM_GROUPS; i++) {
		while (access(group_descriptors[i].device_path, R_OK | W_OK)) {
			usleep(10000); // FIX: Previene il blocco della CPU al 100%
		}
		fds[i] = open(group_descriptors[i].device_path, O_RDWR);
		if (fds[i] < 0) {
			fprintf(stderr, "main_producer: failed to open group %d device, %s\n", i,
				strerror(errno));
			ret = fds[i];
			fds[i] = -1;
			goto failed_io;
		}
	}

	// Create producers threads
	printf("main_producer: starting producers...\n");
	for (i = 0; i < NUM_PRODUCERS; i++) {
		thread_args[i].id = i;
		ret = pthread_create(&threads[i], NULL, producer_job, (void *) &thread_args[i]);
		if (ret < 0) {
			fprintf(stderr, "main producer: failed to create consumer thread, %s\n",
				strerror(errno));
			goto failed_io;
		}
	}

	printf("main_producer: sleeping for %u seconds...\n", TEST_DURATION);
	sleep(TEST_DURATION);
	standard_rw_time_elapsed = 1;

	printf("main_producer: sleeping for %u seconds...\n", TEST_DURATION);
	sleep(TEST_DURATION);
	delayed_rw_time_elapsed = 1;

	// Wait for thread execution
	printf("main_producer: waiting for producers to complete execution...\n");
	for (i = 0; i < NUM_PRODUCERS; i++) {
		pthread_join(threads[i], NULL);
	}

	// Cleanup leftovers messages
	printf("main_producer: cleaning up leftover messages...\n");
	for (i = 0; i < NUM_GROUPS; i++) {
		ioctl(fds[i], IOCTL_REVOKE_DELAYED_MESSAGES);
		while(read(fds[i], buffer, BUFFER_SIZE)) {
			continue;
		}
	}

	// Cancel groups delay
	ret = 0;
	for (i = 0; i < NUM_GROUPS; i++) {
			ioctl(fds[i], IOCTL_SET_SEND_DELAY, &ret);
	}

	// Show profiling result
	for (i = 0; i < NUM_PRODUCERS; i++) {
		printf("producer %u: Average writing time: %f ms\n", i,
			thread_args[i].avg_write_time_ms);
		tot_write_time += thread_args[i].avg_write_time_ms;
	}

	printf("main producer: average writing time: %f ms\n", tot_write_time / NUM_CONSUMERS);


failed_io:
	for (i = 0; i < NUM_GROUPS; i++) {
		if (fds[i] == -1)
			break;
		ret = close(fds[i]);
		if (ret < 0)
			fprintf(stderr, "main_producer: failed to close group %d device, %s\n", i,
				strerror(errno));
	}
failed_install:
	close(master_dev_fd);
	return ret;
}
