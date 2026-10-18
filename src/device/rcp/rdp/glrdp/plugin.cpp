#include "n64_memory.hpp"
#include "plugin_api.hpp"
#include "plugin_context.hpp"
#include "rdp_parser.hpp"
#include "rdp_renderer.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
GFX_INFO g_info{};
PluginGLContext g_gl;
N64Memory g_memory;
std::unique_ptr<RDPCommandParser> g_parser;
std::unique_ptr<RDPRenderer> g_renderer;
std::vector<std::uint8_t> g_rdram_be;
bool g_initiated = false;
void(PJ64_CALL *g_render_callback)(int) = nullptr;
constexpr std::size_t kRdramSize = 8u * 1024u * 1024u;
constexpr std::uint32_t kDpcStatusXbusDmemDma = 0x0001u;
constexpr std::uint32_t kDpcStatusFreeze = 0x0002u;
constexpr std::uint32_t kDpcStatusFlush = 0x0004u;
constexpr std::uint32_t kDpcStatusTmemBusy = 0x0010u;
constexpr std::uint32_t kDpcStatusPipeBusy = 0x0020u;
constexpr std::uint32_t kDpcStatusCommandBusy = 0x0040u;
constexpr std::uint32_t kDpcStatusCommandBufferReady = 0x0080u;

struct DisplayPreset {
    int id;
    int width;
    int height;
    int aspect_mode;
};

constexpr DisplayPreset kDisplayPresets[] = {
    {1, 640, 480, 1},    {2, 800, 600, 1},    {3, 1024, 768, 1},   {4, 1280, 960, 1},
    {5, 1600, 1200, 1},  {6, 1920, 1440, 1},  {10, 1280, 720, 2},  {11, 1600, 900, 2},
    {12, 1920, 1080, 2}, {13, 2560, 1440, 2}, {14, 3840, 2160, 2},
};

const DisplayPreset *find_display_preset(int id) {
    for (const auto &preset : kDisplayPresets)
        if (preset.id == id)
            return &preset;
    return nullptr;
}

bool parse_resolution(const char *text, int &width, int &height) {
    if (!text)
        return false;
    char *end = nullptr;
    const long w = std::strtol(text, &end, 10);
    if (!end || (*end != 'x' && *end != 'X'))
        return false;
    const long h = std::strtol(end + 1, &end, 10);
    if (!end || *end != '\0' || w <= 0 || h <= 0)
        return false;
    width = int(std::clamp<long>(w, 160, 7680));
    height = int(std::clamp<long>(h, 120, 4320));
    return true;
}

void refresh_rdram() {
    if (!g_info.RDRAM)
        return;
    if (g_rdram_be.size() != kRdramSize)
        g_rdram_be.resize(kRdramSize);
    g_memory.copy_be(0, g_rdram_be.data(), g_rdram_be.size());
    if (g_parser)
        g_parser->set_rdram(g_rdram_be);
}
std::uint32_t reg(std::uint32_t *p) {
    return p ? *p : 0;
}
void signal_dp() {
    if (g_info.MI_INTR_REG)
        *g_info.MI_INTR_REG |= 0x20;
    if (g_info.CheckInterrupts)
        g_info.CheckInterrupts();
}
template <typename Function> void safe_gl_call(Function &&function) noexcept {
    try {
        if (g_initiated && g_renderer && g_gl.make_current())
            function();
    } catch (...) {
        // Never allow C++ exceptions to cross the legacy Project64 C ABI.
    }
}

void configure_callbacks() {
    g_parser->callbacks().draw_triangle = [](const RDPTriangle &t, const RDPState &s) {
        if (g_renderer)
            g_renderer->draw_triangle(t, s);
    };
    g_parser->callbacks().draw_texture_rectangle = [](const RDPTextureRectangle &r,
                                                      const RDPState &s) {
        if (g_renderer)
            g_renderer->draw_texture_rectangle(r, s);
    };
    g_parser->callbacks().draw_fill_rectangle = [](const RDPFillRectangle &r, const RDPState &s) {
        if (g_renderer)
            g_renderer->draw_fill_rectangle(r, s);
    };
    // GL submission is ordered within the current context, so the partial
    // RDP synchronization commands require no host-side action here.
    g_parser->callbacks().sync_load = []() {};
    g_parser->callbacks().sync_pipe = []() {};
    g_parser->callbacks().sync_tile = []() {};
    g_parser->callbacks().sync_full = []() { signal_dp(); };
}
struct FRAME_BUFFER_INFO {
    std::uint32_t addr;
    std::uint32_t size;
    std::uint32_t width;
    std::uint32_t height;
};
} // namespace

PJ64_EXPORT void PJ64_CALL GetDllInfo(PLUGIN_INFO *info) {
    if (!info)
        return;
    std::memset(info, 0, sizeof(*info));
    info->Version = PLUGIN_VERSION;
    info->Type = PLUGIN_TYPE_GFX;
    std::strncpy(info->Name, "RDP OpenGL 3.3 LLE", sizeof(info->Name) - 1);
    info->NormalMemory = FALSE;
    info->MemoryBswaped = TRUE;
}

PJ64_EXPORT BOOL PJ64_CALL InitiateGFX(GFX_INFO info) {
    g_info = info;
    g_memory.bind(info.RDRAM, kRdramSize, info.MemoryBswaped != FALSE);
    if (!g_gl.create(info.hWnd))
        return FALSE;

    try {
        g_renderer = std::make_unique<RDPRenderer>();
        auto config = glrdp_default_config();
        auto level = [](const char *key) {
            const char *value = std::getenv(key);
            return !value || !std::strcmp(value, "off") ? 0
                   : !std::strcmp(value, "reduced")     ? 1
                                                        : 2;
        };
        auto toggle = [](const char *key, bool fallback) {
            const char *value = std::getenv(key);
            return !value                            ? fallback
                   : !std::strcmp(value, "enabled")  ? true
                   : !std::strcmp(value, "disabled") ? false
                                                     : fallback;
        };
        config.coverage = static_cast<GLRDPCoverage>(level("PJ64_RDP_COVERAGE"));
        config.vi_aa = static_cast<GLRDPVIAA>(level("PJ64_VI_AA"));
        config.vi_restore = toggle("PJ64_VI_RESTORE", false);
        config.vi_divot = toggle("PJ64_VI_DIVOT", false);
        config.vi_resample = toggle("PJ64_VI_RESAMPLE", true);
        config.vi_gamma = toggle("PJ64_VI_GAMMA", true);
        config.vi_gamma_dither = toggle("PJ64_VI_GAMMA_DITHER", true);
        g_renderer->set_rendering_config(config);
        if (!g_renderer->initialize(&g_memory))
            throw std::runtime_error("OpenGL 3.3 renderer initialization failed");

        int internal_scale = 1;
        if (const char *value = std::getenv("PJ64_RDP_INTERNAL_SCALE"))
            internal_scale = std::clamp(std::atoi(value), 1, 8);
        g_renderer->set_internal_resolution_scale(internal_scale);

        int display_width = 0, display_height = 0;
        if (parse_resolution(std::getenv("PJ64_RDP_DISPLAY_RESOLUTION"), display_width,
                             display_height)) {
            g_renderer->set_display_resolution(display_width, display_height);
            g_gl.set_client_size(display_width, display_height);
        }

        int aspect_mode = 0;
        if (const char *value = std::getenv("PJ64_RDP_DISPLAY_ASPECT")) {
            if (std::strcmp(value, "4:3") == 0 || std::strcmp(value, "4x3") == 0)
                aspect_mode = 1;
            else if (std::strcmp(value, "16:9") == 0 || std::strcmp(value, "16x9") == 0)
                aspect_mode = 2;
        }
        g_renderer->set_display_aspect_mode(aspect_mode);
        g_rdram_be.resize(kRdramSize);
        refresh_rdram();
        g_parser = std::make_unique<RDPCommandParser>(g_rdram_be);
        configure_callbacks();
        g_initiated = true;
        return TRUE;
    } catch (...) {
        if (g_renderer) {
            try {
                g_renderer->shutdown();
            } catch (...) {
            }
        }
        g_renderer.reset();
        g_parser.reset();
        g_rdram_be.clear();
        g_gl.destroy();
        g_initiated = false;
        return FALSE;
    }
}

PJ64_EXPORT void PJ64_CALL RomOpen(void) {
    safe_gl_call([] {
        g_renderer->reset();
        if (g_parser)
            g_parser->reset();
        refresh_rdram();
    });
}
PJ64_EXPORT void PJ64_CALL RomClosed(void) {
    safe_gl_call([] {
        g_renderer->reset();
        if (g_parser)
            g_parser->reset();
    });
}
PJ64_EXPORT void PJ64_CALL CloseDLL(void) {
    if (g_gl.make_current() && g_renderer)
        g_renderer->shutdown();
    g_renderer.reset();
    g_parser.reset();
    g_gl.destroy();
    g_rdram_be.clear();
    g_initiated = false;
}
PJ64_EXPORT void PJ64_CALL ProcessRDPList(void) {
    if (!g_initiated || !g_parser || !g_renderer)
        return;
    if (!g_gl.make_current())
        return;

    const std::uint32_t status = reg(g_info.DPC_STATUS_REG);
    if (status & kDpcStatusFreeze)
        return;

    if (g_info.DPC_STATUS_REG) {
        *g_info.DPC_STATUS_REG =
            (status | kDpcStatusCommandBusy | kDpcStatusPipeBusy) & ~kDpcStatusCommandBufferReady;
    }
    if (g_info.DPC_PIPEBUSY_REG)
        *g_info.DPC_PIPEBUSY_REG = 1;
    if (g_info.DPC_BUFBUSY_REG)
        *g_info.DPC_BUFBUSY_REG = 1;

    std::uint32_t current = reg(g_info.DPC_CURRENT_REG);
    if (current == 0)
        current = reg(g_info.DPC_START_REG);
    const std::uint32_t end_address = reg(g_info.DPC_END_REG);

    try {
        if (status & kDpcStatusXbusDmemDma) {
            // XBUS command DMA reads the 4 KiB SP DMEM ring and wraps DPC
            // addresses at 0x1000. Copy into a contiguous parser buffer so a
            // command that crosses the DMEM boundary remains intact.
            if (g_info.DMEM) {
                const std::uint32_t begin = current & 0x0ff8u;
                const std::uint32_t finish = end_address & 0x0ff8u;
                const std::uint32_t byte_count = (finish - begin) & 0x0fffu;
                std::vector<std::uint8_t> commands(byte_count);
                for (std::uint32_t i = 0; i < byte_count; ++i)
                    commands[i] = g_info.DMEM[(begin + i) & 0x0fffu];
                g_parser->feed(commands);
            }
            current = end_address & 0x0ff8u;
        } else {
            refresh_rdram();
            const std::uint32_t begin = current & 0x00fffff8u;
            const std::uint32_t finish = end_address & 0x00fffff8u;
            if (finish >= begin && finish <= g_rdram_be.size()) {
                g_parser->feed(
                    std::span<const std::uint8_t>(g_rdram_be.data() + begin, finish - begin));
            }
            current = finish;
        }
    } catch (...) {
        // Consume the malformed DMA range and discard a partial parser command
        // so the same poisoned bytes cannot wedge subsequent RDP lists.
        g_parser->discard_pending();
        current = end_address & ((status & kDpcStatusXbusDmemDma) ? 0x0ff8u : 0x00fffff8u);
    }

    if (g_info.DPC_CURRENT_REG)
        *g_info.DPC_CURRENT_REG = current;
    if (g_info.DPC_CLOCK_REG)
        ++*g_info.DPC_CLOCK_REG;

    if (g_info.DPC_STATUS_REG) {
        std::uint32_t final_status = *g_info.DPC_STATUS_REG;
        final_status &=
            ~(kDpcStatusCommandBusy | kDpcStatusPipeBusy | kDpcStatusTmemBusy | kDpcStatusFlush);
        final_status |= kDpcStatusCommandBufferReady;
        *g_info.DPC_STATUS_REG = final_status;
    }
    if (g_info.DPC_PIPEBUSY_REG)
        *g_info.DPC_PIPEBUSY_REG = 0;
    if (g_info.DPC_BUFBUSY_REG)
        *g_info.DPC_BUFBUSY_REG = 0;
}
PJ64_EXPORT void PJ64_CALL ProcessDList(void) {
    ProcessRDPList();
}
PJ64_EXPORT void PJ64_CALL UpdateScreen(void) {
    if (!g_initiated || !g_renderer || !g_parser || !g_gl.make_current())
        return;
    try {
        g_gl.resize();
        g_renderer->set_output_size(g_gl.width(), g_gl.height());
        g_renderer->present(g_parser->state(), reg(g_info.VI_STATUS_REG),
                            reg(g_info.VI_ORIGIN_REG) & 0x00ffffffu, reg(g_info.VI_WIDTH_REG),
                            reg(g_info.VI_H_START_REG), reg(g_info.VI_V_START_REG),
                            reg(g_info.VI_X_SCALE_REG), reg(g_info.VI_Y_SCALE_REG),
                            reg(g_info.VI_V_SYNC_REG), reg(g_info.VI_V_CURRENT_LINE_REG));
        if (g_render_callback)
            g_render_callback(1);
        g_gl.swap();
    } catch (...) {
    }
}
PJ64_EXPORT void PJ64_CALL DrawScreen(void) {
    UpdateScreen();
}
PJ64_EXPORT void PJ64_CALL ShowCFB(void) {
    UpdateScreen();
}
PJ64_EXPORT void PJ64_CALL ChangeWindow(void) {
    g_gl.toggle_fullscreen();
}
PJ64_EXPORT void PJ64_CALL MoveScreen(int, int) {
    g_gl.resize();
}
PJ64_EXPORT void PJ64_CALL ViStatusChanged(void) {
}
PJ64_EXPORT void PJ64_CALL ViWidthChanged(void) {
}
PJ64_EXPORT void PJ64_CALL FBRead(std::uint32_t addr) {
    safe_gl_call([&] { g_renderer->fb_read(addr); });
}
PJ64_EXPORT void PJ64_CALL FBWrite(std::uint32_t addr, std::uint32_t size) {
    if (g_renderer)
        g_renderer->fb_write(addr, size);
}
PJ64_EXPORT void PJ64_CALL FBGetFrameBufferInfo(void *data) {
    if (!data)
        return;
    auto *out = static_cast<FRAME_BUFFER_INFO *>(data);
    for (int i = 0; i < 6; ++i)
        out[i] = {};
    if (g_parser) {
        const auto &state = g_parser->state();
        const auto &image = state.color_image;
        out[0].addr = image.address;
        out[0].size = image.size == RDPImageSize::Bits32
                          ? 4u
                          : (image.size == RDPImageSize::Bits16 ? 2u : 1u);
        out[0].width = image.width;

        std::uint32_t height = 0;
        if (g_renderer) {
            if (auto *framebuffer = g_renderer->framebuffers().find(image.address))
                height = std::uint32_t(std::max(framebuffer->native_height, 0));
        }
        if (height == 0 && state.scissor_yl > state.scissor_yh)
            height = std::uint32_t((state.scissor_yl + 3u) >> 2);
        out[0].height = height != 0 ? height : 240u;
    }
}
PJ64_EXPORT void PJ64_CALL ReadScreen2(void *dest, int *width, int *height, int) {
    safe_gl_call([&] { g_renderer->read_screen(dest, width, height); });
}
PJ64_EXPORT void PJ64_CALL CaptureScreen(char *directory) {
    if (!g_renderer || !directory)
        return;
    safe_gl_call([&] {
        std::string path = directory;
        if (!path.empty() && path.back() != '\\' && path.back() != '/')
            path += '\\';
        path += "PJ64RDPGL33.bmp";
        g_renderer->capture_bmp(path.c_str());
    });
}

// Bit mask: restore=1, divot=2, resample=4, gamma=8, gamma dither=16.
// Coverage: 0=off, 1=four samples, 2=eight samples, 3=two samples.
// AA uses 0=off, 1=reduced, 2=full independently.
PJ64_EXPORT void PJ64_CALL SetRenderingFilters(int coverage, int aa, unsigned filters) {
    if (g_renderer) {
        GLRDPConfig config{static_cast<GLRDPCoverage>(coverage),
                           static_cast<GLRDPVIAA>(aa),
                           (filters & 1) != 0,
                           (filters & 2) != 0,
                           (filters & 8) != 0,
                           (filters & 16) != 0,
                           (filters & 4) != 0};
        config.accurate_shading = true;
        safe_gl_call([&] { g_renderer->set_rendering_config(config); });
    }
}
PJ64_EXPORT void PJ64_CALL SetInternalResolutionScale(int scale) {
    if (!g_renderer)
        return;
    safe_gl_call([&] { g_renderer->set_internal_resolution_scale(scale); });
}
PJ64_EXPORT void PJ64_CALL SetDisplayResolution(int width, int height) {
    if (!g_renderer)
        return;
    g_gl.make_current();
    g_renderer->set_display_resolution(width, height);
    if (width > 0 && height > 0)
        g_gl.set_client_size(width, height);
}

PJ64_EXPORT void PJ64_CALL SetDisplayAspectMode(int mode) {
    if (g_renderer)
        g_renderer->set_display_aspect_mode(mode);
}

PJ64_EXPORT void PJ64_CALL SetDisplayResolutionPreset(int preset_id) {
    if (!g_renderer)
        return;
    if (preset_id == 0) {
        g_renderer->set_display_resolution(0, 0);
        g_renderer->set_display_aspect_mode(0);
        return;
    }
    const DisplayPreset *preset = find_display_preset(preset_id);
    if (!preset)
        return;
    g_renderer->set_display_resolution(preset->width, preset->height);
    g_renderer->set_display_aspect_mode(preset->aspect_mode);
    g_gl.set_client_size(preset->width, preset->height);
}

PJ64_EXPORT void PJ64_CALL SetRenderingCallback(void(PJ64_CALL *callback)(int)) {
    g_render_callback = callback;
}
PJ64_EXPORT void PJ64_CALL DllConfig(HWND) {
    MessageBoxA(g_info.hWnd,
                "RDP OpenGL 3.3 LLE plugin\n\n"
                "Internal scale: PJ64_RDP_INTERNAL_SCALE=1..8\n"
                "Coverage: PJ64_RDP_COVERAGE=off, reduced or full\n"
                "VI AA: PJ64_VI_AA=off, reduced or full\n"
                "VI toggles: PJ64_VI_RESTORE, PJ64_VI_DIVOT, PJ64_VI_RESAMPLE,\n"
                "PJ64_VI_GAMMA, PJ64_VI_GAMMA_DITHER=enabled or disabled\n"
                "Display resolution: PJ64_RDP_DISPLAY_RESOLUTION=1920x1080\n"
                "Canvas aspect: PJ64_RDP_DISPLAY_ASPECT=4:3 or 16:9\n\n"
                "The RDP/VI image always preserves its own aspect ratio and is centered with "
                "letterbox or pillarbox bars. Disable Graphics HLE in Project64.",
                "PJ64RDPGL33", MB_OK | MB_ICONINFORMATION);
}
PJ64_EXPORT void PJ64_CALL DllAbout(HWND) {
    MessageBoxA(g_info.hWnd, "Project64 RDP hardware plugin\nOpenGL 3.3 renderer active.",
                "About PJ64RDPGL33", MB_OK | MB_ICONINFORMATION);
}
PJ64_EXPORT void PJ64_CALL DllTest(HWND) {
    MessageBoxA(g_info.hWnd,
                g_initiated ? "OpenGL context initialized."
                            : "Plugin DLL loaded; graphics context is not initialized.",
                "PJ64RDPGL33 Test", MB_OK);
}
