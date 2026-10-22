#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A link-time patch for a memory-corruption bug in IDF 6.1's i2c_master driver; requiring the
 * component is all it takes (see i2c_bus_guard.c and the kit's README). */

/* How many probes found a failed read's state left on the bus and cleared it first. Each
 * one is a write through a stale pointer that did not happen. */
uint32_t i2c_bus_guard_stale_cleared(void);

#ifdef __cplusplus
}
#endif
