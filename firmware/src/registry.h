#pragma once

#include "control.h"

namespace registry {

// Maximum number of registered controls. Bump when the suite outgrows it.
constexpr size_t MAX_CONTROLS = 8;

// Register a control. The pointer must outlive the program (statics are the
// intended use). Returns false if the table is full.
bool add(Control *c);

size_t count();
Control *at(size_t i);

// Look up by id, or nullptr.
Control *find(const char *id);

// Send the full descriptor message to the host.
void sendDescriptor();

// Send one control's current state.
void sendState(Control *c);

// Send state for every registered control.
void sendAllStates();

} // namespace registry
