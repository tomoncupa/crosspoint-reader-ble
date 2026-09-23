#include "IpadSyncActivity.h"

#include <BluetoothHIDManager.h>
#include <Epub.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <WiFi.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

void IpadSyncActivity::show(const std::string& newStatus, const std::string& newDetail, int newPercent) {
  {
    RenderLock lock(*this);
    status = newStatus;
    detail = newDetail;
    percent = newPercent;
  }
  requestUpdateAndWait();
}

void IpadSyncActivity::fail(const std::string& why) {
  IpadSync::wifiOff();
  {
    RenderLock lock(*this);
    state = State::FAILED;
    status = why;
  }
  requestUpdate(true);
}

void IpadSyncActivity::onEnter() {
  Activity::onEnter();
  if (!IpadSync::loadSetup(setup)) {
    state = State::NO_SETUP;
    requestUpdate();
    return;
  }
  connect();
}

void IpadSyncActivity::connect() {
  state = State::CONNECTING;
  requestUpdate();
  if (WiFi.status() == WL_CONNECTED) {
    onWifiReady(true);
    return;
  }
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiReady(!result.isCancelled); });
}

void IpadSyncActivity::onExit() {
  Activity::onExit();
  IpadSync::wifiOff();
  // WiFi took the radio from Bluetooth; hand it back so the remote works again
  if (SETTINGS.bluetoothEnabled) {
    auto& bt = BluetoothHIDManager::getInstance();
    if (!bt.isEnabled()) bt.enable();
  }
}

void IpadSyncActivity::onWifiReady(const bool connected) {
  if (!connected) {
    fail("No WiFi, so nothing was synced.");
    return;
  }
  state = State::SYNCING;
  show("Setting the clock...");
  IpadSync::syncClock();

  IpadSync::Options options;
  options.onlyPath = bookPath;
  options.openEpub = openEpub;
  options.progress = [this](const std::string& s, const std::string& d, int p) { show(s, d, p); };
  IpadSync::Outcome outcome = IpadSync::run(setup, options);

  // Off again straight away: WiFi is the expensive part of this
  IpadSync::wifiOff();
  if (!outcome.ok) {
    fail(outcome.error);
    return;
  }
  {
    RenderLock lock(*this);
    summary = std::move(outcome.lines);
    state = State::DONE;
  }
  requestUpdate(true);
}

// Back to the reader (it rereads the place) or to the main menu
void IpadSyncActivity::leave() {
  if (bookPath.empty()) {
    onGoHome();
    return;
  }
  ActivityResult result;
  result.isCancelled = state != State::DONE;
  setResult(std::move(result));
  finish();
}

void IpadSyncActivity::loop() {
  if (state == State::SYNCING || state == State::CONNECTING) return;
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    leave();
    return;
  }
  if ((state == State::DONE || state == State::FAILED) &&
      mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    summary.clear();
    connect();
  }
}

void IpadSyncActivity::render(RenderLock&&) {
  const int pageWidth = renderer.getScreenWidth();
  renderer.clearScreen();
  renderer.drawCenteredText(UI_12_FONT_ID, 15, bookPath.empty() ? "iPad Sync" : "Sync this book", true,
                            EpdFontFamily::BOLD);

  if (state == State::NO_SETUP) {
    renderer.drawCenteredText(UI_10_FONT_ID, 200, "Not set up yet", true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, 250, "On the iPad reader: Settings,");
    renderer.drawCenteredText(UI_10_FONT_ID, 275, "Sync between devices,");
    renderer.drawCenteredText(UI_10_FONT_ID, 300, "Save setup file for my X4.");
    renderer.drawCenteredText(UI_10_FONT_ID, 340, "Put ipad-sync.txt at the top");
    renderer.drawCenteredText(UI_10_FONT_ID, 365, "of this SD card, then come back.");
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == State::CONNECTING || state == State::SYNCING) {
    renderer.drawCenteredText(UI_10_FONT_ID, 260, status.empty() ? "Connecting..." : status.c_str(), true,
                              EpdFontFamily::BOLD);
    if (!detail.empty()) {
      const std::string d = renderer.truncatedText(UI_10_FONT_ID, detail.c_str(), pageWidth - 40);
      renderer.drawCenteredText(UI_10_FONT_ID, 295, d.c_str());
    }
    if (percent >= 0) {
      const int barW = pageWidth - 80, x = 40, y = 340;
      renderer.drawRect(x, y, barW, 16);
      renderer.fillRect(x + 2, y + 2, ((barW - 4) * std::min(percent, 100)) / 100, 12);
    }
    renderer.displayBuffer();
    return;
  }

  if (state == State::DONE) {
    renderer.drawCenteredText(UI_10_FONT_ID, 120, "Done", true, EpdFontFamily::BOLD);
    int y = 170;
    for (const auto& line : summary) {
      const std::string l = renderer.truncatedText(UI_10_FONT_ID, line.c_str(), pageWidth - 40);
      renderer.drawText(UI_10_FONT_ID, 20, y, l.c_str());
      y += 30;
    }
    const auto labels = mappedInput.mapLabels(bookPath.empty() ? tr(STR_BACK) : "Read", "Again", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  renderer.drawCenteredText(UI_10_FONT_ID, 260, "Sync stopped", true, EpdFontFamily::BOLD);
  const std::string s = renderer.truncatedText(UI_10_FONT_ID, status.c_str(), pageWidth - 40);
  renderer.drawCenteredText(UI_10_FONT_ID, 300, s.c_str());
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "Again", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
