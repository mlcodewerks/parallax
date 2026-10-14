#pragma once
#include "plugin_api.hpp"
#include <windows.h>

class PluginGLContext {
  public:
    bool create(HWND hwnd);
    void destroy();
    bool make_current();
    void swap();
    void resize();
    void toggle_fullscreen();
    void set_client_size(int width, int height);
    HWND hwnd() const {
        return hwnd_;
    }
    int width() const {
        return width_;
    }
    int height() const {
        return height_;
    }

  private:
    HWND hwnd_ = nullptr;
    HDC dc_ = nullptr;
    HGLRC rc_ = nullptr;
    RECT window_rect_{};
    DWORD window_style_ = 0;
    bool fullscreen_ = false;
    int width_ = 640, height_ = 480;
};
