#include "ServoManager.hpp"
#include "Servo.hpp"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

using namespace servo;

static const char *TAG = "servo";

ServoManager *ServoManager::pinstance_ = nullptr;
std::mutex ServoManager::mutex_;

/* One servo frame is 20 ms; nothing on the wire can change faster than that. */
static const TickType_t UPDATE_PERIOD = pdMS_TO_TICKS(20);

static void servoManagerTask(void *arg)
{
    ServoManager *manager = static_cast<ServoManager *>(arg);
    TickType_t lastWake = xTaskGetTickCount();
    for (;;) {
        manager->Update();
        xTaskDelayUntil(&lastWake, UPDATE_PERIOD);
    }
}

ServoManager *ServoManager::GetInstance()
{
    std::lock_guard<std::mutex> guard(mutex_);
    if (pinstance_ == nullptr) {
        pinstance_ = new ServoManager();
    }
    return pinstance_;
}

void ServoManager::StartTask()
{
    if (taskStarted_) {
        return;
    }
    /* The loop is tiny, but the I2C driver, a failure's ESP_LOGE and the output listener
     * (which may take the application's own locks) run on it. */
    BaseType_t ok = xTaskCreate(servoManagerTask, "servo", 4096 /* 3072 held 1.4 KB before the policy tick and listener */, this, tskIDLE_PRIORITY + 1, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "cannot start the servo task");
        return;
    }
    taskStarted_ = true;
}

bool ServoManager::RegisterController(ServoController *controller)
{
    if (controller == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> guard(lock_);
    controllers_.push_back(controller);
    StartTask();
    return true;
}

void ServoManager::EnsureTask()
{
    std::lock_guard<std::mutex> guard(lock_);
    StartTask();
}

/* At most this many output changes are reported per frame; a droid has 32 servos and a
 * frame that releases more than a handful of them at once is not a thing. */
static const size_t MAX_EVENTS = 8;

void ServoManager::Update()
{
    struct Event {
        const char *ident;
        bool driven;
        bool released;
    } events[MAX_EVENTS];
    size_t n = 0;
    ServoOutputListener listener = nullptr;
    void *ctx = nullptr;
    {
        std::lock_guard<std::mutex> guard(lock_);
        const int64_t now = esp_timer_get_time();
        for (auto &entry : servos_) {
            Servo *servo = entry.second;
            servo->Tick(now);
            if (servo->TakeOutputChange() && n < MAX_EVENTS) {
                /* The ident's storage is the map key, which outlives this frame. */
                events[n++] = { entry.first.c_str(), servo->Driven(), servo->Released() };
            }
        }
        for (ServoController *controller : controllers_) {
            controller->Update();
        }
        listener = outputListener_;
        ctx = outputListenerCtx_;
    }
    if (listener != nullptr) {
        for (size_t i = 0; i < n; i++) {
            listener(events[i].ident, events[i].driven, events[i].released, ctx);
        }
    }
}

void ServoManager::SetOutputListener(ServoOutputListener listener, void *ctx)
{
    std::lock_guard<std::mutex> guard(lock_);
    outputListener_ = listener;
    outputListenerCtx_ = ctx;
}

esp_err_t ServoManager::ResetControllers()
{
    std::lock_guard<std::mutex> guard(lock_);
    esp_err_t result = ESP_OK;
    for (ServoController *controller : controllers_) {
        esp_err_t err = controller->Reset();
        if (err != ESP_OK && result == ESP_OK) {
            result = err;   /* the first failure, but every controller still gets its turn */
        }
    }
    return result;
}

size_t ServoManager::ControllerCount()
{
    std::lock_guard<std::mutex> guard(lock_);
    return controllers_.size();
}

bool ServoManager::RegisterServo(std::string ident, Servo *servo)
{
    if (servo == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> guard(lock_);
    return servos_.emplace(ident, servo).second;
}

bool ServoManager::UnregisterServo(std::string ident)
{
    std::lock_guard<std::mutex> guard(lock_);
    return servos_.erase(ident) > 0;
}

Servo *ServoManager::GetServo(std::string ident)
{
    std::lock_guard<std::mutex> guard(lock_);
    auto it = servos_.find(ident);
    return (it == servos_.end()) ? nullptr : it->second;
}

std::vector<std::pair<std::string, Servo *>> ServoManager::Servos()
{
    std::lock_guard<std::mutex> guard(lock_);
    return std::vector<std::pair<std::string, Servo *>>(servos_.begin(), servos_.end());
}
