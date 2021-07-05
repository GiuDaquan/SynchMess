#include "synchmess_interface.h"

#define BUFFER_SIZE 128
#define TEST_DURATION 30
#define NUM_GROUPS MAX_NUM_GROUPS
#define NUM_CONSUMERS 8
#define NUM_PRODUCERS 8

struct consumer_arg {
	double avg_read_time_ms;
	double avg_flush_time_ms;
	unsigned id;
};

struct producer_arg {
	double avg_write_time_ms;
	unsigned id;
};