#ifndef MR_OFFLINE_EVENTS_H
#define MR_OFFLINE_EVENTS_H

#include "cpu.h"

#include <stdint.h>

int mr_offline_events_init(void);
void mr_offline_events_shutdown(void);

int mr_offline_events_before_block(mr_cpu *cpu, uint32_t pc, uint32_t *result);

int mr_offline_events_ready(void);
void mr_offline_events_report(void);

#endif
