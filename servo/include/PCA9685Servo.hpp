#ifndef PCA9685_SERVO_HPP_
#define PCA9685_SERVO_HPP_

#include "Servo.hpp"

namespace servo {

class PCA9685ServoController;

/** One channel of a PCA9685. The controller owns the wire; this owns the limits. */
class PCA9685Servo : public Servo {
private:
    PCA9685ServoController *controller_;
    uint8_t channel_;

public:
    PCA9685Servo(PCA9685ServoController *controller, uint8_t channel, uint16_t absMin, uint16_t absMax);

protected:
    void ApplyValue(uint16_t value) override;
    void ApplyEnable(bool enable) override;
    bool SetDuty(uint16_t duty) override;
    bool ReleaseDuty() override;
    bool GetDuty(uint16_t *duty) const override;
};

} // namespace servo

#endif // PCA9685_SERVO_HPP_
