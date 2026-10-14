#pragma once
#include <cstdint>
#include <windows.h>

#define PLUGIN_TYPE_GFX 2
#define PLUGIN_VERSION 0x0103

#pragma pack(push, 4)
struct PLUGIN_INFO {
    std::uint16_t Version;
    std::uint16_t Type;
    char Name[100];
    BOOL NormalMemory;
    BOOL MemoryBswaped;
};

struct GFX_INFO {
    HWND hWnd;
    HWND hStatusBar;
    BOOL MemoryBswaped;
    std::uint8_t *HEADER;
    std::uint8_t *RDRAM;
    std::uint8_t *DMEM;
    std::uint8_t *IMEM;
    std::uint32_t *MI_INTR_REG;
    std::uint32_t *DPC_START_REG;
    std::uint32_t *DPC_END_REG;
    std::uint32_t *DPC_CURRENT_REG;
    std::uint32_t *DPC_STATUS_REG;
    std::uint32_t *DPC_CLOCK_REG;
    std::uint32_t *DPC_BUFBUSY_REG;
    std::uint32_t *DPC_PIPEBUSY_REG;
    std::uint32_t *DPC_TMEM_REG;
    std::uint32_t *VI_STATUS_REG;
    std::uint32_t *VI_ORIGIN_REG;
    std::uint32_t *VI_WIDTH_REG;
    std::uint32_t *VI_INTR_REG;
    std::uint32_t *VI_V_CURRENT_LINE_REG;
    std::uint32_t *VI_TIMING_REG;
    std::uint32_t *VI_V_SYNC_REG;
    std::uint32_t *VI_H_SYNC_REG;
    std::uint32_t *VI_LEAP_REG;
    std::uint32_t *VI_H_START_REG;
    std::uint32_t *VI_V_START_REG;
    std::uint32_t *VI_V_BURST_REG;
    std::uint32_t *VI_X_SCALE_REG;
    std::uint32_t *VI_Y_SCALE_REG;
    void(__cdecl *CheckInterrupts)(void);
};
#pragma pack(pop)

#if defined(_MSC_VER)
#define PJ64_EXPORT extern "C" __declspec(dllexport)
#define PJ64_CALL __cdecl
#else
#define PJ64_EXPORT extern "C" __attribute__((dllexport))
#define PJ64_CALL __cdecl
#endif
