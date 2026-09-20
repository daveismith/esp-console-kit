# Servo model and remote access — specification

**Status: implemented on this board (Constrained profile), drive policy included.**
`main/panel_servo_ros.c` offers the four §6.2 interfaces with the `astromech_msgs` types of §7
(a submodule at `extra_ros_packages/astromech_msgs`); `main/panel_servo.c` holds the model and
the §5 refusals; `/dome/joint_command` and `/dome/joint_states` speak radians. The §3.4 drive
policy is in `components/servo` (`Servo::Tick()` from the manager's 20 ms frame, `_drive` /
`_stlms` NVS keys per §9) and reaches both `APPLY_DRIVE` and the panel's DRIVE POLICY page;
the panel fixes `drive_mid` at HOLD, ROS may set it; `settle_ms` floors at 100 on this board
(open question 2: at 0 the output dropped before the horn had moved). Open question 7 is answered: services
work over rmw_zenoh_pico. One measured correction: `raw_step` on this board is 5, since a
PCA9685 tick at 50 Hz is 4.88 µs. §5's rule that an explicit enable or disable wins only
until the next motion command holds: any move drives the output, released or switched off;
an explicit release is what makes a servo still. **This implementation's arm switch gates its
own control panel only** (changed 2026-09-17): commands from ROS, the console and the droid's
buttons are never refused `not armed`, and dropping every output is a separate control. The
refusal stays in the vocabulary below for controllers whose interlock does gate the wire.

A common description of a servo, and a common way to reach one over ROS 2, so that a control
panel can present an edit screen for *any* servo on the droid without knowing which controller
owns it or what kind of actuator it is.

Written to be implementable twice: on a microcontroller (this project — ESP32-S3, PCA9685 PWM)
and on a Linux-class node (Dynamixel chains, ros2_control). Where the two must differ, the
difference is a stated profile rather than an accident.

---

## 1. The problem

Three separable things are being asked of the same servo:

- **Normal motion.** "Open pie panel 1." The natural unit is **radians**, bounded by the URDF's
  `<limit lower= upper=>`, because that is what the rest of the robot — the description, MoveIt,
  `sensor_msgs/JointState` — already speaks.
- **Configuration.** "This panel is fully closed at 1180 µs and fully open at 1840 µs." The
  natural unit is **whatever the actuator takes** — microseconds for a PWM servo, encoder ticks
  for a Dynamixel. Radians are useless here, because establishing the radian↔raw mapping is the
  whole job.
- **Whether the output is driven at all.** Independent of both. A closed dome panel is held shut
  by its own frame; there is nothing for the servo to do but buzz against a hard stop, drawing
  current and wearing the gear train. Somewhere else — a panel held open against gravity — the
  drive must not stop. This is neither joint space nor actuator space; it is a policy, and it is
  the field the initial list was missing entirely.

The first two mean the model carries two coordinate systems and a mapping between them. Almost
every design mistake available here comes from collapsing them into one.

**Joint space is the public interface. Actuator space is the configuration interface.** Anything
that is not a calibration tool should be speaking radians.

---

## 2. What was missing from the initial field list

The starting list was: type, physical min, physical max, open, closed, current. That is the right
core. Adding to it:

| missing | why it matters |
| --- | --- |
| **Drive policy** | Whether the output is driven at the closed endpoint, at the open endpoint, and in between — three independent choices. §3.4. |
| **Settle time** | The companion to the above. "Stop driving once closed" without a delay means "never move", because the release lands before the servo has travelled. |
| **Raw units** | `1500` means microseconds on PWM and is a meaningless tick count on Dynamixel. Without a declared unit an edit screen cannot label a field, and cannot decide whether `±10` is a nudge or a slam. |
| **Joint limits (radians)** | Needed for the radian interface. Comes from URDF `<limit>`, but the servo layer has to hold it to do the mapping at all. |
| **Measured vs commanded** | A PWM servo has **no current value** — only a last-commanded one. Reporting the command as a measurement is a lie the UI would render faithfully. These must be separate fields with a capability flag. And once the drive is released, even the command stops describing where the horn is. |
| **Calibration state** | "Closed/open are real" vs "they are defaulted to the physical range". The panel already calls the latter UNSET; the model needs it so nothing calibrates against a guess. |
| **Step / resolution** | The endpoint editor's ± button has to know what one press means. One microsecond is invisible; one Dynamixel tick may be too. |
| **Enabled state** | Whether the output stage is live *right now*. An observable, mostly set by the drive policy — distinct from armed. |
| **Owning controller** | With several servos per node and several nodes per droid, an edit screen needs to know where to send a command. Discovered, not configured. |
| **Capability flags** | So the screen greys out what a PWM servo cannot do rather than showing dead controls. |
| **Safety interlock** | Something must be able to refuse motion, and the protocol has to carry the refusal — whether or not a given controller has one. This panel's arm switch gates its own screen rather than the wire, so it never sends `not armed`; a controller with a hardware interlock will. |
| **Velocity / effort limits** | URDF already carries them. Not needed to configure endpoints, needed the moment anything moves on a profile. |

**Trim is deliberately absent.** An earlier draft carried a signed `raw_trim` offset applied on
top of the endpoints. It is redundant: correcting where a panel sits when closed *is* editing
`raw_closed`, and a second quantity that shifts the same curve is a second source of truth for
one physical fact. One less field, one less thing to persist, one less thing to disagree.

---

## 3. Data model (normative)

One record per servo. Field names below are normative; the wire and language bindings in §6–§7
use them verbatim.

### 3.1 Identity

| field | type | notes |
| --- | --- | --- |
| `joint` | string | The URDF joint name with no `_joint` suffix — `pp1`, `upper_utility_arm`. **The droid-wide key.** Unique across the whole robot, not just the node. |
| `display` | string | Human label, ≤23 chars. Comes from `<astromech:servo display=...>`. |

There is deliberately no separate "servo id". A servo is identified by the joint it drives.
**Confirmed against this droid: no joint is driven by more than one servo**, so the key is sound.
A ganged pair would break it and would need a servo index beside the joint name; nothing here
should be built to anticipate that, but this is the assumption it would violate.
Anything else (this project's `pca0_0` manager ident, its `40_00` NVS key) is a controller-local
implementation detail and **must not appear on the wire** — those are how *this* firmware finds
the hardware, and they mean nothing to a panel.

### 3.2 Type and capability

| field | type | notes |
| --- | --- | --- |
| `actuator_type` | enum | `PWM`, `DYNAMIXEL`. Extensible. |
| `raw_units` | enum | `MICROSECONDS`, `TICKS`, `MILLIDEGREES`. Needed to label and to reason about magnitude. |
| `has_position_feedback` | bool | If false, `raw_position` is absent and only `raw_target` is meaningful. |
| `has_temperature`, `has_voltage`, `has_load` | bool | Optional telemetry. |
| `can_release` | bool | Whether the output can actually be turned off. False means the drive policy is fixed at `HOLD` everywhere and `RELEASE` must be refused, not silently ignored. |

### 3.3 Actuator space

| field | type | notes |
| --- | --- | --- |
| `raw_min`, `raw_max` | int32 | **Physical limits.** Never exceeded, whatever anything asks for. A property of the hardware and the linkage, not a calibration. |
| `raw_closed`, `raw_open` | int32 | Configured endpoints: the raw values at which the joint is at `joint_lower` and `joint_upper`. `raw_closed > raw_open` is legal and means the actuator runs backwards relative to the joint — that *is* the direction flag, and a separate `invert` is redundant. |
| `raw_step` | int32 | Smallest increment a UI should offer. |
| `calibrated` | bool | False means `raw_closed`/`raw_open` are defaults, not measurements. |

### 3.4 Drive policy

Whether the output stage is driven, decided per **zone**. Zones are named by the *commanded*
joint fraction, because on a servo without feedback the command is the only thing known:

```
fraction = (theta - joint_lower) / (joint_upper - joint_lower)

CLOSED        fraction <= zone_epsilon
OPEN          fraction >= 1 - zone_epsilon
INTERMEDIATE  everything between
```

| field | type | notes |
| --- | --- | --- |
| `drive_closed` | enum | `HOLD` or `RELEASE`, applied once a command in the CLOSED zone has settled. |
| `drive_open` | enum | Same, for the OPEN zone. |
| `drive_mid` | enum | Same, for INTERMEDIATE. |
| `settle_ms` | uint16 | How long the output is driven after a command before `RELEASE` takes effect. Must cover worst-case travel for this servo. |
| `zone_epsilon` | float64 | Half-width of the endpoint zones as a fraction of travel, 0..0.5. Default 0.02. |

Rules:

1. **`HOLD` everywhere is the default and the only safe default.** `RELEASE` is a statement that
   the mechanism holds itself in that zone. Nothing may infer it — not from actuator type, not
   from a joint being at an endpoint.
2. **`RELEASE` never takes effect before `settle_ms` has elapsed** since the most recent command.
   A new command restarts the timer and re-drives the output immediately, whatever zone it is in.
3. **A released servo's position is unknown.** `raw_target` still reports what was last commanded,
   but nothing holds the horn there. `enabled == false` is the flag that says so, and a UI must
   render that differently from a driven servo at the same target.
3. **`can_release == false` means `RELEASE` is refused**, with a reason — not accepted and
   quietly ignored.
5. `drive_mid = RELEASE` is legal and is how a free-swinging linkage is described, but it means
   the servo cannot hold any intermediate position at all. It is not a default anywhere.
6. The policy is a property of the servo and its linkage, so it persists with the calibration
   (§9) and travels with the wiring, not with the joint name.

> On this project the mechanism already exists: `servo_set_enable()` in
> [`components/servo/include/servo.h`](../include/servo.h) — "Off means no pulses: the servo goes
> limp" — and `ServoManager`'s task already visits every controller on a 20 ms frame, which is
> where a settle deadline would be checked. What is missing is the policy, not the ability.

**Dynamixel maps onto exactly the same three fields**, with torque-enable as the mechanism
instead of pulse suppression. That is why this is not called `pwm_hold`.

### 3.5 Joint space

| field | type | notes |
| --- | --- | --- |
| `joint_lower`, `joint_upper` | float64 | Radians, from URDF `<limit>`. |
| `velocity_limit`, `effort_limit` | float64 | From URDF. May be zero for "unspecified". |

### 3.6 State

| field | type | notes |
| --- | --- | --- |
| `raw_target` | int32 | Last value commanded to the hardware, after mapping and clamping. Always present. |
| `raw_position` | int32 | **Measured.** Present only when `has_position_feedback`. |
| `position` | float64 | Radians, derived from `raw_position` if there is feedback, otherwise from `raw_target`. |
| `position_is_measured` | bool | Which of the two the above came from. Do not make a reader infer this. |
| `enabled` | bool | Output stage live right now. Under drive policy this changes on its own. |
| `zone` | enum | `CLOSED`, `OPEN`, `INTERMEDIATE` — which zone `raw_target` is in, so a UI need not recompute it. |
| `moving`, `stalled`, `fault` | bool | Best effort; false when unknowable. |
| `temperature_c`, `voltage`, `load` | float64 | Present per capability. |

---

## 4. The mapping (normative)

Two ranges, linearly related. That is all it is:

```
span_raw    = raw_open - raw_closed          # signed; negative is a reversed actuator
span_joint  = joint_upper - joint_lower

# joint space -> actuator space
fraction    = (theta - joint_lower) / span_joint
raw_command = clamp(raw_closed + span_raw * fraction, raw_min, raw_max)

# actuator space -> joint space (for reporting)
fraction    = (raw_value - raw_closed) / span_raw
theta       = joint_lower + span_joint * fraction
```

Rules:

1. **`clamp` to `raw_min`/`raw_max` is the last operation and is not optional.** It is the only
   thing standing between a bad calibration and a stripped gear.
2. **Clamping is not silent.** A command that clamped is reported as such, or the operator is
   configuring against a value the hardware never reached.
3. `span_joint == 0` or `span_raw == 0` means unusable; refuse joint-space commands rather than
   dividing by zero.
4. Uncalibrated (`calibrated == false`) servos **must refuse joint-space commands** entirely.
   Raw commands are still allowed — that is how you calibrate one.
5. Rounding is the implementation's business, but it must be consistent in both directions, or a
   value written and read back will drift by a unit.

---

## 5. Two command modes

| mode | unit | clamped to | used for | preconditions |
| --- | --- | --- | --- | --- |
| **Joint** | radians | `joint_lower..joint_upper`, then `raw_min..raw_max` | normal motion, ROS, choreography | calibrated (plus any interlock the controller has) |
| **Raw** | raw units | `raw_min..raw_max` only | setting endpoints, bring-up | an explicit maintenance intent (plus any interlock) |

Raw mode deliberately bypasses the endpoints — you cannot calibrate through the calibration.

**Whatever interlock a controller has, it is the same for both modes.** A second maintenance
flag beside it would add a state to reason about without adding a decision, since whatever
could set the second could set the first.

**On this implementation the interlock is not on the wire at all** (changed 2026-09-17). The
arm switch gates the panel's own servo screens and nothing else; ROS, the console and the
droid's buttons command servos armed or not. Two things drove that. An interlock a remote
client cannot see or clear is a refusal it can only report and wait on — and the same switch
was the only way to make the droid physically safe, so "I want nothing to move" and "I want
this panel to stop accepting presses" were one control with one state between them. Dropping
every output is now its own command, available from every surface, and it works whether or not
the panel is armed. A controller whose interlock is a physical key or an e-stop should still
gate the wire and send `not armed`; the protocol carries it either way.

**Separating configuration from ordinary use is the client's job, and it is structural rather
than a flag.** A raw move *is* the declaration of maintenance intent — that is the entire
difference between the two rows above, and nothing extra need travel on the wire to carry it.
What a well-behaved client owes is to not offer raw mode outside a configuration context: a
distinct page, one step deeper than operation, so that reaching it is deliberate. A remote
controller cannot see which page a panel is on and must not try to; it enforces the interlock and
trusts the mode.

Both modes drive the output immediately and restart the settle timer (§3.4 rule 2). An explicit
enable/disable command is also allowed, and **wins until the next motion command**, at which point
the policy resumes; otherwise a technician who releases a servo to move it by hand would find the
policy fighting them.

**Refusals are part of the interface**, with a reason a UI can display: `not armed`,
`unknown joint`, `uncalibrated`, `out of range`, `not fitted`, `driven elsewhere`,
`release not supported`.

---

## 6. ROS 2 binding

### 6.1 Why not parameters

The obvious answer is the ROS 2 parameter server: calibration is configuration, parameters are
built in, `ros2 param set` is free tooling, and descriptors carry ranges. It was measured against
this project and does not fit:

- **rclc's parameter server costs 6 services and 1 publisher** — seven ROS entities. On this board
  each costs roughly 0.9 KB of *internal* DRAM, the scarce heap, so ~6.3 KB before a single
  parameter exists.
- **rclc parameters are `BOOL`, `INT` or `DOUBLE` only.** No strings, no arrays. `joint`,
  `display`, `actuator_type` and `raw_units` cannot be expressed.
- **It does not scale across servos on one node.** A node here can own 32 (two PCA9685 boards).
  At ~12 fields each that is ~380 parameters, every one carrying its own name buffer, in a
  server whose request/response structures are preallocated from `max_params`.
- Parameters are configuration. Using a parameter write to *move* something, or a parameter read
  to sample a live position, is an abuse that costs a round trip per field and has no ack.

Parameters remain a perfectly good binding for a Linux-class controller, and a node MAY mirror
its configuration into parameters for tooling. They are not the interoperable contract.

### 6.2 The binding

Three interfaces per controller node, **all servo-plural**, because a node owns many servos:

| interface | kind | name (relative to the node) | cost |
| --- | --- | --- | --- |
| Inventory + descriptors | service | `~/servo/list` | 1 service |
| Live state | topic | `~/servo/states` | 1 publisher |
| Configuration write | service | `~/servo/configure` | 1 service |
| Raw move / enable | service | `~/servo/move_raw` | 1 service |

Normal joint motion needs **no new interface**: it is `sensor_msgs/JointState` on the existing
command topic, in radians. That is the whole point of joint space being the public interface.

Entity budget on this board: 3 services + 1 publisher ≈ 3.6 KB internal DRAM, against ~6.3 KB for
a parameter server that could not carry the data anyway.

**One state topic per node, not per servo.** `ServoStateArray` carries every servo the node owns.
Thirty-two topics would be thirty-two publishers, ~29 KB of internal DRAM, and this board has
about 35 KB free in total.

### 6.3 Discovery

Standard ROS 2 discovery, plus one convention: a servo controller is any node offering
`~/servo/list` with type `astromech_msgs/srv/ListServos`.

```sh
ros2 service list -t | grep ListServos
```

The panel enumerates those, calls each, and unions the results keyed by `joint`. A joint claimed
by two controllers is a configuration error and should be reported, not silently resolved.

> **Unverified:** rmw_zenoh_pico's service support is present but this project uses no ROS
> services today. Prove a single round trip before building on it. If services turn out not to
> work, the fallback is a latched descriptor topic plus a command topic, losing the ack.

---

## 7. Custom types

A custom package is needed. Nothing standard describes a servo's configuration:
`sensor_msgs/JointState` is joint space only; `control_msgs/DynamicJointState` can carry arbitrary
named `interface_values` and could express *state*, but it cannot express the descriptor, and its
name/value encoding costs far more bytes than a struct for a document that is mostly fixed fields.
It is worth reconsidering if this ever has to interoperate with stock ros2_control tooling.

Package `astromech_msgs`, versioned with the URDF extension namespace.

### `msg/ServoDescriptor.msg`

```
uint8 ACTUATOR_PWM=0
uint8 ACTUATOR_DYNAMIXEL=1

uint8 UNITS_MICROSECONDS=0
uint8 UNITS_TICKS=1
uint8 UNITS_MILLIDEGREES=2

uint8 DRIVE_HOLD=0
uint8 DRIVE_RELEASE=1

string   joint              # URDF joint name, no _joint suffix; the droid-wide key
string   display            # human label, <= 23 chars

uint8    actuator_type
uint8    raw_units

bool     has_position_feedback
bool     has_temperature
bool     has_voltage
bool     has_load
bool     can_release        # false: drive policy is fixed at HOLD, RELEASE is refused

int32    raw_min            # physical limits; never exceeded
int32    raw_max
int32    raw_closed         # configured endpoint at joint_lower
int32    raw_open           # configured endpoint at joint_upper
int32    raw_step           # smallest increment a UI should offer
bool     calibrated         # false: raw_closed/raw_open are defaults, not measurements

uint8    drive_closed       # DRIVE_*: output state once settled in the CLOSED zone
uint8    drive_open
uint8    drive_mid
uint16   settle_ms          # driven for at least this long after any command
float64  zone_epsilon       # endpoint zone half-width, fraction of travel

float64  joint_lower        # radians, from URDF <limit>
float64  joint_upper
float64  velocity_limit     # 0 = unspecified
float64  effort_limit
```

### `msg/ServoState.msg`

```
uint8 ZONE_CLOSED=0
uint8 ZONE_INTERMEDIATE=1
uint8 ZONE_OPEN=2

string   joint
int32    raw_target             # always present: what was last commanded
int32    raw_position           # measured; valid only if position_is_measured
float64  position               # radians
bool     position_is_measured   # false: derived from raw_target, NOT a measurement
bool     enabled                # output live NOW; false after a policy release
uint8    zone                   # ZONE_*, computed from raw_target
bool     moving
bool     stalled
bool     fault
float64  temperature_c          # per capability; NaN if absent
float64  voltage
float64  load
```

### `msg/ServoStateArray.msg`

```
std_msgs/Header header
ServoState[] servos
```

### `srv/ListServos.srv`

```
---
ServoDescriptor[] servos
```

### `srv/ConfigureServo.srv`

Endpoints and drive policy are separate concerns that happen to share a persistence record, so
the request says which of them it is writing. Without the mask, a client that only wants to change
`settle_ms` has to read-modify-write the endpoints and can race another client into clobbering
them.

```
uint8 APPLY_ENDPOINTS=1
uint8 APPLY_DRIVE=2
uint8 APPLY_CLEAR=4      # forget the calibration entirely; other bits ignored

string  joint
uint8   apply            # bitwise OR of the above

int32   raw_closed       # APPLY_ENDPOINTS
int32   raw_open

uint8   drive_closed     # APPLY_DRIVE
uint8   drive_open
uint8   drive_mid
uint16  settle_ms
float64 zone_epsilon
---
bool    accepted
string  reason           # empty when accepted
ServoDescriptor result   # as stored, after clamping
```

### `srv/MoveServoRaw.srv`

```
uint8 ENABLE_POLICY=0    # leave the drive policy in charge
uint8 ENABLE_ON=1        # drive and hold until the next motion command
uint8 ENABLE_OFF=2       # release now, whatever the policy says

string  joint
int32   raw_value
bool    move             # false: do not move, only apply `enable`
uint8   enable
---
bool    accepted
string  reason
int32   raw_applied      # after clamping; may differ from raw_value
bool    clamped
```

Note `raw_applied` and `clamped`: §4 rule 2. A tool that cannot see it clamped is setting an
endpoint to a number the hardware never reached.

---

## 8. Profiles

**Full** — everything in §3, all four interfaces, feedback where the hardware has it.

**Constrained** (this board) — MAY omit:

- `raw_position` and all optional telemetry (set capability flags false)
- `moving`, `stalled`, `fault` (report false)
- `velocity_limit`, `effort_limit` (report 0)
- the `~/servo/states` topic, if the controller already publishes `sensor_msgs/JointState` and
  nothing needs raw readback or `enabled`

MUST implement: identity, `actuator_type`, `raw_units`, the actuator-space ranges, `calibrated`,
the §3.4 drive policy including `settle_ms`, `raw_target`, `enabled`, both command modes with
refusal reasons, and the §4 mapping including the clamp.

A constrained controller that omits a capability must say so through the flag rather than
reporting a plausible zero. **A false capability flag is honest; a fabricated reading is not.**

---

## 9. Persistence

Configuration — `raw_closed`, `raw_open`, `calibrated`, and the whole of the drive policy — is
owned and persisted by the controller, keyed so it follows the *wiring* rather than the joint. On
this board that is NVS namespace `servo`, key `<address>_<channel>` (`40_00`), so re-plugging a
servo into a different channel does not carry the old configuration with it.

The drive policy belongs with the wiring for the same reason the endpoints do: it is a fact about
the servo and the linkage in front of it, not about the joint's name in the description.

Today that record is `_opMin`, `_opMax`, `_invert` (`Servo::LoadOpLimits()`). This adds two keys:

| key | type | contents |
| --- | --- | --- |
| `<ident>_drive` | u8 | 2 bits per zone: closed, open, mid |
| `<ident>_stlms` | u16 | `settle_ms` |

`zone_epsilon` is not persisted per servo at 1.0 — the default is fine and a per-servo value is
speculation until something needs it. NVS keys cap at 15 characters; `40_00_drive` is 11 and
`40_00_stlms` is 11, against the existing `40_00_opMin` at 11.

A servo with no drive record reads as `HOLD` in all three zones, which is the §3.4 rule 1 default,
so an existing installation is unchanged by the upgrade.

`joint_lower`/`joint_upper`/`display` are **not** persisted — they come from the robot description
on every connect and the description is authoritative.

---

## 10. Open questions

1. **Is `invert` retired?** §3.3 makes direction fall out of `raw_closed > raw_open`. This project
   stores an explicit `invert` today and derives it from the same comparison, so the two already
   agree — but the stored flag should probably go rather than be a second source of truth.
2. **What is a sane default `settle_ms`?** It has to cover the slowest full-travel move on the
   droid, and every `RELEASE` zone is unsafe if it is too short. A measured number per panel type
   would be better than one global guess, and measuring it needs the servos in front of you.
3. **Is `zone_epsilon` per servo or one constant?** §9 assumes a constant. A panel whose closed
   stop is slightly past the URDF's `lower` would want its own.
4. **Does anything need `RELEASE` at `drive_open`?** It is the zone where gravity is least
   forgiving. If nothing on this droid wants it, the field is still right but the UI can warn.
5. **`ServoState` rate.** On change, or fixed-rate? An endpoint editor wants ~20 Hz while dragging
   and nothing at rest. Note `enabled` changes on its own when a settle timer expires, so a
   pure on-change publisher still has to fire there.
6. **Does `astromech_msgs` version with the URDF namespace** (`1.0`), or independently?
7. **Prove services work over rmw_zenoh_pico** before any of §6 is built.
