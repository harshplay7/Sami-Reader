#pragma once

#include <cstring>

#include "asset_blob.h"
#include "esp_log.h"
#include "font_partition.h"
#include "microreader/Application.h"
#include "microreader/FontManager.h"
#include "microreader/FontWindow.h"

// ESP32 font manager: extends the core FontManager with spiffs partition
// provisioning.  Declare as `static FontManager font_mgr(app)` in app_main
// to keep objects in BSS (not on the stack).
//
// Font bundles live in the asset blob (see asset_blob.h). The chosen one is
// unpacked into the font partition (~15 s, once per font or firmware update).
//
// A font in the partition is used straight from flash, which takes room in the
// chip's 8 MB flash window next to the firmware. A whole font (all seven sizes,
// up to 2.8 MB) no longer fits beside a 5.5 MB firmware, so it is mapped one
// size at a time: the Font Size in use first, then the sizes headings use, for
// as long as there is room (see microreader/FontWindow.h). Changing the Font
// Size swaps the loaded sizes (select_size()).
class FontManager : public microreader::FontManager {
 public:
  explicit FontManager(microreader::Application& app) : app_(app) {}

  // Returns the asset name for the currently selected built-in font, or
  // nullptr if the user selected a custom (SD-card) font.
  static const char* embedded_asset_for(const std::string& font_name) {
    if (font_name == "Alegreya")
      return "alegreya.bin";
    if (font_name == "Literata")
      return "literata.bin";
    if (font_name == "Atkinson Hyperlegible")
      return "atkinson.bin";
    if (font_name == "" || font_name == "Bookerly")
      return "bookerly.bin";
    return nullptr;
  }

  // Returns true if the file is a valid FNTS v2 bundle (the current format).
  // Version 2 bundles always contain MBF4 v4 inner fonts, so no inner check needed.
  static bool is_valid_font_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f)
      return false;
    uint8_t hdr[6] = {};
    bool ok = fread(hdr, 1, sizeof(hdr), f) == sizeof(hdr) && memcmp(hdr, "FNTS", 4) == 0 && hdr[4] > 0 &&  // num_sizes
              hdr[5] == 2;  // bundle version must be exactly 2
    fclose(f);
    return ok;
  }

#ifdef MICROREADER_X3
  // X3: the built-in fonts ship uncompressed inside the firmware and are read
  // straight from flash. The X3's partition table usually has only a 1 MB
  // "spiffs" partition, too small to unpack them into like the X4 does; that
  // partition is used only for fonts copied from the SD card, when they fit.
  static const char* builtin_asset_x3_(const std::string& font_name) {
    if (font_name == "Alegreya")
      return "alegreya.fnt";
    if (font_name == "Literata")
      return "literata.fnt";
    if (font_name == "Atkinson Hyperlegible")
      return "atkinson.fnt";
    if (font_name == "" || font_name == "Bookerly")
      return "bookerly.fnt";
    return nullptr;
  }

  // Load a built-in font from the firmware. True if it's now in use.
  bool use_builtin_x3_(const char* asset) {
    if (builtin_loaded_ == asset && font_set_.valid())
      return true;
    size_t size = 0;
    esp_partition_mmap_handle_t h = 0;
    const uint8_t* data = static_cast<const uint8_t*>(asset_blob::g_assets.map(asset, size, h));
    if (!data) {
      ESP_LOGE("font", "built-in font %s missing from firmware", asset);
      return false;
    }
    release_();
    load_fonts_from_(data, size);  // assets live in flash for the whole run; no unmap
    if (!font_set_.valid()) {
      ESP_LOGE("font", "built-in font %s did not load", asset);
      builtin_loaded_ = nullptr;
      return false;
    }
    builtin_loaded_ = asset;
    ESP_LOGI("font", "using built-in font %s from firmware", asset);
    return true;
  }

  void init() {
    // Settings aren't loaded yet; ensure_ready() switches to the chosen font
    // before a book opens. Start with Bookerly so the reader always has a font.
    if (use_builtin_x3_("bookerly.fnt"))
      app_.set_reader_font(font_set());
  }

  void ensure_ready(microreader::DrawBuffer& buf) override {
    const std::string custom_font = app_.custom_font_path();
    if (const char* asset = builtin_asset_x3_(custom_font)) {
      if (use_builtin_x3_(asset))
        app_.set_reader_font(font_set());
      return;
    }

    // A font file from the SD card: it has to be copied into the spiffs
    // partition first (and fit there).
    const std::string installed_font = app_.installed_font_path();
    auto fall_back = [&](const char* message) {
      if (message) {
        buf.sync_bw_ram();
        buf.show_loading(message, 0);
      }
      app_.set_custom_font_path("Bookerly");
      if (use_builtin_x3_("bookerly.fnt"))
        app_.set_reader_font(font_set());
    };
    if (custom_font == installed_font && ((part_num_ > 0 && font_set_.valid()) || load_fonts_())) {
      app_.set_reader_font(font_set());
      return;
    }
    if (!is_valid_font_file(custom_font.c_str())) {
      ESP_LOGE("font", "SD card font rejected (invalid header): %s", custom_font.c_str());
      fall_back("Font incompatible!");
      return;
    }
    if (!FontPartition::fits_partition(custom_font.c_str())) {
      ESP_LOGE("font", "SD card font too large for this reader's font space: %s", custom_font.c_str());
      fall_back("Font too large!");
      return;
    }
    buf.sync_bw_ram();
    buf.show_loading("Installing fonts...", 0);
    ESP_LOGI("font", "Provisioning font from SD card: %s", custom_font.c_str());
    if (builtin_loaded_ == nullptr) {
      // The font in use lives in the partition that is about to be rewritten.
      release_();
      app_.set_reader_font(nullptr);
    }
    const bool ok = FontPartition::provision_uncompressed_file(
        custom_font.c_str(), buf.scratch_buf2(), microreader::DrawBuffer::kBufSize,
        [&buf](int pct) { buf.show_loading("Installing fonts...", pct); });
    buf.reset_after_scratch();
    if (ok && load_fonts_()) {
      app_.set_installed_font_path(custom_font);
      app_.set_reader_font(font_set());
      return;
    }
    ESP_LOGE("font", "SD card font install failed: %s", custom_font.c_str());
    fall_back("Font install failed!");
  }
#else
  void init() {
    if (FontPartition::stored_size() > 0) {
      load_fonts_();
      if (font_set_.valid()) {
        for (int i = 0; i < microreader::kMaxFontSizes; i++) {
          if (prop_fonts_[i].valid()) {
            ESP_LOGI("font", "Size %d: %u glyphs, height=%u baseline=%u", i, (unsigned)prop_fonts_[i].num_glyphs(),
                     (unsigned)prop_fonts_[i].glyph_height(), (unsigned)prop_fonts_[i].baseline());
          }
        }
        app_.set_reader_font(font_set());
        const char* target_asset = embedded_asset_for(app_.custom_font_path());
        if (target_asset) {
          uint32_t expected_crc = asset_blob::g_assets.crc(target_asset);
          if (FontPartition::needs_provisioning(expected_crc)) {
            ESP_LOGI("font", "font needs provisioning \u2014 will install before app start");
          }
        }
      } else {
        ESP_LOGW("font", "no valid Normal font found");
      }
    }
  }

  // Called by ReaderScreen before opening a book (IFontEnsurer interface).
  // No-op if fonts are already provisioned.
  void ensure_ready(microreader::DrawBuffer& buf) override {
    const std::string& custom_font = app_.custom_font_path();
    const std::string& installed_font = app_.installed_font_path();

    const char* target_asset = embedded_asset_for(custom_font);
    bool is_embedded = (target_asset != nullptr);
    uint32_t target_crc = is_embedded ? asset_blob::g_assets.crc(target_asset) : 0;

    if (custom_font == installed_font && (!is_embedded || !FontPartition::needs_provisioning(target_crc))) {
      if (font_set_.valid() || load_fonts_()) {
        app_.set_reader_font(font_set());
        return;
      }
      // Font is marked installed but invalid — clear the record and fall through to re-provision.
      ESP_LOGW("font", "installed font invalid, re-provisioning");
      app_.set_installed_font_path("");
    }

    if (!is_embedded) {
      if (!is_valid_font_file(custom_font.c_str())) {
        ESP_LOGE("font", "SD card font rejected (invalid header): %s", custom_font.c_str());
        buf.sync_bw_ram();
        buf.show_loading("Font incompatible!", 0);
        app_.set_custom_font_path(installed_font);
        if (font_set_.valid())
          app_.set_reader_font(font_set());
        return;
      }
      if (!FontPartition::fits_partition(custom_font.c_str())) {
        ESP_LOGE("font", "SD card font too large for partition: %s", custom_font.c_str());
        buf.sync_bw_ram();
        buf.show_loading("Font too large!", 0);
        app_.set_custom_font_path(installed_font);
        if (font_set_.valid())
          app_.set_reader_font(font_set());
        return;
      }
    }

    buf.sync_bw_ram();
    buf.show_loading("Installing fonts...", 0);

    if (is_embedded) {
      ESP_LOGI("font", "Provisioning font \"%s\" from firmware...", target_asset);

      size_t mapped_size = 0;
      esp_partition_mmap_handle_t mmap_h = 0;
      const uint8_t* data = static_cast<const uint8_t*>(asset_blob::g_assets.map(target_asset, mapped_size, mmap_h));
      if (!data) {
        ESP_LOGE("font", "failed to map asset %s", target_asset);
        return;
      }
      // The partition is about to be rewritten: let go of the font mapped from it.
      release_();
      app_.set_reader_font(nullptr);
      bool ok = FontPartition::provision_embedded(
          data, mapped_size, target_crc, buf.scratch_buf1(), microreader::DrawBuffer::kBufSize, buf.scratch_buf2(),
          microreader::DrawBuffer::kBufSize, [&buf](int pct) { buf.show_loading("Installing fonts...", pct); });
      asset_blob::g_assets.unmap(mmap_h);

      if (ok) {
        app_.set_installed_font_path(custom_font);
        buf.reset_after_scratch();
        load_fonts_();
      } else {
        ESP_LOGE("font", "provisioning %s failed", target_asset);
      }
      app_.set_reader_font(font_set());  // nullptr if nothing loaded: the reader falls back to the menu font
    } else {
      ESP_LOGI("font", "Provisioning font from SD card: %s", custom_font.c_str());
      release_();
      app_.set_reader_font(nullptr);
      bool prov_ok = FontPartition::provision_uncompressed_file(
          custom_font.c_str(), buf.scratch_buf2(), microreader::DrawBuffer::kBufSize,
          [&buf](int pct) { buf.show_loading("Installing fonts...", pct); });
      buf.reset_after_scratch();
      if (prov_ok) {
        if (load_fonts_()) {
          app_.set_installed_font_path(custom_font);
        } else {
          ESP_LOGE("font", "SD card font loaded but produced no valid Normal style");
          app_.set_custom_font_path(installed_font);
        }
      } else {
        ESP_LOGE("font", "SD card font provisioning failed: %s", custom_font.c_str());
        buf.show_loading("Font install failed!", 0);
        app_.set_custom_font_path(installed_font);
      }
      app_.set_reader_font(font_set());
    }
  }

#endif  // MICROREADER_X3

  // Call in the main loop when g_font_uploaded is true (serial upload).
  void on_serial_upload() {
    if (load_fonts_()) {
      ESP_LOGI("font", "re-loaded fonts after upload");
      app_.set_reader_font(font_set());
    }
  }

  // The reader is about to use text size `idx`: load the sizes around it.
  void select_size(int idx) override {
    if (part_num_ == 0 || idx < 0 || idx >= part_num_ || idx == part_base_)
      return;
    load_window_(idx);
  }

 private:
  // Free pages kept for whatever else maps flash while a book is open (the
  // firmware check when switching app slots works through it in pieces).
  static constexpr uint32_t kReservePages = 6;

  struct Slot {
    uint32_t offset = 0;  // from the bundle's start
    uint32_t size = 0;
    uint16_t nominal = 0;  // pixel size
    esp_partition_mmap_handle_t handle = 0;
    bool mapped = false;
  };

  // Unmap every size of the partition's font and empty the font set.
  void unmap_slots_() {
    for (int i = 0; i < microreader::kMaxFontSizes; ++i) {
      if (slots_[i].mapped) {
        FontPartition::unmap(slots_[i].handle);
        slots_[i].mapped = false;
      }
      prop_fonts_[i] = microreader::BitmapFont();
    }
    font_set_ = microreader::BitmapFontSet();
    num_fonts_ = 0;
  }

  // Let go of whatever font is loaded.
  void release_() {
    unmap_slots_();
    part_num_ = 0;
  }

  // Load the font stored in the partition (its table of sizes, then the sizes
  // around the current base size). True if it can be used.
  bool load_fonts_() {
    release_();
#ifdef MICROREADER_X3
    builtin_loaded_ = nullptr;  // now showing the partition's font
#endif
    const uint32_t total = FontPartition::stored_size();
    if (total < 40) {
      ESP_LOGW("font", "no font in the partition");
      return false;
    }
    uint8_t hdr[40];
    if (!FontPartition::read(0, hdr, sizeof(hdr)) || memcmp(hdr, "FNTS", 4) != 0 || hdr[5] != 2) {
      ESP_LOGE("font", "Invalid font partition (expected FNTS v2 bundle, got version %u)", hdr[5]);
      return false;
    }
    int num = hdr[4];
    if (num > microreader::kMaxFontSizes)
      num = microreader::kMaxFontSizes;
    char font_name[33] = {};
    memcpy(font_name, hdr + 8, 32);
    uint8_t table[4 * microreader::kMaxFontSizes];
    if (num <= 0 || !FontPartition::read(40, table, 4 * num))
      return false;

    uint32_t off = 40 + 4 * static_cast<uint32_t>(num);
    int usable = 0;
    for (int i = 0; i < num; ++i) {
      const uint8_t* p = table + 4 * i;
      const uint32_t sz = p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
      if (off + sz > total || sz < sizeof(microreader::MbfHeader))
        break;
      uint8_t nom[2] = {};
      FontPartition::read(off + 14, nom, 2);  // MbfHeader::nominal_size
      slots_[i] = Slot{};
      slots_[i].offset = off;
      slots_[i].size = sz;
      slots_[i].nominal = static_cast<uint16_t>(nom[0] | (nom[1] << 8));
      off += sz;
      ++usable;
    }
    ESP_LOGI("font", "Bundle font: \"%s\" (%d sizes)", font_name, usable);
    part_num_ = usable;
    if (part_num_ == 0)
      return false;
    if (part_base_ < 0 || part_base_ >= part_num_)
      part_base_ = part_num_ / 2;
    return load_window_(part_base_);
  }

  // Map the sizes around `base`, most useful first, while there's room.
  bool load_window_(int base) {
    unmap_slots_();
    uint16_t nominal[microreader::kMaxFontSizes] = {};
    for (int i = 0; i < part_num_; ++i)
      nominal[i] = slots_[i].nominal;
    font_set_.reset_slots(part_num_, nominal);
    num_fonts_ = part_num_;
    part_base_ = base;

    int order[microreader::kMaxFontSizes];
    const int n = microreader::font_window_order(nominal, part_num_, base, order);
    int loaded = 0;
    for (int k = 0; k < n; ++k) {
      const int i = order[k];
      Slot& s = slots_[i];
      const uint32_t need = FontPartition::pages_for(s.offset, s.size);
      const uint32_t free_now = FontPartition::free_pages();
      if (i != base && free_now < need + kReservePages)
        continue;  // no room; a smaller size later in the list may still fit
      const uint8_t* data = FontPartition::map_range(s.offset, s.size, s.handle);
      if (!data)
        continue;
      s.mapped = true;
      prop_fonts_[i].init(data, s.size);
      if (!prop_fonts_[i].valid()) {
        FontPartition::unmap(s.handle);
        s.mapped = false;
        prop_fonts_[i] = microreader::BitmapFont();
        continue;
      }
      font_set_.set_slot(i, &prop_fonts_[i]);
      ++loaded;
    }

    if (font_set_.has_slot(base)) {
      font_set_.set_base_size_index(base);
    } else {
      // The chosen size didn't fit: use the nearest one that did.
      for (int d = 1; d < part_num_; ++d) {
        if (font_set_.has_slot(base - d)) {
          font_set_.set_base_size_index(base - d);
          break;
        }
        if (font_set_.has_slot(base + d)) {
          font_set_.set_base_size_index(base + d);
          break;
        }
      }
      ESP_LOGE("font", "size %d (%u px) didn't fit in flash; using the nearest size that did", base,
               (unsigned)slots_[base].nominal);
    }
    ESP_LOGI("font", "loaded %d of %d sizes around %u px; %u free pages left", loaded, part_num_,
             (unsigned)slots_[base].nominal, (unsigned)FontPartition::free_pages());
    return font_set_.valid();
  }

  void load_fonts_from_(const uint8_t* d, size_t sz) {
    for (auto& f : prop_fonts_)
      f = microreader::BitmapFont();
    font_set_ = microreader::BitmapFontSet();
    num_fonts_ = 0;
    if (!d) {
      ESP_LOGE("font", "no font data");
      return;
    }

    if (sz < 40 || memcmp(d, "FNTS", 4) != 0 || d[5] != 2) {
      ESP_LOGE("font", "Invalid font partition (expected FNTS v2 bundle, got version %u)", d[5]);
      return;
    }

    // FNTS v1: [FNTS:4][num:1][version:1][res:2][name:32][num×size:4][data...]
    uint8_t num = d[4];
    if (num > microreader::kMaxFontSizes)
      num = microreader::kMaxFontSizes;

    char font_name[33] = {};
    memcpy(font_name, d + 8, 32);
    font_name[32] = '\0';
    ESP_LOGI("font", "Bundle font: \"%s\" (v%u, %u sizes)", font_name, d[5], num);

    constexpr size_t kSizeTableOff = 8 + 32;
    uint32_t sizes[microreader::kMaxFontSizes] = {};
    for (int i = 0; i < num; i++) {
      const uint8_t* p = d + kSizeTableOff + i * 4;
      sizes[i] = p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
    }
    size_t off = kSizeTableOff + static_cast<size_t>(num) * 4;
    for (int i = 0; i < num; i++) {
      if (off + sizes[i] > sz)
        break;
      load_font(d + off, sizes[i]);
      off += sizes[i];
    }
  }

  microreader::Application& app_;
  Slot slots_[microreader::kMaxFontSizes];
  int part_num_ = 0;   // sizes of the partition's font (0: the font in use isn't from the partition)
  int part_base_ = 1;  // base size the loaded sizes were chosen around (Font Size default: 22 px)
#ifdef MICROREADER_X3
  const char* builtin_loaded_ = nullptr;  // asset name of the built-in font in use, if any
#endif
};
