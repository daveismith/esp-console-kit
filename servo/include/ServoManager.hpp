// based on https://refactoring.guru/design-patterns/singleton/cpp/example#example-1

#ifndef SERVOMANAGER_HPP_
#define SERVOMANAGER_HPP_

#include <stddef.h>
#include <stdint.h>
#include <list>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include "esp_err.h"
#include "ServoController.hpp"

namespace servo {

class Servo;

/* A servo whose output stage changed on its own -- the drive policy released it, or a
 * fresh command re-drove a released one. Delivered from the manager's task, outside its
 * lock, so the listener may call anything. */
typedef void (*ServoOutputListener)(const char *ident, bool driven, bool released, void *ctx);

/**
 * The registry of every servo and of the controllers behind them, and the task that
 * flushes those controllers at the servo frame rate.
 *
 * The task starts when the first controller, or the first servo driven without one, is
 * registered -- not when the singleton is created: a board with no servo hardware should
 * not pay for a task stack in internal DRAM.
 */
class ServoManager {
private:
    static ServoManager *pinstance_;
    static std::mutex mutex_;       // guards pinstance_

    std::mutex lock_;               // guards everything below
    std::list<ServoController *> controllers_;
    std::map<std::string, Servo *> servos_;
    bool taskStarted_ = false;
    ServoOutputListener outputListener_ = nullptr;
    void *outputListenerCtx_ = nullptr;

    ServoManager() {}
    ~ServoManager() {}
    void StartTask();               // with lock_ held

public:
    ServoManager(ServoManager &other) = delete;
    void operator=(const ServoManager &) = delete;

    static ServoManager *GetInstance();

    bool RegisterController(ServoController *controller);

    /** Start the frame task if it is not running: for a servo with no controller behind it
     *  (a pin driven directly), whose drive policy still needs its tick. */
    void EnsureTask();

    /**
     * One frame: every servo's policy tick, then pending changes to every controller.
     * The task calls this every 20 ms; anything that wants its change on the wire now
     * rather than at the next tick calls it too. Output changes are reported to the
     * listener after the lock is released.
     */
    void Update();

    /** Who to tell when a servo's output stage changes by policy. One listener. */
    void SetOutputListener(ServoOutputListener listener, void *ctx);

    /** The lock every per-servo command takes, so a command from an application task and
     *  the policy tick on this task never interleave on one servo's state. */
    std::mutex &Lock() { return lock_; }

    /** Every controller through its Reset(), serialised against the task's Update(). */
    esp_err_t ResetControllers();
    size_t ControllerCount();

    bool RegisterServo(std::string ident, Servo *servo);
    bool UnregisterServo(std::string ident);
    Servo *GetServo(std::string ident);

    /** Every registered servo, by ident. A copy, so the caller holds no lock. */
    std::vector<std::pair<std::string, Servo *>> Servos();
};

} // namespace servo

#endif // SERVOMANAGER_HPP_
