/*
 * `can`: raw CAN frames on the console, for testing a transceiver and a bus. The application says
 * which pins it uses and whether something else holds them, and is told when it takes and gives
 * them back.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* The pins `can start` uses, and whether something else holds them now: a phrase for the
     * refusal (printed after "can: not started on GPIO<tx> and GPIO<rx>: "), or NULL when they
     * are free. Asked at every `can start`. */
    const char *(*pins)(int *tx, int *rx);
    /* `can` has taken the pins (held) or given them back; NULL if the application keeps no
     * record of them */
    void (*on_pins)(int tx, int rx, bool held);
    /* Where `can dump` prints, one line with no newline: the only output made from the `can`
     * reader task rather than the console's. NULL prints to stdout. An application whose stdout
     * can be swapped or closed under another task (IDF 6.1's picolibc has one global stdout, and
     * a network console that borrows and closes it) gives a stream that stays put. */
    void (*print)(const char *line);
} can_cmd_config_t;

/* `can`. From app_main; `config` is copied. */
void register_can_commands(const can_cmd_config_t *config);

#ifdef __cplusplus
}
#endif
