#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "gfx/canvas.h"

namespace ck {

enum class FontStyle { Regular = 0, Bold, Italic, BoldItalic };

struct GlyphMetrics {
  int advance = 0;      // pen advance in pixels
  int left = 0;         // bitmap x offset from the pen position
  int top = 0;          // bitmap y offset from the baseline (negative = above)
  int width = 0;
  int height = 0;
};

struct FontMetrics {
  int ascent = 0;
  int descent = 0;   // positive value
  int line_gap = 0;
  int line_height = 0;
  int x_height = 0;
  int space_advance = 0;
};

// One TrueType face at arbitrary sizes, with a rasterised-glyph cache.
// Glyph bitmaps dominate text-drawing cost on a 1 GHz e-reader, so they are
// cached per (pixel size, code point) and reused across pages.
class FontFace {
 public:
  static std::shared_ptr<FontFace> load(const std::string& path);
  // Synthetic styles let a family with no real italic still show emphasis.
  void set_synthetic(bool oblique, bool embolden);

  const std::string& path() const { return path_; }
  bool valid() const { return !data_.empty(); }

  FontMetrics metrics(int px);
  const GlyphMetrics* glyph(uint32_t cp, int px, const uint8_t** bitmap);
  int kerning(uint32_t a, uint32_t b, int px);
  bool has_glyph(uint32_t cp) const;

  // Opaque handle to the underlying stbtt_fontinfo, for the name-table
  // sniffing the font manager does at scan time.
  void* raw_info() { return info_.data(); }

  // Drops cached bitmaps; called when memory gets tight or the reader
  // switches size, which would otherwise keep both sizes resident.
  void trim_cache(int keep_px = -1);
  size_t cache_bytes() const { return cache_bytes_; }

 private:
  struct Entry {
    GlyphMetrics m;
    std::vector<uint8_t> bitmap;
  };
  struct SizeCache {
    float scale = 0.0f;
    FontMetrics metrics;
    std::unordered_map<uint32_t, Entry> glyphs;
  };

  SizeCache& size_cache(int px);

  std::string path_;
  std::vector<uint8_t> data_;
  std::vector<uint8_t> info_;   // opaque stbtt_fontinfo storage
  std::map<int, SizeCache> sizes_;
  size_t cache_bytes_ = 0;
  bool oblique_ = false;
  bool embolden_ = false;
};

// A family groups the four styles plus fallbacks for missing code points.
struct FontFamily {
  std::string name;
  std::shared_ptr<FontFace> faces[4];
  bool serif = false;

  FontFace* face(FontStyle style) const;
};

// Owns every family CrossKobo knows about: the bundled fonts, the fonts the
// stock Kobo firmware ships, and anything the user dropped in
// /mnt/onboard/fonts.
class FontManager {
 public:
  static FontManager& instance();

  void scan(const std::vector<std::string>& directories);
  // Adds a single font file; returns false if it is not a usable font.
  bool register_file(const std::string& path);

  std::vector<std::string> family_names() const;
  const FontFamily* family(const std::string& name) const;
  // Falls back to the UI family, then to anything at all, so a missing
  // font in the settings file can never leave the screen blank.
  const FontFamily* family_or_default(const std::string& name) const;

  FontFace* resolve(const std::string& family, FontStyle style) const;
  // First face that can draw `cp`, for scripts the chosen family lacks.
  FontFace* fallback_for(uint32_t cp, int px) const;

  const std::string& ui_family() const { return ui_family_; }
  void set_ui_family(const std::string& name) { ui_family_ = name; }
  void trim_caches(int keep_px = -1);
  size_t cache_bytes() const;

 private:
  std::map<std::string, FontFamily> families_;
  std::vector<std::string> registration_order_;
  std::string ui_family_ = "Lexend Deca";
};

// ------------------------------------------------------------------- drawing
struct TextStyle {
  std::string family;
  FontStyle style = FontStyle::Regular;
  int px = 24;
  Color color = Color::gray(0);
  bool underline = false;
  bool strikethrough = false;
  int letter_spacing = 0;
};

int text_width(const std::string& utf8, const TextStyle& st);
int text_height(const TextStyle& st);
// Draws with the baseline at `baseline_y`; returns the final pen x.
int draw_text(Canvas& c, int x, int baseline_y, const std::string& utf8, const TextStyle& st);
// Draws inside `box`, vertically centred, truncating with an ellipsis.
int draw_text_in(Canvas& c, const Rect& box, const std::string& utf8, const TextStyle& st,
                 int align = -1);
std::string ellipsize(const std::string& utf8, const TextStyle& st, int max_width);
// Greedy word wrap; returns the lines.
std::vector<std::string> wrap_text(const std::string& utf8, const TextStyle& st, int max_width);

}  // namespace ck
