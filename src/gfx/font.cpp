#include "gfx/font.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/fs.h"
#include "core/log.h"
#include "core/str.h"
#include "stb/stb_truetype.h"

namespace ck {
namespace {

stbtt_fontinfo* info_of(std::vector<uint8_t>& storage) {
  return reinterpret_cast<stbtt_fontinfo*>(storage.data());
}

// Guesses family name and style from the TrueType name table, falling back
// to the file name. User-supplied fonts rarely have tidy file names.
struct FontNames {
  std::string family;
  FontStyle style = FontStyle::Regular;
};

std::string read_name(stbtt_fontinfo* info, int name_id) {
  int len = 0;
  // Platform 3 (Windows), encoding 1 (UTF-16BE), language 0x409 (en-US).
  const char* raw = stbtt_GetFontNameString(info, &len, STBTT_PLATFORM_ID_MICROSOFT,
                                            STBTT_MS_EID_UNICODE_BMP, 0x409, name_id);
  if (raw && len > 0) {
    std::string out;
    for (int i = 0; i + 1 < len; i += 2) {
      uint32_t cp = ((uint32_t)(unsigned char)raw[i] << 8) | (unsigned char)raw[i + 1];
      if (cp) utf8_append(out, cp);
    }
    return trim(out);
  }
  raw = stbtt_GetFontNameString(info, &len, STBTT_PLATFORM_ID_MAC, STBTT_MAC_EID_ROMAN, 0,
                                name_id);
  if (raw && len > 0) return trim(std::string(raw, (size_t)len));
  return "";
}

// Strips the weight/slant words type designers put in the family name so
// that LexendDeca-SemiBold groups with LexendDeca-Regular instead of
// becoming a family of its own.
struct StyleWord {
  const char* word;
  bool bold;
  bool italic;
};

const StyleWord kStyleWords[] = {
    {"extrabolditalic", true, true},  {"semibolditalic", true, true},
    {"bolditalic", true, true},       {"blackitalic", true, true},
    {"mediumitalic", false, true},    {"lightitalic", false, true},
    {"thinitalic", false, true},      {"extralightitalic", false, true},
    {"extrabold", true, false},       {"semibold", true, false},
    {"demibold", true, false},        {"black", true, false},
    {"heavy", true, false},           {"bold", true, false},
    {"extralight", false, false},     {"ultralight", false, false},
    {"light", false, false},          {"medium", false, false},
    {"regular", false, false},        {"book", false, false},
    {"thin", false, false},           {"italic", false, true},
    {"oblique", false, true},
};

// Splits "Lexend Deca SemiBold" into ("Lexend Deca", bold).
void split_style_suffix(std::string& family, bool& bold, bool& italic) {
  for (int pass = 0; pass < 2; ++pass) {
    std::string collapsed = to_lower(replace_all(family, " ", ""));
    for (const StyleWord& sw : kStyleWords) {
      if (!ends_with(collapsed, sw.word)) continue;
      // Never reduce the family to nothing (a font literally called "Bold").
      size_t keep = collapsed.size() - strlen(sw.word);
      if (keep == 0) continue;
      // Walk back over the original string, skipping spaces, to find the cut.
      size_t cut = family.size();
      size_t seen = 0;
      while (cut > 0 && seen < strlen(sw.word)) {
        --cut;
        if (family[cut] != ' ') ++seen;
      }
      family = trim(family.substr(0, cut));
      bold = bold || sw.bold;
      italic = italic || sw.italic;
      break;
    }
  }
}

FontNames guess_names(stbtt_fontinfo* info, const std::string& path) {
  FontNames out;
  // Name id 16 is the typographic family, which already excludes weights.
  out.family = read_name(info, 16);
  if (out.family.empty()) out.family = read_name(info, 1);
  std::string sub = to_lower(read_name(info, 17));
  if (sub.empty()) sub = to_lower(read_name(info, 2));

  if (out.family.empty()) {
    std::string stem = fs::stem(path);
    size_t dash = stem.find('-');
    if (dash != std::string::npos) {
      if (sub.empty()) sub = to_lower(stem.substr(dash + 1));
      stem = stem.substr(0, dash);
    }
    // "LexendDeca" -> "Lexend Deca"
    std::string spaced;
    for (size_t i = 0; i < stem.size(); ++i) {
      if (i > 0 && isupper((unsigned char)stem[i]) && islower((unsigned char)stem[i - 1])) {
        spaced += ' ';
      }
      spaced += stem[i];
    }
    out.family = spaced;
  }

  bool bold = sub.find("bold") != std::string::npos ||
              sub.find("black") != std::string::npos ||
              sub.find("heavy") != std::string::npos;
  bool italic = sub.find("italic") != std::string::npos ||
                sub.find("oblique") != std::string::npos;
  split_style_suffix(out.family, bold, italic);
  // The file name is the last word: it catches fonts with an empty or
  // misleading name table.
  if (!bold && !italic) {
    std::string stem = to_lower(fs::stem(path));
    bool dummy_bold = false, dummy_italic = false;
    std::string tail = stem;
    size_t dash = tail.find('-');
    if (dash != std::string::npos) {
      tail = tail.substr(dash + 1);
      std::string probe = tail;
      split_style_suffix(probe, dummy_bold, dummy_italic);
      bold = dummy_bold;
      italic = dummy_italic;
    }
  }

  if (bold && italic) {
    out.style = FontStyle::BoldItalic;
  } else if (bold) {
    out.style = FontStyle::Bold;
  } else if (italic) {
    out.style = FontStyle::Italic;
  }
  return out;
}

bool looks_serif(const std::string& family) {
  std::string f = to_lower(family);
  static const char* kSerif[] = {"literata", "serif", "georgia", "garamond", "times",
                                 "bitter",   "charter", "amasis", "malabar", "gothic"};
  for (const char* s : kSerif) {
    if (f.find(s) != std::string::npos) return true;
  }
  return false;
}

}  // namespace

// ------------------------------------------------------------------ FontFace

std::shared_ptr<FontFace> FontFace::load(const std::string& path) {
  auto face = std::make_shared<FontFace>();
  std::string data;
  if (!fs::read_file(path, data) || data.size() < 128) return nullptr;
  face->path_ = path;
  face->data_.assign(data.begin(), data.end());
  face->info_.resize(sizeof(stbtt_fontinfo));
  int offset = stbtt_GetFontOffsetForIndex(face->data_.data(), 0);
  if (offset < 0) return nullptr;
  if (!stbtt_InitFont(info_of(face->info_), face->data_.data(), offset)) return nullptr;
  return face;
}

void FontFace::set_synthetic(bool oblique, bool embolden) {
  oblique_ = oblique;
  embolden_ = embolden;
}

FontFace::SizeCache& FontFace::size_cache(int px) {
  auto it = sizes_.find(px);
  if (it != sizes_.end()) return it->second;
  SizeCache sc;
  stbtt_fontinfo* info = info_of(info_);
  sc.scale = stbtt_ScaleForPixelHeight(info, (float)px);
  int ascent = 0, descent = 0, line_gap = 0;
  stbtt_GetFontVMetrics(info, &ascent, &descent, &line_gap);
  sc.metrics.ascent = (int)std::lround(ascent * sc.scale);
  sc.metrics.descent = (int)std::lround(-descent * sc.scale);
  sc.metrics.line_gap = (int)std::lround(line_gap * sc.scale);
  sc.metrics.line_height = sc.metrics.ascent + sc.metrics.descent + sc.metrics.line_gap;
  int adv = 0, lsb = 0;
  stbtt_GetCodepointHMetrics(info, ' ', &adv, &lsb);
  sc.metrics.space_advance = (int)std::lround(adv * sc.scale);
  int x0, y0, x1, y1;
  if (stbtt_GetCodepointBox(info, 'x', &x0, &y0, &x1, &y1)) {
    sc.metrics.x_height = (int)std::lround((y1 - y0) * sc.scale);
  } else {
    sc.metrics.x_height = sc.metrics.ascent / 2;
  }
  return sizes_.emplace(px, std::move(sc)).first->second;
}

FontMetrics FontFace::metrics(int px) { return size_cache(px).metrics; }

bool FontFace::has_glyph(uint32_t cp) const {
  auto* info = const_cast<stbtt_fontinfo*>(
      reinterpret_cast<const stbtt_fontinfo*>(info_.data()));
  return stbtt_FindGlyphIndex(info, (int)cp) != 0;
}

const GlyphMetrics* FontFace::glyph(uint32_t cp, int px, const uint8_t** bitmap) {
  SizeCache& sc = size_cache(px);
  auto it = sc.glyphs.find(cp);
  if (it == sc.glyphs.end()) {
    stbtt_fontinfo* info = info_of(info_);
    int index = stbtt_FindGlyphIndex(info, (int)cp);
    Entry e;
    int adv = 0, lsb = 0;
    stbtt_GetGlyphHMetrics(info, index, &adv, &lsb);
    e.m.advance = (int)std::lround(adv * sc.scale);

    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    stbtt_GetGlyphBitmapBox(info, index, sc.scale, sc.scale, &x0, &y0, &x1, &y1);
    int w = x1 - x0, h = y1 - y0;
    // Synthetic oblique shears the bitmap, which needs extra width.
    int shear = oblique_ ? (int)std::ceil((float)h * 0.21f) : 0;
    int extra = embolden_ ? std::max(1, px / 14) : 0;
    if (w > 0 && h > 0 && w < 4096 && h < 4096) {
      int bw = w + shear + extra;
      int bh = h;
      e.bitmap.assign((size_t)bw * (size_t)bh, 0);
      std::vector<uint8_t> tmp((size_t)w * (size_t)h, 0);
      stbtt_MakeGlyphBitmap(info, tmp.data(), w, h, w, sc.scale, sc.scale, index);
      for (int y = 0; y < h; ++y) {
        int slide = oblique_ ? (int)std::lround((float)(h - 1 - y) * 0.21f) : 0;
        const uint8_t* src = tmp.data() + (size_t)y * (size_t)w;
        uint8_t* dst = e.bitmap.data() + (size_t)y * (size_t)bw;
        for (int x = 0; x < w; ++x) {
          uint8_t v = src[x];
          if (!v) continue;
          for (int e2 = 0; e2 <= extra; ++e2) {
            int tx = x + slide + e2;
            if (tx < 0 || tx >= bw) continue;
            dst[tx] = std::max(dst[tx], v);
          }
        }
      }
      e.m.width = bw;
      e.m.height = bh;
      e.m.left = x0;
      e.m.top = y0;
      e.m.advance += extra;
      cache_bytes_ += e.bitmap.size();
    }
    it = sc.glyphs.emplace(cp, std::move(e)).first;
  }
  if (bitmap) *bitmap = it->second.bitmap.empty() ? nullptr : it->second.bitmap.data();
  return &it->second.m;
}

int FontFace::kerning(uint32_t a, uint32_t b, int px) {
  SizeCache& sc = size_cache(px);
  stbtt_fontinfo* info = info_of(info_);
  int k = stbtt_GetCodepointKernAdvance(info, (int)a, (int)b);
  if (!k) return 0;
  return (int)std::lround(k * sc.scale);
}

void FontFace::trim_cache(int keep_px) {
  for (auto it = sizes_.begin(); it != sizes_.end();) {
    if (keep_px > 0 && it->first == keep_px) {
      ++it;
      continue;
    }
    for (const auto& g : it->second.glyphs) cache_bytes_ -= g.second.bitmap.size();
    it = sizes_.erase(it);
  }
}

// ---------------------------------------------------------------- FontFamily

FontFace* FontFamily::face(FontStyle style) const {
  int want = (int)style;
  if (faces[want]) return faces[want].get();
  // Degrade gracefully: bold-italic -> bold -> regular, italic -> regular.
  static const int kFallback[4][4] = {
      {0, 1, 2, 3},  // Regular
      {1, 0, 3, 2},  // Bold
      {2, 0, 3, 1},  // Italic
      {3, 1, 2, 0},  // BoldItalic
  };
  for (int i = 0; i < 4; ++i) {
    int idx = kFallback[want][i];
    if (faces[idx]) return faces[idx].get();
  }
  return nullptr;
}

// --------------------------------------------------------------- FontManager

FontManager& FontManager::instance() {
  static FontManager m;
  return m;
}

bool FontManager::register_file(const std::string& path) {
  std::string ext = fs::extension(path);
  if (ext != "ttf" && ext != "otf") return false;
  auto face = FontFace::load(path);
  if (!face) {
    CK_LOGW("font: cannot load %s", path.c_str());
    return false;
  }
  FontNames names = guess_names(static_cast<stbtt_fontinfo*>(face->raw_info()), path);
  FontFamily& fam = families_[names.family];
  if (fam.name.empty()) {
    fam.name = names.family;
    fam.serif = looks_serif(names.family);
    registration_order_.push_back(names.family);
  }
  if (!fam.faces[(int)names.style]) fam.faces[(int)names.style] = face;
  return true;
}

void FontManager::scan(const std::vector<std::string>& directories) {
  for (const std::string& dir : directories) {
    if (!fs::is_dir(dir)) continue;
    std::vector<fs::Entry> entries = fs::list_dir(dir);
    std::sort(entries.begin(), entries.end(),
              [](const fs::Entry& a, const fs::Entry& b) { return a.name < b.name; });
    for (const fs::Entry& e : entries) {
      if (e.is_dir) {
        scan({e.path});
        continue;
      }
      register_file(e.path);
    }
  }
  // Synthesise italics for families that lack them (Lexend has none).
  for (auto& kv : families_) {
    FontFamily& fam = kv.second;
    if (!fam.faces[(int)FontStyle::Italic] && fam.faces[(int)FontStyle::Regular]) {
      auto synth = FontFace::load(fam.faces[(int)FontStyle::Regular]->path());
      if (synth) {
        synth->set_synthetic(true, false);
        fam.faces[(int)FontStyle::Italic] = synth;
      }
    }
    if (!fam.faces[(int)FontStyle::Bold] && fam.faces[(int)FontStyle::Regular]) {
      auto synth = FontFace::load(fam.faces[(int)FontStyle::Regular]->path());
      if (synth) {
        synth->set_synthetic(false, true);
        fam.faces[(int)FontStyle::Bold] = synth;
      }
    }
    if (!fam.faces[(int)FontStyle::BoldItalic] && fam.faces[(int)FontStyle::Regular]) {
      auto synth = FontFace::load(fam.faces[(int)FontStyle::Regular]->path());
      if (synth) {
        synth->set_synthetic(true, true);
        fam.faces[(int)FontStyle::BoldItalic] = synth;
      }
    }
  }
  CK_LOGI("fonts: %zu families available", families_.size());
}

std::vector<std::string> FontManager::family_names() const {
  std::vector<std::string> out;
  out.reserve(families_.size());
  for (const auto& kv : families_) out.push_back(kv.first);
  return out;
}

const FontFamily* FontManager::family(const std::string& name) const {
  auto it = families_.find(name);
  return it == families_.end() ? nullptr : &it->second;
}

const FontFamily* FontManager::family_or_default(const std::string& name) const {
  if (const FontFamily* f = family(name)) return f;
  if (const FontFamily* f = family(ui_family_)) return f;
  return families_.empty() ? nullptr : &families_.begin()->second;
}

FontFace* FontManager::resolve(const std::string& family_name, FontStyle style) const {
  const FontFamily* fam = family_or_default(family_name);
  return fam ? fam->face(style) : nullptr;
}

FontFace* FontManager::fallback_for(uint32_t cp, int px) const {
  for (const auto& kv : families_) {
    FontFace* f = kv.second.face(FontStyle::Regular);
    if (f && f->has_glyph(cp)) return f;
  }
  return nullptr;
}

void FontManager::trim_caches(int keep_px) {
  for (auto& kv : families_) {
    for (auto& face : kv.second.faces) {
      if (face) face->trim_cache(keep_px);
    }
  }
}

size_t FontManager::cache_bytes() const {
  size_t total = 0;
  for (const auto& kv : families_) {
    for (const auto& face : kv.second.faces) {
      if (face) total += face->cache_bytes();
    }
  }
  return total;
}

// ------------------------------------------------------------------- drawing

namespace {

struct Resolved {
  FontFace* face = nullptr;
  FontMetrics metrics;
};

Resolved resolve_style(const TextStyle& st) {
  Resolved r;
  r.face = FontManager::instance().resolve(st.family, st.style);
  if (r.face) r.metrics = r.face->metrics(st.px);
  return r;
}

}  // namespace

int text_width(const std::string& utf8, const TextStyle& st) {
  Resolved r = resolve_style(st);
  if (!r.face) return 0;
  int x = 0;
  size_t i = 0;
  uint32_t prev = 0;
  while (i < utf8.size()) {
    uint32_t cp = utf8_next(utf8, i);
    if (cp == '\n') continue;
    FontFace* face = r.face;
    if (!face->has_glyph(cp) && cp > 0x7F) {
      FontFace* fb = FontManager::instance().fallback_for(cp, st.px);
      if (fb) face = fb;
    }
    if (prev) x += face->kerning(prev, cp, st.px);
    const GlyphMetrics* gm = face->glyph(cp, st.px, nullptr);
    x += gm->advance + st.letter_spacing;
    prev = cp;
  }
  return x;
}

int text_height(const TextStyle& st) {
  Resolved r = resolve_style(st);
  return r.face ? r.metrics.line_height : st.px;
}

int draw_text(Canvas& c, int x, int baseline_y, const std::string& utf8, const TextStyle& st) {
  Resolved r = resolve_style(st);
  if (!r.face) return x;
  int start_x = x;
  size_t i = 0;
  uint32_t prev = 0;
  while (i < utf8.size()) {
    uint32_t cp = utf8_next(utf8, i);
    if (cp == '\n') continue;
    FontFace* face = r.face;
    if (!face->has_glyph(cp) && cp > 0x7F) {
      FontFace* fb = FontManager::instance().fallback_for(cp, st.px);
      if (fb) face = fb;
    }
    if (prev) x += face->kerning(prev, cp, st.px);
    const uint8_t* bitmap = nullptr;
    const GlyphMetrics* gm = face->glyph(cp, st.px, &bitmap);
    if (bitmap && gm->width > 0) {
      c.blit_mask(bitmap, gm->width, gm->height, x + gm->left, baseline_y + gm->top, st.color);
    }
    x += gm->advance + st.letter_spacing;
    prev = cp;
  }
  if (st.underline) {
    int thickness = std::max(1, st.px / 12);
    c.fill_rect(Rect(start_x, baseline_y + std::max(2, r.metrics.descent / 3), x - start_x,
                     thickness),
                st.color);
  }
  if (st.strikethrough) {
    int thickness = std::max(1, st.px / 14);
    c.fill_rect(Rect(start_x, baseline_y - r.metrics.x_height / 2, x - start_x, thickness),
                st.color);
  }
  return x;
}

std::string ellipsize(const std::string& utf8, const TextStyle& st, int max_width) {
  if (max_width <= 0) return "";
  if (text_width(utf8, st) <= max_width) return utf8;
  const std::string dots = "\xE2\x80\xA6";  // U+2026
  int dots_w = text_width(dots, st);
  std::string out;
  int width = 0;
  size_t i = 0;
  while (i < utf8.size()) {
    size_t start = i;
    uint32_t cp = utf8_next(utf8, i);
    std::string chunk = utf8.substr(start, i - start);
    int cw = text_width(chunk, st);
    if (width + cw + dots_w > max_width) break;
    out += chunk;
    width += cw;
    (void)cp;
  }
  return out + dots;
}

std::vector<std::string> wrap_text(const std::string& utf8, const TextStyle& st, int max_width) {
  std::vector<std::string> lines;
  if (max_width <= 0) return lines;
  std::string line;
  std::string word;
  int line_w = 0, word_w = 0;
  int space_w = text_width(" ", st);

  auto flush_word = [&]() {
    if (word.empty()) return;
    if (!line.empty() && line_w + space_w + word_w > max_width) {
      lines.push_back(line);
      line.clear();
      line_w = 0;
    }
    if (!line.empty()) {
      line += ' ';
      line_w += space_w;
    }
    line += word;
    line_w += word_w;
    word.clear();
    word_w = 0;
  };

  size_t i = 0;
  while (i < utf8.size()) {
    size_t start = i;
    uint32_t cp = utf8_next(utf8, i);
    std::string chunk = utf8.substr(start, i - start);
    if (cp == '\n') {
      flush_word();
      lines.push_back(line);
      line.clear();
      line_w = 0;
      continue;
    }
    if (cp == ' ' || cp == '\t') {
      flush_word();
      continue;
    }
    int cw = text_width(chunk, st);
    // A single word longer than the line has to be broken mid-word.
    if (word_w + cw > max_width && !word.empty()) {
      flush_word();
      lines.push_back(line);
      line.clear();
      line_w = 0;
    }
    word += chunk;
    word_w += cw;
  }
  flush_word();
  if (!line.empty() || lines.empty()) lines.push_back(line);
  return lines;
}

int draw_text_in(Canvas& c, const Rect& box, const std::string& utf8, const TextStyle& st,
                 int align) {
  Resolved r = resolve_style(st);
  if (!r.face) return box.x;
  std::string text = ellipsize(utf8, st, box.w);
  int w = text_width(text, st);
  int x = box.x;
  if (align == 0) x = box.x + (box.w - w) / 2;
  if (align == 1) x = box.right() - w;
  int baseline = box.y + (box.h + r.metrics.ascent - r.metrics.descent) / 2;
  draw_text(c, x, baseline, text, st);
  return x + w;
}

}  // namespace ck
