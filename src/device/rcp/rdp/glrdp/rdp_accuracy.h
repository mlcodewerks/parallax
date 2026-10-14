#pragma once
#include <stdbool.h>

// Coverage remains independent of scanout filters. Off retains geometric
// ownership and rejection tests, but treats surviving pixels as fully covered.
enum GLRDPCoverage { GLRDP_COVERAGE_OFF = 0, GLRDP_COVERAGE_REDUCED = 1, GLRDP_COVERAGE_FULL = 2 };
enum GLRDPVIAA { GLRDP_VI_AA_OFF = 0, GLRDP_VI_AA_REDUCED = 1, GLRDP_VI_AA_FULL = 2 };
struct GLRDPConfig {
    enum GLRDPCoverage coverage;
    enum GLRDPVIAA vi_aa;
    bool vi_restore, vi_divot, vi_gamma, vi_gamma_dither, vi_resample;
};
// Frontend defaults preserve the former Fast preset. Low-level renderers use
// full emulation by default for reference clients that supply no configuration.
static inline struct GLRDPConfig glrdp_default_config(void) {
    struct GLRDPConfig config = {
        GLRDP_COVERAGE_OFF, GLRDP_VI_AA_OFF, false, false, true, true, true};
    return config;
}
static inline struct GLRDPConfig glrdp_reference_config(void) {
    struct GLRDPConfig config = {
        GLRDP_COVERAGE_FULL, GLRDP_VI_AA_FULL, true, true, true, true, true};
    return config;
}
static inline struct GLRDPConfig glrdp_validate_config(struct GLRDPConfig config) {
    if ((unsigned)config.coverage > GLRDP_COVERAGE_FULL)
        config.coverage = GLRDP_COVERAGE_FULL;
    if ((unsigned)config.vi_aa > GLRDP_VI_AA_FULL)
        config.vi_aa = GLRDP_VI_AA_FULL;
    return config;
}
// Disabling AA does not enable resampling when the game requested point scanout.
static inline unsigned glrdp_vi_aa_mode(struct GLRDPConfig config, unsigned status) {
    unsigned mode = (status >> 8) & 3;
    return config.vi_aa == GLRDP_VI_AA_OFF && mode < 2 ? 2 : mode;
}
