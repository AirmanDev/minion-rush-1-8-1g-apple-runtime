#ifndef MR_SAFE_AREA_H
#define MR_SAFE_AREA_H

#include "cpu.h"

typedef enum {
    MR_SAFE_AREA_DEFAULT,
    MR_SAFE_AREA_BOTTOM_CONTROL,
    MR_SAFE_AREA_BOTTOM_OVERFLOW,
} mr_safe_area_role;

mr_safe_area_role mr_safe_area_classify_ui_object(const char *name);
void mr_safe_area_register_ui_object(mr_cpu *cpu, uint32_t page, uint32_t object,
                                     mr_safe_area_role role);
int mr_safe_area_before_block(mr_cpu *cpu);

#endif
