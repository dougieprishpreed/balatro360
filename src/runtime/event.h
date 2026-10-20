// Global event queue (love.event). Thread safe.
#ifndef B360_EVENT_H
#define B360_EVENT_H

#include "variant.h"
#include <string>
#include <vector>

namespace rt {
namespace event {

void push(const std::string& name, const std::vector<Variant>& args);
bool pop(std::string& name, std::vector<Variant>& args);
void clear();

// Poll the platform for input and turn it into events.
void pumpPlatform();

}  // namespace event

namespace joystick {
// Generates joystick/gamepad events by diffing the controller state.
void update();
}

namespace input {
void update();   // keyboard events (host testing)
}

}  // namespace rt

#endif
