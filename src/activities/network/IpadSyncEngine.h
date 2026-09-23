#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

class Epub;

/**
 * The iPad Sync itself, with no screen of its own, so it can run from the iPad Sync
 * screen, from the reader menu for one book, and with the screen off while charging.
 *
 * Everything lives under reader/<sync code>/ in the Firebase database the iPad reader
 * syncs to:
 *   lite/<key>      the iPad's book list (name, pieces, finished, "send first" time)
 *   files/<key>/<n> the book, in 1 MB pieces of base64
 *   pos/<key>       the latest place, as a share of the book {p, t, src}
 *   x4/<key>        where this device was in the book when it last synced {p, t}
 *   log/<day>/x4    minutes and pages read here that day {ms, pages}
 *   cfg/tz          the iPad's minutes from UTC, so days here match days there
 */
namespace IpadSync {

struct Setup {
  std::string url;
  std::string code;
};

// /ipad-sync.txt at the top of the SD card: line 1 the database address, line 2 the code
bool loadSetup(Setup& out);
bool hasSetup();

using Progress = std::function<void(const std::string& status, const std::string& detail, int percent)>;

struct Options {
  // Only this book's place (the reader menu). Empty for the whole library.
  std::string onlyPath;
  // The book as the reader already has it open, so it is not loaded twice
  std::shared_ptr<Epub> openEpub;
  Progress progress;
  // Asked between steps and between book pieces; true stops the sync where it is
  std::function<bool()> shouldStop;
};

struct Outcome {
  bool ok = false;
  std::string error;
  std::vector<std::string> lines;
  bool stopped = false;
};

// WiFi must already be connected
Outcome run(const Setup& setup, const Options& options);

void syncClock();
void wifiOff();
// The network this device joined last, with its saved password; no screen
bool connectSavedWifi(unsigned long timeoutMs);

// Called by the reader: a page was turned, counts toward today's minutes
void notePageTurn();
// Called by the reader: this book reached its end screen
void noteFinished(const std::string& path);
// Called by the reader: it paged back off the end screen, so that was a slip
void noteNotFinished(const std::string& path);

}  // namespace IpadSync
