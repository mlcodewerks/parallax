#include "libretro_glrdp.h"
#include "gl_loader.hpp"
#include "gl_state_guard.hpp"
#include "rdp_parser.hpp"
#include "rdp_renderer.hpp"
#include "renderer_options.h"
#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>
extern "C" {
#include "core/backend.h"
#include "rdp_core.h"
extern renderer_memory gfx_info;
extern retro_log_printf_t log_cb;
extern bool libretro_swap_buffer;
}

namespace {
retro_hw_render_callback hw{};
N64Memory rdram_memory;
std::unique_ptr<RDPCommandParser> parser;
std::unique_ptr<RDPRenderer> renderer;
std::unique_ptr<GLStateGuard> frame_guard;
bool ready = false, opened = false, gles3 = false;
std::vector<uint8_t> command_bytes;
using ContextGuard = GLStateGuard;

void error(const char *operation, const char *detail) {
    if (log_cb)
        log_cb(RETRO_LOG_ERROR, "GLRDP %s: %s\n", operation, detail);
}
bool initialize_renderer() {
    ContextGuard guard;
    renderer = std::make_unique<RDPRenderer>();
    renderer->set_rendering_config(renderer_settings.glrdp);
    if (!renderer->initialize(&rdram_memory))
        return false;
    renderer->set_internal_resolution_scale(renderer_settings.upscale);
    renderer->set_output_size(640, 480);
    return true;
}
} // namespace

bool glrdp_context_reset(const retro_hw_render_callback *callback) {
    hw = *callback;
    gles3 = hw.context_type == RETRO_HW_CONTEXT_OPENGLES3 ||
          hw.context_type == RETRO_HW_CONTEXT_OPENGLES_VERSION;
    ready = load_gl33(hw.get_proc_address, gles3);
    if (!ready) {
        error("context", "required GL 3.3 / GLES3 entry points unavailable");
        return false;
    }
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "GLRDP: %s; %s.\n", glGetString(GL_VERSION),
               gles3 ? "native GLES3 RDP" : "native GL 3.3 RDP");
    if (log_cb) {
        const auto &c = renderer_settings.glrdp;
        const char *levels[] = {"off", "reduced", "full", "2x"};
        log_cb(
            RETRO_LOG_INFO,
            "GLRDP filters = coverage=%s aa=%s restore=%u divot=%u resample=%u gamma=%u gamma_dither=%u.\n",
            levels[c.coverage], levels[c.vi_aa], unsigned(c.vi_restore), unsigned(c.vi_divot),
            unsigned(c.vi_resample), unsigned(c.vi_gamma), unsigned(c.vi_gamma_dither));
        log_cb(
            RETRO_LOG_INFO,
            "GLRDP ubershader = blending=%s combiner=%s vi_filters=%u texture_barrier=%u texture_filter=%s.\n",
            c.blending == GLRDP_ARITHMETIC_FAST ? "fast" : "accurate",
            c.combiner == GLRDP_ARITHMETIC_FAST ? "fast" : "accurate", unsigned(c.vi_filter),
            unsigned(glrdp_texture_barrier != nullptr), c.texture_filter ? "native" : "point");
        log_cb(RETRO_LOG_INFO,
               "GLRDP tradeoffs = texcoords=%s lod=%u color_dither=%u alpha_dither=%u.\n",
               c.accurate_texcoords ? "accurate" : "fast", unsigned(c.texture_lod),
               unsigned(c.color_dither), unsigned(c.alpha_dither));
        log_cb(RETRO_LOG_INFO, "GLRDP shader controls = shading=%s specialization=%u.\n",
               c.accurate_shading ? "accurate" : "fast", unsigned(c.specialize_shaders));
    }
    try {
        // Compile before emulation starts. RDRAM is bound by glrdp_open;
        // initialization only creates GPU resources and stores its wrapper.
        ready = initialize_renderer();
    } catch (const std::exception &e) {
        error("context reset", e.what());
        ready = false;
    }
    return ready;
}
void glrdp_context_destroy() {
    frame_guard.reset();
    if (renderer) {
        renderer->shutdown();
        renderer.reset();
    }
    ready = false;
}
bool glrdp_open() {
    if (!ready) {
        error("open", "frontend has not reset its graphics context");
        return false;
    }
    rdram_memory.bind(gfx_info.RDRAM, *gfx_info.RDRAM_SIZE, true);
    try {
        parser = std::make_unique<RDPCommandParser>();
        parser->set_memory(&rdram_memory);
        parser->callbacks().draw_triangle = [](const auto &t, const auto &s) {
            renderer->draw_triangle(t, s);
        };
        parser->callbacks().draw_texture_rectangle = [](const auto &t, const auto &s) {
            renderer->draw_texture_rectangle(t, s);
        };
        parser->callbacks().draw_fill_rectangle = [](const auto &t, const auto &s) {
            renderer->draw_fill_rectangle(t, s);
        };
        parser->callbacks().before_texture_load = [](const auto &state) {
            renderer->fb_read(state.texture_image.address);
        };
        // A retired depth allocation is no longer reported in FBInfo. Commit
        // it before CPU/DMA can reuse that RAM without write notifications.
        parser->callbacks().before_z_image_change = [](const auto &state) {
            renderer->fb_read(state.z_image_address);
        };
        // The core schedules DP completion. Never alter DPC_CLOCK or deliver an early IRQ.
        parser->callbacks().sync_full = [] { if (renderer) renderer->flush(); *gfx_info.MI_INTR_REG |= 0x20; };
        opened = renderer || initialize_renderer();
    } catch (const std::exception &e) {
        error("open", e.what());
        glrdp_close();
    }
    return opened;
}
void glrdp_close() {
    if (renderer) {
        renderer->shutdown();
        renderer.reset();
    }
    parser.reset();
    opened = false;
    command_bytes.clear();
}
void glrdp_process_commands() {
    if (!ready || !opened || !parser || !renderer)
        return;
    const uint32_t start = *gfx_info.DPC_CURRENT_REG & 0x00fffff8;
    const uint32_t end = *gfx_info.DPC_END_REG & 0x00fffff8;
    if ((*gfx_info.DPC_STATUS_REG & 2) || end <= start)
        return;
    const uint32_t length = end - start;
    if (length > 0x1000000)
        return;
    try {
        ContextGuard guard;
        command_bytes.resize(length);
        const uint32_t *hle = rdp_hle_command_buffer(start);
        if (hle) {
            for (uint32_t i = 0; i < length; ++i)
                command_bytes[i] = uint8_t(hle[i / 4] >> ((3 - (i & 3)) * 8));
        } else if (*gfx_info.DPC_STATUS_REG & 1) {
            for (uint32_t i = 0; i < length; ++i)
                command_bytes[i] = gfx_info.DMEM[((start + i) & 0xfff) ^ 3];
        } else {
            rdram_memory.copy_be(start, command_bytes.data(), length);
        }
        parser->feed(command_bytes);
        if (renderer) renderer->flush();
    } catch (const std::exception &e) {
        error("command list", e.what());
        parser->discard_pending();
    }
    *gfx_info.DPC_CURRENT_REG = end;
    // DPC status and completion are maintained by rdp_core, identically across backends.
}
void glrdp_begin_frame() {
    if (!ready)
        return;
    try {
        if (!frame_guard) frame_guard = std::make_unique<GLStateGuard>();
    } catch (const std::exception &e) {
        error("frame state", e.what());
    }
}
void glrdp_end_frame() {
    if (renderer) renderer->flush();
    frame_guard.reset();
}
void glrdp_present() {
    if (!ready || !opened || !renderer || !parser)
        return;
    try {
        ContextGuard guard;
        auto present = [&](auto &gpu) {
            gpu.set_output_framebuffer(GLuint(hw.get_current_framebuffer()));
            gpu.present(parser->state(), *gfx_info.VI_STATUS_REG,
                        *gfx_info.VI_ORIGIN_REG & 0xffffff, *gfx_info.VI_WIDTH_REG,
                        *gfx_info.VI_H_START_REG, *gfx_info.VI_V_START_REG,
                        *gfx_info.VI_X_SCALE_REG, *gfx_info.VI_Y_SCALE_REG, *gfx_info.VI_V_SYNC_REG,
                        *gfx_info.VI_V_CURRENT_LINE_REG);
        };
        present(*renderer);
        libretro_swap_buffer = true;
    } catch (const std::exception &e) {
        error("present", e.what());
    }
}
void glrdp_fb_read(uint32_t address) {
    if (!ready || !renderer)
        return;
    try {
        renderer->fb_read(address);
    } catch (const std::exception &e) {
        error("framebuffer read", e.what());
    }
}
void glrdp_fb_write(uint32_t address, uint32_t size) {
    if (renderer)
        renderer->fb_write(address, size);
}
void glrdp_fb_info(void *data) {
    auto *info = static_cast<core_framebuffer_info *>(data);
    std::memset(info, 0, 6 * sizeof(*info));
    if (!renderer || !parser)
        return;
    auto &manager = renderer->framebuffers();
    std::vector<const GLFramebuffer *> buffers;
    for (const auto &[key, f] : manager.buffers())
        if (f.fbo && f.address && f.size != RDPImageSize::Bits4)
            buffers.push_back(&f);
    std::sort(buffers.begin(), buffers.end(), [](auto a, auto b) { return a->serial > b->serial; });
    unsigned index = 0;
    for (const auto *f : buffers) {
        const unsigned size = f->size == RDPImageSize::Bits32   ? 4
                              : f->size == RDPImageSize::Bits16 ? 2
                                                                : 1;
        if (uint64_t(f->address) + uint64_t(f->native_width) * f->native_height * size >
            rdram_memory.size())
            continue;
        info[index++] = {f->address, size, unsigned(f->native_width), unsigned(f->native_height)};
        if (index == FramebufferManager::cpu_color_slots)
            break;
    }
    const auto &state = parser->state();
    const auto dimensions = renderer->depth_dimensions(state.z_image_address);
    bool already_listed = false;
    for (unsigned i = 0; i < index; ++i)
        already_listed |= info[i].addr == state.z_image_address;
    // SetZImage is a register assignment, not ownership of a GPU image. Only
    // protect an actual depth surface, using its own layout rather than the
    // latest color target (which may be a small offscreen texture).
    if (index && !already_listed && state.z_image_address && dimensions.first > 0 &&
        dimensions.second > 0 &&
        uint64_t(state.z_image_address) + uint64_t(dimensions.first) * dimensions.second * 2 <=
            rdram_memory.size())
        info[index] = {state.z_image_address, 2, unsigned(dimensions.first),
                       unsigned(dimensions.second)};
}
