#include "motion_control.h"

#include <Arduino.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <math.h>
#include <string.h>

namespace motion {
namespace {

struct Pins {
	uint8_t en;   // active low
	uint8_t step; // driven by LEDC, not by software toggling
	uint8_t dir;
	uint8_t uart_tx; // reserved: the TMC2209 config link is not used yet
	uint8_t ledc_ch;
};

// LEDC channels are paired to timers (timer = channel / 2), and channels
// sharing a timer are forced to share a frequency. Channels 0 and 2 land on
// different timers, so the two axes can run at independent speeds.
const Pins PINS[AXIS_COUNT] = {
    {AXIS_X_EN, AXIS_X_STEP, AXIS_X_DIR, AXIS_X_TX, 0},
    {AXIS_Y_EN, AXIS_Y_STEP, AXIS_Y_DIR, AXIS_Y_TX, 2},
};

// Duty resolution is chosen per frequency by dutyBitsFor(); these bound it.
// LEDC_SRC_HZ is the APB clock the peripheral divides down.
constexpr uint32_t LEDC_SRC_HZ = 80000000;
constexpr uint8_t LEDC_MAX_BITS = 14;
constexpr uint8_t LEDC_BITS = 8; // only for the idle setup before a rate is known

constexpr TickType_t TICK = pdMS_TO_TICKS(10); // 100 Hz

// The TMC2209 wants DIR settled before the next STEP edge; nanoseconds are
// required, microseconds are free here.
constexpr uint32_t DIR_SETUP_US = 20;

// Below this the LEDC timer struggles to synthesise a clean rate and the motor
// would barely creep, so a smaller command is treated as "stop".
constexpr float MIN_RUN_SPS = 10.0f;

// How fast the commanded rate may change, in steps/s per second. Low enough
// that the rotor keeps up with the field (an instant jump just buzzes), high
// enough that the PID still feels responsive: a full-scale swing takes ~0.3 s.
constexpr float ACCEL_SPS2 = 12000.0f;

// How close to `home` counts as home. Without an encoder the position is
// dead-reckoned, so insisting on an exact landing would hunt forever.
constexpr int32_t HOME_TOL_STEPS = 4;

// Proportional gain for the return-home move, in steps/s per step of error.
// Gives a gentle taper into the target instead of a hard stop.
constexpr float HOME_KP = 3.0f;

QueueHandle_t mailbox = nullptr;
QueueHandle_t targetBox = nullptr;

Snapshot state{MANUAL,
               false,
               1000,  // lost_ms
               0.30f, // min_conf
               0.02f, // deadband
               800,   // home_sps
               {
                   // enable run dir invert speed  kp ki kd  max  home pos_set epoch
                   {false, false, FWD, false, 400, 1200.0f, 0.0f, 0.0f, 4000, 0, 0, 0},
                   {false, false, FWD, false, 400, 1200.0f, 0.0f, 0.0f, 4000, 0, 0, 0},
               }};

// Published by the motion task for the dispatch task to read back.
std::atomic<int32_t> posSteps[AXIS_COUNT] = {};

// What the motion task last wrote to the hardware, so we only touch a pin when
// its value actually changes.
struct Applied {
	bool enabled;
	bool dirLevel;
	uint32_t freq; // 0 = pulses stopped
};

struct Pid {
	float integ;
	float prevErr;
	bool primed;

	void reset() {
		integ = 0.0f;
		prevErr = 0.0f;
		primed = false;
	}
};

float clampf(float v, float lo, float hi) {
	if (v < lo) return lo;
	if (v > hi) return hi;
	return v;
}

// step returns the signed steps/s the PID wants for one axis. Sign is the
// travel direction; magnitude is the rate.
float pidStep(Pid &pid, const AxisState &a, float err, float deadband, float dt) {
	if (fabsf(err) <= deadband) {
		// Centred. Bleed the integrator rather than holding a standing bias that
		// would kick the axis the moment the target drifts back into range.
		pid.integ = 0.0f;
		pid.prevErr = err;
		pid.primed = true;
		return 0.0f;
	}

	float maxOut = (float)a.max_sps;
	float deriv = pid.primed && dt > 0.0f ? (err - pid.prevErr) / dt : 0.0f;
	pid.prevErr = err;
	pid.primed = true;

	// Conditional integration: only accumulate while the output is not already
	// saturated, which is the cheap and reliable anti-windup.
	float unsat = a.kp * err + a.ki * pid.integ + a.kd * deriv;
	if (a.ki > 0.0f && fabsf(unsat) < maxOut) {
		pid.integ += err * dt;
		float integCap = maxOut / a.ki;
		pid.integ = clampf(pid.integ, -integCap, integCap);
	}

	float out = a.kp * err + a.ki * pid.integ + a.kd * deriv;
	return clampf(out, -maxOut, maxOut);
}

// homeStep returns the signed steps/s that walks an axis back to its home
// position. A plain P move: no integral is wanted on an open-loop seek.
float homeStep(const AxisState &a, int32_t pos, uint16_t home_sps) {
	int32_t err = a.home - pos;
	if (err > -HOME_TOL_STEPS && err < HOME_TOL_STEPS) return 0.0f;
	float out = HOME_KP * (float)err;
	return clampf(out, -(float)home_sps, (float)home_sps);
}

void stopPulses(const Pins &p) { ledcWrite(p.ledc_ch, 0); }

// LEDC derives the step frequency by dividing an 80 MHz clock by
// (2^bits * freq), and the divider is finite — so a fixed duty resolution puts
// a floor under the frequency. At 8 bits that floor is ~305 Hz, which silently
// swallowed every jog below 305 sps and, worse, made the PID output collapse to
// a dead stop instead of a slow crawl near the target. Picking the resolution
// from the frequency removes the floor: low rates get more duty bits (which
// they can afford), high rates get fewer.
uint8_t dutyBitsFor(uint32_t freq) {
	uint8_t bits = LEDC_MAX_BITS;
	while (bits > 1 && ((uint32_t)1 << bits) > (LEDC_SRC_HZ / freq)) bits--;
	return bits;
}

// Returns the frequency actually programmed, or 0 if LEDC refused it — in which
// case the axis stays stopped rather than silently running at some other speed.
uint32_t startPulses(const Pins &p, uint32_t freq) {
	if (freq == 0) {
		stopPulses(p);
		return 0;
	}
	const uint8_t bits = dutyBitsFor(freq);
	if (ledcSetup(p.ledc_ch, freq, bits) == 0) {
		stopPulses(p);
		return 0;
	}
	ledcWrite(p.ledc_ch, 1u << (bits - 1)); // 50% at this resolution
	return freq;
}

void motionTask(void *) {
	Snapshot s = state;
	Applied applied[AXIS_COUNT];
	Pid pid[AXIS_COUNT];
	// Fractional step accumulator. The commanded rate is exact, so integrating
	// it recovers the position to well under a step over a long session.
	float posAccum[AXIS_COUNT];
	uint16_t posEpoch[AXIS_COUNT];
	// Rate currently applied to the hardware, which the slew limiter walks
	// towards the commanded rate rather than jumping.
	float rateNow[AXIS_COUNT];

	for (size_t i = 0; i < AXIS_COUNT; i++) {
		const Pins &p = PINS[i];
		// Park everything in the safe state before enabling the outputs, so a
		// reset can never energise a motor mid-move.
		digitalWrite(p.en, HIGH); // active low: HIGH = driver off
		digitalWrite(p.dir, LOW);
		pinMode(p.en, OUTPUT);
		pinMode(p.dir, OUTPUT);
		digitalWrite(p.en, HIGH);
		digitalWrite(p.dir, LOW);

		ledcSetup(p.ledc_ch, 1000, LEDC_BITS);
		ledcAttachPin(p.step, p.ledc_ch);
		ledcWrite(p.ledc_ch, 0);

		applied[i] = {false, false, 0};
		pid[i].reset();
		posAccum[i] = 0.0f;
		rateNow[i] = 0.0f;
		posEpoch[i] = s.axis[i].pos_epoch;
	}

	Target target{0, 0, 0, 0, 0, false};
	uint32_t lastSeenMs = 0;
	bool everSeen = false;
	uint32_t lastUs = micros();

	for (;;) {
		Snapshot incoming;
		if (mailbox != nullptr && xQueueReceive(mailbox, &incoming, 0) == pdTRUE) {
			s = incoming;
		}

		Target t;
		if (targetBox != nullptr && xQueueReceive(targetBox, &t, 0) == pdTRUE) {
			if (t.valid && t.conf >= s.min_conf) {
				target = t;
				lastSeenMs = millis();
				everSeen = true;
			} else {
				// An explicit "no detection" retires the target at once instead of
				// steering towards a box that is no longer on screen.
				target.valid = false;
			}
		}

		uint32_t nowUs = micros();
		float dt = (float)(uint32_t)(nowUs - lastUs) / 1e6f;
		lastUs = nowUs;
		if (dt <= 0.0f || dt > 0.5f) dt = 0.01f; // first pass or a long stall

		bool fresh = everSeen && target.valid &&
		             (millis() - lastSeenMs) < (uint32_t)s.lost_ms;

		for (size_t i = 0; i < AXIS_COUNT; i++) {
			const Pins &p = PINS[i];
			const AxisState &a = s.axis[i];
			Applied &cur = applied[i];

			// A position write from the host lands once, on the edge.
			if (a.pos_epoch != posEpoch[i]) {
				posEpoch[i] = a.pos_epoch;
				posSteps[i].store(a.pos_set, std::memory_order_relaxed);
				posAccum[i] = 0.0f;
			}

			bool enabled = a.enable && !s.estop;

			// Signed rate request. Positive is FWD before polarity is applied.
			float cmd = 0.0f;
			if (enabled) {
				if (s.estop) {
					cmd = 0.0f;
				} else if (s.mode == MANUAL) {
					pid[i].reset();
					cmd = a.run ? (a.dir == REV ? -(float)a.speed : (float)a.speed) : 0.0f;
				} else if (fresh) {
					float err = (i == AXIS_X) ? target.x : target.y;
					cmd = pidStep(pid[i], a, err, s.deadband, dt);
				} else {
					// Lost, or never seen: park on the configured home position.
					pid[i].reset();
					cmd = homeStep(a, posSteps[i].load(std::memory_order_relaxed),
					               s.home_sps);
				}
			} else {
				pid[i].reset();
			}

			// Slew-limit the rate. A stepper asked to jump straight from one
			// frequency to another loses sync with the field and buzzes instead of
			// turning, so every change — a manual speed edit, a PID swing, a
			// direction reversal — is ramped at ACCEL_SPS2. Crossing zero ramps
			// down to a stop first, which is what makes the DIR flip below safe.
			if (!enabled) {
				// A de-energised axis has no momentum to respect; drop the ramp so
				// re-enabling starts from a standstill rather than mid-profile.
				rateNow[i] = 0.0f;
				cmd = 0.0f;
			} else {
				float step = ACCEL_SPS2 * dt;
				float prev = rateNow[i];
				float goal = cmd;
				if ((prev > 0.0f && goal < 0.0f) || (prev < 0.0f && goal > 0.0f)) goal = 0.0f;
				if (goal > prev + step) goal = prev + step;
				else if (goal < prev - step) goal = prev - step;
				rateNow[i] = goal;
				cmd = goal;
			}

			if (fabsf(cmd) < MIN_RUN_SPS) cmd = 0.0f;

			bool wantRev = cmd < 0.0f;
			bool dirLevel = wantRev != a.invert_dir;
			uint32_t want = (uint32_t)fabsf(cmd);

			if (cur.enabled != enabled) {
				digitalWrite(p.en, enabled ? LOW : HIGH);
				cur.enabled = enabled;
			}

			if (want != 0 && cur.dirLevel != dirLevel) {
				// Never glitch STEP across a direction change: stop, settle the
				// DIR line, then let the restart below bring pulses back.
				if (cur.freq != 0) {
					stopPulses(p);
					cur.freq = 0;
				}
				digitalWrite(p.dir, dirLevel ? HIGH : LOW);
				cur.dirLevel = dirLevel;
				delayMicroseconds(DIR_SETUP_US);
			}

			if (cur.freq != want) {
				cur.freq = startPulses(p, want);
			}

			// Dead-reckon the position from what the hardware is actually doing,
			// so a rate LEDC refused to program does not show up as phantom
			// travel. Direction here is the pre-polarity request: `pos` counts in
			// command space, which is what `home` is expressed in.
			if (cur.freq != 0) {
				posAccum[i] += (wantRev ? -1.0f : 1.0f) * (float)cur.freq * dt;
				float whole = truncf(posAccum[i]);
				if (whole != 0.0f) {
					posAccum[i] -= whole;
					posSteps[i].fetch_add((int32_t)whole, std::memory_order_relaxed);
				}
			}
		}

		vTaskDelay(TICK);
	}
}

} // namespace

Snapshot &shared() { return state; }

void publish() {
	if (mailbox != nullptr) {
		xQueueOverwrite(mailbox, &state);
	}
}

void publishTarget(const Target &t) {
	if (targetBox != nullptr) {
		xQueueOverwrite(targetBox, &t);
	}
}

int32_t position(size_t index) {
	if (index >= AXIS_COUNT) return 0;
	return posSteps[index].load(std::memory_order_relaxed);
}

void startMotionTask() {
	mailbox = xQueueCreate(1, sizeof(Snapshot));
	targetBox = xQueueCreate(1, sizeof(Target));
	xTaskCreatePinnedToCore(motionTask, "motion", 4096, nullptr, 3, nullptr, 1);
}

const char *modeName(Mode m) {
	switch (m) {
	case MANUAL: return "manual";
	case PID: return "pid";
	}
	return "manual";
}

bool parseMode(const char *s, Mode &out) {
	if (s == nullptr) return false;
	if (strcmp(s, "manual") == 0) { out = MANUAL; return true; }
	if (strcmp(s, "pid") == 0) { out = PID; return true; }
	return false;
}

const char *dirName(Dir d) {
	switch (d) {
	case FWD: return "fwd";
	case REV: return "rev";
	}
	return "fwd";
}

bool parseDir(const char *s, Dir &out) {
	if (s == nullptr) return false;
	if (strcmp(s, "fwd") == 0) { out = FWD; return true; }
	if (strcmp(s, "rev") == 0) { out = REV; return true; }
	return false;
}

} // namespace motion

namespace {

const char *const MODE_OPTIONS[] = {"manual", "pid"};
const char *const DIR_OPTIONS[] = {"fwd", "rev"};

constexpr double SPEED_MIN = 0, SPEED_MAX = 20000, SPEED_STEP = 50;
constexpr double GAIN_MIN = 0, GAIN_MAX = 20000, GAIN_STEP = 25;
constexpr double POS_MIN = -1000000, POS_MAX = 1000000, POS_STEP = 10;
constexpr double LOST_MIN = 100, LOST_MAX = 60000, LOST_STEP = 100;
constexpr double UNIT_MIN = 0, UNIT_MAX = 1, UNIT_STEP = 0.05;
constexpr double BAND_MIN = 0, BAND_MAX = 0.5, BAND_STEP = 0.01;

double clampd(double v, double lo, double hi) {
	if (v < lo) return lo;
	if (v > hi) return hi;
	return v;
}

// numField pulls one clamped number out of a set, reporting a type mismatch the
// same way everywhere. Returns false if the caller should reject the update.
bool numField(JsonVariantConst v, const char *key, double lo, double hi,
              double &out, char *err, size_t errLen) {
	if (!v.is<double>()) {
		snprintf(err, errLen, "%.24s must be a number", key);
		return false;
	}
	out = clampd(v.as<double>(), lo, hi);
	return true;
}

bool boolField(JsonVariantConst v, const char *key, bool &out, char *err,
               size_t errLen) {
	if (!v.is<bool>()) {
		snprintf(err, errLen, "%.24s must be a bool", key);
		return false;
	}
	out = v.as<bool>();
	return true;
}

} // namespace

void MotionControl::describe(JsonObject out) const {
	out["id"] = id();
	out["label"] = label();
	JsonArray fields = out["fields"].to<JsonArray>();

	desc::enumeration(fields, "mode", "Mode", MODE_OPTIONS, 2, "manual");
	desc::boolean(fields, "estop", "E-stop", false);
	desc::number(fields, "lost_ms", "Lost timeout", LOST_MIN, LOST_MAX, LOST_STEP,
	             "ms", 1000);
	desc::number(fields, "min_conf", "Min confidence", UNIT_MIN, UNIT_MAX,
	             UNIT_STEP, "", 0.30);
	desc::number(fields, "deadband", "Deadband", BAND_MIN, BAND_MAX, BAND_STEP, "",
	             0.02);
	desc::number(fields, "home_sps", "Home speed", SPEED_MIN, SPEED_MAX,
	             SPEED_STEP, "sps", 800);
}

bool MotionControl::apply(JsonObjectConst v, char *err, size_t errLen) {
	motion::Snapshot next = motion::shared();
	double n = 0;

	for (JsonPairConst kv : v) {
		const char *key = kv.key().c_str();

		if (strcmp(key, "mode") == 0) {
			if (!motion::parseMode(kv.value().as<const char *>(), next.mode)) {
				snprintf(err, errLen, "bad mode");
				return false;
			}
		} else if (strcmp(key, "estop") == 0) {
			if (!boolField(kv.value(), key, next.estop, err, errLen)) return false;
		} else if (strcmp(key, "lost_ms") == 0) {
			if (!numField(kv.value(), key, LOST_MIN, LOST_MAX, n, err, errLen))
				return false;
			next.lost_ms = (uint16_t)n;
		} else if (strcmp(key, "min_conf") == 0) {
			if (!numField(kv.value(), key, UNIT_MIN, UNIT_MAX, n, err, errLen))
				return false;
			next.min_conf = (float)n;
		} else if (strcmp(key, "deadband") == 0) {
			if (!numField(kv.value(), key, BAND_MIN, BAND_MAX, n, err, errLen))
				return false;
			next.deadband = (float)n;
		} else if (strcmp(key, "home_sps") == 0) {
			if (!numField(kv.value(), key, SPEED_MIN, SPEED_MAX, n, err, errLen))
				return false;
			next.home_sps = (uint16_t)n;
		} else {
			snprintf(err, errLen, "unknown field %.32s", key);
			return false;
		}
	}

	motion::shared() = next;
	motion::publish();
	return true;
}

void MotionControl::emitState(JsonObject out) const {
	const motion::Snapshot &s = motion::shared();
	out["mode"] = motion::modeName(s.mode);
	out["estop"] = s.estop;
	out["lost_ms"] = s.lost_ms;
	out["min_conf"] = s.min_conf;
	out["deadband"] = s.deadband;
	out["home_sps"] = s.home_sps;
}

void AxisControl::describe(JsonObject out) const {
	out["id"] = id();
	out["label"] = label();
	JsonArray fields = out["fields"].to<JsonArray>();

	desc::boolean(fields, "enable", "Enable", false);
	desc::boolean(fields, "run", "Run", false);
	desc::enumeration(fields, "dir", "Direction", DIR_OPTIONS, 2, "fwd");
	desc::boolean(fields, "invert_dir", "Invert polarity", false);
	desc::number(fields, "speed", "Jog speed", SPEED_MIN, SPEED_MAX, SPEED_STEP,
	             "sps", 400);
	desc::number(fields, "kp", "Kp", GAIN_MIN, GAIN_MAX, GAIN_STEP, "sps", 1200);
	desc::number(fields, "ki", "Ki", GAIN_MIN, GAIN_MAX, GAIN_STEP, "", 0);
	desc::number(fields, "kd", "Kd", GAIN_MIN, GAIN_MAX, GAIN_STEP, "", 0);
	desc::number(fields, "max_sps", "PID max speed", SPEED_MIN, SPEED_MAX,
	             SPEED_STEP, "sps", 4000);
	desc::number(fields, "home", "Home", POS_MIN, POS_MAX, POS_STEP, "st", 0);
	desc::number(fields, "pos", "Position", POS_MIN, POS_MAX, POS_STEP, "st", 0);
}

bool AxisControl::apply(JsonObjectConst v, char *err, size_t errLen) {
	motion::Snapshot next = motion::shared();
	motion::AxisState &a = next.axis[index_];
	double n = 0;

	for (JsonPairConst kv : v) {
		const char *key = kv.key().c_str();

		if (strcmp(key, "enable") == 0) {
			if (!boolField(kv.value(), key, a.enable, err, errLen)) return false;
		} else if (strcmp(key, "run") == 0) {
			if (!boolField(kv.value(), key, a.run, err, errLen)) return false;
		} else if (strcmp(key, "invert_dir") == 0) {
			if (!boolField(kv.value(), key, a.invert_dir, err, errLen)) return false;
		} else if (strcmp(key, "dir") == 0) {
			if (!motion::parseDir(kv.value().as<const char *>(), a.dir)) {
				snprintf(err, errLen, "bad dir");
				return false;
			}
		} else if (strcmp(key, "speed") == 0) {
			if (!numField(kv.value(), key, SPEED_MIN, SPEED_MAX, n, err, errLen))
				return false;
			a.speed = (uint16_t)n;
		} else if (strcmp(key, "kp") == 0) {
			if (!numField(kv.value(), key, GAIN_MIN, GAIN_MAX, n, err, errLen))
				return false;
			a.kp = (float)n;
		} else if (strcmp(key, "ki") == 0) {
			if (!numField(kv.value(), key, GAIN_MIN, GAIN_MAX, n, err, errLen))
				return false;
			a.ki = (float)n;
		} else if (strcmp(key, "kd") == 0) {
			if (!numField(kv.value(), key, GAIN_MIN, GAIN_MAX, n, err, errLen))
				return false;
			a.kd = (float)n;
		} else if (strcmp(key, "max_sps") == 0) {
			if (!numField(kv.value(), key, SPEED_MIN, SPEED_MAX, n, err, errLen))
				return false;
			a.max_sps = (uint16_t)n;
		} else if (strcmp(key, "home") == 0) {
			if (!numField(kv.value(), key, POS_MIN, POS_MAX, n, err, errLen))
				return false;
			a.home = (int32_t)n;
		} else if (strcmp(key, "pos") == 0) {
			// Writing `pos` redefines the datum -- the only way to tell an
			// open-loop axis where it actually is.
			if (!numField(kv.value(), key, POS_MIN, POS_MAX, n, err, errLen))
				return false;
			a.pos_set = (int32_t)n;
			a.pos_epoch++;
		} else {
			snprintf(err, errLen, "unknown field %.32s", key);
			return false;
		}
	}

	motion::shared() = next;
	motion::publish();
	return true;
}

void AxisControl::emitState(JsonObject out) const {
	const motion::AxisState &a = motion::shared().axis[index_];
	out["enable"] = a.enable;
	out["run"] = a.run;
	out["dir"] = motion::dirName(a.dir);
	out["invert_dir"] = a.invert_dir;
	out["speed"] = a.speed;
	out["kp"] = a.kp;
	out["ki"] = a.ki;
	out["kd"] = a.kd;
	out["max_sps"] = a.max_sps;
	out["home"] = a.home;
	out["pos"] = motion::position(index_);
}
