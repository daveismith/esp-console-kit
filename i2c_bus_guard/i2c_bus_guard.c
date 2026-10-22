/*
 * IDF 6.1's i2c_master writes through a stale pointer after a read that ends in a timeout,
 * and this file stops it from the outside.
 *
 * THE BUG. A read queues a command list with the read's buffers in it and sets the bus's
 * `contains_read`, `read_buf_pos` (the read's index in the list) and `read_len_static`
 * (its byte count). Only the ISR's receive path clears `contains_read`, and only on a
 * completion interrupt. A read that ends in a TIMEOUT or ARBITRATION interrupt -- a target
 * stretching SCL past the timeout, a line held low -- never gets one, so all three are left
 * set. The next transaction resets `read_buf_pos` and `read_len_static` itself, except
 * i2c_master_probe(): it points the bus at a two-entry START/STOP list ON ITS OWN STACK and
 * resets neither. Its completion interrupt then sees `contains_read`, indexes that list at
 * the old `read_buf_pos` (3 for a register read), past its end into the probing task's
 * saved registers, and copies `read_len_static` bytes from the RX FIFO to whatever pointer
 * it finds there, plus one more byte through the next slot. Measured on a bench
 * (2026-10-21): every SCL hold in the middle of one device's 16-byte register read was
 * followed, within 250 ms, by another driver's probe of an absent device writing 15 bytes
 * into the I2C driver's own state beside its command semaphore. The bytes are zeros (the
 * probe resets the RX FIFO, and an empty FIFO reads 0x00). The panics that followed were
 * asserts in FreeRTOS's queue and spinlock code, in the probing task and in the I2C ISR;
 * on another build the same write lands somewhere else entirely. Any code that probes --
 * a driver looking for a device that may be absent, `i2cdetect`, a servo board's attach --
 * on a bus where something else reads is exposed.
 *
 * THE PATCH. Every probe clears the three fields first, under the bus lock if they are set.
 * That alone would race a failed read on another task slipping in between the clear and the
 * probe taking the bus lock -- a binary semaphore hands itself to whichever waiter is
 * highest -- so probes and every call that can read are also serialised on one mutex per
 * bus here, and no read can run between the clear and the probe. Writes are left alone: one that finds
 * the stale state reads zero bytes into its own START entry, harmlessly, and clears the flag.
 * The order is always this mutex, then the driver's bus lock, and nothing calls these from an
 * ISR, so the mutex cannot deadlock; the real call gets what is left of the caller's timeout,
 * so the second wait does not double it.
 *
 * A bus made with a transaction queue (trans_queue_depth > 0) is left alone: its reads return
 * once queued, so `contains_read` there may belong to a read still in flight, and clearing it
 * would lose that read's data. IDF warns when such a bus is made that it is not compatible
 * with i2c_master_probe() anyway.
 */
#include "i2c_bus_guard.h"

#include <assert.h>
#include <stdatomic.h>
#include "driver/i2c_master.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "soc/soc_caps.h"
#include "i2c_private.h"

/* The patch reads and writes the driver's private state (i2c_private.h), so it is only known
 * good against the driver it was written for. An IDF update stops the build here: re-read
 * i2c_master.c's i2c_master_probe() and the ISR's receive path, then move this line -- or
 * delete the component, if the probe resets the read state itself (or clears it on every
 * error) by then. */
/* wanted.cmake repeats both conditions, for cmd_i2c and servo: change the two together. */
#if ESP_IDF_VERSION != ESP_IDF_VERSION_VAL(6, 1, 0)
#error "i2c_bus_guard.c was verified against IDF 6.1.0's i2c_master; check it against this one"
#endif
/* With an independent STOP (the original ESP32) the ISR's receive path is a different one,
 * which never clears `contains_read` after any read: every probe would look like a catch. */
#if SOC_I2C_STOP_INDEPENDENT
#error "i2c_bus_guard.c was verified on targets without SOC_I2C_STOP_INDEPENDENT only"
#endif

static const char *TAG = "i2c_guard";

/* Logged at most this often: a faulted bus can produce one of these every probe, and an
 * application may forward every WARN somewhere (an event log, a host). */
#define WARN_INTERVAL_TICKS pdMS_TO_TICKS(60 * 1000)

/* One guard per port, since the stale state is per bus: a read stalled on one bus never
 * holds up the other. */
static StaticSemaphore_t s_guard_buf[SOC_I2C_NUM];
static SemaphoreHandle_t s_guard[SOC_I2C_NUM];
static atomic_uint_fast32_t s_cleared;
/* Unlocked: two buses warning at once can at worst log twice. */
static bool s_warned;
static TickType_t s_last_warn;

esp_err_t __real_i2c_master_probe(i2c_master_bus_handle_t bus, uint16_t address, int xfer_timeout_ms);
esp_err_t __real_i2c_master_receive(i2c_master_dev_handle_t dev, uint8_t *read_buffer, size_t read_size,
                                    int xfer_timeout_ms);
esp_err_t __real_i2c_master_transmit_receive(i2c_master_dev_handle_t dev, const uint8_t *write_buffer,
                                             size_t write_size, uint8_t *read_buffer, size_t read_size,
                                             int xfer_timeout_ms);
esp_err_t __real_i2c_master_execute_defined_operations(i2c_master_dev_handle_t dev,
                                                       i2c_operation_job_t *ops, size_t n,
                                                       int xfer_timeout_ms);

static TickType_t to_ticks(int timeout_ms)
{
    return timeout_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
}

static void guard_create(int port)
{
    s_guard[port] = xSemaphoreCreateMutexStatic(&s_guard_buf[port]);
}

/* Before app_main, and so before any task could race the creation: a static mutex needs no
 * scheduler. */
__attribute__((constructor)) static void guard_init(void)
{
    for (int i = 0; i < SOC_I2C_NUM; i++) {
        if (s_guard[i] == NULL) {
            guard_create(i);
        }
    }
}

/* What is left of the caller's timeout after waiting for the guard, for the real call, so a
 * call waits about as long as it asked to rather than twice. Never less than 20 ms (two
 * ticks), or the whole timeout if that is shorter: the real call needs a tick or two for the
 * transfer itself even on a free bus. */
#define MIN_LEFT_MS 20
static int time_left(int timeout_ms, TickType_t since)
{
    if (timeout_ms < 0) {
        return timeout_ms;
    }
    const int spent = (int)((xTaskGetTickCount() - since) * portTICK_PERIOD_MS);
    const int left = timeout_ms - spent;
    const int floor = timeout_ms < MIN_LEFT_MS ? timeout_ms : MIN_LEFT_MS;
    return left > floor ? left : floor;
}

static SemaphoreHandle_t guard_of(const i2c_master_bus_t *bus)
{
    const int port = bus->base->port_num;
    assert(port >= 0 && port < SOC_I2C_NUM);
    /* Only NULL when another static constructor uses the bus before guard_init() has run,
     * which is still single-threaded. */
    if (s_guard[port] == NULL) {
        guard_create(port);
    }
    return s_guard[port];
}

/* Take `guard` within the caller's timeout; `left` gets what remains of it. */
static bool guard_enter(SemaphoreHandle_t guard, int timeout_ms, TickType_t since, int *left)
{
    if (xSemaphoreTake(guard, to_ticks(timeout_ms)) != pdTRUE) {
        return false;
    }
    *left = time_left(timeout_ms, since);
    return true;
}

esp_err_t __wrap_i2c_master_probe(i2c_master_bus_handle_t bus, uint16_t address, int xfer_timeout_ms)
{
    if (bus == NULL || bus->async_trans) {
        return __real_i2c_master_probe(bus, address, xfer_timeout_ms);
    }
    const TickType_t since = xTaskGetTickCount();
    const SemaphoreHandle_t guard = guard_of(bus);
    int left;
    if (!guard_enter(guard, xfer_timeout_ms, since, &left)) {
        return ESP_ERR_TIMEOUT;
    }
    /* With the guard held no read is in flight, so nothing can set the flag under this look;
     * the bus lock is only for the clear, since a write may be. */
    const bool stale = bus->contains_read;
    if (stale) {
        if (xSemaphoreTake(bus->bus_lock_mux, to_ticks(left)) != pdTRUE) {
            xSemaphoreGive(guard);
            return ESP_ERR_TIMEOUT;
        }
        bus->contains_read = false;
        bus->read_buf_pos = 0;
        bus->read_len_static = 0;
        atomic_fetch_add(&s_cleared, 1);
        xSemaphoreGive(bus->bus_lock_mux);
        left = time_left(xfer_timeout_ms, since);
    }
    const esp_err_t err = __real_i2c_master_probe(bus, address, left);
    xSemaphoreGive(guard);

    if (stale) {
        const TickType_t now = xTaskGetTickCount();
        if (!s_warned || now - s_last_warn >= WARN_INTERVAL_TICKS) {
            s_warned = true;
            s_last_warn = now;
            ESP_LOGW(TAG, "a failed I2C read left its state on the bus; cleared before probing "
                     "0x%02x (%" PRIu32 " so far)", address,
                     (uint32_t)atomic_load(&s_cleared));
        }
    }
    return err;
}

/* The body of every wrapper that can read: the real call, with the time that is left, under
 * its bus's guard. A NULL device goes straight to the driver, which refuses it, and so does
 * a device on an asynchronous bus (see the top of the file). */
#define GUARDED(dev, timeout_ms, real, ...)                                        \
    do {                                                                           \
        if ((dev) == NULL || (dev)->master_bus->async_trans) {                     \
            return real(dev, __VA_ARGS__, timeout_ms);                             \
        }                                                                          \
        const TickType_t since = xTaskGetTickCount();                              \
        const SemaphoreHandle_t guard = guard_of((dev)->master_bus);               \
        int left;                                                                  \
        if (!guard_enter(guard, timeout_ms, since, &left)) {                       \
            return ESP_ERR_TIMEOUT;                                                \
        }                                                                          \
        const esp_err_t err = real(dev, __VA_ARGS__, left);                        \
        xSemaphoreGive(guard);                                                     \
        return err;                                                                \
    } while (0)

esp_err_t __wrap_i2c_master_receive(i2c_master_dev_handle_t dev, uint8_t *read_buffer, size_t read_size,
                                    int xfer_timeout_ms)
{
    GUARDED(dev, xfer_timeout_ms, __real_i2c_master_receive, read_buffer, read_size);
}

esp_err_t __wrap_i2c_master_transmit_receive(i2c_master_dev_handle_t dev, const uint8_t *write_buffer,
                                             size_t write_size, uint8_t *read_buffer, size_t read_size,
                                             int xfer_timeout_ms)
{
    GUARDED(dev, xfer_timeout_ms, __real_i2c_master_transmit_receive, write_buffer, write_size,
            read_buffer, read_size);
}

esp_err_t __wrap_i2c_master_execute_defined_operations(i2c_master_dev_handle_t dev,
                                                       i2c_operation_job_t *ops, size_t n,
                                                       int xfer_timeout_ms)
{
    GUARDED(dev, xfer_timeout_ms, __real_i2c_master_execute_defined_operations, ops, n);
}

uint32_t i2c_bus_guard_stale_cleared(void)
{
    return (uint32_t)atomic_load(&s_cleared);
}
