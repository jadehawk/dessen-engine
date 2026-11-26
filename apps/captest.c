// Exercises the capability calls from inside the VM against the desktop
// services. Exits 0 on pass, or the number of the first failed check. The host
// test (freeink/test_host.cpp) runs this with a tmp fs root and asserts exit 0.
#include "dessen.h"

static int eq(const unsigned char *a, const unsigned char *b, int n) {
  for (int i = 0; i < n; i++)
    if (a[i] != b[i]) return 0;
  return 1;
}
static void fill(unsigned char *p, int n, unsigned seed) {
  for (int i = 0; i < n; i++) p[i] = (unsigned char)(seed * 1103515245u + i * 12345u + 7u);
}

#define CHECK(n, cond) \
  if (!(cond)) return n

int main(void) {
  // --- core ---
  CHECK(1, dsn_time() > 1000000000u);               // a real clock, past 2001
  unsigned char r1[16], r2[16];
  CHECK(2, dsn_random_bytes(r1, 16) == 16 && dsn_random_bytes(r2, 16) == 16);
  CHECK(3, !eq(r1, r2, 16));                         // two draws differ
  char id[80];
  CHECK(4, dsn_device_id(id, sizeof id) == 64);

  // --- UI language (BCP47 code); the host default is "en" ---
  char loc[8];
  CHECK(40, dsn_locale(loc, sizeof loc) == 2 && loc[0] == 'e' && loc[1] == 'n');

  // --- storage round-trip ---
  unsigned char payload[40];
  fill(payload, sizeof payload, 9);
  CHECK(5, dsn_fs_write("/captest.bin", 12, payload, sizeof payload) == 0);
  unsigned char back[64];
  CHECK(6, dsn_fs_read("/captest.bin", 12, back, sizeof back) == (int)sizeof payload);
  CHECK(7, eq(payload, back, sizeof payload));
  char listing[256];
  int ln = dsn_fs_list("/", 1, listing, sizeof listing);
  CHECK(8, ln > 0);
  CHECK(9, dsn_fs_delete("/captest.bin", 12) == 0);
  CHECK(10, dsn_fs_read("/captest.bin", 12, back, sizeof back) == DSN_ERR_IO);
  CHECK(11, dsn_fs_read("/../escape", 10, back, sizeof back) == DSN_ERR_INVALID); // path rules

  // --- hash: SHA-256("abc") known answer ---
  static const unsigned char abc_sha256[32] = {
      0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
      0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
  unsigned char digest[32];
  CHECK(12, dsn_hash(DSN_HASH_SHA256, "abc", 3, digest, 32) == 32);
  CHECK(13, eq(digest, abc_sha256, 32));

  // --- AES-128-CBC round-trip, padded and raw ---
  unsigned char keyiv[32];
  fill(keyiv, 32, 3);
  unsigned char msg[20], ct[64], pt[64];
  fill(msg, sizeof msg, 5);
  int cl = dsn_aes_cbc(DSN_AES_ENCRYPT, keyiv, 32, msg, sizeof msg, ct, sizeof ct);
  CHECK(14, cl == 32);                               // 20 bytes -> 2 padded blocks
  CHECK(15, dsn_aes_cbc(DSN_AES_DECRYPT, keyiv, 32, ct, cl, pt, sizeof pt) == (int)sizeof msg);
  CHECK(16, eq(msg, pt, sizeof msg));
  unsigned char block[16];
  fill(block, 16, 8);
  cl = dsn_aes_cbc(DSN_AES_ENCRYPT | DSN_AES_NOPAD, keyiv, 32, block, 16, ct, sizeof ct);
  CHECK(17, cl == 16);                               // no padding added
  CHECK(18, dsn_aes_cbc(DSN_AES_DECRYPT | DSN_AES_NOPAD, keyiv, 32, ct, 16, pt, sizeof pt) == 16);
  CHECK(19, eq(block, pt, 16));

  // --- RSA keygen -> encrypt -> decrypt round-trip ---
  unsigned char pub[640], priv[1400];
  int kg = dsn_rsa_keygen(1024, pub, sizeof pub, priv, sizeof priv);
  CHECK(20, kg > 0);
  int publen = kg >> 16, privlen = kg & 0xffff;
  CHECK(21, publen > 0 && privlen > 0);
  unsigned char secret[16], enc[128], dec[128];
  fill(secret, 16, 11);
  CHECK(22, dsn_rsa_encrypt(pub, publen, secret, 16, enc, sizeof enc) == 128); // 1024-bit modulus
  CHECK(23, dsn_rsa_decrypt(DSN_RSA_PKCS1, priv, privlen, enc, 128, dec, sizeof dec) == 16);
  CHECK(24, eq(secret, dec, 16));
  // raw decrypt returns the PKCS#1 v1.5 type-2 block the app can unpad itself
  unsigned char raw[128];
  CHECK(25, dsn_rsa_decrypt(DSN_RSA_RAW, priv, privlen, enc, 128, raw, sizeof raw) == 128);
  CHECK(26, raw[0] == 0 && raw[1] == 2 && raw[128 - 17] == 0);
  CHECK(27, eq(raw + 128 - 16, secret, 16));
  // sign a digest -> modulus-size signature
  unsigned char sig[128];
  CHECK(28, dsn_rsa_sign(DSN_HASH_SHA256, priv, privlen, digest, 32, sig, sizeof sig) == 128);

  // --- board services ---
  CHECK(29, dsn_backlight(60, 40) == 0);
  CHECK(30, dsn_battery() >= 0 && (dsn_battery() & 0xff) <= 100);
  CHECK(31, dsn_haptic(50, 200) == 0);
  unsigned char xyz[6];
  CHECK(32, dsn_accel(xyz, 6) == 6);

  // Unsupported native reading history is discoverable on hosts without it.
  CHECK(41, dsn_reading_stats_read(DSN_STATS_CAPABILITY, 0, back, 0) == DSN_ERR_UNAVAILABLE);
  CHECK(42, dsn_reading_stats_start("", 0, 0) == DSN_ERR_UNAVAILABLE);

  // --- keystore ---
  CHECK(33, dsn_keystore("/book.epub", 10, secret, 16, 0) == 0);
  return 0;
}
