#pragma once
#include "core/backend.h"
void optional_rsp_init(rsp_memory info);
unsigned int optional_rsp_execute(unsigned int cycles);
unsigned int optional_rsp_state_size(void);
void optional_rsp_save(void *buffer);
void optional_rsp_load(const void *buffer);
void optional_rsp_load_v11b(const void *buffer);
void optional_rsp_load_v116(const void *buffer);
void optional_rsp_load_legacy(const void *buffer);
void optional_rsp_load_legacy_stream(const void *buffer);
void optional_rsp_report(void);
void optional_rsp_shutdown(void);
void optional_rsp_timing_dma_write(uint32_t address, uint32_t bytes);
