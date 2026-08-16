// horus-33 device firmware.
//
// Structure: a serial reader task turns incoming lines into commands, a single
// dispatch task applies them to registered controls, and each control's
// hardware is owned by exactly one render task. Adding a control means writing
// a Control subclass and registering it in setup() -- the host discovers it
// automatically via the descriptor.

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "controls/led_control.h"
#include "controls/motion_control.h"
#include "proto.h"
#include "registry.h"

#ifndef FW_VERSION
#define FW_VERSION "dev"
#endif

namespace {

// One inbound line. Fixed-size so the queue needs no heap allocation and cannot
// fragment memory over a long session.
struct Line {
	char text[proto::MAX_LINE];
};

QueueHandle_t cmdQueue = nullptr;

LedControl ledControl;
MotionControl motionControl;
AxisControl axisX{motion::AXIS_X, "axis_x", "X Axis"};
AxisControl axisY{motion::AXIS_Y, "axis_y", "Y Axis"};

// rxTask: read bytes, split on newlines, hand whole lines to the dispatcher.
// Deliberately does no parsing and touches no hardware, so a malformed flood
// cannot stall anything else.
void rxTask(void *) {
	static char buf[proto::MAX_LINE];
	size_t len = 0;
	bool overflowed = false;

	for (;;) {
		while (Serial.available() > 0) {
			int c = Serial.read();
			if (c < 0) break;

			if (c == '\n' || c == '\r') {
				if (len > 0 && !overflowed) {
					Line line;
					memcpy(line.text, buf, len);
					line.text[len] = '\0';
					// Drop rather than block: the host can retry, and stalling
					// the reader would back up the serial buffer.
					xQueueSend(cmdQueue, &line, 0);
				}
				len = 0;
				overflowed = false;
				continue;
			}

			if (len + 1 >= sizeof(buf)) {
				// Mark and keep consuming so we resynchronise at the next
				// newline instead of splitting one long line into two.
				overflowed = true;
				continue;
			}
			buf[len++] = (char)c;
		}
		vTaskDelay(pdMS_TO_TICKS(5));
	}
}

// Frame coordinates are normalised to [-1, 1] with the centre at zero; anything
// outside that is a host bug, so clamp rather than steer off into nowhere.
float clampUnit(double v) {
	if (v < -1.0) return -1.0f;
	if (v > 1.0) return 1.0f;
	return (float)v;
}

void handleLine(const char *text) {
	JsonDocument doc;
	DeserializationError parseErr = deserializeJson(doc, text);
	if (parseErr) {
		proto::sendErr(0, parseErr.c_str());
		return;
	}

	const char *type = doc["t"] | "";
	uint32_t seq = doc["seq"] | 0;

	// Checked first: this is the only high-rate message, arriving once per
	// inference frame. It is answered with nothing at all -- acking a 60 Hz
	// stream would double the traffic for no benefit. Passing a `seq` opts into
	// an ack so the stream can still be hand-tested from a serial monitor.
	if (strcmp(type, proto::T_TRACK) == 0) {
		motion::Target t{0, 0, 0, 0, 0, false};
		bool lost = doc["lost"] | false;
		if (!lost && doc["x"].is<double>() && doc["y"].is<double>()) {
			t.x = clampUnit(doc["x"].as<double>());
			t.y = clampUnit(doc["y"].as<double>());
			t.w = (float)(doc["w"] | 0.0);
			t.h = (float)(doc["h"] | 0.0);
			// An absent confidence means "the producer does not score its boxes",
			// which must not be read as zero confidence and silently gated out.
			t.conf = (float)(doc["c"] | 1.0);
			t.valid = true;
		}
		motion::publishTarget(t);
		if (seq != 0) {
			proto::sendAck(seq);
		}
		return;
	}

	if (strcmp(type, proto::T_DESCRIBE) == 0) {
		registry::sendDescriptor();
		proto::sendAck(seq);
		registry::sendAllStates();
		return;
	}

	if (strcmp(type, proto::T_PING) == 0) {
		proto::sendAck(seq);
		return;
	}

	if (strcmp(type, proto::T_SET) == 0) {
		const char *id = doc["id"] | "";
		Control *c = registry::find(id);
		if (c == nullptr) {
			proto::sendErr(seq, "unknown control");
			return;
		}
		JsonObjectConst v = doc["v"].as<JsonObjectConst>();
		if (v.isNull()) {
			proto::sendErr(seq, "set requires an object 'v'");
			return;
		}

		char err[64] = {0};
		if (!c->apply(v, err, sizeof(err))) {
			proto::sendErr(seq, err[0] ? err : "rejected");
			return;
		}
		proto::sendAck(seq);
		registry::sendState(c);
		return;
	}

	proto::sendErr(seq, "unknown message type");
}

// dispatchTask: the only task that touches control state, so controls need no
// locking against each other.
void dispatchTask(void *) {
	Line line;
	for (;;) {
		if (xQueueReceive(cmdQueue, &line, portMAX_DELAY) == pdTRUE) {
			handleLine(line.text);
		}
	}
}

} // namespace

void setup() {
	proto::begin(115200);

	registry::add(&ledControl);
	registry::add(&motionControl);
	registry::add(&axisX);
	registry::add(&axisY);

	led::startRenderTask();
	ledControl.publish(); // seed the renderer with the boot defaults

	motion::startMotionTask();
	motion::publish(); // seed the motion task with the boot defaults

	cmdQueue = xQueueCreate(8, sizeof(Line));

	xTaskCreatePinnedToCore(rxTask, "rx", 4096, nullptr, 3, nullptr, 1);
	xTaskCreatePinnedToCore(dispatchTask, "dispatch", 8192, nullptr, 2, nullptr, 1);

	// Give the USB CDC port a moment to enumerate so the host does not miss the
	// hello. The host does not depend on catching it, but it makes manual
	// testing in a serial monitor far less confusing.
	delay(1500);
	proto::sendHello("horus-33", FW_VERSION);
}

void loop() {
	// All work happens in tasks.
	vTaskDelay(pdMS_TO_TICKS(1000));
}
