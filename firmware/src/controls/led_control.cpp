#include "led_control.h"

#include <Adafruit_NeoPixel.h>
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <string.h>

#include "../proto.h"

namespace led {
namespace {

// Colour order confirmed NEO_GRB during bring-up.
//
// The onboard WS2812 sits on GPIO 48 (DevKitC-1 v1.1+) or GPIO 38 (earlier
// revs); the board revision was never isolated, so we drive both. Harmless if
// one is unpopulated. To narrow it down, drop one LED_PIN_* from platformio.ini.
Adafruit_NeoPixel stripA(LED_COUNT, LED_PIN_A, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel stripB(LED_COUNT, LED_PIN_B, NEO_GRB + NEO_KHZ800);

QueueHandle_t mailbox = nullptr;

constexpr TickType_t TICK = pdMS_TO_TICKS(20); // 50 Hz

void paint(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness) {
	stripA.setBrightness(brightness);
	stripB.setBrightness(brightness);
	for (uint16_t i = 0; i < LED_COUNT; i++) {
		stripA.setPixelColor(i, stripA.Color(r, g, b));
		stripB.setPixelColor(i, stripB.Color(r, g, b));
	}
	stripA.show();
	stripB.show();
}

// scaleBreathe maps elapsed time within a period onto a triangular ramp. A
// triangle rather than a sine keeps this integer-only and looks near enough.
uint8_t scaleBreathe(uint32_t elapsed, uint16_t period) {
	if (period == 0) return 255;
	uint32_t half = period / 2;
	if (half == 0) return 255;
	uint32_t phase = elapsed % period;
	uint32_t up = phase < half ? phase : period - phase;
	return (uint8_t)((up * 255) / half);
}

void renderTask(void *) {
	State s{BLINK, 0, 0, 255, 500, 64};

	stripA.begin();
	stripB.begin();

	uint32_t start = millis();
	bool blinkOn = true;
	uint32_t lastToggle = start;

	// Track what we last pushed to the strip so we only drive the RMT peripheral
	// when something actually changed -- a 50 Hz unconditional refresh would be
	// wasted work and adds needless bus traffic.
	int32_t lastPainted = -1;
	auto painted = [](uint8_t r, uint8_t g, uint8_t b, uint8_t br) -> int32_t {
		return ((int32_t)br << 24) | ((int32_t)r << 16) | ((int32_t)g << 8) | b;
	};

	for (;;) {
		State incoming;
		if (mailbox != nullptr && xQueueReceive(mailbox, &incoming, 0) == pdTRUE) {
			s = incoming;
			// Restart the animation clock so a rate change takes effect at once
			// instead of waiting out the current cycle.
			start = millis();
			lastToggle = start;
			blinkOn = true;
		}

		uint32_t now = millis();
		uint8_t r = s.r, g = s.g, b = s.b, br = s.brightness;

		switch (s.mode) {
		case OFF:
			br = 0;
			break;
		case SOLID:
			break;
		case BLINK: {
			uint16_t half = s.rate_ms > 0 ? s.rate_ms : 1;
			if (now - lastToggle >= half) {
				blinkOn = !blinkOn;
				lastToggle = now;
			}
			if (!blinkOn) br = 0;
			break;
		}
		case BREATHE: {
			uint8_t scale = scaleBreathe(now - start, s.rate_ms > 0 ? s.rate_ms : 1);
			br = (uint8_t)((uint16_t)s.brightness * scale / 255);
			break;
		}
		}

		int32_t key = painted(r, g, b, br);
		if (key != lastPainted) {
			paint(r, g, b, br);
			lastPainted = key;
		}
		vTaskDelay(TICK);
	}
}

} // namespace

void startRenderTask() {
	mailbox = xQueueCreate(1, sizeof(State));
	xTaskCreatePinnedToCore(renderTask, "ledRender", 4096, nullptr, 2, nullptr, 1);
}

// publishState hands the newest value to the renderer, replacing any value it
// has not yet consumed.
void publishState(const State &s) {
	if (mailbox != nullptr) {
		xQueueOverwrite(mailbox, &s);
	}
}

const char *modeName(Mode m) {
	switch (m) {
	case OFF: return "off";
	case SOLID: return "solid";
	case BLINK: return "blink";
	case BREATHE: return "breathe";
	}
	return "blink";
}

bool parseMode(const char *s, Mode &out) {
	if (s == nullptr) return false;
	if (strcmp(s, "off") == 0) { out = OFF; return true; }
	if (strcmp(s, "solid") == 0) { out = SOLID; return true; }
	if (strcmp(s, "blink") == 0) { out = BLINK; return true; }
	if (strcmp(s, "breathe") == 0) { out = BREATHE; return true; }
	return false;
}

} // namespace led

namespace {

const char *const MODE_OPTIONS[] = {"off", "solid", "blink", "breathe"};

constexpr double RATE_MIN = 50, RATE_MAX = 5000, RATE_STEP = 50;
constexpr double BRIGHT_MIN = 0, BRIGHT_MAX = 255, BRIGHT_STEP = 5;

double clampd(double v, double lo, double hi) {
	if (v < lo) return lo;
	if (v > hi) return hi;
	return v;
}

} // namespace

void LedControl::describe(JsonObject out) const {
	out["id"] = id();
	out["label"] = label();
	JsonArray fields = out["fields"].to<JsonArray>();

	desc::enumeration(fields, "mode", "Mode", MODE_OPTIONS, 4, "blink");
	desc::color(fields, "color", "Color", "#8000ff");
	desc::number(fields, "rate_ms", "Rate", RATE_MIN, RATE_MAX, RATE_STEP, "ms", 50);
	desc::number(fields, "brightness", "Brightness", BRIGHT_MIN, BRIGHT_MAX,
	             BRIGHT_STEP, "", 10);
}

bool LedControl::apply(JsonObjectConst v, char *err, size_t errLen) {
	// Validate everything into a scratch copy first, so a rejected key cannot
	// leave the control half-updated.
	led::State next = state_;

	for (JsonPairConst kv : v) {
		const char *key = kv.key().c_str();

		if (strcmp(key, "mode") == 0) {
			const char *s = kv.value().as<const char *>();
			if (!led::parseMode(s, next.mode)) {
				snprintf(err, errLen, "bad mode");
				return false;
			}
		} else if (strcmp(key, "color") == 0) {
			const char *s = kv.value().as<const char *>();
			if (!proto::parseColor(s, next.r, next.g, next.b)) {
				snprintf(err, errLen, "bad color, want #rrggbb");
				return false;
			}
		} else if (strcmp(key, "rate_ms") == 0) {
			if (!kv.value().is<double>()) {
				snprintf(err, errLen, "rate_ms must be a number");
				return false;
			}
			next.rate_ms = (uint16_t)clampd(kv.value().as<double>(), RATE_MIN, RATE_MAX);
		} else if (strcmp(key, "brightness") == 0) {
			if (!kv.value().is<double>()) {
				snprintf(err, errLen, "brightness must be a number");
				return false;
			}
			next.brightness =
			    (uint8_t)clampd(kv.value().as<double>(), BRIGHT_MIN, BRIGHT_MAX);
		} else {
			snprintf(err, errLen, "unknown field %.32s", key);
			return false;
		}
	}

	state_ = next;
	publish();
	return true;
}

void LedControl::emitState(JsonObject out) const {
	char hex[8];
	proto::formatColor(state_.r, state_.g, state_.b, hex);

	out["mode"] = led::modeName(state_.mode);
	out["color"] = hex;
	out["rate_ms"] = state_.rate_ms;
	out["brightness"] = state_.brightness;
}

void LedControl::publish() const { led::publishState(state_); }
