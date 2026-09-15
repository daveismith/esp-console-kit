#ifndef SERVO_HPP_
#define SERVO_HPP_

#include <stdint.h>
#include <string>

namespace servo {

/* Whether the output stage keeps driving once a command has settled in a zone. */
enum class Drive : uint8_t { Hold = 0, Release = 1 };

/*
 * The drive policy, servo_model_spec.md section 3.4: one choice per zone, and the settle
 * time -- how long the output is driven after any command before a RELEASE takes effect,
 * without which "stop driving once closed" means "never move". HOLD everywhere is the
 * default and the only safe default; a servo with no saved record reads that way.
 */
/* The shortest settle time honoured. At 0 a RELEASE zone dropped the output on the frame
 * after the command, before the horn had moved: "close, then release" read as "never
 * move" on the bench. Anything shorter, saved or loaded, is raised to this. */
static const uint16_t SETTLE_MIN_MS = 100;

struct DrivePolicy {
    Drive closed = Drive::Hold;
    Drive open = Drive::Hold;
    Drive mid = Drive::Hold;
    uint16_t settleMs = SETTLE_MIN_MS;
};

enum class Zone : uint8_t { Closed = 0, Intermediate = 1, Open = 2 };

/**
 * One servo channel. Every time here is a pulse width in microseconds, which fits a
 * uint16_t: the whole 20 ms frame is 20,000 us.
 *
 * absMin..absMax is what the hardware may ever be sent. opMin..opMax is the working
 * range a percentage maps onto; it and `invert` persist in NVS under the servo's ident,
 * so a calibration survives a reboot. `calibrated` says whether that record exists: a
 * servo without one works over the range it was constructed with -- its whole absolute
 * range, unless the application knew better -- which is a default rather than a range
 * somebody chose, and a UI should show it as UNSET for that reason.
 *
 * The output stage is managed here, not in the subclasses: MoveToValue, Enable, Hold and
 * Disable keep the drive policy's state and call ApplyValue / ApplyEnable, which are all
 * a subclass writes. Tick(), from the manager's task every frame, is where a RELEASE lands
 * once a command has settled.
 */
class Servo {
protected:
    std::string ident_;
    bool invert_;
    bool calibrated_ = false;
    uint16_t absMin_;
    uint16_t absMax_;
    uint16_t opMin_;
    uint16_t opMax_;
    DrivePolicy policy_;
    /* The working range the servo was constructed with: what ClearOpLimits() goes back to. */
    uint16_t defOpMin_;
    uint16_t defOpMax_;
    bool defInvert_;

    /* The output stage, as the policy sees it. `wanted_` is whether anything has asked
     * for the output since the last Disable (an Enable, or any move); `driven_` is whether
     * it is live now, which the policy can take below `wanted_`; `released_` says that it
     * did. Any move drives the output again, whichever way it went off. */
    bool wanted_ = false;
    bool driven_ = false;
    bool released_ = false;
    bool holdUntilMove_ = false;   /* Hold(): the policy is suspended until the next move */
    bool outputChanged_ = false;   /* driven_/released_ moved since the manager last looked */
    uint16_t lastValue_;
    int64_t lastCommandUs_ = 0;

    void SetIdent(std::string ident);
    /** Replace the working range and the policy with what is saved for this ident. */
    void LoadSettings();
    Zone ZoneOf(uint16_t value) const;

    virtual void ApplyValue(uint16_t value) = 0;
    virtual void ApplyEnable(bool enable) = 0;

public:
    Servo(uint16_t absMin, uint16_t absMax) : Servo(absMin, absMax, absMin, absMax, false) {}
    Servo(uint16_t absMin, uint16_t absMax, uint16_t opMin, uint16_t opMax, bool invert)
        : invert_(invert), absMin_(absMin), absMax_(absMax), opMin_(opMin), opMax_(opMax),
          defOpMin_(opMin), defOpMax_(opMax), defInvert_(invert),
          lastValue_((uint16_t)((absMin + absMax) / 2)) {}
    virtual ~Servo() {}

    const std::string &Ident() const { return ident_; }
    bool Calibrated() const { return calibrated_; }

    /** Set and save the working range. Refuses one outside absMin..absMax. */
    bool SetOpLimits(uint16_t min, uint16_t max, bool invert);
    /** Forget the saved working range and policy: back to the one it was constructed
     *  with, HOLD everywhere, and no longer calibrated. */
    bool ClearOpLimits();

    void GetLimits(uint16_t *absMin, uint16_t *absMax, uint16_t *opMin, uint16_t *opMax, bool *invert) const;

    const DrivePolicy &Policy() const { return policy_; }
    /** Set and save the drive policy. */
    bool SetDrivePolicy(const DrivePolicy &policy);

    /** A pulse width pulled into absMin..absMax. */
    uint16_t Clamp(uint16_t value) const;

    /** Move to a raw pulse width in microseconds, clamped. Drives the output if it was not
     *  driven -- released by the policy or switched off -- and restarts the settle timer. */
    void MoveToValue(uint16_t value);

    /** Move to a point in the working range: 0..1000, in tenths of a percent.
     *  Returns the pulse width that was sent, in microseconds. */
    uint16_t MoveToPercentage(uint16_t value);

    /** Drive the output now. The policy applies again once the settle time has run. */
    void Enable();
    /** Drive the output now and keep it driven until the next move, whatever the policy
     *  says -- MoveServoRaw's ENABLE_ON. */
    void Hold();
    /** Release the output now, whatever the policy says. Off until Enable() or a move. */
    void Disable();

    /** The pulse width last commanded, in microseconds; the absolute centre until then. */
    uint16_t LastValue() const { return lastValue_; }

    bool Driven() const { return driven_; }
    bool Released() const { return released_; }
    /** The current zone of the last commanded value; Intermediate when uncalibrated. */
    Zone CurrentZone() const { return ZoneOf(lastValue_); }

    /** One frame of the policy, from the manager's task. True when the output changed. */
    bool Tick(int64_t nowUs);
    /** Whether Tick or a re-driving move changed the output since this was last called. */
    bool TakeOutputChange();

    /** Drive the channel as plain PWM rather than as a servo: servo_set_duty(). Moves and
     *  enables go on being kept and stop reaching the wire until ReleaseDuty(). False where
     *  the hardware has no such mode, which is the default; PCA9685Servo has one. */
    virtual bool SetDuty(uint16_t duty) { (void)duty; return false; }
    virtual bool ReleaseDuty() { return false; }
    virtual bool GetDuty(uint16_t *duty) const { (void)duty; return false; }
};

} // namespace servo

#endif // SERVO_HPP_
