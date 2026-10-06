#pragma once
#include "api/m64p_plugin.h"
void optional_rsp_init(RSP_INFO info);
unsigned int optional_rsp_execute(unsigned int cycles);
unsigned int optional_rsp_state_size(void);
void optional_rsp_save(void *buffer);
void optional_rsp_load(const void *buffer);
void optional_rsp_load_legacy(const void *buffer);
void optional_rsp_report(void);
