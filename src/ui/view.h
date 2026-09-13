#pragma once
#include <memory>
#include <string>

#include "gfx/canvas.h"
#include "platform/input.h"
#include "platform/screen.h"

namespace ck {

// A full-screen view. Views are stacked; only the top one draws and
// receives input. Drawing is immediate-mode: a view paints its whole area
// when asked, which suits a display that only changes when told to.
class View {
 public:
  virtual ~View() = default;

  virtual void on_show() {}
  virtual void on_hide() {}
  virtual void draw(Canvas& canvas, const Rect& bounds) = 0;
  // Returns true when the view changed and should be redrawn.
  virtual bool handle(const InputEvent& event) { return false; }

  // How the next flush should be sent to the panel.
  virtual Refresh refresh_hint() const { return Refresh::Auto; }
  // Milliseconds between on_tick() calls, or -1 for no timer.
  virtual int tick_ms() const { return -1; }
  virtual bool on_tick() { return false; }
  // Shown in the top bar unless the view draws its own.
  virtual std::string title() const { return ""; }
  // Set when the view wants the screen fully cleared before it draws.
  virtual bool opaque() const { return true; }
};

using ViewPtr = std::unique_ptr<View>;

}  // namespace ck
