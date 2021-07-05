#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "synchmess_interface.h"

int main(int argc, char **argv)
{
	int ret;
	int master_dev_fd;
	int fd;
	unsigned delay_ms;
	struct group_t group;
	char message[1024] = "ABADCAFE\0";
	char delivery[1024] = { 0 };

	// Open master device
	master_dev_fd = open("/dev/synch/synchmess", O_RDONLY);

	// Install a new group identified by descriptor 1
	group.descriptor = 1;
	ret = ioctl(master_dev_fd, IOCTL_INSTALL_GROUP, &group);

	// Open group device
	while (access(group.device_path, R_OK | W_OK)) {
		continue;
	}
	fd = open(group.device_path, O_RDWR);

	// Send a message
	ret = write(fd, message, strlen(message));

	// Read a message
	ret = read(fd, &delivery, 1024);

	// Set the group delay to 5 seconds
	delay_ms = 5;
	ret = ioctl(fd, IOCTL_SET_SEND_DELAY, &delay_ms);

	// Send a delayed message
	ret = write(fd, message, strlen(message));

	// Revoke delayed messages
	ret = ioctl(fd, IOCTL_REVOKE_DELAYED_MESSAGES);

	// Send another delayed message
	ret = write(fd, message, strlen(message));

	// Flush messages
	ret = fsync(fd);

	ret = read(fd, &delivery, 1024);

	delay_ms = 0;
	ret = ioctl(fd, IOCTL_SET_SEND_DELAY, &delay_ms);

	return 0;
}