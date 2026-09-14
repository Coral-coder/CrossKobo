#include "catalogues/page.h"

#include "catalogues/httpd.h"

namespace catalogues {
namespace {

// Type large enough to tap, contrast high enough for e-ink, and no effects
// that expect a screen to animate. Colour is used sparingly: a Libra Colour
// shows it, a Clara BW shows grey, and both read fine.
const char* kStyle = R"(
html{-webkit-text-size-adjust:100%;text-size-adjust:100%;touch-action:pan-x pan-y;-ms-touch-action:pan-x pan-y}
*{-webkit-text-size-adjust:100%}
body{margin:0;background:#fff;color:#000;font-family:Georgia,"Times New Roman",serif;font-size:22px;line-height:1.35}
a{color:inherit;text-decoration:none}
.top{display:block;background:#1d4f91;color:#fff;padding:14px 18px;overflow:hidden}
.top h1{margin:0;font-size:24px;font-weight:bold;line-height:1.2;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.top .back{display:block;float:left;margin:-4px 14px -4px -8px;padding:8px 12px;font-size:22px;border:2px solid #fff;border-radius:6px;color:#fff}
.body{padding:12px 18px 40px}
.row{display:block;border-bottom:2px solid #ddd;padding:16px 4px;overflow:hidden}
.row .t{display:block;font-size:24px;font-weight:bold}
.row .m{display:block;font-size:18px;color:#444;margin-top:4px}
.row .g{float:right;margin-left:12px;font-size:16px;padding:4px 10px;border:2px solid #1d4f91;border-radius:6px;color:#1d4f91;font-family:Helvetica,Arial,sans-serif}
.row img{float:left;width:64px;margin:0 14px 4px 0;border:1px solid #ccc;background:#eee}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(150px,1fr));gap:14px;margin:14px 0}
.grid .row{border:2px solid #ddd;border-bottom:2px solid #ddd;border-radius:10px;padding:12px 10px;text-align:center}
.grid .row img{float:none;display:block;width:auto;max-width:120px;max-height:180px;margin:0 auto 10px;border:1px solid #ccc;background:#eee}
.grid .row .t{font-size:19px;line-height:1.25}
.grid .row .m{font-size:15px}
.grid .row .g{float:none;display:inline-block;margin:8px 0 0}
.row.nav .t{color:#1d4f91}
.row.nav .t:before{content:"\25B8 ";color:#1d4f91}
form.search{margin:12px 0 18px;overflow:hidden}
form.search input[type=text]{width:100%;box-sizing:border-box;font-size:24px;padding:12px;border:3px solid #1d4f91;border-radius:8px;background:#fff;color:#000;font-family:inherit}
form.search button{margin-top:10px;width:100%}
button,.btn{display:block;width:100%;box-sizing:border-box;font-size:24px;font-family:Helvetica,Arial,sans-serif;font-weight:bold;padding:16px;border-radius:8px;border:3px solid #1d4f91;background:#1d4f91;color:#fff;text-align:center;margin:12px 0}
.btn.second,button.second{background:#fff;color:#1d4f91}
.note,.good,.bad{padding:14px 16px;margin:12px 0;border-radius:8px;border:3px solid #999;background:#f4f4f4}
.good{border-color:#2e7d32;background:#e8f5e9}
.bad{border-color:#b71c1c;background:#fdecea}
h2{font-size:22px;margin:22px 0 8px;font-family:Helvetica,Arial,sans-serif;color:#1d4f91}
p{margin:8px 0}
.small{font-size:17px;color:#444}
dl{margin:0}dt{font-weight:bold;margin-top:10px}dd{margin:0}
pre{font-size:16px;white-space:pre-wrap;word-wrap:break-word;background:#f4f4f4;padding:10px;border-radius:6px}
.foot{padding:16px 18px;border-top:2px solid #ddd;font-size:16px;color:#555}
)";

}  // namespace

std::string esc(const std::string& text) { return html_escape(text); }

// Recolours the parts that carry the blue accent, so a section can look
// like its own thing rather than another page of the same app.
static std::string accent_style(const std::string& c) {
  return ".top{background:" + c + "}"
         ".row .g{border-color:" + c + ";color:" + c + "}"
         ".row.nav .t{color:" + c + "}.row.nav .t:before{color:" + c + "}"
         "form.search input[type=text]{border-color:" + c + "}"
         "button,.btn{border-color:" + c + ";background:" + c + "}"
         ".btn.second,button.second{background:#fff;color:" + c + "}"
         "h2{color:" + c + "}";
}

std::string page(const std::string& title, const std::string& body,
                 const std::string& back_href, const std::string& back_label,
                 const std::string& accent) {
  std::string out = "<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\">\n";
  out += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1, "
         "maximum-scale=1, minimum-scale=1, user-scalable=no\">\n";
  out += "<title>" + esc(title) + "</title>\n<style>" + std::string(kStyle);
  if (!accent.empty()) out += accent_style(accent);
  out += "</style>\n";
  out += "</head><body>\n<div class=\"top\">";
  if (!back_href.empty()) {
    out += "<a class=\"back\" href=\"" + esc(back_href) + "\">&#8249; " + esc(back_label) + "</a>";
  }
  out += "<h1>" + esc(title) + "</h1></div>\n<div class=\"body\">\n" + body + "\n</div>\n";
  // The only script: a button that says it is working once tapped, so a
  // slow download does not look like a tap that missed. Nothing depends on
  // it running.
  out += "<script>(function(){var fs=document.getElementsByTagName('form');"
         "for(var i=0;i<fs.length;i++){fs[i].onsubmit=function(){"
         "var b=this.getElementsByTagName('button');"
         "for(var j=0;j<b.length;j++){if(b[j].getAttribute('data-busy'))"
         "{b[j].innerHTML=b[j].getAttribute('data-busy');}}"
         "window.setTimeout((function(f){return function(){var b=f.getElementsByTagName('button');"
         "for(var j=0;j<b.length;j++){b[j].disabled=true;}};})(this),50);};}})();"
         "</script>\n</body></html>\n";
  return out;
}

std::string row(const std::string& href, const std::string& title, const std::string& meta,
                const std::string& tag, const std::string& image) {
  std::string out = "<a class=\"row" + std::string(tag == "folder" ? " nav" : "") +
                    "\" href=\"" + esc(href) + "\">";
  if (!tag.empty() && tag != "folder") out += "<span class=\"g\">" + esc(tag) + "</span>";
  if (!image.empty()) out += "<img src=\"" + esc(image) + "\" alt=\"\">";
  out += "<span class=\"t\">" + esc(title) + "</span>";
  if (!meta.empty()) out += "<span class=\"m\">" + esc(meta) + "</span>";
  return out + "</a>\n";
}

std::string search_form(const std::string& action, const std::string& value,
                        const std::string& placeholder) {
  return "<form class=\"search\" method=\"get\" action=\"" + esc(action) + "\">" +
         "<input type=\"text\" name=\"q\" value=\"" + esc(value) + "\" placeholder=\"" +
         esc(placeholder) + "\" autocomplete=\"off\">" +
         "<button type=\"submit\" data-busy=\"Searching&#8230;\">Search</button></form>\n";
}

std::string notice(const std::string& kind, const std::string& html) {
  return "<div class=\"" + kind + "\">" + html + "</div>\n";
}

std::string button_form(const std::string& action,
                        const std::vector<std::pair<std::string, std::string>>& fields,
                        const std::string& label, const std::string& busy_label) {
  std::string out = "<form method=\"post\" action=\"" + esc(action) + "\">";
  for (const auto& field : fields) {
    out += "<input type=\"hidden\" name=\"" + esc(field.first) + "\" value=\"" +
           esc(field.second) + "\">";
  }
  out += "<button type=\"submit\" data-busy=\"" + esc(busy_label) + "\">" + esc(label) +
         "</button></form>\n";
  return out;
}

std::string button_link(const std::string& href, const std::string& label, bool secondary) {
  return "<a class=\"btn" + std::string(secondary ? " second" : "") + "\" href=\"" + esc(href) +
         "\">" + esc(label) + "</a>\n";
}

}  // namespace catalogues
