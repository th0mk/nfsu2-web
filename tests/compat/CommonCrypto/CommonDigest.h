/* CommonCrypto's SHA-256 for a native test build off macOS (tests/srvoracle.sh
 * on Linux): runtime/web/sha256.c, as the wasm build has it (runtime/module.c
 * pins the lifted server.dll with this hash). */
#pragma once
#include <stdint.h>
#include <stddef.h>
void nfsu2_sha256(const void *data, size_t n, uint8_t out[32]);
#define CC_SHA256_DIGEST_LENGTH 32
#define CC_SHA256(b, n, d) nfsu2_sha256((b), (n), (d))
#define CC_LONG size_t
