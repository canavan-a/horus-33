#include "registry.h"

#include <string.h>

#include "proto.h"

namespace registry {
namespace {

Control *controls[MAX_CONTROLS] = {nullptr};
size_t n = 0;

} // namespace

bool add(Control *c) {
	if (c == nullptr || n >= MAX_CONTROLS) {
		return false;
	}
	controls[n++] = c;
	return true;
}

size_t count() { return n; }

Control *at(size_t i) { return i < n ? controls[i] : nullptr; }

Control *find(const char *id) {
	if (id == nullptr) return nullptr;
	for (size_t i = 0; i < n; i++) {
		if (strcmp(controls[i]->id(), id) == 0) {
			return controls[i];
		}
	}
	return nullptr;
}

void sendDescriptor() {
	JsonDocument doc;
	doc["t"] = proto::T_DESCRIPTOR;
	JsonArray arr = doc["controls"].to<JsonArray>();
	for (size_t i = 0; i < n; i++) {
		controls[i]->describe(arr.add<JsonObject>());
	}
	proto::send(doc);
}

void sendState(Control *c) {
	if (c == nullptr) return;
	JsonDocument doc;
	doc["t"] = proto::T_STATE;
	doc["id"] = c->id();
	c->emitState(doc["v"].to<JsonObject>());
	proto::send(doc);
}

void sendAllStates() {
	for (size_t i = 0; i < n; i++) {
		sendState(controls[i]);
	}
}

} // namespace registry
