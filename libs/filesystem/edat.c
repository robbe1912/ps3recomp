/* edat.c -- NPDRM (EDAT/SDAT) decryption. See edat.h for why this lives in the FS layer.
 *
 * The algorithm is the published one (RPCS3's Crypto/unedat.cpp is the clearest
 * write-up); the structure below follows it closely so the two can be diffed:
 *
 *   header  0x00  NPD: magic, version, license, type, content_id[0x30],
 *                      digest[0x10], title_hash[0x10], dev_hash[0x10], times
 *           0x80  EDAT: flags, block_size, file_size
 *   0x100         per-block metadata (0x10 bytes each: the block's hash)
 *   then          the encrypted blocks
 *
 * Per block: the block key is dev_hash[0..0xB] + the big-endian block index, AES-ECB
 * encrypted under the file key; the result is both the CBC key for the data and the
 * CMAC key for the block hash. The CBC IV is the NPD digest (version >= 2).
 *
 * AES-128 and AES-CMAC are implemented here rather than pulled in, so the runtime
 * keeps no crypto dependency. edat_selftest() checks both against their standard
 * vectors before anything is decrypted -- see the note in edat.h.
 */

#include "edat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>

/* ---------------------------------------------------------------------------
 * AES-128
 * -----------------------------------------------------------------------*/

static const uint8_t AES_SBOX[256] = {
0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16 };

static uint8_t AES_ISBOX[256];
static int     s_isbox_ready = 0;

static void aes_build_isbox(void)
{
    if (s_isbox_ready) return;
    for (int i = 0; i < 256; i++) AES_ISBOX[AES_SBOX[i]] = (uint8_t)i;
    s_isbox_ready = 1;
}

static uint8_t xtime(uint8_t a) { return (uint8_t)((a << 1) ^ ((a & 0x80) ? 0x1B : 0)); }

static uint8_t gmul(uint8_t a, uint8_t b)
{
    uint8_t r = 0;
    for (int i = 0; i < 8; i++) {
        if (b & 1) r ^= a;
        a = xtime(a);
        b >>= 1;
    }
    return r;
}

/* 11 round keys of 16 bytes. */
static void aes128_expand(const uint8_t key[16], uint8_t rk[176])
{
    memcpy(rk, key, 16);
    uint8_t rcon = 1;
    for (int i = 16; i < 176; i += 4) {
        uint8_t t[4];
        memcpy(t, rk + i - 4, 4);
        if ((i % 16) == 0) {
            uint8_t tmp = t[0];
            t[0] = (uint8_t)(AES_SBOX[t[1]] ^ rcon);
            t[1] = AES_SBOX[t[2]];
            t[2] = AES_SBOX[t[3]];
            t[3] = AES_SBOX[tmp];
            rcon = xtime(rcon);
        }
        for (int j = 0; j < 4; j++) rk[i + j] = (uint8_t)(rk[i - 16 + j] ^ t[j]);
    }
}

static void aes128_encrypt_block(const uint8_t rk[176], const uint8_t in[16], uint8_t out[16])
{
    uint8_t s[16];
    for (int i = 0; i < 16; i++) s[i] = (uint8_t)(in[i] ^ rk[i]);
    for (int round = 1; round <= 10; round++) {
        for (int i = 0; i < 16; i++) s[i] = AES_SBOX[s[i]];
        /* ShiftRows (state is column-major: byte i is row i%4, column i/4). */
        uint8_t t[16];
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++)
                t[c * 4 + r] = s[((c + r) % 4) * 4 + r];
        memcpy(s, t, 16);
        if (round != 10) {
            for (int c = 0; c < 4; c++) {
                uint8_t* p = s + c * 4;
                uint8_t a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
                p[0] = (uint8_t)(xtime(a0) ^ (xtime(a1) ^ a1) ^ a2 ^ a3);
                p[1] = (uint8_t)(a0 ^ xtime(a1) ^ (xtime(a2) ^ a2) ^ a3);
                p[2] = (uint8_t)(a0 ^ a1 ^ xtime(a2) ^ (xtime(a3) ^ a3));
                p[3] = (uint8_t)((xtime(a0) ^ a0) ^ a1 ^ a2 ^ xtime(a3));
            }
        }
        for (int i = 0; i < 16; i++) s[i] ^= rk[round * 16 + i];
    }
    memcpy(out, s, 16);
}

static void aes128_decrypt_block(const uint8_t rk[176], const uint8_t in[16], uint8_t out[16])
{
    aes_build_isbox();
    uint8_t s[16];
    for (int i = 0; i < 16; i++) s[i] = (uint8_t)(in[i] ^ rk[160 + i]);
    for (int round = 9; round >= 0; round--) {
        /* InvShiftRows */
        uint8_t t[16];
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++)
                t[((c + r) % 4) * 4 + r] = s[c * 4 + r];
        memcpy(s, t, 16);
        for (int i = 0; i < 16; i++) s[i] = AES_ISBOX[s[i]];
        for (int i = 0; i < 16; i++) s[i] ^= rk[round * 16 + i];
        if (round != 0) {
            for (int c = 0; c < 4; c++) {
                uint8_t* p = s + c * 4;
                uint8_t a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
                p[0] = (uint8_t)(gmul(a0,14) ^ gmul(a1,11) ^ gmul(a2,13) ^ gmul(a3, 9));
                p[1] = (uint8_t)(gmul(a0, 9) ^ gmul(a1,14) ^ gmul(a2,11) ^ gmul(a3,13));
                p[2] = (uint8_t)(gmul(a0,13) ^ gmul(a1, 9) ^ gmul(a2,14) ^ gmul(a3,11));
                p[3] = (uint8_t)(gmul(a0,11) ^ gmul(a1,13) ^ gmul(a2, 9) ^ gmul(a3,14));
            }
        }
    }
    memcpy(out, s, 16);
}

static void aes128_cbc_decrypt(const uint8_t key[16], const uint8_t iv[16],
                               const uint8_t* in, uint8_t* out, size_t len)
{
    uint8_t rk[176];
    uint8_t prev[16], cur[16];
    aes128_expand(key, rk);
    memcpy(prev, iv, 16);
    for (size_t off = 0; off + 16 <= len; off += 16) {
        memcpy(cur, in + off, 16);
        aes128_decrypt_block(rk, cur, out + off);
        for (int i = 0; i < 16; i++) out[off + i] ^= prev[i];
        memcpy(prev, cur, 16);
    }
}

/* AES-CMAC (RFC 4493). */
static void cmac_shift_left(const uint8_t in[16], uint8_t out[16])
{
    uint8_t carry = 0;
    for (int i = 15; i >= 0; i--) {
        uint8_t b = in[i];
        out[i] = (uint8_t)((b << 1) | carry);
        carry = (uint8_t)((b >> 7) & 1);
    }
}

static void aes128_cmac(const uint8_t key[16], const uint8_t* msg, size_t len, uint8_t mac[16])
{
    uint8_t rk[176], zero[16] = {0}, L[16], K1[16], K2[16], X[16] = {0}, blk[16];
    aes128_expand(key, rk);
    aes128_encrypt_block(rk, zero, L);
    cmac_shift_left(L, K1);  if (L[0]  & 0x80) K1[15] ^= 0x87;
    cmac_shift_left(K1, K2); if (K1[0] & 0x80) K2[15] ^= 0x87;

    size_t n = (len + 15) / 16;
    int complete = (n != 0) && (len % 16 == 0);
    if (n == 0) n = 1;

    for (size_t i = 0; i < n - 1; i++) {
        for (int j = 0; j < 16; j++) X[j] ^= msg[i * 16 + j];
        aes128_encrypt_block(rk, X, X);
    }
    if (complete) {
        memcpy(blk, msg + (n - 1) * 16, 16);
        for (int j = 0; j < 16; j++) blk[j] ^= K1[j];
    } else {
        size_t rem = len % 16;
        memset(blk, 0, 16);
        if (rem) memcpy(blk, msg + (n - 1) * 16, rem);
        blk[rem] = 0x80;
        for (int j = 0; j < 16; j++) blk[j] ^= K2[j];
    }
    for (int j = 0; j < 16; j++) X[j] ^= blk[j];
    aes128_encrypt_block(rk, X, mac);
}

/* ---------------------------------------------------------------------------
 * NPDRM constants (the published set)
 * -----------------------------------------------------------------------*/
/* Candidate klicensees for content not bound to a per-console RAP.
 *
 * Which one applies is not guessed: the NPD header carries dev_hash, which is
 * CMAC(klic ^ NP_OMAC_KEY_2) over its own first 0x60 bytes, so the file itself says
 * which key is right. Trying them and keeping the one that verifies is simpler than
 * branching on license type and strictly safer -- a key that does not verify is
 * never used.
 *
 * Worth knowing: a PSOne Classic uses NP_PSX_KEY for BOTH license type 3 ("free")
 * and license type 2 (retail), so an ordinary retail ISO.BIN.EDAT decrypts with no
 * RAP at all. */
/* ---------------------------------------------------------------------------
 * SHA-1 / HMAC-SHA1
 *
 * Needed for the EDAT hash modes. Only AES-CMAC was here before, which covers
 * hash mode 0x02; modes 0x01 and 0x04 are HMAC-SHA1 (with a 0x14- and a
 * 0x10-byte key respectively) and a file using either could not be checked --
 * so those flag combinations were refused outright rather than mis-verified.
 * -----------------------------------------------------------------------*/

typedef struct { uint32_t h[5]; uint64_t len; uint8_t buf[64]; size_t n; } sha1_ctx;

static uint32_t rol32(uint32_t v, int c) { return (v << c) | (v >> (32 - c)); }

static void sha1_block(sha1_ctx* c, const uint8_t* p)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) |
               ((uint32_t)p[i*4+2] << 8) | (uint32_t)p[i*4+3];
    for (int i = 16; i < 80; i++)
        w[i] = rol32(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);

    uint32_t a = c->h[0], b = c->h[1], d = c->h[2], e = c->h[3], f = c->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t k, t;
        if      (i < 20) { t = (b & d) | (~b & e);            k = 0x5A827999u; }
        else if (i < 40) { t = b ^ d ^ e;                     k = 0x6ED9EBA1u; }
        else if (i < 60) { t = (b & d) | (b & e) | (d & e);   k = 0x8F1BBCDCu; }
        else             { t = b ^ d ^ e;                     k = 0xCA62C1D6u; }
        uint32_t tmp = rol32(a, 5) + t + f + k + w[i];
        f = e; e = d; d = rol32(b, 30); b = a; a = tmp;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += d; c->h[3] += e; c->h[4] += f;
}

static void sha1_init(sha1_ctx* c)
{
    c->h[0] = 0x67452301u; c->h[1] = 0xEFCDAB89u; c->h[2] = 0x98BADCFEu;
    c->h[3] = 0x10325476u; c->h[4] = 0xC3D2E1F0u;
    c->len = 0; c->n = 0;
}

static void sha1_update(sha1_ctx* c, const uint8_t* p, size_t n)
{
    c->len += n;
    while (n) {
        size_t take = 64 - c->n;
        if (take > n) take = n;
        memcpy(c->buf + c->n, p, take);
        c->n += take; p += take; n -= take;
        if (c->n == 64) { sha1_block(c, c->buf); c->n = 0; }
    }
}

static void sha1_final(sha1_ctx* c, uint8_t out[20])
{
    uint64_t bits = c->len * 8;
    uint8_t pad = 0x80;
    sha1_update(c, &pad, 1);
    pad = 0;
    while (c->n != 56) sha1_update(c, &pad, 1);
    uint8_t l[8];
    for (int i = 0; i < 8; i++) l[i] = (uint8_t)(bits >> (56 - i*8));
    sha1_update(c, l, 8);
    for (int i = 0; i < 5; i++) {
        out[i*4]   = (uint8_t)(c->h[i] >> 24); out[i*4+1] = (uint8_t)(c->h[i] >> 16);
        out[i*4+2] = (uint8_t)(c->h[i] >> 8);  out[i*4+3] = (uint8_t)c->h[i];
    }
}

/* HMAC-SHA1. The EDAT key lengths (0x10, 0x14) are both under the 64-byte block
 * size, so the key is only zero-padded, never hashed down. */
static void hmac_sha1(const uint8_t* key, size_t keylen,
                      const uint8_t* msg, size_t msglen, uint8_t out[20])
{
    uint8_t k[64] = {0}, ipad[64], opad[64], inner[20];
    if (keylen > 64) { sha1_ctx t; sha1_init(&t); sha1_update(&t, key, keylen); sha1_final(&t, k); }
    else             memcpy(k, key, keylen);
    for (int i = 0; i < 64; i++) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5C; }

    sha1_ctx c;
    sha1_init(&c); sha1_update(&c, ipad, 64); sha1_update(&c, msg, msglen); sha1_final(&c, inner);
    sha1_init(&c); sha1_update(&c, opad, 64); sha1_update(&c, inner, 20);   sha1_final(&c, out);
}

/* Key material is supplied locally and is never distributed. */
#include "ps3_edat_keys.h"

#define SDAT_FLAG              0x01000000u
#define EDAT_COMPRESSED_FLAG   0x00000001u
#define EDAT_FLAG_0x02         0x00000002u
#define EDAT_ENCRYPTED_KEY_FLAG 0x00000008u
#define EDAT_FLAG_0x10         0x00000010u
#define EDAT_FLAG_0x20         0x00000020u
#define EDAT_DEBUG_DATA_FLAG   0x80000000u

static uint32_t be32(const uint8_t* p)
{ return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint64_t be64(const uint8_t* p) { return ((uint64_t)be32(p) << 32) | be32(p + 4); }

/* ---------------------------------------------------------------------------
 * Self-test
 * -----------------------------------------------------------------------*/
int edat_selftest(void)
{
    /* FIPS-197 AES-128 */
    static const uint8_t k[16] = {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
                                  0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f};
    static const uint8_t p[16] = {0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,
                                  0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff};
    static const uint8_t c[16] = {0x69,0xc4,0xe0,0xd8,0x6a,0x7b,0x04,0x30,
                                  0xd8,0xcd,0xb7,0x80,0x70,0xb4,0xc5,0x5a};
    uint8_t rk[176], out[16];
    aes128_expand(k, rk);
    aes128_encrypt_block(rk, p, out);
    if (memcmp(out, c, 16) != 0) return -1;
    aes128_decrypt_block(rk, c, out);
    if (memcmp(out, p, 16) != 0) return -2;

    /* RFC 4493 AES-CMAC, empty message */
    static const uint8_t ck[16] = {0x2b,0x7e,0x15,0x16,0x28,0xae,0xd2,0xa6,
                                   0xab,0xf7,0x15,0x88,0x09,0xcf,0x4f,0x3c};
    static const uint8_t cm[16] = {0xbb,0x1d,0x69,0x29,0xe9,0x59,0x37,0x28,
                                   0x7f,0xa3,0x7d,0x12,0x9b,0x75,0x67,0x46};
    uint8_t mac[16];
    aes128_cmac(ck, NULL, 0, mac);
    if (memcmp(mac, cm, 16) != 0) return -3;

    /* FIPS-180 SHA-1("abc") */
    static const uint8_t sv[20] = {0xa9,0x99,0x3e,0x36,0x47,0x06,0x81,0x6a,0xba,0x3e,
                                   0x25,0x71,0x78,0x50,0xc2,0x6c,0x9c,0xd0,0xd8,0x9d};
    uint8_t sd[20]; sha1_ctx sc;
    sha1_init(&sc); sha1_update(&sc, (const uint8_t*)"abc", 3); sha1_final(&sc, sd);
    if (memcmp(sd, sv, 20) != 0) return -4;

    /* RFC 2202 HMAC-SHA1 case 1 */
    static const uint8_t hk[20] = {0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,
                                   0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b};
    static const uint8_t hv[20] = {0xb6,0x17,0x31,0x86,0x55,0x05,0x72,0x64,0xe2,0x8b,
                                   0xc0,0xb6,0xfb,0x37,0x8c,0x8e,0xf1,0x46,0xbe,0x00};
    uint8_t hd[20];
    hmac_sha1(hk, 20, (const uint8_t*)"Hi There", 8, hd);
    if (memcmp(hd, hv, 20) != 0) return -5;
    return 0;
}

/* ---------------------------------------------------------------------------
 * EDAT
 * -----------------------------------------------------------------------*/
int edat_is_npd(const char* host_path)
{
    FILE* f = fopen(host_path, "rb");
    if (!f) return 0;
    uint8_t m[4] = {0};
    size_t n = fread(m, 1, 4, f);
    fclose(f);
    return n == 4 && m[0] == 'N' && m[1] == 'P' && m[2] == 'D' && m[3] == 0;
}

/* The RIF key for a license-type-1/2 EDAT: $PS3_RIF_KEY as 32 hex chars, else
 * the 16 raw bytes of a <file>.rifkey sidecar. This is the buyer's own license
 * material for one content id -- deriving it from a .rap is deliberately out of
 * scope here, so nothing in this tree has to carry license-conversion keys. */
static int edat_rif_key(const char* in_path, uint8_t out[16])
{
    const char* e = getenv("PS3_RIF_KEY");
    if (e) {
        int n = 0;
        for (; n < 16 && e[n*2] && e[n*2+1]; n++) {
            char b[3] = { e[n*2], e[n*2+1], 0 };
            char* end = NULL;
            long v = strtol(b, &end, 16);
            if (end != b + 2) break;
            out[n] = (uint8_t)v;
        }
        if (n == 16) return 1;
        fprintf(stderr, "[edat] PS3_RIF_KEY is not 32 hex chars -- ignoring\n");
    }
    char side[1100];
    if ((size_t)snprintf(side, sizeof side, "%s.rifkey", in_path) >= sizeof side) return 0;
    FILE* f = fopen(side, "rb");
    if (!f) return 0;
    size_t n = fread(out, 1, 16, f);
    fclose(f);
    if (n == 16) { fprintf(stderr, "[edat] RIF key from %s\n", side); return 1; }
    return 0;
}
/* RPCS3 Crypto/unedat.cpp validate_dev_klic, verbatim semantics. For license
 * type 3 the NPD dev_hash is CMAC(klic ^ NP_OMAC_KEY_2) over the header's
 * first 0x60 bytes (version/license/type big-endian in place -- which a raw
 * on-disk header already is). Any other license does not bind dev_hash to
 * the klicensee and the check passes vacuously. */
static int edat_dev_klic_ok(const uint8_t hdr[0xA0], const uint8_t klic[16])
{
    if ((be32(hdr + 0x08) & 3) != 3) return 1;
    uint8_t ck[16], mac[16];
    for (int i = 0; i < 16; i++) ck[i] = (uint8_t)(klic[i] ^ NP_OMAC_KEY_2[i]);
    aes128_cmac(ck, hdr, 0x60, mac);
    return memcmp(mac, hdr + 0x60, 16) == 0;
}

int edat_validate_dev_klic(const char* host_path, const uint8_t klic[16])
{
    FILE* f = fopen(host_path, "rb");
    if (!f) return -1;
    uint8_t hdr[0xA0];
    size_t n = fread(hdr, 1, sizeof hdr, f);
    fclose(f);
    if (n != sizeof hdr || memcmp(hdr, "NPD", 3) != 0) return -1;
    return edat_dev_klic_ok(hdr, klic);
}

int edat_decrypt_file_klic(const char* in_path, const char* out_path,
                           const uint8_t klic[16], const char* klic_source)
{
    static int checked = 0;
    if (!checked) {
        int rc = edat_selftest();
        checked = 1;
        if (rc != 0) {
            fprintf(stderr, "[edat] SELF-TEST FAILED (%d) -- refusing to decrypt; the "
                            "AES/CMAC primitives are wrong and would silently produce "
                            "garbage\n", rc);
            return -100;
        }
    }

    FILE* in = fopen(in_path, "rb");
    if (!in) return -1;

    uint8_t hdr[0x100];
    if (fread(hdr, 1, sizeof hdr, in) != sizeof hdr) { fclose(in); return -2; }

    uint32_t version  = be32(hdr + 0x04);
    uint32_t license  = be32(hdr + 0x08);
    const uint8_t* digest   = hdr + 0x40;
    const uint8_t* dev_hash = hdr + 0x60;
    uint32_t flags      = be32(hdr + 0x80);
    uint32_t block_size = be32(hdr + 0x84);
    uint64_t file_size  = be64(hdr + 0x88);

    fprintf(stderr, "[edat] %s: version=%u license=%u flags=0x%08X block=0x%X size=%llu\n",
            in_path, version, license, flags, block_size,
            (unsigned long long)file_size);

    if (flags & (EDAT_COMPRESSED_FLAG | EDAT_DEBUG_DATA_FLAG)) {
        fprintf(stderr, "[edat] unsupported flags 0x%08X (per-block zlib compression "
                        "and debug data are not implemented)\n", flags);
        fclose(in); return -3;
    }
    if (block_size == 0 || block_size > (16u << 20)) { fclose(in); return -4; }

    /* title_hash binds the content id to the file NAME (CMAC ^ NP_OMAC_KEY_3
     * over content_id || basename, extension retried case-folded -- RPCS3
     * validate_npd_hashes). Key-independent, so it doubles as a free check of
     * our header parse; verified MATCH against MotorStorm's pack_toc.edat.
     * Warning only: the block hashes below are the stronger guarantee. */
    {
        const char* base = in_path, *s;
        for (s = in_path; *s; s++) if (*s == '/' || *s == '\\') base = s + 1;
        size_t bl = strlen(base);
        if (bl > 0 && bl <= 0x200) {
            uint8_t buf[0x230], mac[16];
            memcpy(buf, hdr + 0x10, 0x30);
            memcpy(buf + 0x30, base, bl);
            aes128_cmac(NP_OMAC_KEY_3, buf, 0x30 + bl, mac);
            int ok = memcmp(mac, hdr + 0x50, 16) == 0;
            if (!ok) {
                size_t dot = bl;
                while (dot > 0 && base[dot - 1] != '.') dot--;
                if (dot > 0) {
                    for (int up = 0; up < 2 && !ok; up++) {
                        for (size_t i = dot - 1; i < bl; i++)
                            buf[0x30 + i] = (uint8_t)(up ? toupper((unsigned char)base[i])
                                                         : tolower((unsigned char)base[i]));
                        aes128_cmac(NP_OMAC_KEY_3, buf, 0x30 + bl, mac);
                        ok = memcmp(mac, hdr + 0x50, 16) == 0;
                    }
                }
            }
            fprintf(stderr, "[edat] title_hash(%s): %s%s\n", base,
                    ok ? "MATCH" : "MISMATCH", ok ? "" : " (warning only)");
        }
    }

    /* The file key.
     *
     *   SDAT             derived from the header alone.
     *   license type 3   the klicensee itself ("free" content).
     *   license type 1/2 the RIF key. The klicensee still verifies against
     *                    dev_hash for these -- dev_hash authenticates the
     *                    HEADER -- but the DATA is under the RIF key, so a
     *                    dev_hash match alone does NOT mean decryptable. That
     *                    cost a debugging round twice: a retail PSOne
     *                    ISO.BIN.EDAT validates under NP_PSX_KEY and then fails
     *                    every block hash, and LittleBigPlanet's data.edat
     *                    validates under a klicensee baked into its own EBOOT
     *                    and likewise decrypts to noise.
     *
     * The RIF key is per-buyer license material, so it is never built in: it
     * comes from the operator, via $PS3_RIF_KEY (32 hex chars) or a <file>.rifkey
     * sidecar. Whether it is right is not taken on trust -- a wrong key fails
     * block 0's hash below and nothing is written. */
    uint8_t key[16];
    int have_key = 0;
    const char* key_src = "none";       /* which key decrypted (summary line) */
    const char* klic_verdict = "n/a";   /* validate_dev_klic result, when a klic was tried */
    if (flags & SDAT_FLAG) {
        for (int i = 0; i < 16; i++) key[i] = (uint8_t)(dev_hash[i] ^ SDAT_KEY[i]);
        fprintf(stderr, "[edat] SDAT: key derived from dev_hash\n");
        have_key = 1;
        key_src = "sdat(dev_hash)";
    } else {
        int found = -1;
        for (int c = 0; c < (int)(sizeof(KLIC_CANDIDATES) / sizeof(KLIC_CANDIDATES[0])); c++) {
            uint8_t ck[16], mac[16];
            for (int i = 0; i < 16; i++)
                ck[i] = (uint8_t)(KLIC_CANDIDATES[c].key[i] ^ NP_OMAC_KEY_2[i]);
            aes128_cmac(ck, hdr, 0x60, mac);
            if (memcmp(mac, dev_hash, 16) == 0) { found = c; break; }
        }
        /* The guest klicensee comes first: it is the game's OWN key, and for
         * license type 3 it is the file key outright (RPCS3 VerifyEDATHeader-
         * WithKLicense). Never taken on trust -- validate_dev_klic must PASS
         * and the verdict is logged either way. */
        if (klic) {
            int ok = edat_dev_klic_ok(hdr, klic);
            klic_verdict = ok ? "PASS" : "FAIL";
            fprintf(stderr, "[edat] validate_dev_klic: %s (klic from %s)\n",
                    klic_verdict, klic_source ? klic_source : "?");
            if (ok && (license & 3) == 3) {
                memcpy(key, klic, 16);
                have_key = 1;
                key_src = klic_source ? klic_source : "klic";
            } else if (!ok) {
                fprintf(stderr, "[edat] supplied klic does not authenticate "
                                "dev_hash -- falling back\n");
            }
        }
        if (!have_key && found >= 0 && (license & 3) == 3) {
            memcpy(key, KLIC_CANDIDATES[found].key, 16);
            have_key = 1;
            key_src = KLIC_CANDIDATES[found].name;
            fprintf(stderr, "[edat] klicensee %s verified against dev_hash\n",
                    KLIC_CANDIDATES[found].name);
        } else if (!have_key && edat_rif_key(in_path, key)) {
            have_key = 1;
            key_src = "env/.rifkey";
            fprintf(stderr, "[edat] license type %u: using the supplied RIF key%s\n",
                    license, found >= 0 ? "" : " (dev_hash names an unknown klicensee)");
        }
        if (!have_key) {
            fprintf(stderr,
                "[edat] license type %u needs a klicense and none was supplied.\n"
                "[edat]   cellFsSdataOpen's arg should carry the game's 16-byte klic;\n"
                "[edat]   otherwise set PS3_RIF_KEY=<32 hex chars>, or put the 16 raw\n"
                "[edat]   bytes in %s.rifkey -- it is derived from your own .rap for this\n"
                "[edat]   content id, which this runtime deliberately cannot do for you.\n",
                license, in_path);
            fclose(in); return -6;
        }
    }

    uint32_t total_blocks = (uint32_t)((file_size + block_size - 1) / block_size);

    /* Layout. With FLAG_0x20 (or compression) each block carries a 0x20-byte
     * metadata section IMMEDIATELY BEFORE its data; otherwise the 0x10-byte
     * sections form one table at 0x100 and the data follows all of them. */
    const uint32_t meta_size = (flags & (EDAT_COMPRESSED_FLAG | EDAT_FLAG_0x20)) ? 0x20 : 0x10;
    const int      meta_leads = (flags & EDAT_FLAG_0x20) != 0;
    const uint64_t data_base  = 0x100 + (uint64_t)total_blocks * meta_size;

    /* Layout sanity: the container arithmetic must fit inside the real file.
     * Verified byte-exact against MotorStorm's retail EDATs, which carry
     * exactly 16 trailing slack bytes; larger slack is tolerated quietly, but
     * a file SHORTER than its declared blocks means the reads below fail
     * mid-decrypt -- say so up front instead of truncating quietly. */
    if (fseek(in, 0, SEEK_END) == 0) {
        long long disk = ftell(in);
        if (disk >= 0) {
            uint64_t last_len  = file_size - (uint64_t)(total_blocks - 1) * block_size;
            uint64_t data_span = (uint64_t)(total_blocks - 1) * block_size
                               + ((last_len + 15) & ~(uint64_t)15);
            uint64_t expect    = 0x100 + (uint64_t)total_blocks * meta_size + data_span;
            if ((uint64_t)disk < expect)
                fprintf(stderr, "[edat] LAYOUT SHORT: file holds %lld bytes but the "
                                "NPD header implies %llu -- decrypt will fail partway\n",
                        disk, (unsigned long long)expect);
            else if ((uint64_t)disk > expect + 0x1000)
                fprintf(stderr, "[edat] layout: %lld trailing slack bytes beyond the "
                                "%llu-byte container (retail is 16)\n",
                        disk - (long long)expect, (unsigned long long)expect);
        }
    }

    /* Crypto/hash mode, exactly as the flags select it.
     *   ENCRYPTED_KEY (0x08): the per-block ERK is itself wrapped under EDAT_KEY.
     *   0x10 clear          -> hash is AES-CMAC over a 0x10-byte key
     *   0x10 set, 0x20 clear-> HMAC-SHA1 over a 0x10-byte key
     *   0x10 set, 0x20 set  -> HMAC-SHA1 over a 0x14-byte key
     * The 0x14-byte case only ever fills the first 0x10; the tail stays zero. */
    const int enc_key  = (flags & EDAT_ENCRYPTED_KEY_FLAG) != 0;
    const int hash_alg = !(flags & EDAT_FLAG_0x10) ? 2 : (!(flags & EDAT_FLAG_0x20) ? 4 : 1);
    const uint8_t* erk = (version == 4) ? EDAT_KEY_1 : EDAT_KEY_0;

    FILE* out = fopen(out_path, "wb");
    if (!out) { fclose(in); return -7; }

    uint8_t* enc = (uint8_t*)malloc(block_size + 16);
    uint8_t* dec = (uint8_t*)malloc(block_size + 16);
    if (!enc || !dec) { free(enc); free(dec); fclose(in); fclose(out); return -8; }

    uint8_t rk_file[176];
    aes128_expand(key, rk_file);

    int rc = 0;
    uint64_t written = 0;
    uint8_t magic4[4] = {0};
    for (uint32_t b = 0; b < total_blocks; b++) {
        uint64_t meta_off = meta_leads
            ? 0x100 + (uint64_t)b * (meta_size + block_size)
            : 0x100 + (uint64_t)b * meta_size;
        uint64_t data_off = meta_leads ? meta_off + meta_size
                                       : data_base + (uint64_t)b * block_size;

        uint8_t meta[0x20];
        if (fseek(in, (long)meta_off, SEEK_SET) != 0 ||
            fread(meta, 1, meta_size, in) != meta_size) {
            fprintf(stderr, "[edat] block %u/%u: metadata read failed -- file "
                            "shorter than the NPD layout\n", b, total_blocks - 1);
            rc = -9; break;
        }

        /* With FLAG_0x20 the stored hash is the first 0x14 metadata bytes,
         * the first 0x10 of them xor-folded with the second 0x10 (RPCS3
         * decrypt_block). The 0x10-record table layout stores just 0x10. */
        uint8_t block_hash[20] = {0};
        if (meta_leads) {
            memcpy(block_hash, meta, 0x14);
            for (int k = 0; k < 16; k++)
                block_hash[k] = (uint8_t)(meta[k] ^ meta[k + 16]);
        } else
            memcpy(block_hash, meta, 16);

        uint64_t len = block_size;
        if (b == total_blocks - 1 && (file_size % block_size))
            len = file_size % block_size;
        uint64_t padded = (len + 15) & ~(uint64_t)15;

        if (fseek(in, (long)data_off, SEEK_SET) != 0 ||
            fread(enc, 1, (size_t)padded, in) != padded) {
            fprintf(stderr, "[edat] block %u/%u: data read failed (want %llu bytes "
                            "@ %llu) -- file shorter than the NPD layout\n",
                    b, total_blocks - 1, (unsigned long long)padded,
                    (unsigned long long)data_off);
            rc = -10; break;
        }

        /* Block key. Version 0/1 seeds from zero and uses a zero CBC IV;
         * version 2+ seeds from dev_hash and uses the NPD digest as the IV.
         * Getting any of it wrong fails the block hash rather than producing
         * garbage, which is exactly why the hash is checked. */
        uint8_t bk[16] = {0}, kres[16];
        if (version > 1) memcpy(bk, dev_hash, 12);
        bk[12] = (uint8_t)(b >> 24); bk[13] = (uint8_t)(b >> 16);
        bk[14] = (uint8_t)(b >> 8);  bk[15] = (uint8_t)b;
        aes128_encrypt_block(rk_file, bk, kres);

        /* The hash seed is the block key run through the file key a second
         * time when 0x10 is set. */
        uint8_t hseed[16];
        if (flags & EDAT_FLAG_0x10) aes128_encrypt_block(rk_file, kres, hseed);
        else                        memcpy(hseed, kres, 16);

        /* An encrypted ERK unwraps the CBC key and the hash key under EDAT_KEY
         * (note: EDAT_KEY, not EDAT_HASH -- the hash constants are only used by
         * the default-ERK mode, which nothing here selects). The IV is taken
         * as-is either way. */
        static const uint8_t zero_iv[16] = {0};
        uint8_t ckey[16], hkey[20] = {0};
        if (enc_key) {
            aes128_cbc_decrypt(erk, zero_iv, kres,  ckey, 16);
            aes128_cbc_decrypt(erk, zero_iv, hseed, hkey, 16);
        } else {
            memcpy(ckey, kres, 16);
            memcpy(hkey, hseed, 16);
        }

        uint8_t mac[20];
        size_t hash_len = 16;
        if (hash_alg == 2) {
            aes128_cmac(hkey, enc, (size_t)padded, mac);
        } else {
            hmac_sha1(hkey, (hash_alg == 1) ? 20 : 16, enc, (size_t)padded, mac);
            if (hash_alg == 1) hash_len = 20;   /* 0x14-byte HMAC-SHA1 mode */
        }
        if (memcmp(mac, block_hash, hash_len) != 0) {
            fprintf(stderr, "[edat] block %u hash mismatch -- wrong key or corrupt "
                            "file; stopping rather than writing garbage\n", b);
            rc = -11; break;
        }

        if (flags & EDAT_FLAG_0x02) {
            /* crypto_mode 0x1 ("no algorithm", RPCS3 decrypt()): the block is
             * stored plain; only the hash above guards it. */
            memcpy(dec, enc, (size_t)padded);
        } else {
            aes128_cbc_decrypt(ckey, (version <= 1) ? zero_iv : digest,
                               enc, dec, (size_t)padded);
        }
        if (b == 0 && len >= 4) memcpy(magic4, dec, 4);
        if (fwrite(dec, 1, (size_t)len, out) != len) {
            fprintf(stderr, "[edat] block %u/%u: output write failed\n",
                    b, total_blocks - 1);
            rc = -12; break;
        }
        written += len;
    }

    free(enc); free(dec);
    fclose(in);
    fclose(out);

    if (rc == 0) {
        char pm[5];
        for (int i = 0; i < 4; i++) {
            uint8_t c = magic4[i];
            pm[i] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
        }
        pm[4] = 0;
        fprintf(stderr, "[edat] %s: klic=%s dev_klic=%s flags=0x%08X -> %llu bytes, "
                        "magic '%s' (%s)\n",
                in_path, key_src, klic_verdict, flags,
                (unsigned long long)written, pm, out_path);
    } else
        remove(out_path);   /* never leave a half-written cache behind */
    return rc;
}

int edat_decrypt_file(const char* in_path, const char* out_path)
{
    return edat_decrypt_file_klic(in_path, out_path, NULL, NULL);
}

/* The guest's klicensee as sceNpDrmIsAvailable's r4 hands it over (noted by
 * libs/network/sceNp.c). Global on purpose: one title, one klicensee -- and
 * it is never trusted; every file re-runs the dev_hash CMAC gate on it. */
static uint8_t edat_guest_klic[16];
static int     edat_guest_klic_set = 0;

void edat_note_guest_klic(const uint8_t klic[16])
{
    memcpy(edat_guest_klic, klic, 16);
    edat_guest_klic_set = 1;
}

int edat_get_guest_klic(uint8_t out[16])
{
    if (!edat_guest_klic_set) return 0;
    memcpy(out, edat_guest_klic, 16);
    return 1;
}

const char* edat_resolve_klic(const char* host_path, char* buf, size_t cap,
                              const uint8_t klic[16], const char* klic_source)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char* e = getenv("PS3_EDAT");
        enabled = (e && e[0] == '0') ? 0 : 1;
    }
    if (!enabled || !host_path || !edat_is_npd(host_path)) return host_path;

    /* This door got no klic (plain cellFsOpen): fall back to the one
     * sceNpDrmIsAvailable noted, ahead of the env chain. Same rule as the
     * arg door -- the dev_hash CMAC gate decides, per file, verdict logged. */
    uint8_t noted[16];
    if (!klic && edat_get_guest_klic(noted)) {
        klic = noted;
        klic_source = "np";
    }

    if ((size_t)snprintf(buf, cap, "%s.dec", host_path) >= cap) return host_path;

    struct stat ss, ds;
    if (stat(host_path, &ss) == 0 && stat(buf, &ds) == 0 && ds.st_mtime >= ss.st_mtime) {
        /* Cache hit: still report whether the supplied klic is the right one,
         * so the one-line-per-file log stays complete. */
        if (klic) {
            int v = edat_validate_dev_klic(host_path, klic);
            fprintf(stderr, "[edat] %s: cache %s (klic from %s, validate_dev_klic: %s)\n",
                    host_path, buf, klic_source ? klic_source : "?",
                    v == 1 ? "PASS" : v == 0 ? "FAIL" : "n/a");
        }
        return buf;                                    /* cache still valid */
    }

    if (edat_decrypt_file_klic(host_path, buf, klic, klic_source) != 0) return host_path;
    return buf;
}

const char* edat_resolve(const char* host_path, char* buf, size_t cap)
{
    return edat_resolve_klic(host_path, buf, cap, NULL, NULL);
}
