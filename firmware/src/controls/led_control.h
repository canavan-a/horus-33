#pragma once

#include <stdint.h>

#include "../control.h"

namespace led {

enum Mode : uint8_t { OFF = 0, SOLID, BLINK, BREATHE };

// The value the render task consumes. Passed through a 1-deep mailbox queue, so
// the renderer always sees the newest state and never blocks the dispatcher.
struct State {
	Mode mode;
	uint8_t r, g, b;
	uint16_t rate_ms;
	uint8_t brightness;
};

// Creates the mailbox and starts the render task. Call once from setup().
void startRenderTask();

} // namespace led

// LedControl owns the LED's settings. It never touches the strip itself --
// only the render task does -- so the RMT driver has exactly one writer.
class LedControl : public Control {
public:
	const char *id() const override { return "led"; }
	const char *label() const override { return "Status LED"; }

	void describe(JsonObject out) const override;
	bool apply(JsonObjectConst v, char *err, size_t errLen) override;
	void emitState(JsonObject out) const override;

	// Push the current state to the render task.
	void publish() const;

private:
	// Boot default: dim purple, blinking fast.
	led::State state_{led::BLINK, 0x80, 0x00, 0xff, 50, 10};
};
