#ifndef MR_LANGUAGE_UI_H
#define MR_LANGUAGE_UI_H

#include "cpu.h"

#include <stdint.h>

typedef enum {
    MR_LANGUAGE_PAGE_OPTIONS,
    MR_LANGUAGE_PAGE_INITIAL,
    MR_LANGUAGE_PAGE_COUNT,
} mr_language_page;

void mr_language_ui_enter(mr_language_page kind, uint32_t page);
void mr_language_ui_leave(mr_language_page kind, uint32_t page, int destroyed);
int mr_language_ui_defer_refresh(mr_language_page kind, uint32_t page);
int mr_language_ui_selection(mr_language_page kind, uint32_t page, uint32_t button);

// Applies deferred hierarchy changes between engine frames.
int mr_language_ui_apply(mr_cpu *cpu, uint32_t stack_top);

#endif
