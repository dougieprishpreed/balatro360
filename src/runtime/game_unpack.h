// First start: the player's own Balatro.exe (or the Mac version's
// Balatro.love), copied beside the executable, is unpacked into the game
// folder (plat::sourceRoot()), then deleted. Balatro for PC is a LÖVE game:
// Balatro.exe is LÖVE with the game's files appended as a zip archive.
#ifndef B360_GAME_UNPACK_H
#define B360_GAME_UNPACK_H

#include <string>

namespace rt {
namespace unpack {

enum Result {
    NOTHING,   // no Balatro.exe or Balatro.love beside the executable
    WORKING,   // call step() again
    DONE,      // unpacked, and the source file deleted
    FAILED     // error says why; the source file is kept
};

// Unpacks files for about `seconds`, at least one, so the caller can draw a
// loading bar in between. progress goes from 0 to 1.
Result step(double seconds, double& progress, std::string& error);

}  // namespace unpack
}  // namespace rt

#endif
