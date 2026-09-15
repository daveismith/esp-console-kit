#ifndef SERVO_CONTROLLER_HPP_
#define SERVO_CONTROLLER_HPP_

#include "esp_err.h"

namespace servo {

class ServoManager;

/** Something that owns the wire to one or more servos and flushes it on Update(). */
class ServoController {

    friend class ServoManager;

public:
    virtual ~ServoController() {}
    virtual void Update() = 0;
    /** Back to the just-attached state with every channel off. Only the manager calls
     *  this, so it never runs concurrently with Update(). */
    virtual esp_err_t Reset() { return ESP_OK; }

};

} // namespace servo

#endif // SERVO_CONTROLLER_HPP_
