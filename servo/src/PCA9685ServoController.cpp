#include "PCA9685ServoController.hpp"

#include <unistd.h>
#include "esp_check.h"
#include "esp_log.h"

using namespace servo;

static const char *TAG = "PCA9685";

PCA9685ServoController::PCA9685ServoController(i2c_master_bus_handle_t bus, uint32_t busFrequency, uint8_t address)
    : address_(address)
{
    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = busFrequency,
        .scl_wait_us = 0,
        .flags = {},
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &device_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "0x%02x: cannot add to the bus: %s", address, esp_err_to_name(err));
        device_ = nullptr;
        return;
    }

    err = Init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "0x%02x: setup failed: %s", address, esp_err_to_name(err));
        i2c_master_bus_rm_device(device_);
        device_ = nullptr;
    }
}

PCA9685ServoController::~PCA9685ServoController()
{
    if (device_ != nullptr) {
        i2c_master_bus_rm_device(device_);
    }
}

esp_err_t PCA9685ServoController::WriteReg(uint8_t reg, uint8_t value)
{
    const uint8_t data[2] = { reg, value };
    return i2c_master_transmit(device_, data, sizeof(data), TRANSFER_TIMEOUT_MS);
}

/* Set the frame rate. The prescaler can only be written while the oscillator sleeps. */
esp_err_t PCA9685ServoController::Init()
{
    /* round(25 MHz / (4096 * rate)) - 1, as the datasheet has it: 121 at 50 Hz. */
    const uint32_t prescale = (OSC_FREQUENCY_HZ + 2048 * UPDATE_RATE_HZ) / (4096 * UPDATE_RATE_HZ) - 1;

    ESP_RETURN_ON_ERROR(WriteReg(REG_MODE1, MODE1_SLEEP), TAG, "sleep");
    ESP_RETURN_ON_ERROR(WriteReg(REG_MODE2, MODE2_OUTDRV), TAG, "mode2");
    ESP_RETURN_ON_ERROR(WriteReg(REG_PRE_SCALE, (uint8_t)prescale), TAG, "prescale");

    /* Wake with auto-increment on, give the oscillator its 500 us, and only then RESTART:
     * the datasheet's order, and it matters after a warm reboot of the ESP with the
     * PCA9685 still powered and still driving whatever it was last told. */
    ESP_RETURN_ON_ERROR(WriteReg(REG_MODE1, MODE1_AI), TAG, "wake");
    usleep(500);
    ESP_RETURN_ON_ERROR(WriteReg(REG_MODE1, MODE1_AI | MODE1_RESTART), TAG, "restart");
    return ESP_OK;
}

/* Every channel off, then the setup sequence again. For a board whose logic supply
 * dropped and came back: the chip wakes with its power-on prescaler, 200 Hz frames, and
 * every write after that lands at the wrong rate until this runs. */
esp_err_t PCA9685ServoController::Reset()
{
    if (device_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    enable_ = 0;
    /* Plain PWM channels stay claimed -- their Servo must not suddenly drive them -- but
     * go dark with the rest; whoever owns them sets them again. */
    for (size_t ch = 0; ch < NUM_CHANNELS; ch++) {
        duty_[ch] = 0;
    }
    esp_err_t err = Init();
    dirty_ = true;
    return err;
}

void PCA9685ServoController::SetPwm(size_t channel, uint16_t pulseUs)
{
    if (channel >= NUM_CHANNELS) {
        return;
    }
    /* 4096 ticks per 20 ms frame: 0.2048 ticks per microsecond, rounded. */
    uint32_t ticks = ((uint32_t)pulseUs * 4096 + 10000) / 20000;
    if (ticks > 4095) {
        ticks = 4095;
    }
    pulseWidth_[channel] = (uint16_t)ticks;
    dirty_ = true;
}

void PCA9685ServoController::SetEnable(size_t channel, bool enable)
{
    static_assert(NUM_CHANNELS <= 16, "enable_ holds one bit per channel");
    if (channel >= NUM_CHANNELS) {
        return;
    }
    if (enable) {
        enable_ |= (uint16_t)(1u << channel);
    } else {
        enable_ &= (uint16_t)~(1u << channel);
    }
    dirty_ = true;
}

void PCA9685ServoController::SetDuty(size_t channel, uint16_t duty)
{
    if (channel >= NUM_CHANNELS) {
        return;
    }
    if (duty > DUTY_FULL) {
        duty = DUTY_FULL;
    }
    const uint16_t bit = (uint16_t)(1u << channel);
    if ((pwm_ & bit) && duty_[channel] == duty) {
        return;   /* nothing new for the wire: a caller refreshing every frame costs nothing */
    }
    pwm_ |= bit;
    duty_[channel] = duty;
    dirty_ = true;
}

void PCA9685ServoController::ReleaseDuty(size_t channel)
{
    if (channel >= NUM_CHANNELS) {
        return;
    }
    pwm_ &= (uint16_t)~(1u << channel);
    duty_[channel] = 0;
    dirty_ = true;
}

bool PCA9685ServoController::GetDuty(size_t channel, uint16_t *duty) const
{
    if (channel >= NUM_CHANNELS || !(pwm_ & (1u << channel))) {
        return false;
    }
    if (duty != nullptr) {
        *duty = duty_[channel];
    }
    return true;
}

/* Every channel in one auto-incremented write. dirty_ is cleared BEFORE the snapshot,
 * so a change that lands while this runs is never lost, only sent twice. */
void PCA9685ServoController::Update()
{
    if (device_ == nullptr || !dirty_) {
        return;
    }
    dirty_ = false;

    uint8_t data[1 + NUM_CHANNELS * 4] = {};
    data[0] = REG_LED0_ON_L;
    for (size_t ch = 0; ch < NUM_CHANNELS; ch++) {
        uint8_t *led = &data[1 + ch * 4];   // ON_L, ON_H, OFF_L, OFF_H
        if (pwm_ & (1u << ch)) {
            /* A plain PWM channel: its duty, whatever its Servo last asked for. The ends
             * are the chip's own full-off and full-on bits rather than counts, so 0 is
             * dark and DUTY_FULL has no gap at the frame boundary. */
            const uint16_t duty = duty_[ch];
            if (duty == 0) {
                led[3] = LED_FULL_OFF;
            } else if (duty >= DUTY_FULL) {
                led[1] = LED_FULL_ON;
            } else {
                led[2] = (uint8_t)(duty & 0xff);
                led[3] = (uint8_t)(duty >> 8);
            }
        } else if (enable_ & (1u << ch)) {
            const uint16_t off = pulseWidth_[ch];   // the pulse starts at count 0
            led[2] = (uint8_t)(off & 0xff);
            led[3] = (uint8_t)(off >> 8);
        } else {
            led[3] = LED_FULL_OFF;
        }
    }

    esp_err_t err = i2c_master_transmit(device_, data, sizeof(data), TRANSFER_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "0x%02x: write failed: %s", address_, esp_err_to_name(err));
    }
}
