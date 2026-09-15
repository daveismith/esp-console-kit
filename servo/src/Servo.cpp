#include "Servo.hpp"

#include <inttypes.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

using namespace servo;

static const char *TAG = "servo";
static const char *NVS_NAMESPACE = "servo";

/* Endpoint zone half-width, as a fraction of the working range (spec section 3.4). One
 * constant for every servo; a per-servo value is speculation until something needs it. */
static const double ZONE_EPSILON = 0.02;

/* NVS keys are at most 15 characters. An ident is "40_0f" (a PCA9685's address and
 * channel) or "gpio14", and the longest suffix is "_invert", so every key fits. The policy
 * adds "_drive" (2 bits per zone: closed, open, mid) and "_stlms" (settle_ms). */
static const char *KEY_DRIVE = "_drive";
static const char *KEY_SETTLE = "_stlms";

static uint8_t pack_policy(const DrivePolicy &p)
{
    return (uint8_t)(((uint8_t)p.closed & 3) | (((uint8_t)p.open & 3) << 2) | (((uint8_t)p.mid & 3) << 4));
}

static DrivePolicy unpack_policy(uint8_t drive, uint16_t settle)
{
    DrivePolicy p;
    p.closed = ((drive & 3) == 1) ? Drive::Release : Drive::Hold;
    p.open = (((drive >> 2) & 3) == 1) ? Drive::Release : Drive::Hold;
    p.mid = (((drive >> 4) & 3) == 1) ? Drive::Release : Drive::Hold;
    p.settleMs = settle < SETTLE_MIN_MS ? SETTLE_MIN_MS : settle;
    return p;
}

void Servo::SetIdent(std::string ident)
{
    ident_ = ident;
}

void Servo::LoadSettings()
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        /* No namespace yet just means nothing has been calibrated. */
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "%s: cannot open NVS: %s", ident_.c_str(), esp_err_to_name(err));
        }
        return;
    }

    /* All three or nothing, and only a range this servo may actually be sent: the
     * absolute limits are compiled in and may have changed since the record was saved. */
    uint16_t opMin = 0;
    uint16_t opMax = 0;
    uint8_t invert = 0;
    if (nvs_get_u16(handle, (ident_ + "_opMin").c_str(), &opMin) == ESP_OK &&
        nvs_get_u16(handle, (ident_ + "_opMax").c_str(), &opMax) == ESP_OK &&
        nvs_get_u8(handle, (ident_ + "_invert").c_str(), &invert) == ESP_OK) {
        if (opMin >= absMin_ && opMax <= absMax_ && opMin <= opMax) {
            opMin_ = opMin;
            opMax_ = opMax;
            invert_ = (invert != 0);
            calibrated_ = true;
        } else {
            ESP_LOGW(TAG, "%s: saved range %u..%u is outside %u..%u, ignored",
                     ident_.c_str(), opMin, opMax, absMin_, absMax_);
        }
    }

    /* Both or nothing here too. A servo with no drive record reads HOLD in every zone,
     * which is the spec's rule 1 default, so an installation from before the policy
     * existed is unchanged by it. */
    uint8_t drive = 0;
    uint16_t settle = 0;
    if (nvs_get_u8(handle, (ident_ + KEY_DRIVE).c_str(), &drive) == ESP_OK &&
        nvs_get_u16(handle, (ident_ + KEY_SETTLE).c_str(), &settle) == ESP_OK) {
        policy_ = unpack_policy(drive, settle);
    }
    nvs_close(handle);
}

bool Servo::SetOpLimits(uint16_t min, uint16_t max, bool invert)
{
    if (min < absMin_ || max > absMax_ || min > max) {
        ESP_LOGW(TAG, "%s: %u..%u is outside %u..%u", ident_.c_str(), min, max, absMin_, absMax_);
        return false;
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: cannot open NVS: %s", ident_.c_str(), esp_err_to_name(err));
        return false;
    }

    if ((err = nvs_set_u16(handle, (ident_ + "_opMin").c_str(), min)) == ESP_OK &&
        (err = nvs_set_u16(handle, (ident_ + "_opMax").c_str(), max)) == ESP_OK &&
        (err = nvs_set_u8(handle, (ident_ + "_invert").c_str(), invert ? 1 : 0)) == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: cannot save the range: %s", ident_.c_str(), esp_err_to_name(err));
        return false;
    }

    opMin_ = min;
    opMax_ = max;
    invert_ = invert;
    calibrated_ = true;
    return true;
}

bool Servo::SetDrivePolicy(const DrivePolicy &wanted)
{
    DrivePolicy policy = wanted;
    if (policy.settleMs < SETTLE_MIN_MS) {
        policy.settleMs = SETTLE_MIN_MS;
    }
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: cannot open NVS: %s", ident_.c_str(), esp_err_to_name(err));
        return false;
    }
    if ((err = nvs_set_u8(handle, (ident_ + KEY_DRIVE).c_str(), pack_policy(policy))) == ESP_OK &&
        (err = nvs_set_u16(handle, (ident_ + KEY_SETTLE).c_str(), policy.settleMs)) == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: cannot save the drive policy: %s", ident_.c_str(), esp_err_to_name(err));
        return false;
    }
    policy_ = policy;
    return true;
}

bool Servo::ClearOpLimits()
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: cannot open NVS: %s", ident_.c_str(), esp_err_to_name(err));
        return false;
    }

    /* A key that is already absent is the state being asked for, not a failure. The
     * policy goes with the calibration: it is a fact about the servo and the linkage in
     * front of it (spec section 9), and a fresh calibration starts from HOLD. */
    static const char *const suffixes[] = { "_opMin", "_opMax", "_invert", "_drive", "_stlms" };
    for (const char *suffix : suffixes) {
        err = nvs_erase_key(handle, (ident_ + suffix).c_str());
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
        if (err != ESP_OK) {
            break;
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: cannot clear the range: %s", ident_.c_str(), esp_err_to_name(err));
        return false;
    }

    opMin_ = defOpMin_;
    opMax_ = defOpMax_;
    invert_ = defInvert_;
    calibrated_ = false;
    policy_ = DrivePolicy();
    return true;
}

void Servo::GetLimits(uint16_t *absMin, uint16_t *absMax, uint16_t *opMin, uint16_t *opMax, bool *invert) const
{
    if (absMin != nullptr) {
        *absMin = absMin_;
    }
    if (absMax != nullptr) {
        *absMax = absMax_;
    }
    if (opMin != nullptr) {
        *opMin = opMin_;
    }
    if (opMax != nullptr) {
        *opMax = opMax_;
    }
    if (invert != nullptr) {
        *invert = invert_;
    }
}

uint16_t Servo::Clamp(uint16_t value) const
{
    if (value < absMin_) {
        return absMin_;
    }
    if (value > absMax_) {
        return absMax_;
    }
    return value;
}

/* Zones are named by the COMMANDED value, because on a servo without feedback the
 * command is the only thing known. Closed is opMin, open opMax, or the reverse when
 * inverted; uncalibrated, the whole range is intermediate. */
Zone Servo::ZoneOf(uint16_t value) const
{
    if (!calibrated_ || opMax_ == opMin_) {
        return Zone::Intermediate;
    }
    const double closed = invert_ ? opMax_ : opMin_;
    const double open = invert_ ? opMin_ : opMax_;
    double f = ((double)value - closed) / (open - closed);
    if (f <= ZONE_EPSILON) return Zone::Closed;
    if (f >= 1.0 - ZONE_EPSILON) return Zone::Open;
    return Zone::Intermediate;
}

void Servo::MoveToValue(uint16_t value)
{
    lastValue_ = Clamp(value);
    lastCommandUs_ = esp_timer_get_time();
    holdUntilMove_ = false;
    ApplyValue(lastValue_);
    /* A command moves the servo, whatever state the output was in: spec rule 2 for an
     * output the policy released, and spec section 5 for one that was switched off --
     * an explicit enable or disable wins only until the next motion command. The policy
     * then decides again once this command has settled. */
    wanted_ = true;
    if (!driven_) {
        ApplyEnable(true);
        driven_ = true;
        outputChanged_ = true;
    }
    released_ = false;
}

uint16_t Servo::MoveToPercentage(uint16_t value)
{
    if (value > 1000) {
        value = 1000;
    }
    if (invert_) {
        value = 1000 - value;
    }
    const uint32_t span = (opMax_ > opMin_) ? (uint32_t)(opMax_ - opMin_) : 0;
    const uint32_t us = opMin_ + (span * value) / 1000;
    ESP_LOGD(TAG, "%s: %u/1000 -> %" PRIu32 " us", ident_.c_str(), value, us);
    MoveToValue((uint16_t)us);
    return (uint16_t)us;
}

void Servo::Enable()
{
    wanted_ = true;
    released_ = false;
    lastCommandUs_ = esp_timer_get_time();
    if (!driven_) {
        driven_ = true;
        outputChanged_ = true;
    }
    ApplyEnable(true);
}

void Servo::Hold()
{
    Enable();
    holdUntilMove_ = true;
}

void Servo::Disable()
{
    wanted_ = false;
    if (driven_ || released_) {
        outputChanged_ = true;
    }
    driven_ = false;
    released_ = false;
    holdUntilMove_ = false;
    ApplyEnable(false);
}

bool Servo::Tick(int64_t nowUs)
{
    if (!driven_ || holdUntilMove_) {
        return false;
    }
    Drive drive = Drive::Hold;
    switch (ZoneOf(lastValue_)) {
    case Zone::Closed: drive = policy_.closed; break;
    case Zone::Open:   drive = policy_.open; break;
    default:           drive = policy_.mid; break;
    }
    if (drive != Drive::Release) {
        return false;
    }
    if (nowUs - lastCommandUs_ < (int64_t)policy_.settleMs * 1000) {
        return false;
    }
    ApplyEnable(false);
    driven_ = false;
    released_ = true;
    outputChanged_ = true;
    return true;
}

bool Servo::TakeOutputChange()
{
    bool changed = outputChanged_;
    outputChanged_ = false;
    return changed;
}
