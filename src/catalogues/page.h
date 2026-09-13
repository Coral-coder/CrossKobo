#pragma once
#include <string>
#include <vector>

namespace catalogues {

// The HTML the Kobo's browser shows. Plain pages, plain links, plain forms:
// nothing here needs a script to work, so every tap is handled by the
// browser the device already has, with its own keyboard and its own
// scrolling. A little script only improves what already works.
//
// Built for an e-ink screen: large type, large targets, black on white,
// with colour where a colour screen has it and nothing lost where it does
// not.

// A whole document. `back` is a link for the top-left corner, or empty.
std::string page(const std::string& title, const std::string& body,
                 const std::string& back_href = "", const std::string& back_label = "Back");

// One tappable row in a list. `meta` is smaller text under the title;
// `tag` sits on the right; `image` is a thumbnail address, or empty.
std::string row(const std::string& href, const std::string& title, const std::string& meta,
                const std::string& tag = "", const std::string& image = "");

// A search box that submits `name` to `action` by GET.
std::string search_form(const std::string& action, const std::string& value,
                        const std::string& placeholder);

// A message box. `kind` is "note", "good" or "bad".
std::string notice(const std::string& kind, const std::string& html);

// A form with one big button, posting the hidden fields to `action`.
std::string button_form(const std::string& action,
                        const std::vector<std::pair<std::string, std::string>>& fields,
                        const std::string& label, const std::string& busy_label);

// A big link styled as a button.
std::string button_link(const std::string& href, const std::string& label,
                        bool secondary = false);

// Text with the HTML characters escaped. Empty in, empty out.
std::string esc(const std::string& text);

}  // namespace catalogues
