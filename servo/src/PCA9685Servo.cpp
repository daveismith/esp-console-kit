#include "PCA9685Servo.hpp"
#include "PCA9685ServoController.hpp"

#include <stdio.h>

using namespace servo;

PCA9685Servo::PCA9685Servo(PCA9685ServoController *controller, uint8_t channel, uint16_t absMin, uint16_t absMax)
    : Servo(absMin, absMax), controller_(controller), channel_(channel)
{
    /* "40_0f": the board's address and the channel, which is what the saved limits are
     * keyed by -- so a calibration follows the wiring, not the console name. */
    char name[8];
    snprintf(name, sizeof(name), "%02x_%02x", controller_->Address(), channel_);
    SetIdent(name);
    LoadSettings();

    /* Park the pulse at the centre of the range before anything can enable the channel.
     * Nothing reaches the wire until Enable(); this only guarantees that an Enable() with
     * no MoveTo*() before it puts out a sane pulse rather than ON == OFF == 0, which the
     * datasheet forbids. Esp32PwmServo centres itself the same way. */
    controller_->SetPwm(channel_, Clamp((uint16_t)((absMin_ + absMax_) / 2)));
}

void PCA9685Servo::ApplyValue(uint16_t value)
{
    controller_->SetPwm(channel_, Clamp(value));
}

void PCA9685Servo::ApplyEnable(bool enable)
{
    controller_->SetEnable(channel_, enable);
}

bool PCA9685Servo::SetDuty(uint16_t duty)
{
    controller_->SetDuty(channel_, duty);
    return true;
}

bool PCA9685Servo::ReleaseDuty()
{
    controller_->ReleaseDuty(channel_);
    return true;
}

bool PCA9685Servo::GetDuty(uint16_t *duty) const
{
    return controller_->GetDuty(channel_, duty);
}
