CROSS   := arm-none-eabi-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy

CFLAGS  := -mcpu=cortex-a9 -marm -ffreestanding -O2 -Wall -Wextra -g
LDFLAGS := -T linker.ld -nostdlib -nostartfiles -Wl,--build-id=none

OBJS := startup.o host_driver.o uart.o

all: host_driver.elf host_driver.bin

%.o: %.s
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

host_driver.elf: $(OBJS) linker.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS)

host_driver.bin: host_driver.elf
	$(OBJCOPY) -O binary $< $@

clean:
	rm -f *.o host_driver.elf host_driver.bin

.PHONY: all clean
