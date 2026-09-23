#pragma once

#include <memory>
#include <string>
#include <vector>

#include "IpadSyncEngine.h"
#include "activities/Activity.h"

class Epub;

/**
 * iPad Sync: brings the books and reading places of Tom's iPad reader
 * (tomoncupa.github.io/reader) onto this device, through the same Firebase
 * Realtime Database the iPad syncs to. The work itself is in IpadSyncEngine.
 *
 * From the main menu it syncs the whole library. From the reader menu it syncs only the
 * open book's place, then hands back to the reader, which reopens at the synced place.
 * WiFi comes on when this screen opens and goes off the moment the sync ends.
 */
class IpadSyncActivity final : public Activity {
 public:
  explicit IpadSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("IpadSync", renderer, mappedInput) {}
  // One book, from inside the reader
  IpadSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::shared_ptr<Epub> openEpub,
                   std::string bookPath)
      : Activity("IpadSync", renderer, mappedInput), openEpub(std::move(openEpub)), bookPath(std::move(bookPath)) {}

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
  IpadSync::Setup setup;
  std::shared_ptr<Epub> openEpub;
  std::string bookPath;

  void connect();
  void onWifiReady(bool connected);
  void leave();
  void show(const std::string& newStatus, const std::string& newDetail = "", int newPercent = -1);
  void fail(const std::string& why);
};
