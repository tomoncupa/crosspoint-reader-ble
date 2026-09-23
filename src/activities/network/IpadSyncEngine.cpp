#include "IpadSyncEngine.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Epub.h>
#include <HTTPClient.h>
#include <HalStorage.h>
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

#include "CrossPointState.h"
#include "ProgressMapper.h"
#include "RecentBooksStore.h"
#include "WifiCredentialStore.h"
#include "util/StringUtils.h"

namespace IpadSync {
namespace {
constexpr char SETUP_PATH[] = "/ipad-sync.txt";
constexpr char INDEX_PATH[] = "/.crosspoint/ipad-sync.tsv";
constexpr char LOG_PATH[] = "/.crosspoint/ipad-log.tsv";
constexpr char FINISHED_PATH[] = "/.crosspoint/ipad-finished.txt";
constexpr char TZ_PATH[] = "/.crosspoint/ipad-tz.txt";
constexpr char BOOKS_DIR[] = "";  // the top of the SD card, where the X4 keeps its own downloads
constexpr char CACHE_DIR[] = "/.crosspoint";
constexpr unsigned long COUNTED_GAP_MS = 180000;  // the iPad counts gaps under 3 minutes too

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

std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> parts;
  size_t a = 0;
  while (true) {
    const size_t b = s.find(sep, a);
    parts.push_back(s.substr(a, b == std::string::npos ? std::string::npos : b - a));
    if (b == std::string::npos) break;
    a = b + 1;
  }
  return parts;
}

std::string readSmallFile(const char* path, size_t cap = 65536) {
  FsFile f;
  std::string all;
  if (!Storage.openFileForRead("IPS", path, f)) return all;
  char buf[256];
  int n;
  while ((n = f.read(buf, sizeof(buf))) > 0 && all.size() < cap) all.append(buf, n);
  f.close();
  return all;
}

void writeSmallFile(const char* path, const std::string& body) {
  Storage.mkdir(CACHE_DIR);
  FsFile f;
  if (!Storage.openFileForWrite("IPS", path, f)) return;
  f.write(reinterpret_cast<const uint8_t*>(body.data()), body.size());
  f.close();
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

// A JSON string, quoted
std::string jstr(const std::string& s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += static_cast<char>(c);
    } else if (c < 0x20) {
      char b[8];
      snprintf(b, sizeof(b), "\\u%04x", c);
      out += b;
    } else {
      out += static_cast<char>(c);
    }
  }
  return out + "\"";
}

// ---------- days, in the iPad's time zone ----------
int tzCache = -100000;
int tzMinutes() {
  if (tzCache == -100000) {
    const std::string s = trim(readSmallFile(TZ_PATH, 32));
    tzCache = s.empty() ? 480 : atoi(s.c_str());  // Manila until the iPad says otherwise
  }
  return tzCache;
}

// "2026-09-23", or "0" while the clock is not set (the minutes then count on the day they sync)
std::string dayKey() {
  time_t t = time(nullptr);
  if (t < 1700000000) return "0";
  t += static_cast<time_t>(tzMinutes()) * 60;
  struct tm tm;
  gmtime_r(&t, &tm);
  char b[16];
  snprintf(b, sizeof(b), "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
  return b;
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
  if (code == HTTP_CODE_OK && h.http.writeToStream(&sink) < 0) return -1;  // cut off part way
  return code;
}

int sendJson(const std::string& url, const char* method, const std::string& body) {
  Http h(url);
  h.http.addHeader("Content-Type", "application/json");
  return h.http.sendRequest(method, body.c_str());
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
  // Each piece is its own JSON string with its own padding, so the decoder starts clean
  // for every piece. Carrying leftover bits across would corrupt everything after 1 MB.
  void nextPiece() {
    acc_ = 0;
    bits_ = 0;
    started_ = false;
  }
  bool finish() {
    flushOut();
    return ok_ && started_;
  }
  bool pieceOk() const { return ok_ && started_; }
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
  for (const std::string& line : split(readSmallFile(INDEX_PATH, 1 << 20), '\n')) {
    const auto parts = split(line, '\t');
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
  std::string all;
  for (const auto& r : recs) {
    char nums[64];
    snprintf(nums, sizeof(nums), "\t%d\t%d\t%lld\n", r.spine, r.page, static_cast<long long>(r.t));
    all += r.key + "\t" + r.path + nums;
  }
  writeSmallFile(INDEX_PATH, all);
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

// ---------- minutes read here, one line per day: day, ms, pages ----------
struct LogRow {
  std::string day;
  int64_t ms = 0;
  int pages = 0;
};

std::vector<LogRow> loadLog() {
  std::vector<LogRow> rows;
  for (const std::string& line : split(readSmallFile(LOG_PATH), '\n')) {
    const auto p = split(line, '\t');
    if (p.size() < 3 || p[0].empty()) continue;
    rows.push_back({p[0], atoll(p[1].c_str()), atoi(p[2].c_str())});
  }
  return rows;
}

void saveLog(const std::vector<LogRow>& rows) {
  if (rows.empty()) {
    Storage.remove(LOG_PATH);
    return;
  }
  std::string all;
  for (const auto& r : rows) {
    char nums[48];
    snprintf(nums, sizeof(nums), "\t%lld\t%d\n", static_cast<long long>(r.ms), r.pages);
    all += r.day + nums;
  }
  writeSmallFile(LOG_PATH, all);
}

unsigned long lastTurnMs = 0;

void removeBookHere(const std::string& path) {
  if (extOf(path) == "epub") Epub(path, CACHE_DIR).clearCache();
  Storage.remove(path.c_str());
  RECENT_BOOKS.removeBook(path);
  if (APP_STATE.openEpubPath == path) {
    APP_STATE.openEpubPath = "";
    APP_STATE.saveToFile();
  }
}
}  // namespace

// ---------- setup, WiFi and time ----------
bool loadSetup(Setup& out) {
  const std::string all = readSmallFile(SETUP_PATH, 1024);
  const auto lines = split(all, '\n');
  out.url = lines.empty() ? "" : trim(lines[0]);
  out.code = lines.size() < 2 ? "" : trim(lines[1]);
  while (!out.url.empty() && out.url.back() == '/') out.url.pop_back();
  return out.url.rfind("http", 0) == 0 && out.code.size() >= 8;
}

bool hasSetup() {
  Setup s;
  return loadSetup(s);
}

void syncClock() {
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

bool connectSavedWifi(const unsigned long timeoutMs) {
  WIFI_STORE.loadFromFile();
  const std::string ssid = WIFI_STORE.getLastConnectedSsid();
  if (ssid.empty()) return false;
  const auto* cred = WIFI_STORE.findCredential(ssid);
  if (!cred) return false;
  WiFi.mode(WIFI_STA);
  if (cred->password.empty()) {
    WiFi.begin(ssid.c_str());
  } else {
    WiFi.begin(ssid.c_str(), cred->password.c_str());
  }
  const unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) delay(100);
  return WiFi.status() == WL_CONNECTED;
}

// ---------- counted by the reader ----------
void notePageTurn() {
  const unsigned long now = millis();
  const unsigned long gap = lastTurnMs ? now - lastTurnMs : 0;
  lastTurnMs = now;
  auto rows = loadLog();
  const std::string day = dayKey();
  LogRow* row = nullptr;
  for (auto& r : rows)
    if (r.day == day) row = &r;
  if (!row) {
    rows.push_back({day, 0, 0});
    row = &rows.back();
  }
  if (gap > 0 && gap < COUNTED_GAP_MS) row->ms += gap;
  row->pages++;
  saveLog(rows);
}

void noteFinished(const std::string& path) {
  const std::string all = readSmallFile(FINISHED_PATH);
  for (const auto& line : split(all, '\n'))
    if (line == path) return;
  writeSmallFile(FINISHED_PATH, all + path + "\n");
}

void noteNotFinished(const std::string& path) {
  const std::string all = readSmallFile(FINISHED_PATH);
  if (all.empty()) return;
  std::string keep;
  for (const auto& line : split(all, '\n'))
    if (!line.empty() && line != path) keep += line + "\n";
  if (keep.size() == all.size()) return;
  if (keep.empty()) {
    Storage.remove(FINISHED_PATH);
  } else {
    writeSmallFile(FINISHED_PATH, keep);
  }
}

// ---------- the sync ----------
Outcome run(const Setup& setup, const Options& options) {
  Outcome out;
  const std::string base = setup.url + "/reader/" + enc(setup.code);
  const bool whole = options.onlyPath.empty();
  auto show = [&options](const std::string& s, const std::string& d = "", int p = -1) {
    if (options.progress) options.progress(s, d, p);
  };
  auto fail = [&out](const std::string& why) {
    out.ok = false;
    out.error = why;
    return out;
  };
  auto stop = [&options, &out]() {
    if (out.stopped) return true;
    if (!options.shouldStop || !options.shouldStop()) return false;
    out.stopped = true;
    return true;
  };

  // The iPad's time zone, so "today" here is today there
  {
    std::string tz;
    if (getString(base + "/cfg/tz.json", tz) == HTTP_CODE_OK && trim(tz) != "null" && !trim(tz).empty()) {
      tzCache = atoi(trim(tz).c_str());
      writeSmallFile(TZ_PATH, std::to_string(tzCache));
    }
  }

  std::vector<Rec> index = loadIndex();
  const int64_t now = nowMs();

  // 1. What the iPad has
  struct Remote {
    std::string key, name, title;
    int chunks = 0;
    bool finished = false;
    int64_t pri = 0;
  };
  std::vector<Remote> remote;
  int skippedKinds = 0, notUploaded = 0;
  if (whole) {
    show("Looking at your iPad library...");
    std::string body;
    const int code = getString(base + "/lite.json", body);
    if (code != HTTP_CODE_OK) {
      return fail(code == 401 || code == 403 ? "The database refused. Check its rules."
                                             : "Could not reach the database (" + std::to_string(code) + ").");
    }
    if (trim(body) == "null" || trim(body).empty()) return fail("Nothing from the iPad yet. Turn on sync there first.");
    JsonDocument doc;
    if (deserializeJson(doc, body)) return fail("The iPad library could not be read.");
    body.clear();
    body.shrink_to_fit();
    for (JsonPair kv : doc.as<JsonObject>()) {
      JsonObject b = kv.value().as<JsonObject>();
      if (b["deleted"] | false) continue;
      Remote r;
      r.key = kv.key().c_str();
      r.name = b["name"] | "";
      r.title = b["title"] | "";
      r.chunks = b["chunks"] | 0;
      r.finished = b["finished"] | false;
      r.pri = b["pri"] | static_cast<int64_t>(0);
      const std::string ext = extOf(r.name);
      if (ext != "epub" && ext != "txt") {
        skippedKinds++;
        continue;
      }
      remote.push_back(r);
    }
  }

  if (stop()) return fail("Stopped.");

  // 2. Books finished here: marked finished on the iPad too
  int finishedSent = 0;
  {
    std::string keep;
    for (const auto& path : split(readSmallFile(FINISHED_PATH), '\n')) {
      if (path.empty()) continue;
      const Rec* rec = nullptr;
      for (const auto& r : index)
        if (r.path == path) rec = &r;
      if (!rec) continue;  // not an iPad book
      if (now == 0) {
        keep += path + "\n";
        continue;
      }
      char json[64];
      snprintf(json, sizeof(json), "{\"finished\":true,\"t\":%lld}", static_cast<long long>(now));
      if (sendJson(base + "/meta/" + enc(rec->key) + ".json", "PATCH", json) == HTTP_CODE_OK &&
          sendJson(base + "/lite/" + enc(rec->key) + ".json", "PATCH", json) == HTTP_CODE_OK) {
        finishedSent++;
        for (auto& r : remote)
          if (r.key == rec->key) r.finished = true;
      } else {
        keep += path + "\n";
      }
    }
    if (keep.empty()) {
      Storage.remove(FINISHED_PATH);
    } else {
      writeSmallFile(FINISHED_PATH, keep);
    }
  }

  // 3. Books finished on either side leave this device; the iPad keeps them
  int removed = 0;
  if (whole) {
    for (const auto& r : remote) {
      if (!r.finished) continue;
      for (size_t i = 0; i < index.size(); i++) {
        if (index[i].key != r.key) continue;
        if (Storage.exists(index[i].path.c_str())) {
          removeBookHere(index[i].path);
          removed++;
        }
        index.erase(index.begin() + i);
        saveIndex(index);
        break;
      }
    }
  }

  // 4. Books: find each one here, or fetch it. "Send to X4 first" books go first.
  int downloaded = 0, downloadFailed = 0;
  if (whole) {
    std::stable_sort(remote.begin(), remote.end(), [](const Remote& a, const Remote& b) { return a.pri > b.pri; });
    int n = 0;
    for (const auto& r : remote) {
      n++;
      if (stop()) return fail("Stopped.");
      if (r.finished) continue;
      Rec* rec = findRec(index, r.key);
      if (rec && Storage.exists(rec->path.c_str())) continue;
      // Copied over by hand under its own name?
      std::string found;
      for (const std::string& dir : {std::string(""), std::string("/iPad")}) {
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
        // Two iPad names can clean up to the same file name here; never write over another book
        const std::string clean = std::string(BOOKS_DIR) + "/" + StringUtils::sanitizeFilename(stem);
        std::string dest = clean + "." + ext;
        for (int k = 2; Storage.exists(dest.c_str()) && k < 100; k++) dest = clean + " (" + std::to_string(k) + ")." + ext;
        const std::string part = dest + ".part";
        const std::string label = r.title.empty() ? r.name : r.title;
        FsFile f;
        bool ok = Storage.openFileForWrite("IPS", part, f);
        if (ok) {
          Base64ToFile sink(f);
          for (int c = 0; c < r.chunks && ok && !stop(); c++) {
            char line[48];
            snprintf(line, sizeof(line), "Book %d of %d", n, static_cast<int>(remote.size()));
            show(std::string("Downloading ") + line, label, (c * 100) / r.chunks);
            sink.nextPiece();
            const int got = getStream(base + "/files/" + enc(r.key) + "/" + std::to_string(c) + ".json", sink);
            ok = got == HTTP_CODE_OK && sink.pieceOk();
          }
          ok = sink.finish() && ok && !out.stopped;
          f.close();
        }
        if (!ok) {
          Storage.remove(part.c_str());
          downloadFailed++;
          continue;
        }
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
  }

  if (stop()) return fail("Stopped.");

  // 5. Places: whichever side moved since the last sync wins; if both did, the further one
  show("Matching reading places...");
  JsonDocument posDoc;
  {
    std::string posBody;
    std::string url = base + "/pos.json";
    if (!whole) {
      const Rec* only = nullptr;
      for (const auto& r : index)
        if (r.path == options.onlyPath) only = &r;
      if (!only) return fail("This book did not come from the iPad.");
      url = base + "/pos/" + enc(only->key) + ".json";
    }
    const int code = getString(url, posBody);
    if (code != HTTP_CODE_OK) return fail("Could not reach the database (" + std::to_string(code) + ").");
    if (trim(posBody) != "null") deserializeJson(posDoc, posBody);
  }
  int pulled = 0, pushed = 0, unstamped = 0;
  std::string x4Rows;  // where this device is now, for "Last read on X4" on the iPad
  for (auto& rec : index) {
    if (!whole && rec.path != options.onlyPath) continue;
    if (extOf(rec.path) != "epub" || !Storage.exists(rec.path.c_str())) continue;
    if (stop()) break;

    JsonVariant rp = whole ? posDoc[rec.key.c_str()].as<JsonVariant>() : posDoc.as<JsonVariant>();
    const bool hasRemote = rp["p"].is<float>() || rp["p"].is<int>();
    const float remoteP = rp["p"] | 0.0f;
    const int64_t remoteT = rp["t"] | static_cast<int64_t>(0);

    std::shared_ptr<Epub> epub =
        options.openEpub && !whole ? options.openEpub : std::make_shared<Epub>(rec.path, CACHE_DIR);
    int spine = 0, page = 0, count = 0;
    const bool hasLocal = readLocalPlace(*epub, spine, page, count);
    if (!hasLocal && !hasRemote) continue;

    const bool localMoved = hasLocal && (spine != rec.spine || page != rec.page);
    const bool remoteMoved = hasRemote && remoteT > rec.t;
    if (!localMoved && !remoteMoved) {
      rec.t = std::max(rec.t, remoteT);
      continue;  // nothing to do, and no need to open the book
    }
    if (epub != options.openEpub && !epub->load(true, true)) continue;
    const float localP =
        hasLocal ? epub->calculateProgress(spine, count > 0 ? static_cast<float>(page) / count : 0) : 0.0f;

    bool pull = remoteMoved && !localMoved;
    bool push = localMoved && !remoteMoved;
    if (remoteMoved && localMoved) {
      pull = remoteP > localP;
      push = !pull;
    }

    if (pull) {
      KOReaderPosition ko{"", std::max(0.0f, std::min(1.0f, remoteP))};
      const CrossPointPosition cp = ProgressMapper::toCrossPoint(epub, ko, hasLocal ? spine : -1, hasLocal ? count : 0);
      writeLocalPlace(*epub, cp.spineIndex, cp.pageNumber, cp.totalPages);
      rec.spine = cp.spineIndex;
      rec.page = cp.pageNumber;
      rec.t = remoteT;
      pulled++;
    } else if (push) {
      if (now == 0) {
        unstamped++;
        continue;
      }
      char json[96];
      snprintf(json, sizeof(json), "{\"p\":%.5f,\"t\":%lld,\"src\":\"x4\"}", localP, static_cast<long long>(now));
      if (sendJson(base + "/pos/" + enc(rec.key) + ".json", "PUT", json) == HTTP_CODE_OK) {
        rec.spine = spine;
        rec.page = page;
        rec.t = now;
        pushed++;
      }
    }
    if (localMoved && now != 0) {
      char row[80];
      snprintf(row, sizeof(row), ":{\"p\":%.5f,\"t\":%lld}", pull ? remoteP : localP, static_cast<long long>(now));
      x4Rows += (x4Rows.empty() ? "" : ",") + jstr(rec.key) + row;
    }
    saveIndex(index);
  }
  saveIndex(index);
  if (!x4Rows.empty()) sendJson(base + "/x4.json", "PATCH", "{" + x4Rows + "}");

  // 6. Minutes read here, added to the day's row
  int minutesSent = 0;
  {
    auto rows = loadLog();
    std::vector<LogRow> keep;
    const std::string today = dayKey();
    for (const auto& r : rows) {
      const std::string day = r.day == "0" ? today : r.day;
      if (day == "0") {
        keep.push_back(r);
        continue;
      }
      const std::string url = base + "/log/" + enc(day) + "/x4.json";
      std::string had;
      if (getString(url, had) != HTTP_CODE_OK) {
        keep.push_back(r);
        continue;
      }
      int64_t ms = r.ms;
      int pages = r.pages;
      JsonDocument hd;
      if (trim(had) != "null" && !deserializeJson(hd, had)) {
        ms += hd["ms"] | static_cast<int64_t>(0);
        pages += hd["pages"] | 0;
      }
      char json[96];
      snprintf(json, sizeof(json), "{\"ms\":%lld,\"pages\":%d,\"chars\":0,\"t\":%lld}", static_cast<long long>(ms),
               pages, static_cast<long long>(now));
      if (sendJson(url, "PUT", json) == HTTP_CODE_OK) {
        minutesSent += static_cast<int>(r.ms / 60000);
      } else {
        keep.push_back(r);
      }
    }
    saveLog(keep);
  }

  auto add = [&out](int count, const char* one, const char* many) {
    if (count <= 0) return;
    char buf[96];
    snprintf(buf, sizeof(buf), count == 1 ? one : many, count);
    out.lines.emplace_back(buf);
  };
  add(downloaded, "%d book downloaded", "%d books downloaded");
  add(pulled, "%d place brought over from the iPad", "%d places brought over from the iPad");
  add(pushed, "%d place sent to the iPad", "%d places sent to the iPad");
  add(finishedSent, "%d book marked finished on the iPad", "%d books marked finished on the iPad");
  add(removed, "%d finished book removed from here", "%d finished books removed from here");
  add(minutesSent, "%d minute of reading sent", "%d minutes of reading sent");
  add(downloadFailed, "%d book failed to download", "%d books failed to download");
  add(notUploaded, "%d book not uploaded from the iPad yet", "%d books not uploaded from the iPad yet");
  add(skippedKinds, "%d PDF or Kindle book left on the iPad", "%d PDF or Kindle books left on the iPad");
  add(unstamped, "%d place not sent: clock not set", "%d places not sent: clock not set");
  if (out.lines.empty()) out.lines.emplace_back("Everything already matches.");
  out.ok = true;
  return out;
}

}  // namespace IpadSync
