/* edat.h -- transparent NPDRM (EDAT/SDAT) decryption for the filesystem layer.
 *
 * PSN content ships its payload as an EDAT: an NPD header followed by AES-encrypted
 * blocks. On hardware the guest does NOT decrypt these itself -- it calls
 * sceNpDrmIsAvailable(klicensee, path), which primes the kernel, and every later
 * cellFsOpen/read of that path returns PLAINTEXT. A runtime that opens the file
 * literally hands the guest ciphertext, and the guest reads a garbage header.
 *
 * ps1_netemu hits exactly this: it opens USRDIR/ISO.BIN.EDAT, finds nonsense, and
 * exits with `ExitPS1(): code=3` / `CoreBoot() failed`.
 *
 * So the FS layer resolves it instead: on open, a file beginning "NPD\0" is decrypted
 * once into a cache file and the cache is opened in its place. Every read/seek/stat
 * path above then works unchanged, which is why this is a path substitution rather
 * than a read hook.
 *
 * Scope: uncompressed EDATs with AES-CMAC block hashes -- the common case, and what
 * PSOne Classics use. Compressed blocks and the SHA1-HMAC hash modes are detected and
 * refused by name rather than silently mis-decrypted.
 *
 * Keys are the published NPDRM constants (the same ones RPCS3, scetool and every EDAT
 * tool carry); a license-type-3 "free" EDAT needs no per-console RAP. A type-1/2 EDAT
 * is bound to a RAP we do not have, and is refused with that stated.
 */
#ifndef PS3RECOMP_EDAT_H
#define PS3RECOMP_EDAT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* True if the file at host_path starts with the NPD magic. Cheap: one 4-byte read. */
int edat_is_npd(const char* host_path);

/* Decrypt in_path -> out_path. 0 on success, negative on failure (reason logged). */
int edat_decrypt_file(const char* in_path, const char* out_path);

/* RPCS3 Crypto/unedat.cpp validate_dev_klic, exposed: for license type 3 the
 * NPD dev_hash must equal AES-CMAC(klic ^ NP_OMAC_KEY_2) over the header's
 * first 0x60 bytes. This is what PROVES a supplied klicensee is the file's
 * real key before any data is trusted. Returns 1 PASS, 0 FAIL, -1 not NPD. */
int edat_validate_dev_klic(const char* host_path, const uint8_t klic[16]);

/* edat_decrypt_file with an explicit klicensee -- the one the guest handed
 * cellFsSdataOpen through its arg/size pair (MotorStorm's pack EDATs are
 * license type 3: the klicensee IS the file key). The klic is never taken on
 * trust: it must PASS edat_validate_dev_klic (logged PASS/FAIL either way)
 * before it keys the blocks; on FAIL the built-in chain (known klicensees,
 * then the env/.rifkey RIF key) runs exactly as before. klic==NULL degrades
 * to plain edat_decrypt_file. */
int edat_decrypt_file_klic(const char* in_path, const char* out_path,
                           const uint8_t klic[16], const char* klic_source);

/* The path the FS layer should actually open for `host_path`.
 *
 * Returns host_path unchanged when it is not an EDAT, or when decryption fails (the
 * caller then opens the original and the guest reports its own error -- better than
 * an open() failure it cannot attribute). Otherwise returns a cache path, written
 * into `buf`. The cache lives beside the source as "<name>.dec" and is reused while
 * it is newer than the source. Set PS3_EDAT=0 to disable the whole path. */
const char* edat_resolve(const char* host_path, char* buf, size_t cap);

/* edat_resolve with the guest klicensee: same "<name>.dec" cache, but the
 * decrypt is keyed by `klic` after validation. klic==NULL -> edat_resolve,
 * i.e. the env-key-only path cellFsOpen keeps. */
const char* edat_resolve_klic(const char* host_path, char* buf, size_t cap,
                              const uint8_t klic[16], const char* klic_source);

/* Hand the guest's klicensee to the resolver: sceNpDrmIsAvailable receives
 * it (firmware ABI: r4 points at the 16 bytes) long before any file is
 * opened, and the plain cellFsOpen door has no klic argument of its own.
 * Once noted, edat_resolve picks it up and runs it through the SAME
 * validated chain -- it must still PASS edat_validate_dev_klic for each
 * file (PASS/FAIL logged) before it keys anything. */
void edat_note_guest_klic(const uint8_t klic[16]);

/* Copy the noted klicensee into out[16]. Returns 1 with the bytes, 0 if
 * none was noted. */
int edat_get_guest_klic(uint8_t out[16]);

/* Self-check of the AES-128 and AES-CMAC primitives against the FIPS-197 and
 * RFC 4493 vectors. Returns 0 if both match. Run once on first use: silently
 * wrong crypto here produces plausible-looking garbage, which is the single most
 * expensive failure mode this file can have. */
int edat_selftest(void);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_EDAT_H */
