/* Opt-in complete IMEM DMA images for ROM/overlay auditing. No ROM bytes
 * are embedded in the program. Captures use host-native word order. */
#include <stdio.h>
#include <stdlib.h>
void rsp_capture_imem(const unsigned char *image, const unsigned char *dmem,
    unsigned int begin, unsigned int source, unsigned int encoded, unsigned int pc)
{
    static int initialized;
    static const char *directory;
    static uint64_t seen[256];
    static unsigned int nseen;
    static int truncated;
    uint64_t hash = UINT64_C(1469598103934665603);
    unsigned int bytes = (((encoded & 0xfffu) | 7u) + 1u) *
                         (((encoded >> 12) & 255u) + 1u);
    unsigned int i;
    char path[1024];
    FILE *f;
    if (!initialized) { directory = getenv("RSP_UCODE_AUDIT_DIR"); initialized = 1; }
    if (!directory) return;
    begin &= 0xff8u;
    if (bytes > 4096u) bytes = 4096u;
    for (i = 0; i < bytes; i++) hash = (hash ^ image[(begin + i) & 4095u]) * UINT64_C(1099511628211);
    hash ^= (uint64_t)begin << 32 | bytes;
    for (i = 0; i < nseen; i++) if (seen[i] == hash) return;
    if (nseen == 256) {
        if (!truncated) fprintf(stderr, "RSP microcode audit truncated after 256 unique IMEM transfers\n");
        truncated = 1;
        return;
    }
    seen[nseen++] = hash;
    snprintf(path, sizeof(path), "%s/imem-%03u-%016llx.bin", directory, nseen - 1,
             (unsigned long long)hash);
    f = fopen(path, "wb");
    if (!f) return;
    fwrite(image, 4096, 1, f); fclose(f);
    snprintf(path, sizeof(path), "%s/imem-%03u-%016llx.dmem", directory, nseen - 1,
             (unsigned long long)hash);
    f = fopen(path, "wb");
    if (f) { fwrite(dmem, 4096, 1, f); fclose(f); }
    snprintf(path, sizeof(path), "%s/imem.csv", directory);
    f = fopen(path, nseen == 1 ? "w" : "a");
    if (!f) return;
    if (nseen == 1) fprintf(f, "image,hash,imem,rdram,bytes,encoded_length,pc,task_text,task_data\n");
    fprintf(f, "%u,%016llx,%x,%x,%x,%x,%x,%x,%x\n", nseen - 1,
        (unsigned long long)hash, begin, source & 0x7ffff8u, bytes,
        encoded, pc & 4095u, ((const uint32_t *)dmem)[0xfd0u / 4],
        ((const uint32_t *)dmem)[0xfd8u / 4]);
    fclose(f);
}
