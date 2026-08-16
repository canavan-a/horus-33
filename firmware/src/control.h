// A Control is one addressable thing on the device -- the LED today, a motor or
// a sensor tomorrow. Adding one means subclassing this and registering it; no
// host-side change is required so long as its fields use existing types.
#pragma once

#include <ArduinoJson.h>

class Control {
public:
	virtual ~Control() = default;

	// Stable identifier used in set/state messages.
	virtual const char *id() const = 0;

	// Human-readable name for the host UI.
	virtual const char *label() const = 0;

	// Fill `out` with this control's descriptor: {id, label, fields:[...]}.
	// Called from the dispatch task only.
	virtual void describe(JsonObject out) const = 0;

	// Apply a partial update. Only keys present in `v` should be touched.
	// Return false and write a reason into `err` (of size errLen) to reject the
	// whole update; reject before mutating anything so a bad key cannot leave
	// the control half-applied.
	virtual bool apply(JsonObjectConst v, char *err, size_t errLen) = 0;

	// Fill `out` with the complete current value of every field.
	virtual void emitState(JsonObject out) const = 0;
};

// Helpers for building descriptors, keeping the subclasses readable.
namespace desc {

inline JsonObject addField(JsonArray fields, const char *key, const char *type,
                           const char *label) {
	JsonObject f = fields.add<JsonObject>();
	f["key"] = key;
	f["type"] = type;
	f["label"] = label;
	return f;
}

inline void number(JsonArray fields, const char *key, const char *label,
                   double min, double max, double step, const char *unit,
                   double def) {
	JsonObject f = addField(fields, key, "number", label);
	f["min"] = min;
	f["max"] = max;
	f["step"] = step;
	if (unit != nullptr && unit[0] != '\0') {
		f["unit"] = unit;
	}
	f["default"] = def;
}

inline void color(JsonArray fields, const char *key, const char *label,
                  const char *def) {
	JsonObject f = addField(fields, key, "color", label);
	f["default"] = def;
}

inline void enumeration(JsonArray fields, const char *key, const char *label,
                        const char *const *options, size_t count,
                        const char *def) {
	JsonObject f = addField(fields, key, "enum", label);
	JsonArray opts = f["options"].to<JsonArray>();
	for (size_t i = 0; i < count; i++) {
		opts.add(options[i]);
	}
	f["default"] = def;
}

inline void boolean(JsonArray fields, const char *key, const char *label,
                    bool def) {
	JsonObject f = addField(fields, key, "bool", label);
	f["default"] = def;
}

} // namespace desc
