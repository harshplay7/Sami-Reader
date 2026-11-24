// Loading only some text sizes of a font (the X4 can't map a whole font next to
// its firmware): the order sizes are loaded in, and a font set with gaps.

#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

#include "FontLoader.h"
#include "microreader/FontWindow.h"
#include "microreader/content/BitmapFont.h"

using namespace microreader;

namespace {

const uint16_t kSizes[] = {20, 22, 24, 26, 28, 30, 32};  // the built-in fonts' sizes

std::vector<int> order_for(int base) {
  int out[kMaxFontSizes] = {};
  const int n = font_window_order(kSizes, 7, base, out);
  return std::vector<int>(out, out + n);
}

}  // namespace

TEST(FontWindowOrder, EverySizeOnceBaseFirst) {
  for (int base = 0; base < 7; ++base) {
    std::vector<int> o = order_for(base);
    ASSERT_EQ(o.size(), 7u) << base;
    EXPECT_EQ(o[0], base);
    std::vector<int> sorted = o;
    std::sort(sorted.begin(), sorted.end());
    for (int i = 0; i < 7; ++i)
      EXPECT_EQ(sorted[i], i) << "base " << base;
  }
}

TEST(FontWindowOrder, HeadingSizesComeNext) {
  // 22 px base: 120% = 26.4 → 26, 140% = 30.8 → 30, 80% = 17.6 → 20, 160% = 35.2 → 32.
  std::vector<int> o = order_for(1);
  EXPECT_EQ(o[0], 1);
  EXPECT_EQ(o[1], 3);
  EXPECT_EQ(o[2], 5);
  EXPECT_EQ(o[3], 0);
  EXPECT_EQ(o[4], 6);
}

TEST(FontWindowOrder, OutOfRangeBaseIsClamped) {
  int out[kMaxFontSizes] = {};
  EXPECT_EQ(font_window_order(kSizes, 7, 12, out), 7);
  EXPECT_EQ(out[0], 6);
  EXPECT_EQ(font_window_order(kSizes, 7, -3, out), 7);
  EXPECT_EQ(out[0], 0);
  EXPECT_EQ(font_window_order(kSizes, 0, 0, out), 0);
}

TEST(FontWindowOrder, WorksWithoutKnownSizes) {
  const uint16_t unknown[3] = {0, 0, 0};
  int out[kMaxFontSizes] = {};
  ASSERT_EQ(font_window_order(unknown, 3, 1, out), 3);
  EXPECT_EQ(out[0], 1);
  EXPECT_EQ(out[1], 2);
  EXPECT_EQ(out[2], 0);
}

// ---------------------------------------------------------------------------
// A font set with only some sizes loaded (uses the real Bookerly bundle).
// ---------------------------------------------------------------------------
#ifndef SMOKE_TESTS_ONLY

namespace {

struct Bookerly {
  std::vector<std::vector<uint8_t>> data;
  std::vector<BitmapFont> fonts;
  uint16_t nominal[kMaxFontSizes] = {};
  int count = 0;

  Bookerly() {
    const auto bundle = load_file_bytes(repo_root() + "/resources/fonts/bookerly.mfb");
    for (size_t i = 0; i < kMaxFontSizes; ++i) {
      auto bytes = extract_font_from_bundle(bundle, i);
      if (bytes.empty())
        break;
      data.push_back(std::move(bytes));
    }
    count = static_cast<int>(data.size());
    fonts.resize(data.size());
    for (int i = 0; i < count; ++i) {
      fonts[i].init(data[i].data(), data[i].size());
      nominal[i] = fonts[i].nominal_size();
    }
  }
};

}  // namespace

TEST(FontSetSlots, OnlyLoadedSizesAreUsed) {
  Bookerly b;
  ASSERT_EQ(b.count, 7);
  BitmapFontSet set;
  set.reset_slots(b.count, b.nominal);
  EXPECT_EQ(set.num_fonts(), 7);
  EXPECT_FALSE(set.valid()) << "nothing loaded yet";

  // Load 22 px (base) and 30 px only.
  set.set_slot(1, &b.fonts[1]);
  set.set_slot(5, &b.fonts[5]);
  set.set_base_size_index(1);
  ASSERT_TRUE(set.valid());
  EXPECT_TRUE(set.has_slot(1));
  EXPECT_FALSE(set.has_slot(3));

  // Every size still has a name for the menus.
  for (int i = 0; i < 7; ++i)
    EXPECT_EQ(set.nominal_size_at(i), 20 + 2 * i);
  EXPECT_EQ(set.get_font(3), nullptr);

  // 100% is the base; a 140% heading (30.8 px) gets 30 px; 120% (26.4 px)
  // isn't loaded and gets the nearest loaded one, 30 px; 80% gets 22 px.
  EXPECT_EQ(set.get(100), &b.fonts[1]);
  EXPECT_EQ(set.get(140), &b.fonts[5]);
  EXPECT_EQ(set.get(120), &b.fonts[5]);
  EXPECT_EQ(set.get(80), &b.fonts[1]);
  EXPECT_GT(set.word_width("Sami", 4, FontStyle::Regular, 140), set.word_width("Sami", 4, FontStyle::Regular, 100));
}

TEST(FontSetSlots, BaseNotLoadedIsInvalid) {
  Bookerly b;
  BitmapFontSet set;
  set.reset_slots(b.count, b.nominal);
  set.set_slot(4, &b.fonts[4]);
  set.set_base_size_index(2);
  EXPECT_FALSE(set.valid()) << "size 2 isn't loaded: still on the empty size 0";
  set.set_base_size_index(4);
  EXPECT_TRUE(set.valid());
  // Asking for a size that isn't loaded keeps the one in use.
  set.set_base_size_index(1);
  EXPECT_EQ(set.base_size_index(), 4);
  EXPECT_TRUE(set.valid());
}

TEST(FontSetSlots, AddStillWorksForWholeFonts) {
  Bookerly b;
  BitmapFontSet set;
  for (auto& f : b.fonts)
    set.add(&f);
  EXPECT_EQ(set.num_fonts(), 7);
  for (int i = 0; i < 7; ++i) {
    EXPECT_TRUE(set.has_slot(i));
    EXPECT_EQ(set.nominal_size_at(i), 20 + 2 * i);
  }
  set.set_base_size_index(2);
  EXPECT_EQ(set.get(160), &b.fonts[6]);  // 38.4 px → the biggest, 32 px
}

#endif  // SMOKE_TESTS_ONLY
