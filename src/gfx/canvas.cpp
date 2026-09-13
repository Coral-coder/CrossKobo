#include "gfx/canvas.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "core/fs.h"
#include "core/log.h"
#include "stb/stb_image.h"
#include "stb/stb_image_write.h"

namespace ck {

void Canvas::reset(int w, int h) {
  width_ = std::max(0, w);
  height_ = std::max(0, h);
  stride_ = width_;
  owned_.assign((size_t)width_ * (size_t)height_, 0);
  pixels_ = owned_.empty() ? nullptr : owned_.data();
  clip_ = bounds();
  clip_stack_.clear();
}

void Canvas::wrap(uint32_t* pixels, int w, int h, int stride) {
  owned_.clear();
  owned_.shrink_to_fit();
  pixels_ = pixels;
  width_ = w;
  height_ = h;
  stride_ = stride;
  clip_ = bounds();
  clip_stack_.clear();
}

void Canvas::move_from(Canvas& o) {
  owned_ = std::move(o.owned_);
  width_ = o.width_;
  height_ = o.height_;
  stride_ = o.stride_;
  clip_ = o.clip_;
  clip_stack_ = std::move(o.clip_stack_);
  // A canvas that owned its memory must repoint at the moved vector; one
  // that wrapped foreign memory keeps the same pointer.
  pixels_ = owned_.empty() ? o.pixels_ : owned_.data();
  o.pixels_ = nullptr;
  o.width_ = o.height_ = o.stride_ = 0;
}

void Canvas::push_clip(const Rect& r) {
  clip_stack_.push_back(clip_);
  clip_ = clip_.intersect(r);
}

void Canvas::pop_clip() {
  if (clip_stack_.empty()) {
    clip_ = bounds();
    return;
  }
  clip_ = clip_stack_.back();
  clip_stack_.pop_back();
}

void Canvas::clear(Color c) {
  if (!valid()) return;
  uint32_t px = c.pack();
  for (int y = 0; y < height_; ++y) {
    uint32_t* p = row(y);
    for (int x = 0; x < width_; ++x) p[x] = px;
  }
}

void Canvas::set_pixel(int x, int y, Color c) {
  if (!valid() || !clip_.contains(x, y)) return;
  if (c.a == 255) {
    row(y)[x] = c.pack();
  } else if (c.a != 0) {
    row(y)[x] = blend(Color::unpack(row(y)[x]), c).pack();
  }
}

Color Canvas::get_pixel(int x, int y) const {
  if (!valid() || x < 0 || y < 0 || x >= width_ || y >= height_) return Color::transparent();
  return Color::unpack(row(y)[x]);
}

void Canvas::fill_rect(const Rect& r, Color c) {
  if (!valid()) return;
  if (c.a != 255) {
    fill_rect_blend(r, c);
    return;
  }
  Rect a = r.intersect(clip_);
  if (a.empty()) return;
  uint32_t px = c.pack();
  for (int y = a.y; y < a.bottom(); ++y) {
    uint32_t* p = row(y) + a.x;
    for (int x = 0; x < a.w; ++x) p[x] = px;
  }
}

void Canvas::blend_span(int y, int x0, int x1, Color c) {
  uint32_t* p = row(y);
  for (int x = x0; x < x1; ++x) p[x] = blend(Color::unpack(p[x]), c).pack();
}

void Canvas::fill_rect_blend(const Rect& r, Color c) {
  if (!valid() || c.a == 0) return;
  Rect a = r.intersect(clip_);
  if (a.empty()) return;
  for (int y = a.y; y < a.bottom(); ++y) blend_span(y, a.x, a.right(), c);
}

void Canvas::draw_rect(const Rect& r, Color c, int thickness) {
  if (r.empty() || thickness <= 0) return;
  int t = std::min(thickness, std::min(r.w, r.h));
  fill_rect(Rect(r.x, r.y, r.w, t), c);
  fill_rect(Rect(r.x, r.bottom() - t, r.w, t), c);
  fill_rect(Rect(r.x, r.y + t, t, r.h - 2 * t), c);
  fill_rect(Rect(r.right() - t, r.y + t, t, r.h - 2 * t), c);
}

void Canvas::draw_hline(int x, int y, int len, Color c, int thickness) {
  fill_rect(Rect(x, y, len, thickness), c);
}

void Canvas::draw_vline(int x, int y, int len, Color c, int thickness) {
  fill_rect(Rect(x, y, thickness, len), c);
}

void Canvas::draw_dotted_hline(int x, int y, int len, Color c, int dot, int gap) {
  int step = dot + gap;
  if (step <= 0) return;
  for (int i = 0; i < len; i += step) {
    fill_rect(Rect(x + i, y, std::min(dot, len - i), std::max(1, dot / 2 + 1)), c);
  }
}

void Canvas::fill_circle(int cx, int cy, int radius, Color c) {
  if (radius <= 0) return;
  // Supersampled coverage on the boundary keeps small dots from looking
  // like squares, which matters for the pen cursor and radio buttons.
  int r2_out = (radius + 1) * (radius + 1);
  int r2_in = (radius - 1) * (radius - 1);
  for (int dy = -radius - 1; dy <= radius + 1; ++dy) {
    int y = cy + dy;
    for (int dx = -radius - 1; dx <= radius + 1; ++dx) {
      int x = cx + dx;
      int d2 = dx * dx + dy * dy;
      if (d2 > r2_out) continue;
      if (d2 <= r2_in) {
        set_pixel(x, y, c);
        continue;
      }
      float d = std::sqrt((float)d2);
      float cov = std::max(0.0f, std::min(1.0f, (float)radius + 0.5f - d));
      if (cov <= 0.0f) continue;
      set_pixel(x, y, c.with_alpha((uint8_t)std::lround(c.a * cov)));
    }
  }
}

void Canvas::fill_round_rect(const Rect& r, int radius, Color c) {
  if (r.empty()) return;
  int rad = std::min(radius, std::min(r.w, r.h) / 2);
  if (rad <= 0) {
    fill_rect(r, c);
    return;
  }
  fill_rect(Rect(r.x + rad, r.y, r.w - 2 * rad, r.h), c);
  fill_rect(Rect(r.x, r.y + rad, rad, r.h - 2 * rad), c);
  fill_rect(Rect(r.right() - rad, r.y + rad, rad, r.h - 2 * rad), c);
  fill_circle(r.x + rad, r.y + rad, rad, c);
  fill_circle(r.right() - rad - 1, r.y + rad, rad, c);
  fill_circle(r.x + rad, r.bottom() - rad - 1, rad, c);
  fill_circle(r.right() - rad - 1, r.bottom() - rad - 1, rad, c);
}

void Canvas::draw_round_rect(const Rect& r, int radius, Color c, int thickness) {
  if (r.empty() || thickness <= 0) return;
  int rad = std::min(radius, std::min(r.w, r.h) / 2);
  if (rad <= 0) {
    draw_rect(r, c, thickness);
    return;
  }
  // Outline as the difference of two filled shapes: simple, and the AA on
  // the corners comes out consistent with fill_round_rect.
  Canvas tmp(r.w, r.h);
  tmp.clear(Color::transparent());
  tmp.fill_round_rect(Rect(0, 0, r.w, r.h), rad, c);
  tmp.fill_round_rect(Rect(thickness, thickness, r.w - 2 * thickness, r.h - 2 * thickness),
                      std::max(0, rad - thickness), Color::transparent());
  // fill_round_rect with a transparent colour is a no-op for blending, so
  // punch the hole by hand.
  for (int y = thickness; y < r.h - thickness; ++y) {
    for (int x = thickness; x < r.w - thickness; ++x) {
      int dx = 0, dy = 0;
      if (x < rad) dx = rad - x;
      if (x >= r.w - rad) dx = x - (r.w - rad - 1);
      if (y < rad) dy = rad - y;
      if (y >= r.h - rad) dy = y - (r.h - rad - 1);
      int inner = rad - thickness;
      if (dx > 0 && dy > 0 && dx * dx + dy * dy > inner * inner) continue;
      tmp.row(y)[x] = 0;
    }
  }
  blit(tmp, r.x, r.y);
}

void Canvas::draw_line(int x0, int y0, int x1, int y1, Color c, int thickness) {
  if (thickness > 1) {
    draw_thick_line_aa((float)x0, (float)y0, (float)x1, (float)y1, (float)thickness,
                       (float)thickness, c);
    return;
  }
  int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  while (true) {
    set_pixel(x0, y0, c);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) {
      err += dy;
      x0 += sx;
    }
    if (e2 <= dx) {
      err += dx;
      y0 += sy;
    }
  }
}

void Canvas::draw_thick_line_aa(float x0, float y0, float x1, float y1, float w0, float w1,
                                Color c) {
  // Signed-distance capsule rasterisation with linearly varying radius.
  // This is what gives pressure-sensitive pen strokes their taper, and it
  // keeps strokes smooth without needing a full vector rasteriser.
  float r0 = std::max(0.25f, w0 * 0.5f);
  float r1 = std::max(0.25f, w1 * 0.5f);
  float max_r = std::max(r0, r1);
  Rect box(
      (int)std::floor(std::min(x0, x1) - max_r - 1), (int)std::floor(std::min(y0, y1) - max_r - 1),
      (int)std::ceil(std::abs(x1 - x0) + 2 * max_r + 3),
      (int)std::ceil(std::abs(y1 - y0) + 2 * max_r + 3));
  Rect area = box.intersect(clip_);
  if (area.empty()) return;

  float vx = x1 - x0, vy = y1 - y0;
  float len2 = vx * vx + vy * vy;

  for (int y = area.y; y < area.bottom(); ++y) {
    uint32_t* p = row(y);
    for (int x = area.x; x < area.right(); ++x) {
      float px = (float)x + 0.5f, py = (float)y + 0.5f;
      float t = len2 > 0.0f ? ((px - x0) * vx + (py - y0) * vy) / len2 : 0.0f;
      t = std::max(0.0f, std::min(1.0f, t));
      float cx = x0 + vx * t, cy = y0 + vy * t;
      float dist = std::sqrt((px - cx) * (px - cx) + (py - cy) * (py - cy));
      float radius = r0 + (r1 - r0) * t;
      float cov = radius + 0.5f - dist;
      if (cov <= 0.0f) continue;
      if (cov > 1.0f) cov = 1.0f;
      uint8_t a = (uint8_t)std::lround(c.a * cov);
      if (a == 0) continue;
      p[x] = blend(Color::unpack(p[x]), c.with_alpha(a)).pack();
    }
  }
}

void Canvas::invert_rect(const Rect& r) {
  Rect a = r.intersect(clip_);
  if (a.empty()) return;
  for (int y = a.y; y < a.bottom(); ++y) {
    uint32_t* p = row(y);
    for (int x = a.x; x < a.right(); ++x) {
      Color c = Color::unpack(p[x]);
      p[x] = c.inverted().pack();
    }
  }
}

void Canvas::blit(const Canvas& src, int dst_x, int dst_y) {
  blit(src, src.bounds(), dst_x, dst_y);
}

void Canvas::blit(const Canvas& src, const Rect& src_rect, int dst_x, int dst_y) {
  if (!valid() || !src.valid()) return;
  Rect s = src_rect.intersect(src.bounds());
  if (s.empty()) return;
  Rect d = Rect(dst_x, dst_y, s.w, s.h).intersect(clip_);
  if (d.empty()) return;
  int off_x = d.x - dst_x;
  int off_y = d.y - dst_y;
  for (int y = 0; y < d.h; ++y) {
    const uint32_t* sp = src.row(s.y + off_y + y) + s.x + off_x;
    uint32_t* dp = row(d.y + y) + d.x;
    for (int x = 0; x < d.w; ++x) {
      uint32_t px = sp[x];
      uint8_t a = (uint8_t)(px >> 24);
      if (a == 255) {
        dp[x] = px;
      } else if (a != 0) {
        dp[x] = blend(Color::unpack(dp[x]), Color::unpack(px)).pack();
      }
    }
  }
}

void Canvas::blit_scaled(const Canvas& src, const Rect& dst_rect) {
  if (!valid() || !src.valid() || dst_rect.empty()) return;
  Rect d = dst_rect.intersect(clip_);
  if (d.empty()) return;

  bool shrinking = dst_rect.w < src.width() || dst_rect.h < src.height();
  float sx_step = (float)src.width() / (float)dst_rect.w;
  float sy_step = (float)src.height() / (float)dst_rect.h;

  for (int y = d.y; y < d.bottom(); ++y) {
    uint32_t* dp = row(y);
    float fy = ((float)(y - dst_rect.y)) * sy_step;
    for (int x = d.x; x < d.right(); ++x) {
      float fx = ((float)(x - dst_rect.x)) * sx_step;
      int r = 0, g = 0, b = 0, a = 0, n = 0;
      if (shrinking) {
        // Box filter: averaging is what keeps downscaled cover art from
        // turning into noise once the panel dithers it.
        int x0 = (int)fx, x1 = std::min(src.width(), (int)(fx + sx_step) + 1);
        int y0 = (int)fy, y1 = std::min(src.height(), (int)(fy + sy_step) + 1);
        x1 = std::max(x1, x0 + 1);
        y1 = std::max(y1, y0 + 1);
        for (int sy = y0; sy < y1; ++sy) {
          const uint32_t* sp = src.row(sy);
          for (int sx = x0; sx < x1; ++sx) {
            Color c = Color::unpack(sp[sx]);
            r += c.r;
            g += c.g;
            b += c.b;
            a += c.a;
            ++n;
          }
        }
      } else {
        int sx = std::min(src.width() - 1, (int)fx);
        int sy = std::min(src.height() - 1, (int)fy);
        int sx2 = std::min(src.width() - 1, sx + 1);
        int sy2 = std::min(src.height() - 1, sy + 1);
        float tx = fx - (float)sx, ty = fy - (float)sy;
        Color c00 = Color::unpack(src.row(sy)[sx]);
        Color c10 = Color::unpack(src.row(sy)[sx2]);
        Color c01 = Color::unpack(src.row(sy2)[sx]);
        Color c11 = Color::unpack(src.row(sy2)[sx2]);
        Color top = lerp_color(c00, c10, tx);
        Color bottom = lerp_color(c01, c11, tx);
        Color out = lerp_color(top, bottom, ty);
        r = out.r;
        g = out.g;
        b = out.b;
        a = out.a;
        n = 1;
      }
      if (n == 0) continue;
      Color out((uint8_t)(r / n), (uint8_t)(g / n), (uint8_t)(b / n), (uint8_t)(a / n));
      if (out.a == 255) {
        dp[x] = out.pack();
      } else if (out.a != 0) {
        dp[x] = blend(Color::unpack(dp[x]), out).pack();
      }
    }
  }
}

void Canvas::blit_mask(const uint8_t* mask, int mask_w, int mask_h, int dst_x, int dst_y,
                       Color c) {
  if (!valid() || !mask || mask_w <= 0 || mask_h <= 0 || c.a == 0) return;
  Rect d = Rect(dst_x, dst_y, mask_w, mask_h).intersect(clip_);
  if (d.empty()) return;
  for (int y = d.y; y < d.bottom(); ++y) {
    const uint8_t* mp = mask + (size_t)(y - dst_y) * (size_t)mask_w;
    uint32_t* dp = row(y);
    for (int x = d.x; x < d.right(); ++x) {
      uint8_t m = mp[x - dst_x];
      if (!m) continue;
      uint8_t a = c.a == 255 ? m : (uint8_t)((m * c.a + 127) / 255);
      if (a == 255) {
        dp[x] = c.pack();
      } else {
        dp[x] = blend(Color::unpack(dp[x]), c.with_alpha(a)).pack();
      }
    }
  }
}

void Canvas::to_grayscale() {
  if (!valid()) return;
  for (int y = 0; y < height_; ++y) {
    uint32_t* p = row(y);
    for (int x = 0; x < width_; ++x) {
      Color c = Color::unpack(p[x]);
      uint8_t l = c.luma();
      p[x] = Color(l, l, l, c.a).pack();
    }
  }
}

void Canvas::apply_saturation(float amount) {
  if (!valid() || amount == 0.0f) return;
  for (int y = 0; y < height_; ++y) {
    uint32_t* p = row(y);
    for (int x = 0; x < width_; ++x) {
      p[x] = saturate(Color::unpack(p[x]), amount).pack();
    }
  }
}

void Canvas::invert() {
  if (!valid()) return;
  for (int y = 0; y < height_; ++y) {
    uint32_t* p = row(y);
    for (int x = 0; x < width_; ++x) p[x] = Color::unpack(p[x]).inverted().pack();
  }
}

void Canvas::dither_gray(int levels) {
  if (!valid() || levels < 2) return;
  std::vector<float> err((size_t)width_ * 2, 0.0f);
  float step = 255.0f / (float)(levels - 1);
  for (int y = 0; y < height_; ++y) {
    float* cur = err.data() + (y % 2) * width_;
    float* next = err.data() + ((y + 1) % 2) * width_;
    std::fill(next, next + width_, 0.0f);
    uint32_t* p = row(y);
    for (int x = 0; x < width_; ++x) {
      Color c = Color::unpack(p[x]);
      float v = (float)c.luma() + cur[x];
      float q = std::round(v / step) * step;
      q = std::max(0.0f, std::min(255.0f, q));
      float e = v - q;
      if (x + 1 < width_) cur[x + 1] += e * 7.0f / 16.0f;
      if (x > 0) next[x - 1] += e * 3.0f / 16.0f;
      next[x] += e * 5.0f / 16.0f;
      if (x + 1 < width_) next[x + 1] += e * 1.0f / 16.0f;
      uint8_t g = (uint8_t)q;
      p[x] = Color(g, g, g, c.a).pack();
    }
  }
}

bool Canvas::save_png(const std::string& path) const {
  if (!valid()) return false;
  fs::mkdir_p(fs::dirname(path));
  // stb wants tightly packed rows.
  std::vector<uint32_t> packed;
  const uint32_t* data = pixels_;
  if (stride_ != width_) {
    packed.resize((size_t)width_ * height_);
    for (int y = 0; y < height_; ++y) {
      memcpy(packed.data() + (size_t)y * width_, row(y), (size_t)width_ * 4);
    }
    data = packed.data();
  }
  return stbi_write_png(path.c_str(), width_, height_, 4, data, width_ * 4) != 0;
}

bool Canvas::encode_png(std::string& out) const {
  if (!valid()) return false;
  std::vector<uint32_t> packed((size_t)width_ * height_);
  for (int y = 0; y < height_; ++y) {
    memcpy(packed.data() + (size_t)y * width_, row(y), (size_t)width_ * 4);
  }
  out.clear();
  auto sink = [](void* ctx, void* data, int size) {
    static_cast<std::string*>(ctx)->append((const char*)data, (size_t)size);
  };
  if (!stbi_write_png_to_func(sink, &out, width_, height_, 4, packed.data(), width_ * 4)) {
    return false;
  }
  return true;
}

bool Canvas::load_image(const std::string& path, Canvas& out) {
  std::string data;
  if (!fs::read_file(path, data)) return false;
  return decode_image(data.data(), data.size(), out);
}

bool Canvas::decode_image(const void* data, size_t size, Canvas& out) {
  int w = 0, h = 0, comp = 0;
  stbi_uc* px = stbi_load_from_memory((const stbi_uc*)data, (int)size, &w, &h, &comp, 4);
  if (!px) return false;
  out.reset(w, h);
  memcpy(out.pixels(), px, (size_t)w * (size_t)h * 4);
  stbi_image_free(px);
  return true;
}

}  // namespace ck
