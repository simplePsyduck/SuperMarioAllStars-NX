// UI font glyphs on the Switch: see glyphs_nx.h.
#include "platform/switch/glyphs_nx.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_SYNTHESIS_H
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "platform/switch/nx_sys.h"

namespace {

FT_Library g_ft = nullptr;
bool g_ftTried = false;

struct Face {
  FT_Face face = nullptr;
  bool tried = false;
  std::vector<uint8_t> own;  // (a font read from a file stays in memory as long as its face)
};
Face g_title;
Face g_shared[6];

bool ensureFreeType() {
  if (!g_ftTried) {
    g_ftTried = true;
    if (FT_Init_FreeType(&g_ft) != 0) g_ft = nullptr;
  }
  return g_ft != nullptr;
}

FT_Face sharedFace(int type) {
  Face& f = g_shared[type];
  if (!f.tried) {
    f.tried = true;
    size_t size = 0;
    const void* data = nx::sharedFont(type, &size);
    if (data && size) {
      if (FT_New_Memory_Face(g_ft, (const FT_Byte*)data, (FT_Long)size, 0, &f.face) != 0) f.face = nullptr;
    }
  }
  return f.face;
}

FT_Face titleFace() {
  Face& f = g_title;
  if (!f.tried) {
    f.tried = true;
    const char* paths[] = {"romfs:/LuckiestGuy-Regular.ttf", "LuckiestGuy-Regular.ttf"};
    for (const char* p : paths) {
      FILE* fp = std::fopen(p, "rb");
      if (!fp) continue;
      std::fseek(fp, 0, SEEK_END);
      long n = std::ftell(fp);
      std::fseek(fp, 0, SEEK_SET);
      if (n > 0 && n < (8 << 20)) {
        f.own.resize((size_t)n);
        if (std::fread(f.own.data(), 1, (size_t)n, fp) == (size_t)n &&
            FT_New_Memory_Face(g_ft, f.own.data(), (FT_Long)n, 0, &f.face) == 0) {
          std::fclose(fp);
          break;
        }
        f.face = nullptr;
      }
      std::fclose(fp);
    }
  }
  return f.face;
}

// What stands in for a symbol none of the system's fonts has.
uint32_t substitute(uint32_t cp) {
  switch (cp) {
    case 0x2190: case 0x25C0: return '<';
    case 0x2192: case 0x25B6: return '>';
    case 0x2191: return '^';
    case 0x2193: return 'v';
    case 0x2713: return '+';
    case 0x2715: case 0x00D7: return 'x';
    case 0x2022: case 0x266A: case 0x2605: return '*';
    case 0x2026: return '.';
    case 0x2013: case 0x2014: return '-';
    case 0x2018: case 0x2019: return '\'';
    case 0x201C: case 0x201D: return '"';
    case 0x24B6: return 'A';
    case 0x24B7: return 'B';
    case 0x2122: return 't';
    default: return 0;
  }
}

}  // namespace

bool nxRenderGlyphs(const uint32_t* cps, size_t n, float px, bool bold, bool title, std::vector<int>* meta,
                    std::vector<uint8_t>* bits) {
  if (!ensureFreeType()) return false;
  // the faces to look in, in order: the title font (for the title), then the system's
  std::vector<FT_Face> order;
  if (title)
    if (FT_Face t = titleFace()) order.push_back(t);
  for (int type : {0, 5, 1, 4, 2, 3})
    if (FT_Face f = sharedFace(type)) order.push_back(f);
  if (order.empty()) return false;
  const bool fake = bold && !(title && order[0] == g_title.face);  // (the title font is bold already)

  const FT_UInt ipx = (FT_UInt)std::max(4, (int)std::lround(px));
  FT_Face primary = order[0];
  FT_Set_Pixel_Sizes(primary, 0, ipx);
  meta->assign(2 + n * 5, 0);
  bits->clear();
  (*meta)[0] = (int)((primary->size->metrics.ascender + 63) >> 6);
  (*meta)[1] = (int)((primary->size->metrics.height + 63) >> 6);

  for (size_t i = 0; i < n; i++) {
    const size_t m = 2 + i * 5;
    (*meta)[m] = -1;
    uint32_t cps2[2] = {cps[i], substitute(cps[i])};
    bool done = false;
    for (int s = 0; s < 2 && !done; s++) {
      const uint32_t cp = cps2[s];
      if (!cp) break;
      for (FT_Face face : order) {
        FT_UInt gi = FT_Get_Char_Index(face, cp);
        if (!gi) continue;
        FT_Set_Pixel_Sizes(face, 0, ipx);
        if (FT_Load_Glyph(face, gi, FT_LOAD_NO_BITMAP | FT_LOAD_TARGET_LIGHT) != 0) continue;
        FT_GlyphSlot slot = face->glyph;
        if (fake && slot->format == FT_GLYPH_FORMAT_OUTLINE) FT_GlyphSlot_Embolden(slot);
        if (slot->format != FT_GLYPH_FORMAT_BITMAP && FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL) != 0) continue;
        (*meta)[m] = (int)slot->advance.x;  // (26.6 fixed point: already x 64)
        const FT_Bitmap& bm = slot->bitmap;
        if (bm.width == 0 || bm.rows == 0 || !bm.buffer || bm.pixel_mode != FT_PIXEL_MODE_GRAY) {
          (*meta)[m + 3] = (*meta)[m + 4] = 0;
          done = true;
          break;
        }
        // a pixel of room for anti-aliasing on each side, as the Android glyphs have
        const int w = (int)bm.width + 2, h = (int)bm.rows + 2;
        (*meta)[m + 1] = slot->bitmap_left - 1;
        (*meta)[m + 2] = -slot->bitmap_top - 1;
        (*meta)[m + 3] = w;
        (*meta)[m + 4] = h;
        const size_t at = bits->size();
        bits->resize(at + (size_t)w * h, 0);
        const int pitch = bm.pitch;
        for (int y = 0; y < (int)bm.rows; y++) {
          const uint8_t* src = pitch >= 0 ? bm.buffer + (size_t)y * pitch : bm.buffer + (size_t)(bm.rows - 1 - y) * (size_t)(-pitch);
          std::memcpy(&(*bits)[at + (size_t)(y + 1) * w + 1], src, bm.width);
        }
        done = true;
        break;
      }
    }
  }
  return true;
}
