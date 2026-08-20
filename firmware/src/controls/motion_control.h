#pragma once

#include <stdint.h>

#include "../control.h"

namespace motion {

// Which loop owns the step rate. The switch is global: it governs every axis at
// once, so the machine can never be half manual and half servoed. HOME is a
// third loop, not a PID sub-state: it always seeks `home` regardless of
// whether a target is currently being tracked, so a "go home" request works
// the same whether the machine was idle, jogging, or actively centring on
// something.
enum Mode : uint8_t { MANUAL = 0, PID, HOME };

enum Dir : uint8_t { FWD = 0, REV };

constexpr size_t AXIS_COUNT = 2;
constexpr size_t AXIS_X = 0;
constexpr size_t AXIS_Y = 1;

// A detection from the host, already normalised: (0,0) is the centre of the
// frame, +x is right of centre and +y is *above* centre. The host does the
// pixel maths and the y flip, so the device never needs to know the frame size
// or which model produced the box.
struct Target {
	float x, y; // [-1, 1]
	float w, h; // [0, 1] box size; recorded, not yet used by the loop
	float conf; // [0, 1]
	bool valid; // false = the host explicitly reported no detection
};

struct AxisState {
	bool enable;     // energise the driver (EN is active low on the TMC2209)
	bool run;        // manual jog: step continuously at `speed`
	Dir dir;         // requested travel direction (manual mode only)
	bool invert_dir; // software polarity; applies to manual and PID alike
	uint16_t speed;  // manual jog rate, steps/s
	bool auto_deenergize; // in PID mode, drop EN once idle at home; opt-in per
	                      // axis since a gravity-loaded axis loses holding
	                      // torque the instant it de-energises

	float kp, ki, kd;   // PID gains, output in steps/s per unit of frame error
	uint16_t max_sps;   // clamp on the PID output
	int32_t home;       // position to return to when the target is lost, in steps
	int32_t pos_set;    // requested new value for the position counter
	uint16_t pos_epoch; // bumped on each pos_set so the task acts on it once
};

// The whole motion picture in one value. Mode, estop and both axes travel
// together through a single 1-deep mailbox so the task can never act on a new
// mode with a stale axis, or vice versa.
struct Snapshot {
	Mode mode;
	bool estop;
	uint16_t lost_ms;  // silence after which the axes return to `home`
	float min_conf;    // detections below this are ignored
	float deadband;    // |error| under this counts as centred
	uint16_t home_sps; // speed cap while returning home
	AxisState axis[AXIS_COUNT];
};

// The dispatch task is the only writer (see the comment on dispatchTask in
// main.cpp), so these need no locking; publish() hands a copy to the motion
// task.
Snapshot &shared();
void publish();

// publishTarget is the hot path: called from the dispatch task for every
// inbound `track` line and consumed by the motion task. Newest wins; a frame
// the loop never got round to reading is simply overwritten.
void publishTarget(const Target &t);

// Current dead-reckoned position of an axis, in steps. Written by the motion
// task, readable from anywhere.
int32_t position(size_t index);

// Creates the mailboxes, parks the driver pins safely and starts the motion
// task. Call once from setup().
void startMotionTask();

const char *modeName(Mode m);
bool parseMode(const char *s, Mode &out);
const char *dirName(Dir d);
bool parseDir(const char *s, Dir &out);

} // namespace motion

// MotionControl owns the global mode switch, the emergency stop and the
// settings shared by both loops. It holds no per-axis settings of its own.
class MotionControl : public Control {
public:
	const char *id() const override { return "motion"; }
	const char *label() const override { return "Motion"; }

	void describe(JsonObject out) const override;
	bool apply(JsonObjectConst v, char *err, size_t errLen) override;
	void emitState(JsonObject out) const override;
};

// AxisControl is one stepper. Two instances exist, distinguished only by their
// index into the shared snapshot; the pin sets live in motion_control.cpp so no
// other translation unit can reach the hardware.
class AxisControl : public Control {
public:
	AxisControl(size_t index, const char *id, const char *label)
	    : index_(index), id_(id), label_(label) {}

	const char *id() const override { return id_; }
	const char *label() const override { return label_; }

	void describe(JsonObject out) const override;
	bool apply(JsonObjectConst v, char *err, size_t errLen) override;
	void emitState(JsonObject out) const override;

private:
	size_t index_;
	const char *id_;
	const char *label_;
};
