#include "gl/s3tc.h"

#include <cstring>

namespace s3tc {

namespace {

void rgb565(uint16_t c, uint8_t* out) {
    const int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    out[0] = uint8_t((r << 3) | (r >> 2));
    out[1] = uint8_t((g << 2) | (g >> 4));
    out[2] = uint8_t((b << 3) | (b >> 2));
}

// Colour block (8 bytes) -> 16 RGBA texels. `four_colour`: DXT3/DXT5 always use 4 colours; DXT1
// uses 3 colours + transparent black when c0 <= c1.
void colour_block(const uint8_t* b, bool four_colour, bool punch_alpha, uint8_t px[16][4]) {
    const uint16_t c0 = uint16_t(b[0] | (b[1] << 8)), c1 = uint16_t(b[2] | (b[3] << 8));
    uint8_t pal[4][4];
    rgb565(c0, pal[0]);
    rgb565(c1, pal[1]);
    pal[0][3] = pal[1][3] = 255;
    if (four_colour || c0 > c1) {
        for (int k = 0; k < 3; ++k) {
            pal[2][k] = uint8_t((2 * pal[0][k] + pal[1][k] + 1) / 3);
            pal[3][k] = uint8_t((pal[0][k] + 2 * pal[1][k] + 1) / 3);
        }
        pal[2][3] = pal[3][3] = 255;
    } else {
        for (int k = 0; k < 3; ++k) pal[2][k] = uint8_t((pal[0][k] + pal[1][k]) / 2);
        pal[2][3] = 255;
        pal[3][0] = pal[3][1] = pal[3][2] = 0;
        pal[3][3] = punch_alpha ? 0 : 255;
    }
    const uint32_t idx = uint32_t(b[4]) | (uint32_t(b[5]) << 8) | (uint32_t(b[6]) << 16) | (uint32_t(b[7]) << 24);
    for (int i = 0; i < 16; ++i) std::memcpy(px[i], pal[(idx >> (2 * i)) & 3], 4);
}

void alpha_dxt3(const uint8_t* b, uint8_t px[16][4]) {
    for (int i = 0; i < 16; ++i) {
        const int a = (b[i / 2] >> ((i & 1) * 4)) & 15;
        px[i][3] = uint8_t(a * 17);
    }
}

void alpha_dxt5(const uint8_t* b, uint8_t px[16][4]) {
    const int a0 = b[0], a1 = b[1];
    int pal[8] = {a0, a1};
    if (a0 > a1) {
        for (int k = 1; k <= 6; ++k) pal[k + 1] = ((7 - k) * a0 + k * a1 + 3) / 7;
    } else {
        for (int k = 1; k <= 4; ++k) pal[k + 1] = ((5 - k) * a0 + k * a1 + 2) / 5;
        pal[6] = 0;
        pal[7] = 255;
    }
    uint64_t bits = 0;
    for (int k = 0; k < 6; ++k) bits |= uint64_t(b[2 + k]) << (8 * k);
    for (int i = 0; i < 16; ++i) px[i][3] = uint8_t(pal[(bits >> (3 * i)) & 7]);
}

}  // namespace

bool decode(unsigned format, int w, int h, const uint8_t* data, size_t size, std::vector<uint8_t>& out) {
    if (w <= 0 || h <= 0 || !is_s3tc(format)) return false;
    const int bw = (w + 3) / 4, bh = (h + 3) / 4;
    const size_t block = format == kDxt1Rgb || format == kDxt1Rgba ? 8 : 16;
    if (size < size_t(bw) * bh * block) return false;
    out.assign(size_t(w) * h * 4, 0);
    uint8_t px[16][4];
    for (int by = 0; by < bh; ++by)
        for (int bx = 0; bx < bw; ++bx) {
            const uint8_t* b = data + (size_t(by) * bw + bx) * block;
            if (block == 8) {
                colour_block(b, false, format == kDxt1Rgba, px);
            } else {
                colour_block(b + 8, true, false, px);
                if (format == kDxt3) alpha_dxt3(b, px);
                else alpha_dxt5(b, px);
            }
            for (int y = 0; y < 4; ++y) {
                const int ty = by * 4 + y;
                if (ty >= h) break;
                for (int x = 0; x < 4; ++x) {
                    const int tx = bx * 4 + x;
                    if (tx >= w) break;
                    std::memcpy(&out[(size_t(ty) * w + tx) * 4], px[y * 4 + x], 4);
                }
            }
        }
    return true;
}

}  // namespace s3tc
