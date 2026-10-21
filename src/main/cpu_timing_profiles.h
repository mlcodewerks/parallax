#ifndef PARALLAX_CPU_TIMING_PROFILES_H
#define PARALLAX_CPU_TIMING_PROFILES_H
#include <string.h>

enum cpu_issue_policy { CPU_ISSUE_DEFAULT, CPU_ISSUE_NATIVE, CPU_ISSUE_COMPATIBILITY };

/* Measured compatibility profiles, keyed by the complete ROM digest. These
 * select an existing CPU model; they do not alter COUNT/VI rates or game code.
 * Unknown revisions retain the default policy until separately validated. */
static inline unsigned rom_cpu_issue_policy(const char* md5)
{
    /* Beetle Adventure Racing (USA): reference intro loading/pacing. */
    if (!strcmp(md5, "CF97C336479DDBF1217E4DDE89D9D2D3")) return CPU_ISSUE_NATIVE;
    /* Donkey Kong 64 (USA): both gaps in the post-Rap vine demonstration. */
    if (!strcmp(md5, "9EC41ABF2519FC386CADD0731F6E868C")) return CPU_ISSUE_COMPATIBILITY;
    return CPU_ISSUE_DEFAULT;
}
#endif
