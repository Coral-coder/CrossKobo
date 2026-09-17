#pragma once
#include "gfx/canvas.h"

namespace ck {

// Icons are drawn from primitives rather than taken from a font: the
// bundled reading fonts carry no symbol glyphs, and a downloaded UI font
// cannot be relied on either. Drawing them also keeps them crisp at the
// odd sizes a 300 dpi panel asks for.
enum class Icon {
  Back,
  Forward,
  Home,
  Menu,       // three dots
  Plus,
  Close,
  Check,
  Star,       // bookmark marker
  Pen,
  Highlighter,
  Eraser,
  Undo,
  Redo,
  Search,
  Sort,
  Light,
  Note,
  Book,
  Settings,
  ChevronUp,
  ChevronDown,
  Shift,
  Backspace,
  Wifi,
  WifiOff,
};

// Draws `icon` centred in `box`, sized to fit, in `color`.
void draw_icon(Canvas& canvas, const Rect& box, Icon icon, Color color, int weight = 0);

}  // namespace ck
