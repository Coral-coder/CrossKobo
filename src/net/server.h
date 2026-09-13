#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ck {

// The wireless transfer server: a small HTTP server that lets a browser on
// the same network list, download and upload books and notebooks. It runs
// on its own thread and only ever touches the filesystem, so it cannot
// interfere with drawing.
//
// Access is gated by a four-digit key shown on the device, which is part of
// the address the device displays. Without it every request is refused, so
// nobody else on the network can browse the drive.
class TransferServer {
 public:
  static TransferServer& instance();

  bool start(int port = 8080);
  void stop();
  bool running() const { return running_; }
  int port() const { return port_; }
  const std::string& key() const { return key_; }
  // The address to type or scan, including the key.
  std::string url() const;
  std::string url(const std::string& ip) const;

  struct Activity {
    int64_t when = 0;
    std::string text;
  };
  // Recent uploads and downloads, newest first, for the on-device screen.
  std::vector<Activity> recent_activity() const;
  int upload_count() const { return uploads_; }
  int download_count() const { return downloads_; }
  // Bumped whenever a file lands, so the UI knows to rescan the library.
  int change_counter() const { return changes_; }

 private:
  TransferServer() = default;
  void serve();
  void note(const std::string& text);

  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> stopping_{false};
  std::atomic<int> uploads_{0};
  std::atomic<int> downloads_{0};
  std::atomic<int> changes_{0};
  int listen_fd_ = -1;
  int port_ = 0;
  std::string key_;
  mutable std::mutex mutex_;
  std::vector<Activity> activity_;
};

}  // namespace ck
