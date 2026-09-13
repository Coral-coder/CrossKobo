// The notebook list: every notebook on the device, newest first, with a
// preview of how much is in it.
#include <algorithm>
#include <memory>

#include "app/app.h"
#include "app/settings.h"
#include "core/clock.h"
#include "core/fs.h"
#include "core/str.h"
#include "notes/notes.h"
#include "ui/list_view.h"
#include "ui/theme.h"

namespace ck {
namespace {

constexpr int kNewNotebookId = -2000;

class NotesBrowser : public ListView {
 public:
  NotesBrowser() : ListView("Notebooks", {}, nullptr) {
    set_empty_message("No notebooks yet.\nTap \"New notebook\" to start writing.");
    set_actions({{"New notebook", kNewNotebookId}});
    on_select_ = [this](int id) { activate(id); };
    set_on_long_press([this](int id) { long_press(id); });
    reload();
  }

  void on_show() override { reload(); }

 private:
  void reload() {
    paths_ = Notebook::list();
    std::vector<Item> items;
    for (size_t i = 0; i < paths_.size(); ++i) {
      auto nb = Notebook::open(paths_[i]);
      Item item;
      item.id = (int)i;
      if (nb) {
        item.row.title = nb->title();
        std::string detail = format("%zu page%s \xC2\xB7 %d strokes \xC2\xB7 %s",
                                    nb->page_count(), nb->page_count() == 1 ? "" : "s",
                                    nb->stroke_count(),
                                    relative_time(nb->modified()).c_str());
        if (!nb->linked_book().empty()) {
          detail += " \xC2\xB7 " + fs::stem(nb->linked_book());
        }
        item.row.subtitle = detail;
      } else {
        item.row.title = fs::stem(paths_[i]);
        item.row.subtitle = "Could not be read";
      }
      items.push_back(std::move(item));
    }
    set_items(std::move(items), true);
  }

  void activate(int id) {
    if (id == kNewNotebookId) {
      // Notebooks are named by date; the user can rename the file over USB.
      std::string title = "Notes " + format_date(wall_seconds());
      auto nb = Notebook::create(title);
      if (!nb) {
        App::instance().show_message("Notes",
                                     "Could not create a notebook in\n" + Notebook::directory());
        return;
      }
      App::instance().push(make_note_editor(std::move(nb), 0));
      return;
    }
    if (id < 0 || id >= (int)paths_.size()) return;
    auto nb = Notebook::open(paths_[(size_t)id]);
    if (!nb) {
      App::instance().show_message("Notes", "This notebook could not be opened:\n" +
                                                paths_[(size_t)id]);
      return;
    }
    App::instance().push(make_note_editor(std::move(nb), 0));
  }

  void long_press(int id) {
    if (id < 0 || id >= (int)paths_.size()) return;
    std::string path = paths_[(size_t)id];
    auto nb = Notebook::open(path);
    std::string title = nb ? nb->title() : fs::stem(path);

    enum { kExportPdf = 1, kExportPng, kDelete };
    std::vector<Item> items;
    auto add = [&](int action_id, std::string label, std::string subtitle = "") {
      Item item;
      item.id = action_id;
      item.row.title = std::move(label);
      item.row.subtitle = std::move(subtitle);
      items.push_back(std::move(item));
    };
    add(kExportPdf, "Export as PDF");
    add(kExportPng, "Export pages as PNG");
    add(kDelete, "Delete notebook");

    App::instance().push(std::make_unique<ListView>(title, std::move(items), [this, path](
                                                                                  int action) {
      auto notebook = Notebook::open(path);
      if (!notebook) return;
      switch (action) {
        case kExportPdf: {
          std::string out = fs::join_path(Notebook::directory(),
                                          fs::sanitize_filename(notebook->title()) + ".pdf");
          App::instance().show_message(notebook->export_pdf(out) ? "Exported" : "Export failed",
                                       out);
          break;
        }
        case kExportPng: {
          std::string dir = fs::join_path(
              Notebook::directory(), fs::sanitize_filename(notebook->title()) + " pages");
          App::instance().show_message(
              notebook->export_all_png(dir) ? "Exported" : "Export failed", dir);
          break;
        }
        case kDelete:
          if (App::instance().confirm("Delete notebook",
                                      "Delete \"" + notebook->title() + "\"?", "Delete",
                                      "Keep")) {
            notebook->remove();
            App::instance().pop();
            reload();
            App::instance().invalidate(Refresh::Flash);
          }
          break;
        default:
          break;
      }
    }));
  }

  std::vector<std::string> paths_;
};

}  // namespace

ViewPtr make_notes_browser() { return std::make_unique<NotesBrowser>(); }

}  // namespace ck
