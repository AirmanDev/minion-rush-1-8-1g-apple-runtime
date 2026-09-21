#ifndef MR_OFFLINE_MODE_H
#define MR_OFFLINE_MODE_H

#include "cpu.h"

int mr_offline_init(mr_cpu *cpu);
void mr_offline_shutdown(void);

int mr_offline_before_block(mr_cpu *cpu);

int mr_offline_main_menu_ready(void);
void mr_offline_report(void);

#endif
