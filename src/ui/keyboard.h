#pragma once
#include <functional>
#include <string>
#include <vector>

#include "ui/view.h"
#include "ui/widgets.h"

namespace ck {

// A modal on-screen keyboard. Used for library search and anywhere else
// text has to be typed on a device with no keys.
class KeyboardView : public View {
 public:
  using DoneFn = std::function<void(const std::string& text, bool accepted)>;

  KeyboardView(std::string title, std::string initial, DoneFn on_done);

  void draw(Canvas& canvas, const Rect& bounds) override;
  bool handle(const InputEvent& event) override;
  Refresh refresh_hint() const override { return Refresh::Fast; }
  std::string title() const override { return title_; }

 private:
  void press(const std::string& key);

  std::string title_;
  std::string text_;
  DoneFn on_done_;
  bool shift_ = false;
  bool symbols_ = false;
  HitList hits_;
  std::vector<std::string> keys_;  // parallel to hit ids
};

}  // namespace ck
