#pragma once
// File override layer: the game's data stays untouched (original/ is read-only). When the game
// opens a data file that has a registered transform, the layer writes the transformed copy once
// into <sandbox>/overrides/ and the game reads that copy instead. Used for text changes
// (string tables), switchable from settings.
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace hle::overrides {

// Returns true if it changed the data.
using Transform = std::function<bool(std::vector<uint8_t>& data)>;

void set_dir(const std::filesystem::path& dir);
// file_name: the data file's name, case-insensitive, e.g. "init.english". Several transforms per
// file run in registration order.
void add(const std::string& file_name, Transform t);
// Host path to open for a read of `host` (the override copy, or `host` unchanged).
std::filesystem::path redirect(const std::filesystem::path& host);

// Gameloft string tables (*.english etc.): u16 count, then count x (u16 byte length, UTF-8 bytes).
bool string_table_set(std::vector<uint8_t>& data, int index, const std::string& text);

}  // namespace hle::overrides
