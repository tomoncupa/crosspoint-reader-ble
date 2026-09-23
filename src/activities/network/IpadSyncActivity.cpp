#include "IpadSyncActivity.h"

#include <ArduinoJson.h>
#include <BluetoothHIDManager.h>
#include <Epub.h>
#include <GfxRenderer.h>
#include <HTTPClient.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <NetworkClient.h>
#include <NetworkClientSecure.h>
#include <StreamString.h>
#include <WiFi.h>
#include <esp_sntp.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <memory>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "ProgressMapper.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/StringUtils.h"

namespace {
constexpr char SETUP_PATH[] = "/ipad-sync.txt";
constexpr char INDEX_PATH[] = "/.crosspoint/ipad-sync.tsv";
constexpr char BOOKS_DIR[] = "/iPad";
constexpr char CACHE_DIR[] = "/.crosspoint";

// ---------- WiFi and time, the same way the KOReader sync does it ----------
void syncTimeWithNTP() {
  if (esp_sntp_enabled()) esp_sntp_stop();
  esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, "pool.ntp.org");
  esp_sntp_init();
  for (int i = 0; i < 50 && sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED; i++) {
    vTaskDelay(100 / portTICK_PERIOD_MS);
  }
}

void wifiOff() {
  if (esp_sntp_enabled()) esp_sntp_stop();
  WiFi.disconnect(false);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);
}

// Milliseconds since 1970, or 0 when the clock was never set
int64_t nowMs() {
  const time_t t = time(nullptr);
  return t > 1700000000 ? static_cast<int64_t>(t) * 1000 : 0;
}

std::string trim(const std::string& s) {
  const auto a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  const auto b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

std::string extOf(const std::string& name) {
  const auto dot = name.rfind('.');
  return dot == std::string::npos ? "" : lower(name.substr(dot + 1));
}

// One path segment, percent-encoded. A key the iPad wrote can hold "%2E", which must
// reach the database as those three characters, so '%' is encoded too.
std::string enc(const std::string& s) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : s) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 15];
    }
  }
  return out;
}

// ---------- plain HTTP calls against the Firebase REST API ----------
struct Http {
  std::unique_ptr<NetworkClient> client;
  HTTPClient http;
  explicit Http(const std::string& url) {
    if (url.rfind("https://", 0) == 0) {
      auto* secure = new NetworkClientSecure();
      secure->setInsecure();
      client.reset(secure);
    } else {
      client.reset(new NetworkClient());
    }
    http.begin(*client, url.c_str());
    http.setTimeout(20000);
    http.addHeader("User-Agent", "CrossPoint-iPadSync");
  }
  ~Http() { http.end(); }
};

int getString(const std::string& url, std::string& out) {
  Http h(url);
  const int code = h.http.GET();
  if (code == HTTP_CODE_OK) {
    StreamString body;
    h.http.writeToStream(&body);
    out = body.c_str();
  }
  return code;
}

int getStream(const std::string& url, Stream& sink) {
  Http h(url);
  const int code = h.http.GET();
  if (code == HTTP_CODE_OK) h.http.writeToStream(&sink);
  return code;
}

int putJson(const std::string& url, const std::string& body) {
  Http h(url);
  h.http.addHeader("Content-Type", "application/json");
  return h.http.PUT(body.c_str());
}

// A book arrives as JSON strings of base64, 1 MB of book per piece. This turns the
// string back into bytes as it streams in, so a piece never has to fit in memory.
class Base64ToFile final : public Stream {
 public:
  explicit Base64ToFile(FsFile& file) : file_(file) {}
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t* buf, size_t n) override {
    for (size_t i = 0; i < n; i++) {
      const uint8_t c = buf[i];
      if (!started_) {
        if (c == ' ' || c == '\r' || c == '\n') continue;
        started_ = true;
        if (c != '"') ok_ = false;  // "null" means the piece is missing, not a book
        continue;
      }
      if (!ok_) continue;
      const int v = value(c);
      if (v < 0) continue;  // the closing quote, escapes, padding
      acc_ = (acc_ << 6) | static_cast<uint32_t>(v);
      bits_ += 6;
      if (bits_ >= 8) {
        bits_ -= 8;
        out_[len_++] = static_cast<uint8_t>((acc_ >> bits_) & 0xFF);
        if (len_ == sizeof(out_)) flushOut();
      }
    }
    return n;
  }
  bool finish() {
    flushOut();
    return ok_ && started_;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}

 private:
  static int value(uint8_t c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  }
  void flushOut() {
    if (!len_) return;
    if (file_.write(out_, len_) != len_) ok_ = false;
    len_ = 0;
  }
  FsFile& file_;
  uint32_t acc_ = 0;
  int bits_ = 0;
  uint8_t out_[512];
  size_t len_ = 0;
  bool started_ = false;
  bool ok_ = true;
};

// ---------- what this device remembers between syncs ----------
// One line per iPad book: its key, where it lives here, the place it was at when last
// synced, and the time on the iPad's row then. Comparing against that tells which side moved.
struct Rec {
  std::string key;
  std::string path;
  int spine = -1;
  int page = -1;
  int64_t t = 0;
};

std::vector<Rec> loadIndex() {
  std::vector<Rec> out;
  FsFile f;
  if (!Storage.openFileForRead("IPS", INDEX_PATH, f)) return out;
  std::string all;
  char buf[256];
  int n;
  while ((n = f.read(buf, sizeof(buf))) > 0) all.append(buf, n);
  f.close();
  size_t pos = 0;
  while (pos < all.size()) {
    size_t end = all.find('\n', pos);
    if (end == std::string::npos) end = all.size();
    const std::string line = all.substr(pos, end - pos);
    pos = end + 1;
    std::vector<std::string> parts;
    size_t a = 0;
    while (true) {
      const size_t b = line.find('\t', a);
      parts.push_back(line.substr(a, b == std::string::npos ? std::string::npos : b - a));
      if (b == std::string::npos) break;
      a = b + 1;
    }
    if (parts.size() < 5 || parts[0].empty()) continue;
    Rec r;
    r.key = parts[0];
    r.path = parts[1];
    r.spine = atoi(parts[2].c_str());
    r.page = atoi(parts[3].c_str());
    r.t = atoll(parts[4].c_str());
    out.push_back(r);
  }
  return out;
}

void saveIndex(const std::vector<Rec>& recs) {
  Storage.mkdir(CACHE_DIR);
  FsFile f;
  if (!Storage.openFileForWrite("IPS", INDEX_PATH, f)) return;
  for (const auto& r : recs) {
    char nums[64];
    snprintf(nums, sizeof(nums), "\t%d\t%d\t%lld\n", r.spine, r.page, static_cast<long long>(r.t));
    const std::string line = r.key + "\t" + r.path + nums;
    f.write(reinterpret_cast<const uint8_t*>(line.data()), line.size());
  }
  f.close();
}

Rec* findRec(std::vector<Rec>& recs, const std::string& key) {
  for (auto& r : recs)
    if (r.key == key) return &r;
  return nullptr;
}

// The reader's own saved place for a book: chapter, page, and pages in that chapter
bool readLocalPlace(const Epub& epub, int& spine, int& page, int& count) {
  FsFile f;
  if (!Storage.openFileForRead("IPS", epub.getCachePath() + "/progress.bin", f)) return false;
  uint8_t d[6];
  const int n = f.read(d, 6);
  f.close();
  if (n != 4 && n != 6) return false;
  spine = d[0] + (d[1] << 8);
  page = d[2] + (d[3] << 8);
  count = n == 6 ? d[4] + (d[5] << 8) : 0;
  return true;
}

void writeLocalPlace(const Epub& epub, int spine, int page, int count) {
  epub.setupCacheDir();
  FsFile f;
  if (!Storage.openFileForWrite("IPS", epub.getCachePath() + "/progress.bin", f)) return;
  uint8_t d[6] = {static_cast<uint8_t>(spine & 0xFF), static_cast<uint8_t>((spine >> 8) & 0xFF),
                  static_cast<uint8_t>(page & 0xFF),  static_cast<uint8_t>((page >> 8) & 0xFF),
                  static_cast<uint8_t>(count & 0xFF), static_cast<uint8_t>((count >> 8) & 0xFF)};
  f.write(d, count > 0 ? 6 : 4);
  f.close();
}
}  // namespace

// ---------- the screen ----------
bool IpadSyncActivity::loadSetup() {
  FsFile f;
  if (!Storage.openFileForRead("IPS", SETUP_PATH, f)) return false;
  std::string all;
  char buf[128];
  int n;
  while ((n = f.read(buf, sizeof(buf))) > 0 && all.size() < 1024) all.append(buf, n);
  f.close();
  const auto nl = all.find('\n');
  dbUrl = trim(all.substr(0, nl));
  syncCode = nl == std::string::npos ? "" : trim(all.substr(nl + 1));
  const auto nl2 = syncCode.find('\n');
  if (nl2 != std::string::npos) syncCode = trim(syncCode.substr(0, nl2));
  while (!dbUrl.empty() && dbUrl.back() == '/') dbUrl.pop_back();
  return dbUrl.rfind("http", 0) == 0 && syncCode.size() >= 8;
}

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
  wifiOff();
  {
    RenderLock lock(*this);
    state = State::FAILED;
    status = why;
  }
  requestUpdate(true);
}

void IpadSyncActivity::onEnter() {
  Activity::onEnter();
  if (!loadSetup()) {
    state = State::NO_SETUP;
    requestUpdate();
    return;
  }
  state = State::CONNECTING;
  if (WiFi.status() == WL_CONNECTED) {
    onWifiReady(true);
    return;
  }
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiReady(!result.isCancelled); });
}

void IpadSyncActivity::onExit() {
  Activity::onExit();
  wifiOff();
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
  syncTimeWithNTP();
  runSync();
}

void IpadSyncActivity::runSync() {
  const std::string base = dbUrl + "/reader/" + enc(syncCode);

  // 1. What the iPad has
  show("Looking at your iPad library...");
  std::string body;
  const int code = getString(base + "/lite.json", body);
  if (code != HTTP_CODE_OK) {
    fail(code == 401 || code == 403 ? "The database refused. Check its rules."
                                    : "Could not reach the database (" + std::to_string(code) + ").");
    return;
  }
  if (trim(body) == "null" || trim(body).empty()) {
    fail("Nothing from the iPad yet. Turn on sync in the iPad reader first.");
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    fail("The iPad library could not be read.");
    return;
  }
  body.clear();
  body.shrink_to_fit();

  struct Remote {
    std::string key, name, title;
    int chunks = 0;
  };
  std::vector<Remote> remote;
  int skippedKinds = 0, notUploaded = 0;
  for (JsonPair kv : doc.as<JsonObject>()) {
    JsonObject b = kv.value().as<JsonObject>();
    if (b["deleted"] | false) continue;
    Remote r;
    r.key = kv.key().c_str();
    r.name = b["name"] | "";
    r.title = b["title"] | "";
    r.chunks = b["chunks"] | 0;
    const std::string ext = extOf(r.name);
    if (ext != "epub" && ext != "txt") {
      skippedKinds++;
      continue;
    }
    remote.push_back(r);
  }
  doc.clear();

  std::vector<Rec> index = loadIndex();

  // 2. Books: find each one here, or fetch it
  int downloaded = 0, downloadFailed = 0;
  int n = 0;
  Storage.mkdir(BOOKS_DIR);
  for (const auto& r : remote) {
    n++;
    Rec* rec = findRec(index, r.key);
    if (rec && Storage.exists(rec->path.c_str())) continue;
    // Copied over by hand under its own name?
    std::string found;
    for (const std::string& dir : {std::string(""), std::string(BOOKS_DIR)}) {
      const std::string p = dir + "/" + r.name;
      if (!r.name.empty() && Storage.exists(p.c_str())) {
        found = p;
        break;
      }
    }
    if (found.empty()) {
      if (r.chunks <= 0) {
        notUploaded++;
        continue;
      }
      const std::string ext = extOf(r.name);
      const std::string stem = r.name.substr(0, r.name.size() - ext.size() - 1);
      const std::string dest = std::string(BOOKS_DIR) + "/" + StringUtils::sanitizeFilename(stem) + "." + ext;
      const std::string part = dest + ".part";
      const std::string label = r.title.empty() ? r.name : r.title;
      FsFile f;
      bool ok = Storage.openFileForWrite("IPS", part, f);
      if (ok) {
        Base64ToFile sink(f);
        for (int c = 0; c < r.chunks && ok; c++) {
          char line[48];
          snprintf(line, sizeof(line), "Book %d of %d", n, static_cast<int>(remote.size()));
          show(std::string("Downloading ") + line, label, (c * 100) / r.chunks);
          const int got = getStream(base + "/files/" + enc(r.key) + "/" + std::to_string(c) + ".json", sink);
          ok = got == HTTP_CODE_OK;
        }
        ok = sink.finish() && ok;
        f.close();
      }
      if (!ok) {
        Storage.remove(part.c_str());
        downloadFailed++;
        continue;
      }
      if (Storage.exists(dest.c_str())) Storage.remove(dest.c_str());
      Storage.rename(part.c_str(), dest.c_str());
      if (ext == "epub") Epub(dest, CACHE_DIR).clearCache();
      found = dest;
      downloaded++;
    }
    if (rec) {
      rec->path = found;
    } else {
      Rec fresh;
      fresh.key = r.key;
      fresh.path = found;
      index.push_back(fresh);
    }
    saveIndex(index);
  }

  // 3. Places: whichever side moved since the last sync wins; if both did, the further one
  int pulled = 0, pushed = 0, unstamped = 0;
  n = 0;
  for (auto& rec : index) {
    n++;
    if (extOf(rec.path) != "epub" || !Storage.exists(rec.path.c_str())) continue;
    show("Matching reading places...", "", (n * 100) / std::max<int>(1, index.size()));

    std::string posBody;
    const std::string posUrl = base + "/pos/" + enc(rec.key) + ".json";
    const int pcode = getString(posUrl, posBody);
    if (pcode != HTTP_CODE_OK) continue;
    bool hasRemote = false;
    float remoteP = 0;
    int64_t remoteT = 0;
    if (trim(posBody) != "null") {
      JsonDocument pd;
      if (!deserializeJson(pd, posBody)) {
        hasRemote = pd["p"].is<float>() || pd["p"].is<int>();
        remoteP = pd["p"] | 0.0f;
        remoteT = pd["t"] | static_cast<int64_t>(0);
      }
    }

    auto epub = std::make_shared<Epub>(rec.path, CACHE_DIR);
    int spine = 0, page = 0, count = 0;
    const bool hasLocal = readLocalPlace(*epub, spine, page, count);
    if (!hasLocal && !hasRemote) continue;
    if (!epub->load(true, true)) continue;

    const bool localMoved = hasLocal && (spine != rec.spine || page != rec.page);
    const bool remoteMoved = hasRemote && remoteT > rec.t;
    const float localP = hasLocal ? epub->calculateProgress(spine, count > 0 ? static_cast<float>(page) / count : 0)
                                  : 0.0f;

    bool pull = remoteMoved && !localMoved;
    bool push = localMoved && !remoteMoved;
    if (remoteMoved && localMoved) {
      pull = remoteP > localP;
      push = !pull;
    }

    if (pull) {
      KOReaderPosition ko{"", std::max(0.0f, std::min(1.0f, remoteP))};
      const CrossPointPosition cp =
          ProgressMapper::toCrossPoint(epub, ko, hasLocal ? spine : -1, hasLocal ? count : 0);
      writeLocalPlace(*epub, cp.spineIndex, cp.pageNumber, cp.totalPages);
      rec.spine = cp.spineIndex;
      rec.page = cp.pageNumber;
      rec.t = remoteT;
      pulled++;
    } else if (push) {
      const int64_t t = nowMs();
      if (t == 0) {
        unstamped++;
        continue;
      }
      char json[96];
      snprintf(json, sizeof(json), "{\"p\":%.5f,\"t\":%lld,\"src\":\"x4\"}", localP, static_cast<long long>(t));
      if (putJson(posUrl, json) == HTTP_CODE_OK) {
        rec.spine = spine;
        rec.page = page;
        rec.t = t;
        pushed++;
      }
    } else {
      if (hasLocal) {
        rec.spine = spine;
        rec.page = page;
      }
      rec.t = std::max(rec.t, remoteT);
    }
    saveIndex(index);
  }

  // 4. Off again straight away: WiFi is the expensive part of this
  wifiOff();

  std::vector<std::string> lines;
  auto add = [&lines](int count, const char* one, const char* many) {
    if (count <= 0) return;
    char buf[96];
    snprintf(buf, sizeof(buf), count == 1 ? one : many, count);
    lines.emplace_back(buf);
  };
  add(downloaded, "%d book downloaded to /iPad", "%d books downloaded to /iPad");
  add(pulled, "%d place brought over from the iPad", "%d places brought over from the iPad");
  add(pushed, "%d place sent to the iPad", "%d places sent to the iPad");
  add(downloadFailed, "%d book failed to download", "%d books failed to download");
  add(notUploaded, "%d book not uploaded from the iPad yet", "%d books not uploaded from the iPad yet");
  add(skippedKinds, "%d PDF or Kindle book left on the iPad", "%d PDF or Kindle books left on the iPad");
  add(unstamped, "%d place not sent: clock not set", "%d places not sent: clock not set");
  if (lines.empty()) lines.emplace_back("Everything already matches.");

  {
    RenderLock lock(*this);
    summary = std::move(lines);
    state = State::DONE;
  }
  requestUpdate(true);
}

void IpadSyncActivity::loop() {
  if (state == State::SYNCING || state == State::CONNECTING) return;
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome();
    return;
  }
  if ((state == State::DONE || state == State::FAILED) &&
      mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    // Sync again
    summary.clear();
    state = State::CONNECTING;
    requestUpdate();
    if (WiFi.status() == WL_CONNECTED) {
      onWifiReady(true);
    } else {
      startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                             [this](const ActivityResult& result) { onWifiReady(!result.isCancelled); });
    }
  }
}

void IpadSyncActivity::render(RenderLock&&) {
  const int pageWidth = renderer.getScreenWidth();
  renderer.clearScreen();
  renderer.drawCenteredText(UI_12_FONT_ID, 15, "iPad Sync", true, EpdFontFamily::BOLD);

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
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "Again", "", "");
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
