// love.filesystem core: a read-only source directory overlaid by a writable
// save directory, addressed with LÖVE-style '/' separated relative paths.
#ifndef B360_FILESYSTEM_H
#define B360_FILESYSTEM_H

#include "../platform/platform.h"
#include <string>
#include <vector>

namespace rt {
namespace fs {

void init();

// Normalises a game path ("./a//b/" -> "a/b"). Returns false for paths
// that try to escape the root ("..").
bool normalize(const std::string& in, std::string& out);

// Reads from the save directory first, then the source directory.
bool read(const std::string& path, std::vector<u8>& out);
bool read(const std::string& path, std::string& out);
bool write(const std::string& path, const void* data, size_t size, bool append);
bool remove(const std::string& path);
bool createDirectory(const std::string& path);
bool info(const std::string& path, plat::PathInfo& out);
void list(const std::string& path, std::vector<std::string>& names);

std::string saveDirectory();
std::string sourceDirectory();

}  // namespace fs
}  // namespace rt

#endif
