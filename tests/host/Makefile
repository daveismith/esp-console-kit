# The cyphal component's host tests: cyphal_util.c, cyphal_registers.c and cyphal_alloc.c (the
# plug-and-play allocator's plain parts, CONFIG_CYPHAL_PNP_ALLOCATOR) built with the computer's
# cc and run, no ESP-IDF. `make -C tests/host` from the kit's root; CC=... for another compiler.
# The binaries go in build/ here (BUILD=... elsewhere). fixtures/ holds holo-player-fw's generated
# port table, a real one, for the register tests.
CC ?= cc
BUILD ?= build
CYPHAL := ../../cyphal
CFLAGS := -std=c11 -Wall -Wextra -Werror -O1 -I$(CYPHAL)/include -I$(CYPHAL)/dsdl -Ifixtures

.PHONY: all clean
all: $(BUILD)/test_cyphal_util $(BUILD)/test_cyphal_registers $(BUILD)/test_cyphal_alloc
	$(BUILD)/test_cyphal_util
	$(BUILD)/test_cyphal_registers
	$(BUILD)/test_cyphal_alloc

$(BUILD)/test_cyphal_util: test_cyphal_util.c check.h $(CYPHAL)/cyphal_util.c $(wildcard $(CYPHAL)/include/*.h)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ test_cyphal_util.c $(CYPHAL)/cyphal_util.c

$(BUILD)/test_cyphal_registers: test_cyphal_registers.c check.h $(CYPHAL)/cyphal_registers.c $(CYPHAL)/cyphal_util.c \
                                $(wildcard $(CYPHAL)/include/*.h) fixtures/r2/holo/ports.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ test_cyphal_registers.c $(CYPHAL)/cyphal_registers.c $(CYPHAL)/cyphal_util.c -lm

$(BUILD)/test_cyphal_alloc: test_cyphal_alloc.c check.h $(CYPHAL)/cyphal_alloc.c $(CYPHAL)/cyphal_util.c \
                            $(wildcard $(CYPHAL)/include/*.h)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ test_cyphal_alloc.c $(CYPHAL)/cyphal_alloc.c $(CYPHAL)/cyphal_util.c

clean:
	rm -rf $(BUILD)
