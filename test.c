#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <stdlib.h>

#include "synchmess_interface.h"

#define CHECK_ERR(op, msg) do { if ((op) < 0) { perror(msg); exit(EXIT_FAILURE); } } while(0)

int main(int argc, char **argv)
{
	int ret;
	int master_dev_fd;
	int fd;
	unsigned delay_ms;
	struct group_t group;
	char message[1024] = "ABADCAFE\0";
	char delivery[1024] = { 0 };

	printf("[*] Opening master device...\n");
	master_dev_fd = open("/dev/synch/synchmess", O_RDONLY);
	CHECK_ERR(master_dev_fd, "Failed to open master device");

	printf("[*] Installing group 1...\n");
	group.descriptor = 1;
	ret = ioctl(master_dev_fd, IOCTL_INSTALL_GROUP, &group);
	CHECK_ERR(ret, "IOCTL_INSTALL_GROUP failed");

	printf("[*] Waiting for udev to create device file: %s\n", group.device_path);
	while (access(group.device_path, R_OK | W_OK)) {
		usleep(10000);
	}
	
	fd = open(group.device_path, O_RDWR);
	CHECK_ERR(fd, "Failed to open group device");

	printf("[*] Sending standard message...\n");
	ret = write(fd, message, strlen(message));
	CHECK_ERR(ret, "Write failed");

	printf("[*] Reading message...\n");
	ret = read(fd, delivery, sizeof(delivery));
	CHECK_ERR(ret, "Read failed");
	printf("[+] Received: %s\n", delivery);

	printf("[*] Setting delay to 5 seconds...\n");
	delay_ms = 5000; // Modificato per riflettere millisecondi come da specifica
	ret = ioctl(fd, IOCTL_SET_SEND_DELAY, &delay_ms);
	CHECK_ERR(ret, "IOCTL_SET_SEND_DELAY failed");

	printf("[*] Sending delayed message...\n");
	ret = write(fd, message, strlen(message));
	CHECK_ERR(ret, "Delayed write failed");

	printf("[*] Revoking delayed messages...\n");
	ret = ioctl(fd, IOCTL_REVOKE_DELAYED_MESSAGES);
	CHECK_ERR(ret, "IOCTL_REVOKE_DELAYED_MESSAGES failed");

	printf("[*] Sending another delayed message and flushing...\n");
	ret = write(fd, message, strlen(message));
	CHECK_ERR(ret, "Second delayed write failed");
	
	ret = fsync(fd);
	CHECK_ERR(ret, "Fsync failed");

	ret = read(fd, delivery, sizeof(delivery));
	CHECK_ERR(ret, "Read after fsync failed");
	printf("[+] Received after flush: %s\n", delivery);

	delay_ms = 0;
	ret = ioctl(fd, IOCTL_SET_SEND_DELAY, &delay_ms);
	CHECK_ERR(ret, "Failed to reset delay");

	close(fd);
	close(master_dev_fd);
	printf("[*] Tests completed successfully.\n");
	return 0;
}