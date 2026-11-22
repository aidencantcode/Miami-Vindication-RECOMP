#pragma once
// S3TC / DXT texture decoding (DXT1 RGB / RGBA, DXT3, DXT5) to RGBA8, for GPUs without the
// extension (Adreno, Mali: Phase 5B). The game ships DXT textures because it picks them for the
// "NVIDIA Tegra" identity the port reports.
#include <cstddef>
#include <cstdint>
#include <vector>

namespace s3tc {

constexpr unsigned kDxt1Rgb = 0x83F0, kDxt1Rgba = 0x83F1, kDxt3 = 0x83F2, kDxt5 = 0x83F3;

inline bool is_s3tc(unsigned format) { return format >= kDxt1Rgb && format <= kDxt5; }

// Decodes w x h texels (any size; blocks are 4x4, partial blocks at the edges are clipped) into
// `out` (w * h * 4 bytes, rows top to bottom as stored). False if `size` is too small.
bool decode(unsigned format, int w, int h, const uint8_t* data, size_t size, std::vector<uint8_t>& out);

}  // namespace s3tc
