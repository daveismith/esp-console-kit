/*
 * servo.h's implementation: the C face over the ServoManager singleton and the PCA9685
 * and MCPWM classes. The application is C, so everything it needs is a plain function here.
 */
#include "servo.h"

#include <map>
#include <mutex>
#include <string>

#include "driver/i2c_master.h"
#include "esp_log.h"

#include "Esp32PwmServo.hpp"
#include "PCA9685Servo.hpp"
#include "PCA9685ServoController.hpp"
#include "ServoManager.hpp"

using namespace servo;

static const char *TAG = "servo";

static const int PROBE_TIMEOUT_MS = 50;

/* Attached boards by address, so a second attach of the same address is a no-op. The lock
 * covers the whole probe-and-attach sequence: the console's servo_register and the boot
 * path can race, and two controllers on one chip would fight over its outputs. */
static std::mutex s_attach_lock;
static std::map<uint8_t, PCA9685ServoController *> s_controllers;

extern "C" esp_err_t servo_attach_pca9685(int i2c_port, uint8_t address, const char *prefix,
                                          uint16_t abs_min_us, uint16_t abs_max_us)
{
    if (prefix == nullptr || abs_min_us > abs_max_us) {
        return ESP_ERR_INVALID_ARG;
    }
    std::lock_guard<std::mutex> guard(s_attach_lock);

    if (s_controllers.count(address) != 0) {
        return ESP_OK;
    }

    i2c_master_bus_handle_t bus = NULL;
    if (i2c_master_get_bus_handle((i2c_port_num_t)i2c_port, &bus) != ESP_OK) {
        ESP_LOGW(TAG, "no I2C master bus on port %d yet", i2c_port);
        return ESP_ERR_INVALID_STATE;
    }

    /* Probe first: adding a device never touches the bus, so without this an absent board
     * would get a controller that fails every write it is ever asked for. */
    esp_err_t err = i2c_master_probe(bus, address, PROBE_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "no PCA9685 at 0x%02x (%s)", address, esp_err_to_name(err));
        return ESP_ERR_NOT_FOUND;
    }

    auto *controller = new PCA9685ServoController(bus, address);
    if (!controller->Ready()) {
        delete controller;
        return ESP_FAIL;
    }

    ServoManager *manager = ServoManager::GetInstance();
    manager->RegisterController(controller);
    for (uint8_t ch = 0; ch < PCA9685ServoController::NUM_CHANNELS; ch++) {
        const std::string ident = std::string(prefix) + std::to_string(ch);
        auto *servo = new PCA9685Servo(controller, ch, abs_min_us, abs_max_us);
        if (!manager->RegisterServo(ident, servo)) {
            /* Two boards given the same prefix. The board is attached and its channels
             * can still be reached by the first board's names -- say so rather than leak. */
            ESP_LOGE(TAG, "'%s' is already a servo; 0x%02x channel %u is unreachable",
                     ident.c_str(), address, (unsigned)ch);
            delete servo;
        }
    }
    s_controllers[address] = controller;
    ESP_LOGI(TAG, "PCA9685 at 0x%02x: servos %s0..%s%u", address, prefix, prefix,
             (unsigned)(PCA9685ServoController::NUM_CHANNELS - 1));
    return ESP_OK;
}

/* Attached pins, so a second attach of the same pin is a no-op. Under s_attach_lock too. */
static std::map<int, Esp32PwmServo *> s_gpio_servos;

extern "C" esp_err_t servo_attach_gpio(int gpio, const char *ident, uint16_t abs_min_us,
                                       uint16_t abs_max_us, uint16_t op_min_us, uint16_t op_max_us)
{
    if (ident == nullptr || abs_min_us > op_min_us || op_min_us > op_max_us || op_max_us > abs_max_us) {
        return ESP_ERR_INVALID_ARG;
    }
    std::lock_guard<std::mutex> guard(s_attach_lock);

    if (s_gpio_servos.count(gpio) != 0) {
        return ESP_OK;
    }

    auto *servo = new Esp32PwmServo(gpio, abs_min_us, abs_max_us, op_min_us, op_max_us, false);
    if (!servo->Ready()) {
        delete servo;   /* its constructor logged why */
        return ESP_FAIL;
    }
    ServoManager *manager = ServoManager::GetInstance();
    if (!manager->RegisterServo(ident, servo)) {
        ESP_LOGE(TAG, "'%s' is already a servo; gpio %d not attached", ident, gpio);
        delete servo;
        return ESP_ERR_INVALID_ARG;
    }
    /* No controller to flush for a pin driven directly, but the drive policy still needs
     * its frame tick. */
    manager->EnsureTask();
    s_gpio_servos[gpio] = servo;
    uint16_t op_min = 0, op_max = 0;
    servo->GetLimits(nullptr, nullptr, &op_min, &op_max, nullptr);
    ESP_LOGI(TAG, "gpio %d: servo %s, %u..%u us%s", gpio, ident, op_min, op_max,
             servo->Calibrated() ? " (saved)" : "");
    return ESP_OK;
}

extern "C" size_t servo_controller_count(void)
{
    return ServoManager::GetInstance()->ControllerCount();
}

extern "C" esp_err_t servo_reset_controllers(void)
{
    return ServoManager::GetInstance()->ResetControllers();
}

/* Every per-servo call starts here. A missing ident is logged once per call rather than
 * asserted: the application's table can name a channel on a board that is not fitted. */
static Servo *find(const char *id)
{
    if (id == nullptr) {
        return nullptr;
    }
    Servo *servo = ServoManager::GetInstance()->GetServo(id);
    if (servo == nullptr) {
        ESP_LOGW(TAG, "no servo '%s'", id);
    }
    return servo;
}

/* The plain PWM calls. No warning for a missing ident, unlike find(): the LED's owner
 * refreshes it every second whether or not its board has answered yet, and that must not
 * fill the log. Servo's defaults refuse; PCA9685Servo has the mode. */
static Servo *find_quietly(const char *id)
{
    return id != nullptr ? ServoManager::GetInstance()->GetServo(id) : nullptr;
}

extern "C" bool servo_set_duty(const char *id, uint16_t duty)
{
    Servo *servo = find_quietly(id);
    if (servo == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> guard(ServoManager::GetInstance()->Lock());
    return servo->SetDuty(duty);
}

extern "C" bool servo_release_duty(const char *id)
{
    Servo *servo = find_quietly(id);
    if (servo == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> guard(ServoManager::GetInstance()->Lock());
    return servo->ReleaseDuty();
}

extern "C" bool servo_get_duty(const char *id, uint16_t *duty)
{
    Servo *servo = find_quietly(id);
    if (servo == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> guard(ServoManager::GetInstance()->Lock());
    return servo->GetDuty(duty);
}

extern "C" bool servo_exists(const char *id)
{
    return id != nullptr && ServoManager::GetInstance()->GetServo(id) != nullptr;
}

/*
 * Every command below runs under the manager's lock, so it never interleaves with the
 * policy tick on the servo task -- and releases it before Update(), which takes it again.
 */
extern "C" bool servo_set_enable(const char *id, bool enable)
{
    Servo *servo = find(id);
    if (servo == nullptr) {
        return false;
    }
    ServoManager *manager = ServoManager::GetInstance();
    {
        std::lock_guard<std::mutex> guard(manager->Lock());
        if (enable) {
            servo->Enable();
        } else {
            servo->Disable();
        }
    }
    /* On the wire now: switching a channel off is the one command that should not wait
     * a frame, since it is what a RELEASE or a disarm means. */
    manager->Update();
    return true;
}

extern "C" bool servo_hold(const char *id)
{
    Servo *servo = find(id);
    if (servo == nullptr) {
        return false;
    }
    ServoManager *manager = ServoManager::GetInstance();
    {
        std::lock_guard<std::mutex> guard(manager->Lock());
        servo->Hold();
    }
    manager->Update();
    return true;
}

extern "C" bool servo_set_percentage(const char *id, uint16_t percentage)
{
    Servo *servo = find(id);
    if (servo == nullptr) {
        return false;
    }
    ServoManager *manager = ServoManager::GetInstance();
    {
        std::lock_guard<std::mutex> guard(manager->Lock());
        servo->Enable();
        servo->MoveToPercentage(percentage);
    }
    manager->Update();
    return true;
}

extern "C" bool servo_move_us(const char *id, uint16_t us)
{
    Servo *servo = find(id);
    if (servo == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> guard(ServoManager::GetInstance()->Lock());
    servo->MoveToValue(us);
    return true;
}

extern "C" bool servo_get_drive_policy(const char *id, servo_drive_policy_t *out)
{
    if (out == nullptr || id == nullptr) {
        return false;
    }
    Servo *servo = ServoManager::GetInstance()->GetServo(id);
    if (servo == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> guard(ServoManager::GetInstance()->Lock());
    const DrivePolicy &p = servo->Policy();
    out->drive_closed = (p.closed == Drive::Release) ? SERVO_DRIVE_RELEASE : SERVO_DRIVE_HOLD;
    out->drive_open = (p.open == Drive::Release) ? SERVO_DRIVE_RELEASE : SERVO_DRIVE_HOLD;
    out->drive_mid = (p.mid == Drive::Release) ? SERVO_DRIVE_RELEASE : SERVO_DRIVE_HOLD;
    out->settle_ms = p.settleMs;
    return true;
}

extern "C" bool servo_set_drive_policy(const char *id, const servo_drive_policy_t *policy)
{
    Servo *servo = find(id);
    if (servo == nullptr || policy == nullptr) {
        return false;
    }
    DrivePolicy p;
    p.closed = policy->drive_closed == SERVO_DRIVE_RELEASE ? Drive::Release : Drive::Hold;
    p.open = policy->drive_open == SERVO_DRIVE_RELEASE ? Drive::Release : Drive::Hold;
    p.mid = policy->drive_mid == SERVO_DRIVE_RELEASE ? Drive::Release : Drive::Hold;
    p.settleMs = policy->settle_ms;
    /* The NVS write happens under the lock: a few milliseconds during which the servo
     * task waits one frame, which is nothing against a page program's own duration. */
    std::lock_guard<std::mutex> guard(ServoManager::GetInstance()->Lock());
    return servo->SetDrivePolicy(p);
}

extern "C" bool servo_get_output(const char *id, bool *driven, bool *released)
{
    if (id == nullptr) {
        return false;
    }
    Servo *servo = ServoManager::GetInstance()->GetServo(id);
    if (servo == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> guard(ServoManager::GetInstance()->Lock());
    if (driven != nullptr) {
        *driven = servo->Driven();
    }
    if (released != nullptr) {
        *released = servo->Released();
    }
    return true;
}

extern "C" void servo_set_output_callback(servo_output_cb_t cb, void *ctx)
{
    ServoManager::GetInstance()->SetOutputListener(cb, ctx);
}

extern "C" bool servo_get_value_us(const char *id, uint16_t *us)
{
    if (id == nullptr || us == nullptr) {
        return false;
    }
    Servo *servo = ServoManager::GetInstance()->GetServo(id);
    if (servo == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> guard(ServoManager::GetInstance()->Lock());
    *us = servo->LastValue();
    return true;
}

extern "C" bool servo_get_limits(const char *id, servo_limits_t *out)
{
    if (out == nullptr || id == nullptr) {
        return false;
    }
    Servo *servo = ServoManager::GetInstance()->GetServo(id);
    if (servo == nullptr) {
        return false;
    }
    servo->GetLimits(&out->abs_min_us, &out->abs_max_us, &out->op_min_us, &out->op_max_us, &out->invert);
    out->calibrated = servo->Calibrated();
    return true;
}

extern "C" bool servo_set_op_limits(const char *id, uint16_t min_us, uint16_t max_us, bool invert)
{
    Servo *servo = find(id);
    if (servo == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> guard(ServoManager::GetInstance()->Lock());
    return servo->SetOpLimits(min_us, max_us, invert);
}

extern "C" bool servo_clear_op_limits(const char *id)
{
    Servo *servo = find(id);
    if (servo == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> guard(ServoManager::GetInstance()->Lock());
    return servo->ClearOpLimits();
}
