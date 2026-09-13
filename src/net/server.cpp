#include "net/server.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/clock.h"
#include "core/fs.h"
#include "core/log.h"
#include "core/paths.h"
#include "core/str.h"
#include "library/library.h"
#include "net/http.h"
#include "notes/notes.h"
#include "platform/net.h"

namespace ck {
namespace {

constexpr size_t kMaxUploadBytes = 512ull * 1024 * 1024;
constexpr int kMaxActivity = 20;

struct Request {
  std::string method;
  std::string path;
  std::string query;
  std::map<std::string, std::string> headers;
  std::string body;          // empty for uploads, which stream to disk
  int64_t content_length = 0;
};

std::string query_value(const std::string& query, const std::string& key) {
  for (const std::string& part : split(query, '&')) {
    size_t eq = part.find('=');
    if (eq == std::string::npos) continue;
    if (url_decode(part.substr(0, eq)) == key) return url_decode(part.substr(eq + 1));
  }
  return "";
}

std::string content_type_for(const std::string& path) {
  std::string ext = fs::extension(path);
  if (ext == "epub") return "application/epub+zip";
  if (ext == "pdf") return "application/pdf";
  if (ext == "png") return "image/png";
  if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
  if (ext == "txt" || ext == "md") return "text/plain; charset=utf-8";
  if (ext == "cbz") return "application/vnd.comicbook+zip";
  if (ext == "ckn" || ext == "json") return "application/json";
  if (ext == "html") return "text/html; charset=utf-8";
  return "application/octet-stream";
}

std::string html_escape(const std::string& text) {
  std::string out;
  for (char c : text) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out += c;
    }
  }
  return out;
}

// Keeps every path inside the user partition: no "../.." escaping onto the
// root filesystem, and no absolute paths from the client.
bool safe_user_path(const std::string& relative, std::string& out) {
  if (relative.empty()) return false;
  if (relative.find('\0') != std::string::npos) return false;
  std::string normalised = fs::normalize(relative);
  if (!normalised.empty() && normalised[0] == '/') return false;
  if (starts_with(normalised, "..")) return false;
  out = fs::join_path(paths().onboard, normalised);
  // Belt and braces: the result must still start with the onboard root.
  return starts_with(out, paths().onboard);
}

bool write_all(int fd, const char* data, size_t size) {
  size_t sent = 0;
  while (sent < size) {
    ssize_t n = send(fd, data + sent, size - sent, MSG_NOSIGNAL);
    if (n <= 0) {
      if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
      return false;
    }
    sent += (size_t)n;
  }
  return true;
}

bool send_response(int fd, int status, const std::string& content_type, const std::string& body,
                   const std::string& extra_headers = "") {
  const char* reason = status == 200 ? "OK"
                       : status == 201 ? "Created"
                       : status == 204 ? "No Content"
                       : status == 400 ? "Bad Request"
                       : status == 403 ? "Forbidden"
                       : status == 404 ? "Not Found"
                       : status == 405 ? "Method Not Allowed"
                       : status == 413 ? "Payload Too Large"
                                       : "Error";
  std::string head = format("HTTP/1.1 %d %s\r\n", status, reason);
  head += "Content-Type: " + content_type + "\r\n";
  head += format("Content-Length: %zu\r\n", body.size());
  head += "Connection: close\r\n";
  head += "Cache-Control: no-store\r\n";
  head += extra_headers;
  head += "\r\n";
  if (!write_all(fd, head.data(), head.size())) return false;
  return body.empty() || write_all(fd, body.data(), body.size());
}

bool send_file(int fd, const std::string& path, const std::string& download_name) {
  FILE* f = fopen(path.c_str(), "rbe");
  if (!f) return send_response(fd, 404, "text/plain", "not found\n");
  uint64_t size = fs::file_size(path);
  std::string head = "HTTP/1.1 200 OK\r\n";
  head += "Content-Type: " + content_type_for(path) + "\r\n";
  head += format("Content-Length: %llu\r\n", (unsigned long long)size);
  head += "Content-Disposition: attachment; filename=\"" + download_name + "\"\r\n";
  head += "Connection: close\r\n\r\n";
  bool ok = write_all(fd, head.data(), head.size());
  char buffer[65536];
  size_t n;
  while (ok && (n = fread(buffer, 1, sizeof(buffer), f)) > 0) {
    ok = write_all(fd, buffer, n);
  }
  fclose(f);
  return ok;
}

// --------------------------------------------------------------- the page
std::string page_html(const std::string& key) {
  // Deliberately one self-contained page: no external assets, so it works
  // on a network with no internet, which is the normal case here.
  std::string html = R"HTML(<!doctype html>
<html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>CrossKobo</title>
<style>
:root { color-scheme: light dark; }
body { font: 16px/1.5 system-ui, -apple-system, "Segoe UI", Roboto, sans-serif;
       margin: 0; padding: 1.5rem; max-width: 60rem; margin-inline: auto; }
h1 { font-size: 1.4rem; margin: 0 0 .25rem; }
p.sub { color: #666; margin: 0 0 1.5rem; }
section { border: 1px solid #ccc; border-radius: .75rem; padding: 1rem; margin-bottom: 1.5rem; }
h2 { font-size: 1.1rem; margin: 0 0 .75rem; }
#drop { border: 2px dashed #999; border-radius: .75rem; padding: 2rem 1rem; text-align: center;
        color: #666; }
#drop.over { border-color: #1146c8; color: #1146c8; }
table { width: 100%; border-collapse: collapse; }
td, th { text-align: left; padding: .4rem .3rem; border-bottom: 1px solid #eee;
         font-size: .95rem; }
td.size, th.size { text-align: right; white-space: nowrap; color: #666; }
a { color: #1146c8; }
button { font: inherit; padding: .5rem 1rem; border-radius: .5rem; border: 1px solid #999;
         background: #f4f4f4; cursor: pointer; }
progress { width: 100%; }
.row { display: flex; gap: .75rem; align-items: center; flex-wrap: wrap; }
.muted { color: #666; font-size: .9rem; }
@media (prefers-color-scheme: dark) {
  body { background: #16181c; color: #e8e8e8; }
  section, #drop { border-color: #444; }
  td, th { border-color: #2a2d33; }
  p.sub, .muted, td.size { color: #9aa0aa; }
  button { background: #23262c; color: #e8e8e8; border-color: #444; }
}
</style></head><body>
<h1>CrossKobo</h1>
<p class="sub" id="device"></p>

<section>
  <h2>Send files to the reader</h2>
  <div id="drop">
    <p>Drop EPUB, TXT or CBZ files here</p>
    <input type="file" id="picker" multiple hidden>
    <button type="button" onclick="document.getElementById('picker').click()">Choose files</button>
  </div>
  <div id="uploads"></div>
</section>

<section>
  <h2>Books on the reader</h2>
  <table id="books"><tbody></tbody></table>
</section>

<section>
  <h2>Notebooks</h2>
  <table id="notes"><tbody></tbody></table>
  <p class="muted">Notebooks download as PDF. The .ckn file is the editable original.</p>
</section>

<script>
const KEY = new URLSearchParams(location.search).get('k') || '';
const api = (path, params = {}) => {
  const url = new URL(path, location.origin);
  url.searchParams.set('k', KEY);
  for (const [k, v] of Object.entries(params)) url.searchParams.set(k, v);
  return url.toString();
};

async function refresh() {
  const response = await fetch(api('/list'));
  if (!response.ok) { document.body.innerHTML = '<h1>Wrong key</h1>'; return; }
  const data = await response.json();
  document.getElementById('device').textContent =
    `${data.device} · ${data.books.length} books · ${data.notebooks.length} notebooks · ${data.free} free`;

  const render = (target, items, kind) => {
    const body = document.querySelector(`#${target} tbody`);
    body.innerHTML = '';
    if (!items.length) {
      body.innerHTML = '<tr><td class="muted">Nothing here yet</td></tr>';
      return;
    }
    for (const item of items) {
      const tr = document.createElement('tr');
      const name = document.createElement('td');
      const link = document.createElement('a');
      link.href = kind === 'notebook'
        ? api('/notebook.pdf', { path: item.path })
        : api('/download', { path: item.path });
      link.textContent = item.name;
      name.appendChild(link);
      if (item.detail) {
        const detail = document.createElement('div');
        detail.className = 'muted';
        detail.textContent = item.detail;
        name.appendChild(detail);
      }
      const size = document.createElement('td');
      size.className = 'size';
      size.textContent = item.size;
      tr.append(name, size);
      body.appendChild(tr);
    }
  };
  render('books', data.books, 'book');
  render('notes', data.notebooks, 'notebook');
}

async function upload(file) {
  const list = document.getElementById('uploads');
  const row = document.createElement('div');
  row.className = 'row';
  const label = document.createElement('span');
  label.textContent = file.name;
  const bar = document.createElement('progress');
  bar.max = 100;
  bar.value = 0;
  row.append(label, bar);
  list.appendChild(row);

  await new Promise((resolve) => {
    const request = new XMLHttpRequest();
    request.open('PUT', api('/upload', { name: file.name }));
    request.upload.onprogress = (e) => {
      if (e.lengthComputable) bar.value = (e.loaded / e.total) * 100;
    };
    request.onload = () => {
      bar.remove();
      label.textContent = request.status < 300
        ? `${file.name} — sent`
        : `${file.name} — failed (${request.responseText.trim() || request.status})`;
      resolve();
    };
    request.onerror = () => {
      bar.remove();
      label.textContent = `${file.name} — failed`;
      resolve();
    };
    request.send(file);
  });
  refresh();
}

const drop = document.getElementById('drop');
drop.addEventListener('dragover', (e) => { e.preventDefault(); drop.classList.add('over'); });
drop.addEventListener('dragleave', () => drop.classList.remove('over'));
drop.addEventListener('drop', (e) => {
  e.preventDefault();
  drop.classList.remove('over');
  for (const file of e.dataTransfer.files) upload(file);
});
document.getElementById('picker').addEventListener('change', (e) => {
  for (const file of e.target.files) upload(file);
  e.target.value = '';
});

refresh();
setInterval(refresh, 15000);
</script>
</body></html>
)HTML";
  (void)key;
  return html;
}

}  // namespace

TransferServer& TransferServer::instance() {
  static TransferServer s;
  return s;
}

std::string TransferServer::url() const { return url(Net::instance().ip_address()); }

std::string TransferServer::url(const std::string& ip) const {
  std::string host = ip.empty() ? "the reader's address" : ip;
  return format("http://%s:%d/?k=%s", host.c_str(), port_, key_.c_str());
}

std::vector<TransferServer::Activity> TransferServer::recent_activity() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return activity_;
}

void TransferServer::note(const std::string& text) {
  std::lock_guard<std::mutex> lock(mutex_);
  activity_.insert(activity_.begin(), Activity{wall_seconds(), text});
  if (activity_.size() > kMaxActivity) activity_.resize(kMaxActivity);
}

bool TransferServer::start(int port) {
  if (running_) return true;
  stopping_ = false;

  listen_fd_ = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (listen_fd_ < 0) {
    CK_LOGE("transfer: socket: %s", strerror(errno));
    return false;
  }
  int one = 1;
  setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  // Try a few ports: something else may already have 8080.
  bool bound = false;
  for (int candidate = port; candidate < port + 8; ++candidate) {
    addr.sin_port = htons((uint16_t)candidate);
    if (bind(listen_fd_, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
      port_ = candidate;
      bound = true;
      break;
    }
  }
  if (!bound || listen(listen_fd_, 4) != 0) {
    CK_LOGE("transfer: cannot listen: %s", strerror(errno));
    close(listen_fd_);
    listen_fd_ = -1;
    return false;
  }

  // A short key, shown on the device and baked into the address, so a
  // stranger on the same network cannot read the drive.
  unsigned seed = (unsigned)(now_ms() ^ (int64_t)getpid());
  key_ = format("%04u", 1000 + (unsigned)(rand_r(&seed) % 9000));

  running_ = true;
  activity_.clear();
  uploads_ = 0;
  downloads_ = 0;
  thread_ = std::thread([this] { serve(); });
  CK_LOGI("transfer: listening on port %d (key %s)", port_, key_.c_str());
  return true;
}

void TransferServer::stop() {
  if (!running_) return;
  stopping_ = true;
  // Closing the listening socket wakes the accept loop.
  if (listen_fd_ >= 0) {
    shutdown(listen_fd_, SHUT_RDWR);
    close(listen_fd_);
    listen_fd_ = -1;
  }
  if (thread_.joinable()) thread_.join();
  running_ = false;
  CK_LOGI("transfer: stopped");
}

void TransferServer::serve() {
  while (!stopping_) {
    struct pollfd p;
    p.fd = listen_fd_;
    p.events = POLLIN;
    p.revents = 0;
    int pr = poll(&p, 1, 500);
    if (pr <= 0) continue;
    int client = accept(listen_fd_, nullptr, nullptr);
    if (client < 0) continue;

    struct timeval tv;
    tv.tv_sec = 30;
    tv.tv_usec = 0;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    // ------------------------------------------------------ read the head
    std::string buffer;
    char chunk[8192];
    size_t header_end = std::string::npos;
    while (buffer.size() < 64 * 1024) {
      ssize_t n = recv(client, chunk, sizeof(chunk), 0);
      if (n <= 0) break;
      buffer.append(chunk, (size_t)n);
      header_end = buffer.find("\r\n\r\n");
      if (header_end != std::string::npos) break;
    }
    if (header_end == std::string::npos) {
      close(client);
      continue;
    }

    Request request;
    std::vector<std::string> lines = split(buffer.substr(0, header_end), '\n');
    std::vector<std::string> first = split(trim(lines.empty() ? "" : lines[0]), ' ');
    if (first.size() < 2) {
      send_response(client, 400, "text/plain", "bad request\n");
      close(client);
      continue;
    }
    request.method = first[0];
    std::string target = first[1];
    size_t question = target.find('?');
    request.path = question == std::string::npos ? target : target.substr(0, question);
    request.query = question == std::string::npos ? "" : target.substr(question + 1);
    for (size_t i = 1; i < lines.size(); ++i) {
      std::string line = trim(lines[i]);
      size_t colon = line.find(':');
      if (colon == std::string::npos) continue;
      request.headers[to_lower(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
    }
    auto content_length_it = request.headers.find("content-length");
    if (content_length_it != request.headers.end()) {
      request.content_length = (int64_t)to_double(content_length_it->second, 0);
    }
    std::string leftover = buffer.substr(header_end + 4);

    // ----------------------------------------------------------- the key
    if (query_value(request.query, "k") != key_) {
      send_response(client, 403, "text/plain",
                    "This address needs the key shown on the reader.\n");
      close(client);
      continue;
    }

    // ------------------------------------------------------------ routes
    if (request.method == "GET" && (request.path == "/" || request.path == "/index.html")) {
      send_response(client, 200, "text/html; charset=utf-8", page_html(key_));
      close(client);
      continue;
    }

    if (request.method == "GET" && request.path == "/list") {
      // Books, then notebooks, as JSON for the page to render.
      std::string json = "{";
      json += "\"device\":\"CrossKobo\",";
      json += format("\"free\":\"%s\",", human_size(fs::free_space_bytes(paths().onboard)).c_str());

      auto append_books = [&](const std::string& dir, const std::string& prefix,
                              std::string& out, int depth) -> void {
        // A plain recursive walk, two levels deep: enough for the usual
        // "books in folders" layout without scanning a huge tree.
        std::function<void(const std::string&, const std::string&, int)> walk =
            [&](const std::string& directory, const std::string& relative, int level) {
              for (const fs::Entry& e : fs::list_dir(directory)) {
                std::string rel = relative.empty() ? e.name : relative + "/" + e.name;
                if (e.is_dir) {
                  if (level > 0 && e.name != "Notebooks") walk(e.path, rel, level - 1);
                  continue;
                }
                if (!is_readable_book(e.path)) continue;
                if (!out.empty()) out += ",";
                out += "{\"name\":\"" + replace_all(rel, "\"", "\\\"") + "\",";
                out += "\"path\":\"" + replace_all(rel, "\"", "\\\"") + "\",";
                out += "\"size\":\"" + human_size(e.size) + "\",";
                out += "\"detail\":\"\"}";
              }
            };
        walk(dir, prefix, depth);
      };
      std::string books;
      append_books(paths().onboard, "", books, 2);
      json += "\"books\":[" + books + "],";

      std::string notebooks;
      for (const std::string& path : Notebook::list()) {
        auto nb = Notebook::open(path);
        if (!nb) continue;
        std::string rel = "Notebooks/" + fs::basename(path);
        if (!notebooks.empty()) notebooks += ",";
        notebooks += "{\"name\":\"" + replace_all(nb->title(), "\"", "\\\"") + "\",";
        notebooks += "\"path\":\"" + replace_all(rel, "\"", "\\\"") + "\",";
        notebooks += "\"size\":\"" + human_size(fs::file_size(path)) + "\",";
        notebooks += format("\"detail\":\"%zu pages, %d strokes\"}", nb->page_count(),
                            nb->stroke_count());
      }
      json += "\"notebooks\":[" + notebooks + "]}";
      send_response(client, 200, "application/json", json);
      close(client);
      continue;
    }

    if (request.method == "GET" && request.path == "/download") {
      std::string full;
      if (!safe_user_path(query_value(request.query, "path"), full) || !fs::exists(full) ||
          fs::is_dir(full)) {
        send_response(client, 404, "text/plain", "not found\n");
      } else {
        ++downloads_;
        note("Sent " + fs::basename(full));
        send_file(client, full, fs::basename(full));
      }
      close(client);
      continue;
    }

    if (request.method == "GET" && request.path == "/notebook.pdf") {
      std::string full;
      if (!safe_user_path(query_value(request.query, "path"), full) || !fs::exists(full)) {
        send_response(client, 404, "text/plain", "not found\n");
        close(client);
        continue;
      }
      auto nb = Notebook::open(full);
      if (!nb) {
        send_response(client, 400, "text/plain", "not a notebook\n");
        close(client);
        continue;
      }
      // Export to a scratch file, send it, then clean up.
      std::string tmp = paths().cache_dir() + "/transfer-" + fs::stem(full) + ".pdf";
      if (!nb->export_pdf(tmp)) {
        send_response(client, 500, "text/plain", "could not export\n");
      } else {
        ++downloads_;
        note("Sent " + nb->title() + " as PDF");
        send_file(client, tmp, fs::sanitize_filename(nb->title()) + ".pdf");
        fs::remove_file(tmp);
      }
      close(client);
      continue;
    }

    if (request.method == "PUT" && request.path == "/upload") {
      std::string name = fs::sanitize_filename(query_value(request.query, "name"));
      if (name.empty()) {
        send_response(client, 400, "text/plain", "no file name\n");
        close(client);
        continue;
      }
      if (request.content_length <= 0 || (size_t)request.content_length > kMaxUploadBytes) {
        send_response(client, 413, "text/plain", "that file is too large\n");
        close(client);
        continue;
      }
      std::string folder = query_value(request.query, "folder");
      std::string full;
      std::string relative = folder.empty() ? name : folder + "/" + name;
      if (!safe_user_path(relative, full)) {
        send_response(client, 400, "text/plain", "bad destination\n");
        close(client);
        continue;
      }
      // Never overwrite silently: add a counter instead.
      std::string target = full;
      for (int i = 2; fs::exists(target) && i < 100; ++i) {
        target = fs::join_path(fs::dirname(full),
                               fs::stem(full) + format(" (%d)", i) + "." + fs::extension(full));
      }
      fs::mkdir_p(fs::dirname(target));
      FILE* out = fopen((target + ".part").c_str(), "wbe");
      if (!out) {
        send_response(client, 500, "text/plain", "cannot write to the drive\n");
        close(client);
        continue;
      }
      int64_t written = 0;
      bool ok = true;
      if (!leftover.empty()) {
        size_t take = std::min(leftover.size(), (size_t)request.content_length);
        ok = fwrite(leftover.data(), 1, take, out) == take;
        written += (int64_t)take;
      }
      while (ok && written < request.content_length) {
        ssize_t n = recv(client, chunk, sizeof(chunk), 0);
        if (n <= 0) {
          ok = false;
          break;
        }
        size_t take = std::min((size_t)n, (size_t)(request.content_length - written));
        ok = fwrite(chunk, 1, take, out) == take;
        written += (int64_t)take;
      }
      fclose(out);
      if (!ok || written != request.content_length) {
        fs::remove_file(target + ".part");
        send_response(client, 400, "text/plain", "the upload was cut short\n");
      } else {
        fs::rename(target + ".part", target);
        ++uploads_;
        ++changes_;
        note("Received " + fs::basename(target));
        CK_LOGI("transfer: received %s (%lld bytes)", target.c_str(), (long long)written);
        send_response(client, 201, "text/plain", "ok\n");
      }
      close(client);
      continue;
    }

    send_response(client, 404, "text/plain", "no such thing here\n");
    close(client);
  }
}

}  // namespace ck
