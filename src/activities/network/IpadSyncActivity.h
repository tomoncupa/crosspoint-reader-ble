#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"

/**
 * iPad Sync: brings the books and reading places of Tom's iPad reader
 * (tomoncupa.github.io/reader) onto this device, through the same Firebase
 * Realtime Database the iPad syncs to. Nothing runs in the background: WiFi
 * comes on when this screen opens and goes off the moment the sync ends.
 *
 * Set up by a two-line text file at the root of the SD card, /ipad-sync.txt,
 * which the iPad reader saves for you (Settings, Sync, Save setup file):
 *   https://<database>.firebasedatabase.app
 *   <sync code>
 *
 * Places travel as a percentage of the book, so they land within a page.
 */
class IpadSyncActivity final : public Activity {
 public:
  explicit IpadSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("IpadSync", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == State::CONNECTING || state == State::SYNCING; }

 private:
  enum class State { NO_SETUP, CONNECTING, SYNCING, DONE, FAILED };

  State state = State::CONNECTING;
  std::string status;
  std::string detail;
  int percent = -1;
  std::vector<std::string> summary;
  std::string dbUrl;
  std::string syncCode;

  bool loadSetup();
  void onWifiReady(bool connected);
  void runSync();
  void show(const std::string& newStatus, const std::string& newDetail = "", int newPercent = -1);
  void fail(const std::string& why);
};
