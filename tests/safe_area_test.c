#include "game_bindings.h"
#include "platform_window.h"
#include "safe_area.h"

#include <assert.h>
#include <math.h>
#include <string.h>

enum {
    PARENT = 0x08,
    CHILD_BEGIN = 0x0c,
    CHILD_END = 0x10,
    POSITION_Y = 0x178,
    OFFSET_Y = 0x180,
    WIDTH = 0x184,
    HEIGHT = 0x188,
};

static mr_win_insets INSETS;

uint32_t mr_load_word_slow(mr_cpu *cpu, uint32_t address) {
    assert(mr_mem_ok(cpu, address, sizeof(uint32_t)));
    uint32_t value;
    memcpy(&value, mr_mem(cpu, address), sizeof value);
    return value;
}

mr_win_insets mr_win_safe_area(void) {
    return INSETS;
}

void mr_win_surface_size(uint32_t *width, uint32_t *height) {
    *width = 1024;
    *height = 2200;
}

static void object(mr_cpu *cpu, uint32_t address, uint32_t parent, uint32_t vtable, float y,
                   float height) {
    mr_st32(cpu, address, vtable + 8);
    mr_st32(cpu, address + PARENT, parent);
    mr_stf32(cpu, address + POSITION_Y, y);
    mr_stf32(cpu, address + WIDTH, 120);
    mr_stf32(cpu, address + HEIGHT, height);
}

static void children(mr_cpu *cpu, uint32_t parent, uint32_t array, uint32_t first,
                     uint32_t second) {
    mr_st32(cpu, parent + CHILD_BEGIN, array);
    mr_st32(cpu, parent + CHILD_END, array + 8);
    mr_st32(cpu, array, first);
    mr_st32(cpu, array + 4, second);
}

static void check_offset(mr_cpu *cpu, uint32_t address, float expected) {
    assert(fabsf(mr_ldf32(cpu, address + OFFSET_Y) - expected) < 0.01f);
}

static void render(mr_cpu *cpu, uint32_t page) {
    cpu->r[0] = page;
    cpu->r[MR_R_PC] = MR_GAME_BASIC_PAGE_RENDER;
    assert(mr_safe_area_before_block(cpu) == 0);
}

static void check_group(mr_cpu *cpu, uint32_t page, float first_y, float second_y,
                        int top_aligned) {
    uint32_t counter = page + 0x200;
    uint32_t group = page + 0x400;
    uint32_t score = page + 0x600;
    uint32_t distance = page + 0x800;
    object(cpu, page, 0, 0, 0, 2200);
    object(cpu, counter, page, MR_GAME_INTERFACE_TEXT_VTABLE, 80, 40);
    object(cpu, group, page, 0, 0, 0);
    mr_stf32(cpu, group + WIDTH, 0);
    object(cpu, score, group, MR_GAME_INTERFACE_TEXT_VTABLE, first_y, 40);
    object(cpu, distance, group, MR_GAME_INTERFACE_TEXT_VTABLE, second_y, 40);
    children(cpu, page, page + 0xa00, counter, group);
    children(cpu, group, page + 0xa20, score, distance);
    mr_safe_area_role role = mr_safe_area_classify_ui_object("Common_Score_Value");
    assert(role == MR_SAFE_AREA_LAYOUT_GROUP);
    mr_safe_area_register_ui_object(cpu, page, score, role);

    const double top_insets[] = {0.03, 0.05, 0.0, 0.03};
    for (unsigned index = 0; index < sizeof top_insets / sizeof top_insets[0]; index++) {
        INSETS.top = top_insets[index];
        float adjustment = (float)(INSETS.top * 2200);
        for (unsigned frame = 0; frame < 100; frame++) {
            render(cpu, page);
            check_offset(cpu, counter, adjustment);
            check_offset(cpu, group, top_aligned ? adjustment : 0);
            check_offset(cpu, score, 0);
            check_offset(cpu, distance, 0);
        }
        mr_safe_area_register_ui_object(cpu, page, score, role);
    }
}

int main(void) {
    uint8_t memory[0x10000] = {0};
    mr_cpu cpu = {.mem_host = memory, .mem_guest_base = 0x1000, .mem_size = sizeof memory};
    check_group(&cpu, 0x2000, 500, 600, 0);
    check_group(&cpu, 0x4000, 150, 250, 1);
    assert(mr_safe_area_classify_ui_object(NULL) == MR_SAFE_AREA_DEFAULT);
    assert(mr_safe_area_classify_ui_object("Unknown") == MR_SAFE_AREA_DEFAULT);
    return 0;
}
