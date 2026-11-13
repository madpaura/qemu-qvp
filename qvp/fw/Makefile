# qvp stub firmware: one ELF per core (build/core<n>.elf, n = 0..4)
CROSS   ?= arm-none-eabi-
CC      := $(CROSS)gcc
CFLAGS  := -mcpu=cortex-r5 -marm -mfloat-abi=soft -Og -g3 \
           -ffreestanding -fno-builtin -Wall -Wextra
LDFLAGS := -nostdlib -T link.ld -Wl,--no-warn-rwx-segments
OUT     := build
CORES   := 0 1 2 3 4

all: $(foreach c,$(CORES),$(OUT)/core$(c).elf)

$(OUT)/core%.elf: start.S main.c qvp.h link.ld Makefile | $(OUT)
	$(CC) $(CFLAGS) -DCORE_ID=$* $(LDFLAGS) start.S main.c -lgcc -o $@

$(OUT):
	mkdir -p $@

clean:
	rm -rf $(OUT)

.PHONY: all clean
