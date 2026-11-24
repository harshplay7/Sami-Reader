#pragma once

// Which text sizes of a font to load when not all of them fit.
//
// The X4's ESP32-C3 can only "see" 8 MB of flash at a time (128 pages of
// 64 KB), shared by the running firmware and the reader font, which is used
// straight from flash. A built-in font has seven sizes (20-32 px) and needs up
// to 2.8 MB for all of them; with a 5.5+ MB firmware they no longer fit
// together. So the X4 loads the sizes one by one, most useful first, for as
// long as there is room (platforms/esp32/font_manager.h). Any size that isn't
// loaded is drawn with the nearest loaded one (BitmapFontSet).

#include <cstdint>

#include "content/ContentModel.h"

namespace microreader {

// Fill `out` with the size indices 0..count-1 in the order to load them, and
// return how many were written (= count). First the base size; then the sizes
// closest to what headings and small print ask for (120%, 140%, 80% and 160%
// of the base, see CssParser); then the rest, nearest the base first, the
// bigger one before the smaller one.
inline int font_window_order(const uint16_t* nominal, int count, int base, int* out) {
  if (count <= 0)
    return 0;
  if (count > kMaxFontSizes)
    count = kMaxFontSizes;
  if (base < 0)
    base = 0;
  if (base >= count)
    base = count - 1;

  bool used[kMaxFontSizes] = {};
  int n = 0;
  auto push = [&](int i) {
    if (i >= 0 && i < count && !used[i]) {
      used[i] = true;
      out[n++] = i;
    }
  };

  push(base);
  static constexpr int kPcts[] = {120, 140, 80, 160};
  const int base_px = nominal ? nominal[base] : 0;
  if (base_px > 0) {
    for (int pct : kPcts) {
      const int target = base_px * pct;  // x100
      int best = -1;
      int best_diff = 0;
      for (int i = 0; i < count; ++i) {
        if (nominal[i] == 0)
          continue;
        int diff = nominal[i] * 100 - target;
        if (diff < 0)
          diff = -diff;
        if (best < 0 || diff < best_diff) {
          best = i;
          best_diff = diff;
        }
      }
      push(best);
    }
  }
  for (int d = 1; d < count; ++d) {
    push(base + d);
    push(base - d);
  }
  return n;
}

}  // namespace microreader
