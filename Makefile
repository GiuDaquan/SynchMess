obj-m += synchmess.o

CURRENT_PATH = $(shell pwd)
LINUX_KERNEL = $(shell uname -r)
LINUX_KERNEL_PATH = /lib/modules/$(LINUX_KERNEL)/build/

all:
			make -C $(LINUX_KERNEL_PATH) M=$(CURRENT_PATH) modules
			gcc benchmark_producer.c -o producer -l pthread
			gcc benchmark_consumer.c -o consumer -l pthread
clean:
			make -C $(LINUX_KERNEL_PATH) M=$(CURRENT_PATH) clean
