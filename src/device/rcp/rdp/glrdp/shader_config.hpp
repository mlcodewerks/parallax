#pragma once
#include "rdp_accuracy.h"
#include <cstdint>
#include <string>

// Only draw-shader settings belong here. VI choices do not invalidate a
// specialization. The generic program handles later configuration changes.
inline std::uint32_t glrdp_shader_config_key(const GLRDPConfig &c) {
    return unsigned(c.coverage) | (unsigned(c.blending) << 2) |
           (unsigned(c.combiner) << 3) | (unsigned(c.texture_filter) << 4) |
           (unsigned(c.accurate_texcoords) << 5) | (unsigned(c.texture_lod) << 6) |
           (unsigned(c.color_dither) << 7) | (unsigned(c.alpha_dither) << 8) |
           (unsigned(c.accurate_shading) << 9);
}
inline void glrdp_shader_constant(std::string &source, const std::string &type,
                                   const std::string &name, const std::string &value) {
    const std::string declaration = "uniform " + type + " " + name + ";";
    const auto at = source.find(declaration);
    if (at != std::string::npos)
        source.replace(at, declaration.size(), "const " + type + " " + name + " = " + value + ";");
}
inline std::string glrdp_specialize_shader(std::string source, const GLRDPConfig &c, bool es2) {
    if (es2) {
        glrdp_shader_constant(source, "vec4", "accuracy_config",
            "vec4(" + std::to_string(unsigned(c.coverage)) + ".0," +
            std::to_string(unsigned(c.blending)) + ".0," +
            std::to_string(unsigned(c.combiner)) + ".0," +
            (c.accurate_texcoords ? "1.0)" : "0.0)"));
    } else {
        glrdp_shader_constant(source, "int", "coverage_mode", std::to_string(unsigned(c.coverage)));
        // Split declarations permit independent compile-time constants.
        glrdp_shader_constant(source, "bool", "fast_blending", c.blending ? "true" : "false");
        glrdp_shader_constant(source, "bool", "fast_combiner", c.combiner ? "true" : "false");
        glrdp_shader_constant(source, "bool", "accurate_texcoords", c.accurate_texcoords ? "true" : "false");
        if (!c.texture_filter) glrdp_shader_constant(source, "int", "texture_filter_mode", "0");
        if (!c.texture_lod) glrdp_shader_constant(source, "bool", "lod_enable", "false");
    }
    glrdp_shader_constant(source, "bool", "accurate_shading", c.accurate_shading ? "true" : "false");
    return source;
}
