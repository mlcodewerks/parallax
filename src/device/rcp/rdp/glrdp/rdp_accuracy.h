#pragma once
#include <stdbool.h>

// Coverage remains independent of scanout filters. Off retains geometric
// ownership and rejection tests, but treats surviving pixels as fully covered.
enum GLRDPCoverage { GLRDP_COVERAGE_OFF = 0, GLRDP_COVERAGE_REDUCED = 1, GLRDP_COVERAGE_FULL = 2 };
enum GLRDPVIAA { GLRDP_VI_AA_OFF = 0, GLRDP_VI_AA_REDUCED = 1, GLRDP_VI_AA_FULL = 2 };
enum GLRDPArithmetic { GLRDP_ARITHMETIC_ACCURATE = 0, GLRDP_ARITHMETIC_FAST = 1 };
struct GLRDPConfig {
    enum GLRDPCoverage coverage;
    enum GLRDPVIAA vi_aa;
    bool vi_restore, vi_divot, vi_gamma, vi_gamma_dither, vi_resample;
    enum GLRDPArithmetic blending, combiner;
    bool vi_filter;
    bool texture_filter;
    bool accurate_texcoords, texture_lod, color_dither, alpha_dither;
    bool vi_bob;
};
// Frontend defaults preserve the former Fast preset. Low-level renderers use
// full emulation by default for reference clients that supply no configuration.
static inline struct GLRDPConfig glrdp_default_config(void) {
    struct GLRDPConfig config = {GLRDP_COVERAGE_OFF,
                                 GLRDP_VI_AA_OFF,
                                 false,
                                 false,
                                 true,
                                 true,
                                 true,
                                 GLRDP_ARITHMETIC_ACCURATE,
                                 GLRDP_ARITHMETIC_ACCURATE,
                                 true,
                                 true,
                                 true,
                                 true,
                                 true,
                                 true,
                                 false};
    return config;
}
static inline struct GLRDPConfig glrdp_reference_config(void) {
    struct GLRDPConfig config = {GLRDP_COVERAGE_FULL,
                                 GLRDP_VI_AA_FULL,
                                 true,
                                 true,
                                 true,
                                 true,
                                 true,
                                 GLRDP_ARITHMETIC_ACCURATE,
                                 GLRDP_ARITHMETIC_ACCURATE,
                                 true,
                                 true,
                                 true,
                                 true,
                                 true,
                                 true,
                                 false};
    return config;
}
static inline struct GLRDPConfig glrdp_validate_config(struct GLRDPConfig config) {
    if ((unsigned)config.coverage > GLRDP_COVERAGE_FULL)
        config.coverage = GLRDP_COVERAGE_FULL;
    if ((unsigned)config.vi_aa > GLRDP_VI_AA_FULL)
        config.vi_aa = GLRDP_VI_AA_FULL;
    if ((unsigned)config.blending > GLRDP_ARITHMETIC_FAST)
        config.blending = GLRDP_ARITHMETIC_ACCURATE;
    if ((unsigned)config.combiner > GLRDP_ARITHMETIC_FAST)
        config.combiner = GLRDP_ARITHMETIC_ACCURATE;
    return config;
}
// Apply the master switch at scanout, retaining the individual selections.
static inline struct GLRDPConfig glrdp_scanout_config(struct GLRDPConfig config) {
    if (!config.vi_filter) {
        config.vi_aa = GLRDP_VI_AA_OFF;
        config.vi_restore = config.vi_divot = config.vi_resample = false;
        config.vi_gamma = config.vi_gamma_dither = false;
    }
    return config;
}
// Disabling AA does not enable resampling when the game requested point scanout.
static inline unsigned glrdp_vi_aa_mode(struct GLRDPConfig config, unsigned status) {
    unsigned mode = (status >> 8) & 3;
    return config.vi_aa == GLRDP_VI_AA_OFF && mode < 2 ? 2 : mode;
}
