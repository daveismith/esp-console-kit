#ifndef ESP32_PWM_SERVO_HPP_
#define ESP32_PWM_SERVO_HPP_

#include "Servo.hpp"
#include "driver/mcpwm_types.h"
#include "esp_err.h"

namespace servo {

/**
 * A servo on one of the S3's own pins, driven by MCPWM. Each instance takes a whole
 * MCPWM timer, so the chip's two groups of three timers allow six of these. The driver
 * hands the timers out and says when they run out; a servo that could not get one logs
 * why and then ignores every command.
 */
class Esp32PwmServo : public Servo {
private:
    mcpwm_timer_handle_t timer_ = nullptr;
    mcpwm_oper_handle_t operator_ = nullptr;
    mcpwm_cmpr_handle_t comparator_ = nullptr;
    mcpwm_gen_handle_t generator_ = nullptr;
    bool enabled_ = false;

    esp_err_t Setup(int gpio);
    void Release();
    void Stop();

public:
    Esp32PwmServo(int gpio, uint16_t absMin, uint16_t absMax)
        : Esp32PwmServo(gpio, absMin, absMax, absMin, absMax, false) {}
    Esp32PwmServo(int gpio, uint16_t absMin, uint16_t absMax, uint16_t opMin, uint16_t opMax, bool invert);
    ~Esp32PwmServo() override;

    bool Ready() const { return generator_ != nullptr; }

protected:
    void ApplyValue(uint16_t value) override;
    void ApplyEnable(bool enable) override;
};

} // namespace servo

#endif // ESP32_PWM_SERVO_HPP_
