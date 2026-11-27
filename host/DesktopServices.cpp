#include "DesktopServices.h"

#include <dirent.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <openssl/pkcs12.h>
#include <openssl/rand.h>
#include <openssl/x509.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

#include "../sdk/dessen_abi.h"

#include <FreeInkUICore.h>

namespace dessen {
void DesktopServices::drawButtonHints(freeink::ui::DrawTarget& target, const freeink::ui::Rect& band,
                                      const char* const labels[4]) {
  using namespace freeink::ui;
  for (int i = 0; i < 4; ++i) {
    if (!labels[i][0]) continue;
    const int16_t left = static_cast<int16_t>(band.x + i * band.width / 4);
    const int16_t right = static_cast<int16_t>(band.x + (i + 1) * band.width / 4);
    const Rect box{left, band.y, static_cast<int16_t>(right - left), band.height};
    target.stroke(box, Paint::solid(Color::Black), 1, 0, 0);
    TextStyle style;
    style.font = DSN_FONT_SMALL;
    style.align = TextAlign::Center;
    const int16_t line = target.lineHeight(style.font);
    target.text(Rect{box.x, static_cast<int16_t>(box.y + (box.height - line) / 2), box.width, line}, labels[i], style);
  }
}

namespace {

// A completed crypto result goes into the app buffer with this guard.
int give(const uint8_t* src, size_t n, uint8_t* out, uint32_t cap) {
  if (n > cap) return DSN_ERR_TOO_BIG;
  if (n) memcpy(out, src, n);
  return static_cast<int>(n);
}

struct Fd {
  int value;
  ~Fd() { if (value >= 0) close(value); }
};

// Walk relative to open directories so a symlink cannot redirect a guest path.
int openParent(const std::string& root, std::string_view path, std::string& leaf) {
  if (root.empty() || path.empty() || path.front() != '/') return -1;
  path.remove_prefix(1);
  if (!path.empty() && path.back() == '/') path.remove_suffix(1);
  if (path.empty()) leaf = ".";
  Fd dir{open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
  if (dir.value < 0) return -1;
  while (!path.empty()) {
    const auto slash = path.find('/');
    const auto part = path.substr(0, slash);
    if (part.empty() || part == "." || part == "..") return -1;
    if (slash == std::string_view::npos) {
      leaf = part;
      break;
    }
    const int next = openat(dir.value, std::string(part).c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (next < 0) return -1;
    close(dir.value);
    dir.value = next;
    path.remove_prefix(slash + 1);
  }
  const int result = dir.value;
  dir.value = -1;
  return result;
}

FILE* openRegular(int parent, const char* leaf) {
  const int fd = openat(parent, leaf, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return nullptr;
  struct stat st;
  if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) {
    if (FILE* file = fdopen(fd, "rb")) return file;
  }
  close(fd);
  return nullptr;
}

int readBounded(FILE* file, std::vector<uint8_t>& data, uint32_t cap) {
  data.resize(static_cast<size_t>(cap) + 1);
  const size_t n = fread(data.data(), 1, data.size(), file);
  const bool readOk = !ferror(file);
  const bool closeOk = fclose(file) == 0;
  if (!readOk || !closeOk) return DSN_ERR_IO;
  if (n > cap) return DSN_ERR_TOO_BIG;
  data.resize(n);
  return static_cast<int>(n);
}

template <typename Write>
int replaceFile(const std::string& root, const char* path, Write write) {
  std::string leaf;
  const Fd dir{openParent(root, path, leaf)};
  if (dir.value < 0 || leaf == "." || path[strlen(path) - 1] == '/') return DSN_ERR_IO;
  struct stat st;
  if (fstatat(dir.value, leaf.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0) {
    if (!S_ISREG(st.st_mode)) return DSN_ERR_IO;
  } else if (errno != ENOENT) {
    return DSN_ERR_IO;
  }

  char temp[32];
  int fd;
  do {
    uint64_t id;
    if (RAND_bytes(reinterpret_cast<uint8_t*>(&id), sizeof id) != 1) return DSN_ERR_IO;
    snprintf(temp, sizeof temp, ".dessen-%016llx", static_cast<unsigned long long>(id));
    fd = openat(dir.value, temp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  } while (fd < 0 && errno == EEXIST);
  if (fd < 0) return DSN_ERR_IO;

  FILE* file = fdopen(fd, "wb");
  if (file) {
    const int result = write(file);
    const bool flushed = result >= 0 && fflush(file) == 0 && fsync(fd) == 0;
    const bool closeOk = fclose(file) == 0;
    if (flushed && closeOk && renameat(dir.value, temp, dir.value, leaf.c_str()) == 0) return result;
  } else {
    close(fd);
  }
  unlinkat(dir.value, temp, 0);
  return DSN_ERR_IO;
}

std::string fixturePath(std::string_view url) {
  const auto scheme = url.find("://");
  if (scheme != std::string_view::npos) url.remove_prefix(scheme + 3);
  if (url.empty() || url.front() == '/' || url.back() == '/') return {};
  return "/" + std::string(url);
}

struct Evp {
  EVP_PKEY_CTX* c = nullptr;
  ~Evp() { if (c) EVP_PKEY_CTX_free(c); }
};

EVP_PKEY* loadPriv(const uint8_t* der, uint32_t n) {
  const uint8_t* p = der;
  return d2i_AutoPrivateKey(nullptr, &p, n); // accepts PKCS#8 and legacy DER
}

EVP_PKEY* loadPub(const uint8_t* der, uint32_t n) {
  const uint8_t* p = der; // try an X.509 cert first, then a bare SPKI public key
  if (X509* x = d2i_X509(nullptr, &p, n)) {
    EVP_PKEY* k = X509_get_pubkey(x);
    X509_free(x);
    return k;
  }
  p = der;
  return d2i_PUBKEY(nullptr, &p, n);
}

const EVP_MD* mdFor(int algo) {
  switch (algo & 0x7f) {
    case DSN_HASH_SHA1: return EVP_sha1();
    case DSN_HASH_SHA256: return EVP_sha256();
    default: return nullptr;
  }
}

}  // namespace

DesktopServices::DesktopServices(std::string fsRoot, std::string appDir, std::string netRoot)
    : fsRoot_(std::move(fsRoot)), appDir_(std::move(appDir)), netRoot_(std::move(netRoot)) {}

// ---- core ----
uint32_t DesktopServices::timeNow() { return std::getenv("DESSEN_TEST_NO_TIME") ? 0 : static_cast<uint32_t>(::time(nullptr)); }

int DesktopServices::clock(uint8_t* out) {
  if (std::getenv("DESSEN_TEST_NO_CLOCK") || std::getenv("DESSEN_TEST_NO_TIME")) return DSN_ERR_UNAVAILABLE;
  const time_t now = ::time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  const unsigned year = static_cast<unsigned>(t.tm_year + 1900);
  out[0] = year & 0xff;
  out[1] = (year >> 8) & 0xff;
  out[2] = static_cast<uint8_t>(t.tm_mon + 1);
  out[3] = static_cast<uint8_t>(t.tm_mday);
  out[4] = static_cast<uint8_t>(t.tm_hour);
  out[5] = static_cast<uint8_t>(t.tm_min);
  out[6] = static_cast<uint8_t>(t.tm_sec);
  out[7] = static_cast<uint8_t>(t.tm_wday);
  return 0;
}
bool DesktopServices::randomBytes(uint8_t* out, uint32_t n) { return RAND_bytes(out, static_cast<int>(n)) == 1; }
int DesktopServices::deviceId(char* out, uint32_t cap) {
  static const char* kId = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
  const size_t n = strlen(kId);
  if (n + 1 > cap) return DSN_ERR_TOO_BIG;
  memcpy(out, kId, n + 1);
  return static_cast<int>(n);
}

int DesktopServices::locale(char* out, uint32_t cap) {
  const char* code = getenv("DESSEN_LOCALE");
  if (!code || !*code) code = "en";
  const size_t n = strlen(code);
  if (n + 1 > cap) return DSN_ERR_TOO_BIG;
  memcpy(out, code, n + 1);
  return static_cast<int>(n);
}

// ---- storage ----
int DesktopServices::fsRead(const char* path, uint8_t* out, uint32_t cap) {
  if (fsRoot_.empty()) return DSN_ERR_UNAVAILABLE;
  if (!*path || path[strlen(path) - 1] == '/') return DSN_ERR_IO;
  std::string leaf;
  const Fd dir{openParent(fsRoot_, path, leaf)};
  if (dir.value < 0) return DSN_ERR_IO;
  FILE* file = openRegular(dir.value, leaf.c_str());
  if (!file) return DSN_ERR_IO;
  std::vector<uint8_t> data;
  const int result = readBounded(file, data, cap);
  return result < 0 ? result : give(data.data(), data.size(), out, cap);
}

int DesktopServices::fsReadAt(const char* path, uint32_t offset, uint8_t* out, uint32_t cap) {
  if (fsRoot_.empty()) return DSN_ERR_UNAVAILABLE;
  if (!*path || path[strlen(path) - 1] == '/') return DSN_ERR_IO;
  std::string leaf;
  const Fd dir{openParent(fsRoot_, path, leaf)};
  if (dir.value < 0) return DSN_ERR_IO;
  FILE* file = openRegular(dir.value, leaf.c_str());
  if (!file) return DSN_ERR_IO;
  const bool sought = fseek(file, static_cast<long>(offset), SEEK_SET) == 0;
  const size_t n = sought ? fread(out, 1, cap, file) : 0;
  const bool ok = sought && !ferror(file);
  fclose(file);
  return ok ? static_cast<int>(n) : DSN_ERR_IO;
}

int DesktopServices::fsWrite(const char* path, const uint8_t* data, uint32_t n) {
  if (fsRoot_.empty()) return DSN_ERR_UNAVAILABLE;
  return replaceFile(fsRoot_, path, [&](FILE* file) {
    return fwrite(data, 1, n, file) == n ? 0 : DSN_ERR_IO;
  });
}

int DesktopServices::fsList(const char* path, char* out, uint32_t cap) {
  if (fsRoot_.empty()) return DSN_ERR_UNAVAILABLE;
  std::string leaf;
  const Fd parent{openParent(fsRoot_, path, leaf)};
  if (parent.value < 0) return DSN_ERR_IO;
  const int fd = openat(parent.value, leaf.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return DSN_ERR_IO;
  DIR* dir = fdopendir(fd);
  if (!dir) { close(fd); return DSN_ERR_IO; }

  std::string entries;
  int result = 0;
  for (;;) {
    errno = 0;
    const dirent* entry = readdir(dir);
    if (!entry) {
      if (errno) result = DSN_ERR_IO;
      break;
    }
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
    entries += entry->d_name;
    if (entry->d_type == DT_DIR) entries += '/';
    entries += '\n';
    if (entries.size() > cap) { result = DSN_ERR_TOO_BIG; break; }
  }
  if (closedir(dir) != 0) return DSN_ERR_IO;
  return result < 0 ? result : give(reinterpret_cast<const uint8_t*>(entries.data()), entries.size(),
                                    reinterpret_cast<uint8_t*>(out), cap);
}

int DesktopServices::fsRename(const char* source, const char* dest) {
  if (fsRoot_.empty()) return DSN_ERR_UNAVAILABLE;
  std::string a, b;
  const Fd from{openParent(fsRoot_, source, a)}, to{openParent(fsRoot_, dest, b)};
  if (from.value < 0 || to.value < 0 || a == "." || b == ".") return DSN_ERR_IO;
  struct stat st;
  if (fstatat(from.value, a.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISREG(st.st_mode))
    return DSN_ERR_IO;
  // Publish without replacement, including when another process creates dest.
  if (linkat(from.value, a.c_str(), to.value, b.c_str(), 0) != 0) return DSN_ERR_IO;
  if (unlinkat(from.value, a.c_str(), 0) == 0) return 0;
  unlinkat(to.value, b.c_str(), 0);
  return DSN_ERR_IO;
}

int DesktopServices::fsDelete(const char* path) {
  if (fsRoot_.empty()) return DSN_ERR_UNAVAILABLE;
  std::string leaf;
  const Fd dir{openParent(fsRoot_, path, leaf)};
  if (dir.value < 0 || leaf == ".") return DSN_ERR_IO;
  if (path[strlen(path) - 1] != '/') {
    if (unlinkat(dir.value, leaf.c_str(), 0) == 0) return 0;
    if (errno != EISDIR && errno != EPERM) return DSN_ERR_IO;
  }
  if (unlinkat(dir.value, leaf.c_str(), AT_REMOVEDIR) == 0) return 0;
  return DSN_ERR_IO;
}

int DesktopServices::fsAppDir(char* out, uint32_t cap) {
  if (fsRoot_.empty()) return DSN_ERR_UNAVAILABLE;
  if (appDir_.size() + 1 > cap) return DSN_ERR_TOO_BIG;
  memcpy(out, appDir_.c_str(), appDir_.size() + 1);
  return static_cast<int>(appDir_.size());
}

int DesktopServices::keystore(const char* path, const uint8_t* key, uint32_t n, uint32_t) {
  return fsWrite((std::string(path) + ".key").c_str(), key, n);
}

int DesktopServices::recents(char* out, uint32_t cap) {
  return give(reinterpret_cast<const uint8_t*>(recentsFixture.data()), recentsFixture.size(),
              reinterpret_cast<uint8_t*>(out), cap);
}

int DesktopServices::recentShelf(uint32_t first, uint32_t count, char* out, uint32_t cap) {
  if (count > DSN_SHELF_PAGE_MAX) return DSN_ERR_INVALID;
  uint32_t used = 0;
  std::vector<std::string_view> opened;
  for (const auto* fixture : {&recentsFixture, &addedFixture}) {
    std::string_view remaining = *fixture;
    while (count) {
      const size_t end = remaining.find('\n');
      if (end == std::string_view::npos) break;
      const auto row = remaining.substr(0, end + 1);
      remaining.remove_prefix(row.size());
      const auto path = row.substr(0, row.find('\t'));
      if (fixture == &recentsFixture) opened.push_back(path);
      else if (std::find(opened.begin(), opened.end(), path) != opened.end()) continue;
      if (first) { --first; continue; }
      // Match the firmware shelf ABI: path, title, author, progress,
      // and an optional fifth field containing the formatted series/index.
      size_t field = 0;
      for (int i = 0; i < 3; ++i) {
        field = row.find('\t', field);
        if (field == std::string_view::npos) break;
        ++field;
      }
      if (field == std::string_view::npos) {
        // Preserve malformed/blank shelf rows as empty records, so pagination
        // keeps its existing record positions without exposing partial data.
        if (used >= cap) return used ? static_cast<int>(used) : DSN_ERR_TOO_BIG;
        out[used++] = '\n';
        --count;
        continue;
      }
      const std::string_view base = row.substr(0, field);
      const size_t seriesTab = row.find('\t', field);
      const std::string_view series = seriesTab == std::string_view::npos
                                          ? std::string_view{} : row.substr(seriesTab, row.size() - seriesTab - 1);
      constexpr std::string_view progress = "-1";
      const size_t need = base.size() + progress.size() + series.size() + 1;
      if (need > cap - used) return used ? static_cast<int>(used) : DSN_ERR_TOO_BIG;
      memcpy(out + used, base.data(), base.size());
      used += base.size();
      memcpy(out + used, progress.data(), progress.size());
      used += progress.size();
      if (!series.empty()) {
        memcpy(out + used, series.data(), series.size());
        used += series.size();
      }
      out[used++] = '\n';
      --count;
    }
  }
  return static_cast<int>(used);
}

int DesktopServices::bookProgress(const char* path) {
  // Pull the percent for `path` out of the recents fixture; -1 if not listed.
  std::string needle = std::string(path) + "\t";
  const size_t at = recentsFixture.find(needle);
  if (at == std::string::npos) return -1;
  // Progress is the fourth column even if the fifth series column exists.
  size_t pos = at;
  for (int i = 0; i < 3; ++i) {
    pos = recentsFixture.find('\t', pos);
    if (pos == std::string::npos) return -1;
    ++pos;
  }
  return atoi(recentsFixture.c_str() + pos);
}

// ---- crypto ----
int DesktopServices::hash(int algo, const uint8_t* data, uint32_t n, uint8_t* out, uint32_t cap) {
  const EVP_MD* md = mdFor(algo);
  if (!md) return DSN_ERR_INVALID;
  uint8_t digest[EVP_MAX_MD_SIZE];
  unsigned dl = 0;
  if (EVP_Digest(data, n, digest, &dl, md, nullptr) != 1) return DSN_ERR_INVALID;
  return give(digest, dl, out, cap);
}

int DesktopServices::aesCbc(int flags, const uint8_t* keyiv, const uint8_t* data, uint32_t n, uint8_t* out,
                            uint32_t cap) {
  const bool enc = flags & DSN_AES_ENCRYPT;
  const bool pad = !(flags & DSN_AES_NOPAD);
  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  if (!ctx) return DSN_ERR_IO;
  int rc = DSN_ERR_INVALID, outl = 0, finl = 0;
  std::vector<uint8_t> buf(n + 16);
  if (EVP_CipherInit_ex(ctx, EVP_aes_128_cbc(), nullptr, keyiv, keyiv + 16, enc) == 1 &&
      (EVP_CIPHER_CTX_set_padding(ctx, pad ? 1 : 0), 1) &&
      EVP_CipherUpdate(ctx, buf.data(), &outl, data, n) == 1 &&
      EVP_CipherFinal_ex(ctx, buf.data() + outl, &finl) == 1) {
    rc = give(buf.data(), outl + finl, out, cap);
  }
  EVP_CIPHER_CTX_free(ctx);
  return rc;
}

int DesktopServices::rsaEncrypt(const uint8_t* cert, uint32_t cn, const uint8_t* data, uint32_t n, uint8_t* out,
                                uint32_t cap) {
  EVP_PKEY* k = loadPub(cert, cn);
  if (!k) return DSN_ERR_INVALID;
  Evp e;
  e.c = EVP_PKEY_CTX_new(k, nullptr);
  size_t outn = cap;
  int rc = DSN_ERR_INVALID;
  if (e.c && EVP_PKEY_encrypt_init(e.c) == 1 && EVP_PKEY_CTX_set_rsa_padding(e.c, RSA_PKCS1_PADDING) == 1 &&
      EVP_PKEY_encrypt(e.c, out, &outn, data, n) == 1) {
    rc = static_cast<int>(outn);
  }
  EVP_PKEY_free(k);
  return rc == DSN_ERR_INVALID ? rc : (static_cast<uint32_t>(rc) > cap ? DSN_ERR_TOO_BIG : rc);
}

int DesktopServices::rsaDecrypt(int flags, const uint8_t* priv, uint32_t pn, const uint8_t* data, uint32_t n,
                                uint8_t* out, uint32_t cap) {
  EVP_PKEY* k = loadPriv(priv, pn);
  if (!k) return DSN_ERR_INVALID;
  Evp e;
  e.c = EVP_PKEY_CTX_new(k, nullptr);
  const int padding = (flags & DSN_RSA_RAW) ? RSA_NO_PADDING : RSA_PKCS1_PADDING;
  size_t outn = cap;
  int rc = DSN_ERR_INVALID;
  if (e.c && EVP_PKEY_decrypt_init(e.c) == 1 && EVP_PKEY_CTX_set_rsa_padding(e.c, padding) == 1 &&
      EVP_PKEY_decrypt(e.c, out, &outn, data, n) == 1) {
    rc = static_cast<int>(outn);
  }
  EVP_PKEY_free(k);
  return rc;
}

int DesktopServices::rsaSign(int algo, const uint8_t* priv, uint32_t pn, const uint8_t* digest, uint32_t dn,
                             uint8_t* out, uint32_t cap) {
  EVP_PKEY* k = loadPriv(priv, pn);
  if (!k) return DSN_ERR_INVALID;
  Evp e;
  e.c = EVP_PKEY_CTX_new(k, nullptr);
  size_t outn = cap;
  int rc = DSN_ERR_INVALID;
  if (e.c && EVP_PKEY_sign_init(e.c) == 1 && EVP_PKEY_CTX_set_rsa_padding(e.c, RSA_PKCS1_PADDING) == 1) {
    // With a signature MD the DigestInfo prefix is added; without it the raw
    // digest is PKCS#1 v1.5 padded as-is (DSN_SIGN_NODIGESTINFO).
    bool ok = true;
    if (!(algo & DSN_SIGN_NODIGESTINFO)) ok = EVP_PKEY_CTX_set_signature_md(e.c, mdFor(algo)) == 1;
    if (ok && EVP_PKEY_sign(e.c, out, &outn, digest, dn) == 1) rc = static_cast<int>(outn);
  }
  EVP_PKEY_free(k);
  return rc;
}

int DesktopServices::pkcs12Parse(const uint8_t* p12d, uint32_t n, const char* pass, uint8_t* out, uint32_t cap) {
  const uint8_t* p = p12d;
  PKCS12* p12 = d2i_PKCS12(nullptr, &p, n);
  if (!p12) return DSN_ERR_INVALID;
  EVP_PKEY* key = nullptr;
  X509* cert = nullptr;
  int rc = DSN_ERR_INVALID;
  if (PKCS12_parse(p12, pass, &key, &cert, nullptr) == 1 && key && cert) {
    uint8_t* keyDer = nullptr;
    uint8_t* certDer = nullptr;
    const int kl = i2d_PrivateKey(key, &keyDer);   // PKCS#8-compatible DER
    const int cl = i2d_X509(cert, &certDer);
    if (kl > 0 && cl > 0 && static_cast<uint32_t>(2 + kl + cl) <= cap && kl < 0x10000) {
      out[0] = (kl >> 8) & 0xff;
      out[1] = kl & 0xff;
      memcpy(out + 2, keyDer, kl);
      memcpy(out + 2 + kl, certDer, cl);
      rc = 2 + kl + cl;
    } else if (kl > 0 && cl > 0) {
      rc = DSN_ERR_TOO_BIG;
    }
    OPENSSL_free(keyDer);
    OPENSSL_free(certDer);
  }
  EVP_PKEY_free(key);
  X509_free(cert);
  PKCS12_free(p12);
  return rc;
}

// ---- board ----
int DesktopServices::backlightSet(int brightness, int warm) {
  if (brightness >= 0) lastBrightness = brightness > 100 ? 100 : brightness;
  if (warm >= 0) lastWarm = warm > 100 ? 100 : warm;
  return 0;
}
int DesktopServices::backlightGet() {
  return (lastBrightness < 0 ? 0 : lastBrightness) | (lastWarm < 0 ? 0 : lastWarm) << 8;
}
int DesktopServices::battery() { return (batteryPercent & 0xff) | (batteryPower & 0xff) << 8; }
int DesktopServices::haptic(int ms, int intensity) {
  lastHapticMs = ms > 2000 ? 2000 : ms;
  lastHapticIntensity = intensity;
  return 0;
}
int DesktopServices::accel(uint8_t* out) {
  for (int i = 0; i < 3; i++) {
    out[2 * i] = accelXYZ[i] & 0xff;
    out[2 * i + 1] = (accelXYZ[i] >> 8) & 0xff;
  }
  return 6;
}

// ---- async (synchronous on desktop) ----
int DesktopServices::wifiConnectStart() { return 0; }  // desktop is always online

int DesktopServices::netStart(const char* head, const uint8_t*, uint32_t, uint8_t* resp, uint32_t cap) {
  // Fixture: first line "METHOD url"; map to <netRoot>/<host><path> + .status.
  if (netRoot_.empty()) return DSN_ERR_UNAVAILABLE;
  std::string_view line(head);
  line = line.substr(0, line.find('\n'));
  const size_t sp = line.find(' ');
  if (sp == std::string_view::npos) return DSN_ERR_INVALID;
  std::string leaf;
  const Fd dir{openParent(netRoot_, fixturePath(line.substr(sp + 1)), leaf)};
  if (dir.value < 0) return DSN_ERR_IO;
  if (cap < 4) return DSN_ERR_TOO_BIG;
  FILE* file = openRegular(dir.value, leaf.c_str());
  if (!file) return DSN_ERR_IO;
  std::vector<uint8_t> body;
  const int result = readBounded(file, body, cap - 4);
  if (result < 0) return result;

  uint16_t status = 200;
  if (FILE* sf = openRegular(dir.value, (leaf + ".status").c_str())) {
    unsigned code;
    if (fscanf(sf, "%3u", &code) == 1 && code >= 100 && code <= 599) status = static_cast<uint16_t>(code);
    fclose(sf);
  }
  // u16 status (BE), u16 header length (zero), then the body.
  resp[0] = status >> 8;
  resp[1] = status & 0xff;
  resp[2] = 0;
  resp[3] = 0;
  return give(body.data(), body.size(), resp + 4, cap - 4);
}

int DesktopServices::fetchStart(const char* url, const char* dest) {
  if (netRoot_.empty() || fsRoot_.empty()) return DSN_ERR_UNAVAILABLE;
  std::string leaf;
  const Fd dir{openParent(netRoot_, fixturePath(url), leaf)};
  if (dir.value < 0) return DSN_ERR_IO;
  FILE* input = openRegular(dir.value, leaf.c_str());
  if (!input) return DSN_ERR_IO;
  const int result = replaceFile(fsRoot_, dest, [&](FILE* output) {
    uint8_t chunk[4096];
    size_t count;
    int total = 0;
    while ((count = fread(chunk, 1, sizeof chunk, input)) > 0) {
      if (count > static_cast<size_t>(INT_MAX - total) || fwrite(chunk, 1, count, output) != count)
        return DSN_ERR_IO;
      total += static_cast<int>(count);
    }
    const bool readOk = !ferror(input);
    const bool closeOk = fclose(input) == 0;
    input = nullptr;
    return readOk && closeOk ? total : DSN_ERR_IO;
  });
  if (input) fclose(input);
  return result;
}

int DesktopServices::textInputStart(const char*, char* out, uint32_t cap) {
  if (!fgets(out, static_cast<int>(cap), stdin)) return -1; // cancelled (EOF)
  size_t n = strlen(out);
  if (n && out[n - 1] == '\n') out[--n] = '\0';
  return static_cast<int>(n);
}

int DesktopServices::keygenStart(uint32_t bits, uint8_t* pub, uint32_t pubCap, uint8_t* priv, uint32_t privCap) {
  EVP_PKEY* k = EVP_RSA_gen(bits);
  if (!k) return DSN_ERR_IO;
  uint8_t* pubDer = nullptr;
  uint8_t* privDer = nullptr;
  const int pl = i2d_PUBKEY(k, &pubDer);
  const int sl = i2d_PrivateKey(k, &privDer);
  EVP_PKEY_free(k);
  int rc = DSN_ERR_IO;
  if (pl > 0 && sl > 0) {
    if (static_cast<uint32_t>(pl) > pubCap || static_cast<uint32_t>(sl) > privCap) {
      rc = DSN_ERR_TOO_BIG;
    } else {
      memcpy(pub, pubDer, pl);
      memcpy(priv, privDer, sl);
      rc = pl << 16 | sl; // pubLen << 16 | privLen
    }
  }
  OPENSSL_free(pubDer);
  OPENSSL_free(privDer);
  return rc;
}

}  // namespace dessen
