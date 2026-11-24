# The cyphal component's host tests: cyphal_util.c, cyphal_registers.c, cyphal_alloc.c (the
# plug-and-play allocator's plain parts, CONFIG_CYPHAL_PNP_ALLOCATOR) and cyphal_client.c (the client's,
# CONFIG_CYPHAL_CLIENT, with the register table built with that option on, and libcanard, through which
# its tests send requests and answer them) built with the computer's cc and run, no ESP-IDF. And the holo
# engine's (holo/holo.c, built whole into test_holo.c against the stub headers in stubs/: when a motion ends).
# `make -C tests/host` from the kit's root; CC=... for another compiler.
# The binaries go in build/ here (BUILD=... elsewhere). fixtures/ holds holo-player-fw's generated
# port table, a real one, for the register tests; values.h the register values both register tests send.
# The first three need only cc. The client's needs libcanard, a submodule (third_party/libcanard): in a
# checkout without it, the other three are built and run and the client's is skipped, said so.
CC ?= cc
BUILD ?= build
CYPHAL := ../../cyphal
CANARD := ../../third_party/libcanard
CFLAGS := -std=c11 -Wall -Wextra -Werror -O1 -I$(CYPHAL)/include -I$(CYPHAL)/dsdl -Ifixtures

TESTS := $(BUILD)/test_cyphal_util $(BUILD)/test_cyphal_registers $(BUILD)/test_cyphal_alloc $(BUILD)/test_holo
ifneq ($(wildcard $(CANARD)/libcanard/canard.c),)
TESTS += $(BUILD)/test_cyphal_client
else
NO_CANARD := test_cyphal_client: not built, $(CANARD) is empty (git submodule update --init --recursive)
endif

.PHONY: all clean
all: $(TESTS)
	@set -e; for t in $(TESTS); do echo $$t; $$t; done
ifdef NO_CANARD
	@echo "$(NO_CANARD)"
endif

$(BUILD)/test_cyphal_util: test_cyphal_util.c check.h $(CYPHAL)/cyphal_util.c $(wildcard $(CYPHAL)/include/*.h)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ test_cyphal_util.c $(CYPHAL)/cyphal_util.c

$(BUILD)/test_cyphal_registers: test_cyphal_registers.c check.h values.h $(CYPHAL)/cyphal_registers.c $(CYPHAL)/cyphal_util.c \
                                $(wildcard $(CYPHAL)/include/*.h) fixtures/r2/holo/ports.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ test_cyphal_registers.c $(CYPHAL)/cyphal_registers.c $(CYPHAL)/cyphal_util.c -lm

$(BUILD)/test_cyphal_alloc: test_cyphal_alloc.c check.h $(CYPHAL)/cyphal_alloc.c $(CYPHAL)/cyphal_util.c \
                            $(wildcard $(CYPHAL)/include/*.h)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ test_cyphal_alloc.c $(CYPHAL)/cyphal_alloc.c $(CYPHAL)/cyphal_util.c

$(BUILD)/test_cyphal_client: test_cyphal_client.c check.h values.h $(CYPHAL)/cyphal_client.c $(CYPHAL)/cyphal_registers.c \
                             $(CYPHAL)/cyphal_util.c $(CANARD)/libcanard/canard.c $(wildcard $(CYPHAL)/include/*.h)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -DCONFIG_CYPHAL_CLIENT=1 -I$(CANARD)/libcanard -I$(CANARD)/lib/cavl2 -o $@ test_cyphal_client.c \
	    $(CYPHAL)/cyphal_client.c $(CYPHAL)/cyphal_registers.c $(CYPHAL)/cyphal_util.c $(CANARD)/libcanard/canard.c -lm

HOLO := ../../holo
$(BUILD)/test_holo: test_holo.c check.h $(HOLO)/holo.c $(HOLO)/include/holo.h $(wildcard stubs/*.h stubs/freertos/*.h)
	@mkdir -p $(BUILD)
	$(CC) -std=c11 -Wall -Wextra -Werror -O1 -Istubs -I$(HOLO)/include -I../../servo/include -o $@ test_holo.c -lm

clean:
	rm -rf $(BUILD)
