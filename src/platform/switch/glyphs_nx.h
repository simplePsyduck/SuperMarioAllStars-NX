// UI font glyphs on the Switch: the same job SmasActivity.renderGlyphs does on
// Android (see platform/android/video_gles.cpp), with FreeType and the
// system's shared font (the title font is Luckiest Guy, from the romfs).
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

// Renders each code point at `px` pixels. meta (resized to 2 + 5 per code
// point): [0] ascent, [1] line height, then per code point: advance x 64,
// left, top (relative to the pen and the baseline), width, height; width 0 =
// nothing to draw, advance -1 = no glyph. bits: the 8-bit coverage bitmaps of
// the glyphs with a width, one after another. False if no font could be set up.
bool nxRenderGlyphs(const uint32_t* cps, size_t n, float px, bool bold, bool title, std::vector<int>* meta,
                    std::vector<uint8_t>* bits);
