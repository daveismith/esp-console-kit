#ifndef PCA9685_SERVO_CONTROLLER_HPP_
#define PCA9685_SERVO_CONTROLLER_HPP_

#include <atomic>
#include <stddef.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "ServoController.hpp"

namespace servo {

class PCA9685Servo;

/** Sixteen servo channels behind one PCA9685 on the I2C bus. */
class PCA9685ServoController : public ServoController {
public:
    static constexpr size_t NUM_CHANNELS = 16;
    /* The chip is good to 1 MHz; 100 kHz is the safe default for a header with a cable on it. */
    static constexpr uint32_t DEFAULT_I2C_FREQUENCY_HZ = 100 * 1000;
    /* SetDuty()'s full scale: always on. The counter has 4096 steps a frame. */
    static constexpr uint16_t DUTY_FULL = 4096;

private:
    static constexpr uint32_t OSC_FREQUENCY_HZ = 25 * 1000 * 1000;  // the internal oscillator
    static constexpr uint32_t UPDATE_RATE_HZ = 50;                   // the servo frame rate
    static constexpr int TRANSFER_TIMEOUT_MS = 50;

    static constexpr uint8_t REG_MODE1 = 0x00;
    static constexpr uint8_t REG_MODE2 = 0x01;
    static constexpr uint8_t REG_LED0_ON_L = 0x06;
    static constexpr uint8_t REG_PRE_SCALE = 0xfe;

    static constexpr uint8_t MODE1_RESTART = 1 << 7;
    static constexpr uint8_t MODE1_EXTCLK = 1 << 6;
    static constexpr uint8_t MODE1_AI = 1 << 5;        // auto-increment the register address
    static constexpr uint8_t MODE1_SLEEP = 1 << 4;
    static constexpr uint8_t MODE1_ALLCALL = 1 << 0;
    static constexpr uint8_t MODE2_OUTDRV = 1 << 2;    // totem-pole outputs, the power-on default

    /* Bit 4 of LEDn_OFF_H: the channel is off whatever the counts say. This is the
     * documented way to silence a channel. ON == OFF == 0 is not -- the datasheet says
     * never to program the two counts equal. */
    static constexpr uint8_t LED_FULL_OFF = 1 << 4;
    /* Bit 4 of LEDn_ON_H: the channel is on for the whole frame. FULL_OFF outranks it. */
    static constexpr uint8_t LED_FULL_ON = 1 << 4;

    uint8_t address_;
    i2c_master_dev_handle_t device_ = nullptr;
    uint16_t pulseWidth_[NUM_CHANNELS] = {};   // in PCA9685 ticks, not microseconds
    uint16_t enable_ = 0;                      // one bit per channel
    uint16_t pwm_ = 0;                         // channels SetDuty() owns, one bit each
    uint16_t duty_[NUM_CHANNELS] = {};         // their duty, 0..DUTY_FULL
    std::atomic<bool> dirty_{true};            // the first Update() writes every channel

    esp_err_t WriteReg(uint8_t reg, uint8_t value);
    esp_err_t Init();
    void SetPwm(size_t channel, uint16_t pulseUs);
    void SetEnable(size_t channel, bool enable);

public:
    PCA9685ServoController(i2c_master_bus_handle_t bus, uint8_t address)
        : PCA9685ServoController(bus, DEFAULT_I2C_FREQUENCY_HZ, address) {}
    PCA9685ServoController(i2c_master_bus_handle_t bus, uint32_t busFrequency, uint8_t address);
    ~PCA9685ServoController() override;

    uint8_t Address() const { return address_; }
    /** False when the chip could not be added to the bus or refused its setup writes. */
    bool Ready() const { return device_ != nullptr; }

    void Update() override;
    esp_err_t Reset() override;

    /**
     * Drive a channel as a plain PWM output -- an LED on a servo header -- rather than as a
     * servo: `duty` of DUTY_FULL (0 is off, DUTY_FULL always on) at the servos' 50 Hz frame
     * rate, which is the only rate the chip has. From the first call the channel is this
     * call's, and the Servo on it no longer reaches the wire: its moves and enables are kept
     * and ignored until ReleaseDuty(). Callers hold the manager's lock, as for every write.
     */
    void SetDuty(size_t channel, uint16_t duty);
    /** Hand a channel back to its Servo, as it was before the first SetDuty(). */
    void ReleaseDuty(size_t channel);
    /** The duty of a channel SetDuty() owns; false for a channel it does not. */
    bool GetDuty(size_t channel, uint16_t *duty) const;

    friend class PCA9685Servo;
};

} // namespace servo

#endif // PCA9685_SERVO_CONTROLLER_HPP_
