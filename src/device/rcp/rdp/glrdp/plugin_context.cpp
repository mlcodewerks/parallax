#include "plugin_context.hpp"
#include "gl_loader.hpp"

#include <algorithm>

namespace {
constexpr int WGL_CONTEXT_MAJOR_VERSION_ARB = 0x2091;
constexpr int WGL_CONTEXT_MINOR_VERSION_ARB = 0x2092;
constexpr int WGL_CONTEXT_PROFILE_MASK_ARB = 0x9126;
constexpr int WGL_CONTEXT_CORE_PROFILE_BIT_ARB = 0x00000001;

bool pixel_format_usable(HDC dc, int format) {
    if (!dc || format <= 0)
        return false;

    PIXELFORMATDESCRIPTOR pfd{};
    if (!DescribePixelFormat(dc, format, sizeof(pfd), &pfd))
        return false;

    const DWORD required = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    return (pfd.dwFlags & required) == required && pfd.iPixelType == PFD_TYPE_RGBA;
}
} // namespace

bool PluginGLContext::create(HWND hwnd) {
    destroy();
    hwnd_ = hwnd;
    if (!hwnd_)
        return false;

    dc_ = GetDC(hwnd_);
    if (!dc_)
        return false;

    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    pfd.iLayerType = PFD_MAIN_PLANE;

    const int existing_format = GetPixelFormat(dc_);
    if (existing_format != 0) {
        if (!pixel_format_usable(dc_, existing_format)) {
            destroy();
            return false;
        }
    } else {
        const int format = ChoosePixelFormat(dc_, &pfd);
        if (!format || !SetPixelFormat(dc_, format, &pfd)) {
            destroy();
            return false;
        }
    }

    HGLRC bootstrap = wglCreateContext(dc_);
    if (!bootstrap || !wglMakeCurrent(dc_, bootstrap)) {
        if (bootstrap)
            wglDeleteContext(bootstrap);
        destroy();
        return false;
    }

    using PFNWGLCREATECONTEXTATTRIBSARBPROC = HGLRC(WINAPI *)(HDC, HGLRC, const int *);
    auto create_context_attribs = reinterpret_cast<PFNWGLCREATECONTEXTATTRIBSARBPROC>(
        wglGetProcAddress("wglCreateContextAttribsARB"));
    if (!create_context_attribs) {
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(bootstrap);
        destroy();
        return false;
    }

    const int attributes[] = {
        WGL_CONTEXT_MAJOR_VERSION_ARB,
        3,
        WGL_CONTEXT_MINOR_VERSION_ARB,
        3,
        WGL_CONTEXT_PROFILE_MASK_ARB,
        WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
        0,
    };

    HGLRC core = create_context_attribs(dc_, nullptr, attributes);
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(bootstrap);
    if (!core || !wglMakeCurrent(dc_, core)) {
        if (core)
            wglDeleteContext(core);
        destroy();
        return false;
    }
    rc_ = core;

    auto resolve = [](const char *name) -> retro_proc_address_t {
        PROC p = wglGetProcAddress(name);
        if (!p || p == (PROC)1 || p == (PROC)2 || p == (PROC)3 || p == (PROC)-1)
            p = GetProcAddress(GetModuleHandleA("opengl32.dll"), name);
        return reinterpret_cast<retro_proc_address_t>(p);
    };
    if (!load_gl33(resolve, false)) {
        destroy();
        return false;
    }

    GLint major = 0;
    GLint minor = 0;
    GLint profile = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    glGetIntegerv(GL_CONTEXT_PROFILE_MASK, &profile);
    if (major < 3 || (major == 3 && minor < 3) ||
        (profile & WGL_CONTEXT_CORE_PROFILE_BIT_ARB) == 0) {
        destroy();
        return false;
    }

    resize();
    return true;
}

void PluginGLContext::destroy() {
    if (rc_) {
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(rc_);
        rc_ = nullptr;
    }
    if (dc_ && hwnd_) {
        ReleaseDC(hwnd_, dc_);
        dc_ = nullptr;
    }
    hwnd_ = nullptr;
}

bool PluginGLContext::make_current() {
    return dc_ && rc_ && wglMakeCurrent(dc_, rc_) == TRUE;
}

void PluginGLContext::swap() {
    if (dc_)
        SwapBuffers(dc_);
}

void PluginGLContext::resize() {
    if (hwnd_) {
        RECT r{};
        if (GetClientRect(hwnd_, &r)) {
            width_ = int(std::max<LONG>(1, r.right - r.left));
            height_ = int(std::max<LONG>(1, r.bottom - r.top));
        }
    }
}

void PluginGLContext::set_client_size(int width, int height) {
    if (!hwnd_ || fullscreen_ || width <= 0 || height <= 0)
        return;

    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_STYLE));
    const DWORD ex_style = static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_EXSTYLE));
    RECT rect{0, 0, width, height};
    AdjustWindowRectEx(&rect, style, FALSE, ex_style);
    SetWindowPos(hwnd_, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    resize();
}

void PluginGLContext::toggle_fullscreen() {
    if (!hwnd_)
        return;

    if (!fullscreen_) {
        window_style_ = GetWindowLongW(hwnd_, GWL_STYLE);
        GetWindowRect(hwnd_, &window_rect_);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongW(hwnd_, GWL_STYLE, window_style_ & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(hwnd_, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        fullscreen_ = true;
    } else {
        SetWindowLongW(hwnd_, GWL_STYLE, window_style_);
        SetWindowPos(hwnd_, nullptr, window_rect_.left, window_rect_.top,
                     window_rect_.right - window_rect_.left, window_rect_.bottom - window_rect_.top,
                     SWP_FRAMECHANGED | SWP_NOZORDER | SWP_SHOWWINDOW);
        fullscreen_ = false;
    }
    resize();
}
