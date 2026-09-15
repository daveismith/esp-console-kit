#include "Esp32PwmServo.hpp"

#include <string>
#include "driver/mcpwm_prelude.h"
#include "esp_check.h"
#include "esp_log.h"

using namespace servo;

static const char *TAG = "Esp32PwmServo";

#define SERVO_TIMEBASE_RESOLUTION_HZ 1000000  // 1 MHz: one tick is one microsecond
#define SERVO_TIMEBASE_PERIOD        20000    // 20 ms frame

Esp32PwmServo::Esp32PwmServo(int gpio, uint16_t absMin, uint16_t absMax, uint16_t opMin, uint16_t opMax, bool invert)
    : Servo(absMin, absMax, opMin, opMax, invert)
{
    SetIdent("gpio" + std::to_string(gpio));
    LoadSettings();

    if (Setup(gpio) != ESP_OK) {
        ESP_LOGE(TAG, "%s: not driven", ident_.c_str());
        Release();
    }
}

Esp32PwmServo::~Esp32PwmServo()
{
    Release();
}

esp_err_t Esp32PwmServo::Setup(int gpio)
{
    mcpwm_timer_config_t timer_config = {
        .group_id = 0,
        .clk_src = MCPWM_TIMER_CLK_SRC_DEFAULT,
        .resolution_hz = SERVO_TIMEBASE_RESOLUTION_HZ,
        .count_mode = MCPWM_TIMER_COUNT_MODE_UP,
        .period_ticks = SERVO_TIMEBASE_PERIOD,
        .intr_priority = 0,
        .flags = {},
    };

    /* Let the driver do the accounting. It knows how many groups and timers this chip
     * has -- IDF 6 no longer publishes SOC_MCPWM_GROUPS and friends -- and answers
     * ESP_ERR_NOT_FOUND for a full group and ESP_ERR_INVALID_ARG for one that does not
     * exist. */
    esp_err_t err;
    do {
        err = mcpwm_new_timer(&timer_config, &timer_);
        if (err == ESP_ERR_NOT_FOUND) {
            timer_config.group_id++;
        }
    } while (err == ESP_ERR_NOT_FOUND);
    ESP_RETURN_ON_ERROR(err, TAG, "gpio %d: no MCPWM timer free", gpio);

    /* An operator must live in its timer's group. */
    mcpwm_operator_config_t operator_config = {
        .group_id = timer_config.group_id,
        .intr_priority = 0,
        .flags = {},
    };
    ESP_RETURN_ON_ERROR(mcpwm_new_operator(&operator_config, &operator_), TAG, "gpio %d: operator", gpio);
    ESP_RETURN_ON_ERROR(mcpwm_operator_connect_timer(operator_, timer_), TAG, "gpio %d: connect", gpio);

    mcpwm_comparator_config_t comparator_config = {
        .intr_priority = 0,
        .flags = {
            .update_cmp_on_tez = true,
            .update_cmp_on_tep = false,
            .update_cmp_on_sync = false,
        },
    };
    ESP_RETURN_ON_ERROR(mcpwm_new_comparator(operator_, &comparator_config, &comparator_), TAG, "gpio %d: comparator", gpio);

    mcpwm_generator_config_t generator_config = {
        .gen_gpio_num = gpio,
        .flags = {},
    };
    ESP_RETURN_ON_ERROR(mcpwm_new_generator(operator_, &generator_config, &generator_), TAG, "gpio %d: generator", gpio);

    /* Centred until told otherwise. */
    const uint16_t centre = Clamp((uint16_t)((absMin_ + absMax_) / 2));
    ESP_RETURN_ON_ERROR(mcpwm_comparator_set_compare_value(comparator_, centre), TAG, "gpio %d: compare", gpio);

    /* High when the counter empties, low when it reaches the compare value: a pulse of
     * compare-value microseconds at the start of every 20 ms frame. */
    ESP_RETURN_ON_ERROR(mcpwm_generator_set_action_on_timer_event(generator_,
                            MCPWM_GEN_TIMER_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, MCPWM_TIMER_EVENT_EMPTY, MCPWM_GEN_ACTION_HIGH)),
                        TAG, "gpio %d: timer action", gpio);
    ESP_RETURN_ON_ERROR(mcpwm_generator_set_action_on_compare_event(generator_,
                            MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, comparator_, MCPWM_GEN_ACTION_LOW)),
                        TAG, "gpio %d: compare action", gpio);

    /* Disabled means a low line, not a stopped timer parked wherever it happened to be. */
    ESP_RETURN_ON_ERROR(mcpwm_generator_set_force_level(generator_, 0, true), TAG, "gpio %d: force low", gpio);
    return ESP_OK;
}

/* Children before parents, and the timer only once it is back in its init state. */
void Esp32PwmServo::Release()
{
    Stop();
    if (generator_ != nullptr) {
        mcpwm_del_generator(generator_);
        generator_ = nullptr;
    }
    if (comparator_ != nullptr) {
        mcpwm_del_comparator(comparator_);
        comparator_ = nullptr;
    }
    if (operator_ != nullptr) {
        mcpwm_del_operator(operator_);
        operator_ = nullptr;
    }
    if (timer_ != nullptr) {
        mcpwm_del_timer(timer_);
        timer_ = nullptr;
    }
}

void Esp32PwmServo::ApplyValue(uint16_t value)
{
    if (comparator_ == nullptr) {
        return;
    }
    esp_err_t err = mcpwm_comparator_set_compare_value(comparator_, Clamp(value));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: compare: %s", ident_.c_str(), esp_err_to_name(err));
    }
}

void Esp32PwmServo::ApplyEnable(bool enable)
{
    if (!enable) {
        Stop();
        return;
    }
    if (!Ready() || enabled_) {
        return;
    }
    esp_err_t err = mcpwm_timer_enable(timer_);
    if (err == ESP_OK) {
        err = mcpwm_timer_start_stop(timer_, MCPWM_TIMER_START_NO_STOP);
    }
    if (err == ESP_OK) {
        /* -1 lifts the force: the line follows the generator's actions again. */
        err = mcpwm_generator_set_force_level(generator_, -1, true);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: enable: %s", ident_.c_str(), esp_err_to_name(err));
        return;
    }
    enabled_ = true;
}

void Esp32PwmServo::Stop()
{
    if (!Ready() || !enabled_) {
        return;
    }
    /* Force the line low first: a timer stopped at empty leaves the generator HIGH,
     * since that is the empty action. */
    mcpwm_generator_set_force_level(generator_, 0, true);
    mcpwm_timer_start_stop(timer_, MCPWM_TIMER_STOP_EMPTY);
    mcpwm_timer_disable(timer_);
    enabled_ = false;
}
