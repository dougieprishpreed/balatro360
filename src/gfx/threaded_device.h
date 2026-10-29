// Rendering on a thread of its own, with frame interpolation.
//
// The game's thread records each frame, from one present() to the next, into
// a packet: the device calls, with the vertices, pixels and uniform values
// they pass copied. A render thread owns the real device and replays the
// packets, presenting one a vblank at most, so the game's thread only waits
// for the GPU when it's a whole frame ahead.
//
// With setFrameInterpolation(true) (off by default: the console's GPU can't
// draw two frames a game frame, and in-between frames go wrong where draw
// order changes, as when a card is selected), when the game can only keep
// up at 30 Hz (setPresentInterval(2)), the render thread still presents
// every vblank: before each new packet it shows one halfway from the packet
// before. Each draw that matches a draw of that
// packet (same target, texture, shader, blend and vertex count, in order)
// has its vertices' positions and colours averaged, and shaders their
// uniforms named after time. The picture is then a game frame (33 ms)
// behind the game, as interpolation needs the frame after.
//
// Shader creation, init and the other calls that return something wait for
// the render thread.
#ifndef B360_GFX_THREADED_DEVICE_H
#define B360_GFX_THREADED_DEVICE_H

#include "device.h"

namespace gfx {

// Takes real, not initialised yet: its init() and every other call run on the
// render thread, on hardware thread core.
Device* createThreadedDevice(Device* real, int core);

}  // namespace gfx

#endif
