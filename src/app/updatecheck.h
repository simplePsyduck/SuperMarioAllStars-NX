// Is there a newer release? The latest release on GitHub (its version tag,
// from api.github.com's public "latest release" page: no account, nothing of
// the player's sent), read in the background. Checked at most once a day
// (App: Options > Check for Updates); the launcher shows a badge when newer.
#pragma once
#include <string>

#ifdef __SWITCH__
// The Switch build has no network access and no updater: new versions come as a new .nro. The
// same interface, doing nothing (and no <thread> in every file that includes app.h).
class UpdateCheck {
public:
  static constexpr const char* kApi = "";
  static std::string pageFor(const std::string& tag) { return tag; }
  void start() {}
  bool running() const { return false; }
  bool poll(std::string*) { return false; }
  static bool isNewer(const std::string&, const std::string&) { return false; }
};
#else
#include <atomic>
#include <thread>

class UpdateCheck {
public:
  static constexpr const char* kApi = "https://api.github.com/repos/Darhkfox/SuperMarioAllStars/releases/latest";
  static std::string pageFor(const std::string& tag) {
    return "https://github.com/Darhkfox/SuperMarioAllStars/releases/tag/" + tag;
  }

  ~UpdateCheck();
  void start();                  // (in the background; nothing if one is running)
  bool running() const { return state_ == 1; }
  // Once, when a check finished: true and the latest release's tag ("v1.2"),
  // or true and "" when it couldn't be read (offline, ...).
  bool poll(std::string* tag);

  // Is the tag ("v1.2", "1.2.1") a later version than `current` ("1.1")?
  static bool isNewer(const std::string& tag, const std::string& current);

private:
  std::thread thread_;
  std::atomic<int> state_{0};  // 0 idle, 1 running, 2 done
  std::string tag_;
};
#endif  // __SWITCH__
