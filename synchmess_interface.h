# pragma once

# include <linux/ioctl.h>

// Maximum number of groups
#define MAX_NUM_GROUPS 64

// CRC-64("SynchMess")
#define HASH 0x7B8C14A8DCB78AEF
#define SYNCHMESS_IOC_MAGIC HASH % 255

// IOCTL custom routines
#define IOCTL_INSTALL_GROUP             _IOWR(SYNCHMESS_IOC_MAGIC, 1, struct group_t *)
#define IOCTL_SLEEP                     _IO(SYNCHMESS_IOC_MAGIC, 2)
#define IOCTL_AWAKE                     _IO(SYNCHMESS_IOC_MAGIC, 3)
#define IOCTL_SET_SEND_DELAY            _IOW(SYNCHMESS_IOC_MAGIC, 4, unsigned *)
#define IOCTL_REVOKE_DELAYED_MESSAGES   _IO(SYNCHMESS_IOC_MAGIC, 5)

struct group_t {
	unsigned descriptor;
	char device_path[256];
};
