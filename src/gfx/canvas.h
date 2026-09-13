#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "gfx/color.h"
#include "gfx/geometry.h"

namespace ck {

// A 32-bit RGBA drawing surface. Canvas either owns its pixels (offscreen
// buffers, notebook pages, thumbnails) or wraps memory somebody else owns
// (the mmap'd framebuffer), which lets the UI draw straight into the panel
// buffer and then hand the dirty rectangle to the EPD driver.
class Canvas {
 public:
  Canvas() = default;
  Canvas(int w, int h) { reset(w, h); }
  Canvas(const Canvas&) = delete;
  Canvas& operator=(const Canvas&) = delete;
  Canvas(Canvas&& o) noexcept { move_from(o); }
  Canvas& operator=(Canvas&& o) noexcept {
    if (this != &o) move_from(o);
    return *this;
  }

  void reset(int w, int h);
  // Wraps external pixels. `stride` is in pixels, not bytes.
  void wrap(uint32_t* pixels, int w, int h, int stride);
  bool valid() const { return pixels_ != nullptr && width_ > 0 && height_ > 0; }

  int width() const { return width_; }
  int height() const { return height_; }
  int stride() const { return stride_; }
  Rect bounds() const { return Rect(0, 0, width_, height_); }
  uint32_t* pixels() { return pixels_; }
  const uint32_t* pixels() const { return pixels_; }
  uint32_t* row(int y) { return pixels_ + (size_t)y * stride_; }
  const uint32_t* row(int y) const { return pixels_ + (size_t)y * stride_; }

  // ------------------------------------------------------------ clipping
  void push_clip(const Rect& r);
  void pop_clip();
  Rect clip() const { return clip_; }
  void set_clip(const Rect& r) { clip_ = r.intersect(bounds()); }
  void clear_clip() { clip_ = bounds(); }

  // ------------------------------------------------------------- drawing
  void clear(Color c);
  void set_pixel(int x, int y, Color c);
  Color get_pixel(int x, int y) const;
  void fill_rect(const Rect& r, Color c);
  void draw_rect(const Rect& r, Color c, int thickness = 1);
  void fill_round_rect(const Rect& r, int radius, Color c);
  void draw_round_rect(const Rect& r, int radius, Color c, int thickness = 1);
  void fill_circle(int cx, int cy, int radius, Color c);
  void draw_hline(int x, int y, int len, Color c, int thickness = 1);
  void draw_vline(int x, int y, int len, Color c, int thickness = 1);
  // A dotted horizontal rule, used for the reader's guide dots.
  void draw_dotted_hline(int x, int y, int len, Color c, int dot = 2, int gap = 4);
  void draw_line(int x0, int y0, int x1, int y1, Color c, int thickness = 1);
  // Anti-aliased variable-width segment: the pen stroke primitive.
  void draw_thick_line_aa(float x0, float y0, float x1, float y1, float w0, float w1, Color c);
  void invert_rect(const Rect& r);
  void fill_rect_blend(const Rect& r, Color c);  // honours c.a

  // --------------------------------------------------------------- blits
  void blit(const Canvas& src, int dst_x, int dst_y);
  void blit(const Canvas& src, const Rect& src_rect, int dst_x, int dst_y);
  // Area-averaged when shrinking, bilinear when enlarging.
  void blit_scaled(const Canvas& src, const Rect& dst_rect);
  // Blits `src` as a tint mask: alpha comes from the source's alpha channel.
  void blit_mask(const uint8_t* mask, int mask_w, int mask_h, int dst_x, int dst_y, Color c);

  // ----------------------------------------------------- whole-surface fx
  void to_grayscale();
  void apply_saturation(float amount);
  void invert();
  // Floyd-Steinberg dither to the given number of grey levels. Used when
  // the target panel has no colour filter array.
  void dither_gray(int levels);

  bool save_png(const std::string& path) const;
  static bool load_image(const std::string& path, Canvas& out);
  static bool decode_image(const void* data, size_t size, Canvas& out);
  // Re-encodes as PNG in memory (notebook export, cover cache).
  bool encode_png(std::string& out) const;

 private:
  void move_from(Canvas& o);
  void blend_span(int y, int x0, int x1, Color c);

  std::vector<uint32_t> owned_;
  uint32_t* pixels_ = nullptr;
  int width_ = 0;
  int height_ = 0;
  int stride_ = 0;
  Rect clip_;
  std::vector<Rect> clip_stack_;
};

}  // namespace ck
