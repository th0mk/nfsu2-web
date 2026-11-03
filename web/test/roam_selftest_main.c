/* roam_selftest_main.c -- tests/roam_selftest.c's byte stream from the wasm
 * build of the free-roam core: its length and FNV-1a, to compare with the
 * Mac (arm64) and the 32-bit x86 builds (tools/roam_x86check.sh: 37,880
 * bytes, FNV-1a e90f893c).  Run by web/test/roam-wasm.test.mjs. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
int roam_selftest(uint8_t *out, int cap);
int main(void){
    int cap = 1 << 20;
    uint8_t *b = malloc((size_t)cap);
    int n = roam_selftest(b, cap);
    uint32_t h = 0x811C9DC5u;
    for (int i = 0; i < n; i++){ h ^= b[i]; h *= 0x01000193u; }
    printf("roam_selftest: %d bytes, FNV-1a %08x\n", n, h);
    return 0;
}
