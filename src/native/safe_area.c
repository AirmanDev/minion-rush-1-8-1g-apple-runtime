#include "safe_area.h"

#include "game_bindings.h"
#include "platform_window.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    INTERFACE_PARENT_OFFSET = 0x08,
    INTERFACE_CHILD_BEGIN_OFFSET = 0x0c,
    INTERFACE_CHILD_END_OFFSET = 0x10,
    INTERFACE_POSITION_Y_OFFSET = 0x178,
    INTERFACE_OFFSET_Y_OFFSET = 0x180,
    INTERFACE_WIDTH_OFFSET = 0x184,
    INTERFACE_HEIGHT_OFFSET = 0x188,
    INTERFACE_READ_SIZE = 0x18c,
    MAX_CHILDREN = 512,
    MAX_TREE_DEPTH = 16,
    MAX_TREE_OBJECTS = 512,
    OBJECT_STATE_CAPACITY = 4096,
    REGISTERED_OBJECT_CAPACITY = 128,
    OVERFLOW_GROUP_CAPACITY = 8,
    LOGGED_PAGE_CAPACITY = 256,
};

static const float REFERENCE_UI_WIDTH = 1024.0f;
static const float BOTTOM_CONTROL_CLEARANCE_WIDTH_RATIO = 0.025f;
static int SAFE_AREA_ADAPTATION_ACTIVE;

typedef struct {
    float min_y;
    float max_y;
    unsigned remaining;
    int valid;
} vertical_bounds;

typedef struct {
    uint32_t page;
    uint32_t object;
    float base_offset_y;
    float applied_offset_y;
    float base_height;
    float applied_height;
    int initialized;
    int offset_initialized;
    int height_initialized;
    int moves_with_safe_area;
    int resizes_with_safe_area;
} object_state;

typedef struct {
    uint32_t object;
    uint32_t owner;
    uint32_t parent;
    uint32_t vtable;
    mr_safe_area_role role;
    int applied;
} registered_object;

typedef struct {
    uint32_t owner;
    uint32_t parent;
    float first_y;
    unsigned anchor_count;
} overflow_group;

static object_state OBJECT_STATES[OBJECT_STATE_CAPACITY];
static registered_object REGISTERED_OBJECTS[REGISTERED_OBJECT_CAPACITY];

static int apply_offset(mr_cpu *cpu, object_state *state, uint32_t object, float adjustment);

static int name_in_list(const char *name, const char *const *list, size_t count) {
    if (!name) return 0;
    for (size_t index = 0; index < count; index++)
        if (strcmp(name, list[index]) == 0) return 1;
    return 0;
}

mr_safe_area_role mr_safe_area_classify_ui_object(const char *name) {
    static const char *const BOTTOM_CONTROLS[] = {
        "EvilMinion_powerup_Left",
        "EvilMinion_recharge_Button_Left",
        "EvilMinion_Active_Button_Left",
        "EvilMinion_powerup_pbar_Left",
        "EvilMinion_powerup_icons_parent_Left",
        "powerup_EvilMinion_Left",
        "EvilMinion_powerup_CountDown_Left",
        "EvilMinion_powerup_bonus_Left",
        "EvilMinion_powerup_recharge_Left",
        "EvilMinion_powerup_Right",
        "EvilMinion_recharge_Button_Right",
        "EvilMinion_Active_Button_Right",
        "EvilMinion_powerup_pbar_Right",
        "EvilMinion_powerup_icons_parent_Right",
        "powerup_EvilMinion_Right",
        "EvilMinion_powerup_CountDown_Right",
        "EvilMinion_powerup_bonus_Right",
        "EvilMinion_powerup_recharge_Right",
    };
    static const char *const BOTTOM_OVERFLOW[] = {
        "Revive_UseButton",  "ReviveButton",     "ReviveTokensGraph",   "Revive_Tag_Red",
        "Revive_Number_Red", "Revive_Tag_Green", "Revive_Number_Green",
    };
    if (name_in_list(name, BOTTOM_CONTROLS, sizeof BOTTOM_CONTROLS / sizeof BOTTOM_CONTROLS[0]))
        return MR_SAFE_AREA_BOTTOM_CONTROL;
    if (name_in_list(name, BOTTOM_OVERFLOW, sizeof BOTTOM_OVERFLOW / sizeof BOTTOM_OVERFLOW[0]))
        return MR_SAFE_AREA_BOTTOM_OVERFLOW;
    if (name && strcmp(name, "Common_Score_Value") == 0) return MR_SAFE_AREA_LAYOUT_GROUP;
    return MR_SAFE_AREA_DEFAULT;
}

static const char *role_name(mr_safe_area_role role) {
    switch (role) {
    case MR_SAFE_AREA_BOTTOM_CONTROL:
        return "bottom control";
    case MR_SAFE_AREA_BOTTOM_OVERFLOW:
        return "bottom overflow";
    case MR_SAFE_AREA_LAYOUT_GROUP:
        return "layout group";
    default:
        return "default";
    }
}

static void extend_bounds(vertical_bounds *bounds, float first, float second) {
    if (!isfinite(first) || !isfinite(second)) return;
    float min_y = fminf(first, second);
    float max_y = fmaxf(first, second);
    if (!bounds->valid) {
        bounds->min_y = min_y;
        bounds->max_y = max_y;
        bounds->valid = 1;
        return;
    }
    bounds->min_y = fminf(bounds->min_y, min_y);
    bounds->max_y = fmaxf(bounds->max_y, max_y);
}

static void measure_subtree(mr_cpu *cpu, uint32_t object, float parent_offset_y, unsigned depth,
                            vertical_bounds *bounds) {
    if (!object || !bounds->remaining || depth > MAX_TREE_DEPTH ||
        !mr_mem_ok(cpu, object, INTERFACE_READ_SIZE))
        return;
    bounds->remaining--;

    float offset_y = parent_offset_y + mr_ldf32(cpu, object + INTERFACE_OFFSET_Y_OFFSET);
    float y = offset_y + mr_ldf32(cpu, object + INTERFACE_POSITION_Y_OFFSET);
    float height = mr_ldf32(cpu, object + INTERFACE_HEIGHT_OFFSET);
    extend_bounds(bounds, y, y + height);

    uint32_t first = mr_ld32(cpu, object + INTERFACE_CHILD_BEGIN_OFFSET);
    uint32_t last = mr_ld32(cpu, object + INTERFACE_CHILD_END_OFFSET);
    if (!first || last < first || (last - first) % sizeof(uint32_t)) return;
    unsigned child_count = (last - first) / sizeof(uint32_t);
    if (child_count > MAX_CHILDREN || !mr_mem_ok(cpu, first, child_count * sizeof(uint32_t)))
        return;
    for (unsigned index = 0; index < child_count && bounds->remaining; index++) {
        uint32_t child = mr_ld32(cpu, first + index * sizeof(uint32_t));
        measure_subtree(cpu, child, offset_y, depth + 1u, bounds);
    }
}

static object_state *find_object_state(uint32_t page, uint32_t object) {
    unsigned index = ((page >> 4u) ^ (object >> 4u)) & (OBJECT_STATE_CAPACITY - 1u);
    for (unsigned probe = 0; probe < OBJECT_STATE_CAPACITY; probe++) {
        object_state *state = &OBJECT_STATES[index];
        if (!state->object) {
            state->page = page;
            state->object = object;
            return state;
        }
        if (state->page == page && state->object == object) return state;
        index = (index + 1u) & (OBJECT_STATE_CAPACITY - 1u);
    }
    return NULL;
}

void mr_safe_area_register_ui_object(mr_cpu *cpu, uint32_t page, uint32_t object,
                                     mr_safe_area_role role) {
    if (!cpu || !page || !object || role == MR_SAFE_AREA_DEFAULT ||
        !mr_mem_ok(cpu, object, INTERFACE_READ_SIZE))
        return;
    if (role == MR_SAFE_AREA_LAYOUT_GROUP) {
        object = mr_ld32(cpu, object + INTERFACE_PARENT_OFFSET);
        if (!object || object == page || !mr_mem_ok(cpu, object, INTERFACE_READ_SIZE)) return;
    }
    registered_object *available = NULL;
    for (unsigned index = 0; index < REGISTERED_OBJECT_CAPACITY; index++) {
        registered_object *entry = &REGISTERED_OBJECTS[index];
        if (entry->object == object) {
            available = entry;
            break;
        }
        if (!entry->object && !available) available = entry;
    }
    if (!available) return;

    uint32_t parent = mr_ld32(cpu, object + INTERFACE_PARENT_OFFSET);
    uint32_t vtable = mr_ld32(cpu, object);
    int changed = available->object != object || available->owner != page ||
                  available->parent != parent || available->vtable != vtable ||
                  available->role != role;
    int applied = changed ? 0 : available->applied;
    *available = (registered_object){
        .object = object,
        .owner = page,
        .parent = parent,
        .vtable = vtable,
        .role = role,
        .applied = applied,
    };
    if (changed && getenv("MR_DIAGNOSTICS"))
        printf("[Safe area] registered %s object %08x on owner %08x\n", role_name(role), object,
               page);
}

static int registered_object_is_valid(mr_cpu *cpu, registered_object *entry) {
    if (!mr_mem_ok(cpu, entry->object, INTERFACE_READ_SIZE) ||
        entry->parent != mr_ld32(cpu, entry->object + INTERFACE_PARENT_OFFSET) ||
        entry->vtable != mr_ld32(cpu, entry->object)) {
        *entry = (registered_object){0};
        return 0;
    }
    return 1;
}

static registered_object *registered_ui_object(mr_cpu *cpu, uint32_t object) {
    for (unsigned index = 0; index < REGISTERED_OBJECT_CAPACITY; index++) {
        registered_object *entry = &REGISTERED_OBJECTS[index];
        if (entry->object != object) continue;
        return registered_object_is_valid(cpu, entry) ? entry : NULL;
    }
    return NULL;
}

static int has_registered_ancestor(mr_cpu *cpu, const registered_object *entry) {
    uint32_t ancestor = entry->parent;
    for (unsigned depth = 0; ancestor && depth <= MAX_TREE_DEPTH; depth++) {
        registered_object *registered = registered_ui_object(cpu, ancestor);
        if (registered && registered->role == entry->role) return 1;
        if (!mr_mem_ok(cpu, ancestor, INTERFACE_PARENT_OFFSET + sizeof(uint32_t))) break;
        ancestor = mr_ld32(cpu, ancestor + INTERFACE_PARENT_OFFSET);
    }
    return 0;
}

static unsigned find_overflow_groups(mr_cpu *cpu, overflow_group *groups) {
    unsigned group_count = 0;
    for (unsigned index = 0; index < REGISTERED_OBJECT_CAPACITY; index++) {
        registered_object *entry = &REGISTERED_OBJECTS[index];
        if (!entry->object || entry->role != MR_SAFE_AREA_BOTTOM_OVERFLOW ||
            !registered_object_is_valid(cpu, entry) || has_registered_ancestor(cpu, entry))
            continue;
        float y = mr_ldf32(cpu, entry->object + INTERFACE_POSITION_Y_OFFSET);
        if (!isfinite(y)) continue;

        unsigned group_index = 0;
        while (group_index < group_count && groups[group_index].parent != entry->parent)
            group_index++;
        if (group_index == group_count) {
            if (group_count >= OVERFLOW_GROUP_CAPACITY) continue;
            groups[group_count++] = (overflow_group){
                .owner = entry->owner,
                .parent = entry->parent,
                .first_y = y,
            };
        }
        overflow_group *group = &groups[group_index];
        group->first_y = fminf(group->first_y, y);
        group->anchor_count++;
    }
    return group_count;
}

static int overflow_group_contains(const overflow_group *groups, unsigned group_count,
                                   const registered_object *entry) {
    for (unsigned index = 0; index < group_count; index++)
        if (groups[index].anchor_count > 1 && groups[index].parent == entry->parent) return 1;
    return 0;
}

static unsigned update_overflow_groups(mr_cpu *cpu, overflow_group *groups, unsigned group_count,
                                       float adjustment) {
    unsigned moved = 0;
    for (unsigned group_index = 0; group_index < group_count; group_index++) {
        overflow_group *group = &groups[group_index];
        if (group->anchor_count < 2 ||
            !mr_mem_ok(cpu, group->parent, INTERFACE_CHILD_END_OFFSET + sizeof(uint32_t)))
            continue;
        uint32_t first = mr_ld32(cpu, group->parent + INTERFACE_CHILD_BEGIN_OFFSET);
        uint32_t last = mr_ld32(cpu, group->parent + INTERFACE_CHILD_END_OFFSET);
        if (!first || last < first || (last - first) % sizeof(uint32_t)) continue;
        unsigned child_count = (last - first) / sizeof(uint32_t);
        if (child_count > MAX_CHILDREN || !mr_mem_ok(cpu, first, child_count * sizeof(uint32_t)))
            continue;

        for (unsigned index = 0; index < child_count; index++) {
            uint32_t child = mr_ld32(cpu, first + index * sizeof(uint32_t));
            if (!child || !mr_mem_ok(cpu, child, INTERFACE_READ_SIZE)) continue;
            float y = mr_ldf32(cpu, child + INTERFACE_POSITION_Y_OFFSET);
            if (!isfinite(y) || y + 0.05f < group->first_y) continue;
            object_state *state = find_object_state(group->owner, child);
            if (state) moved += (unsigned)apply_offset(cpu, state, child, adjustment);
        }
    }
    return moved;
}

static unsigned update_registered_objects(mr_cpu *cpu, float bottom_inset) {
    overflow_group groups[OVERFLOW_GROUP_CAPACITY] = {0};
    unsigned group_count = find_overflow_groups(cpu, groups);
    unsigned moved = update_overflow_groups(cpu, groups, group_count, bottom_inset);
    float control_clearance = REFERENCE_UI_WIDTH * BOTTOM_CONTROL_CLEARANCE_WIDTH_RATIO;
    float control_adjustment = bottom_inset > 0.05f ? -(bottom_inset + control_clearance) : 0.0f;
    for (unsigned index = 0; index < REGISTERED_OBJECT_CAPACITY; index++) {
        registered_object *entry = &REGISTERED_OBJECTS[index];
        if (!entry->object || !registered_object_is_valid(cpu, entry) ||
            entry->role == MR_SAFE_AREA_LAYOUT_GROUP)
            continue;
        if (entry->role == MR_SAFE_AREA_BOTTOM_OVERFLOW &&
            overflow_group_contains(groups, group_count, entry)) {
            entry->applied = 1;
            continue;
        }
        object_state *state = find_object_state(entry->owner, entry->object);
        if (!state) continue;
        float adjustment = has_registered_ancestor(cpu, entry)          ? 0.0f
                           : entry->role == MR_SAFE_AREA_BOTTOM_CONTROL ? control_adjustment
                                                                        : bottom_inset;
        moved += (unsigned)apply_offset(cpu, state, entry->object, adjustment);
        if (!entry->applied && getenv("MR_DIAGNOSTICS")) {
            uint32_t parent_first = 0;
            uint32_t parent_last = 0;
            if (mr_mem_ok(cpu, entry->parent, INTERFACE_CHILD_END_OFFSET + sizeof(uint32_t))) {
                parent_first = mr_ld32(cpu, entry->parent + INTERFACE_CHILD_BEGIN_OFFSET);
                parent_last = mr_ld32(cpu, entry->parent + INTERFACE_CHILD_END_OFFSET);
            }
            unsigned sibling_count = parent_first && parent_last >= parent_first
                                         ? (parent_last - parent_first) / sizeof(uint32_t)
                                         : 0;
            printf("[Safe area] applied %s object %08x by %.1f UI units (parent %08x, %u "
                   "children)\n",
                   role_name(entry->role), entry->object, adjustment, entry->parent, sibling_count);
            entry->applied = 1;
        }
    }
    return moved;
}

static int is_2d_visual(mr_cpu *cpu, uint32_t object) {
    static const uint32_t VTABLES[] = {
        MR_GAME_INTERFACE_DEVICE_ANIM_VTABLE + 8u,
        MR_GAME_INTERFACE_SCROLL_BAR_VTABLE + 8u,
        MR_GAME_INTERFACE_GRID_VTABLE + 8u,
        MR_GAME_INTERFACE_SLIDE_VTABLE + 8u,
        MR_GAME_INTERFACE_LIST_BUTTON_SCROLL_VTABLE + 8u,
        MR_GAME_INTERFACE_LIST_VTABLE + 8u,
        MR_GAME_INTERFACE_FILL_RECT_VTABLE + 8u,
        MR_GAME_INTERFACE_BUTTON_VTABLE + 8u,
        MR_GAME_INTERFACE_GRAPH_VTABLE + 8u,
        MR_GAME_INTERFACE_PROGRESS_BAR_VTABLE + 8u,
        MR_GAME_INTERFACE_TEXT_VTABLE + 8u,
    };
    uint32_t vtable = mr_ld32(cpu, object);
    for (unsigned index = 0; index < sizeof(VTABLES) / sizeof(VTABLES[0]); index++)
        if (vtable == VTABLES[index]) return 1;
    return 0;
}

static int is_vertical_container(mr_cpu *cpu, uint32_t object) {
    uint32_t vtable = mr_ld32(cpu, object);
    return vtable == MR_GAME_INTERFACE_AREA_VTABLE + 8u ||
           vtable == MR_GAME_INTERFACE_SCROLL_BAR_VTABLE + 8u ||
           vtable == MR_GAME_INTERFACE_GRID_VTABLE + 8u ||
           vtable == MR_GAME_INTERFACE_SLIDE_VTABLE + 8u ||
           vtable == MR_GAME_INTERFACE_LIST_BUTTON_SCROLL_VTABLE + 8u ||
           vtable == MR_GAME_INTERFACE_LIST_VTABLE + 8u;
}

static int is_projected_3d_container(mr_cpu *cpu, uint32_t object) {
    uint32_t vtable = mr_ld32(cpu, object);
    return vtable == MR_GAME_INTERFACE_3D_TOUCH_VTABLE + 8u ||
           vtable == MR_GAME_INTERFACE_3D_BODY_PART_VTABLE + 8u;
}

static int page_was_logged(uint32_t page) {
    static uint32_t pages[LOGGED_PAGE_CAPACITY];
    static unsigned page_count;
    for (unsigned index = 0; index < page_count; index++)
        if (pages[index] == page) return 1;
    if (page_count >= LOGGED_PAGE_CAPACITY) return 1;
    pages[page_count++] = page;
    return 0;
}

static int apply_offset(mr_cpu *cpu, object_state *state, uint32_t object, float adjustment) {
    float current_offset = mr_ldf32(cpu, object + INTERFACE_OFFSET_Y_OFFSET);
    if (!isfinite(current_offset)) return 0;
    if (!state->offset_initialized) {
        state->base_offset_y = current_offset;
        state->applied_offset_y = current_offset;
        state->offset_initialized = 1;
    }
    if (fabsf(current_offset - state->applied_offset_y) > 0.05f) {
        float previous_adjustment = state->applied_offset_y - state->base_offset_y;
        float distance_from_base = fabsf(current_offset - state->base_offset_y);
        float distance_from_applied = fabsf(current_offset - state->applied_offset_y);
        state->base_offset_y = distance_from_base < distance_from_applied
                                   ? current_offset
                                   : current_offset - previous_adjustment;
    }
    float wanted_offset = state->base_offset_y + adjustment;
    if (fabsf(current_offset - wanted_offset) > 0.05f)
        mr_stf32(cpu, object + INTERFACE_OFFSET_Y_OFFSET, wanted_offset);
    state->applied_offset_y = wanted_offset;
    return 1;
}

static int apply_height(mr_cpu *cpu, object_state *state, uint32_t object, float inset) {
    float current_height = mr_ldf32(cpu, object + INTERFACE_HEIGHT_OFFSET);
    if (!isfinite(current_height)) return 0;
    if (!state->height_initialized) {
        state->base_height = current_height;
        state->applied_height = current_height;
        state->height_initialized = 1;
    }
    if (fabsf(current_height - state->applied_height) > 0.05f) {
        float previous_adjustment = state->applied_height - state->base_height;
        float distance_from_base = fabsf(current_height - state->base_height);
        float distance_from_applied = fabsf(current_height - state->applied_height);
        state->base_height = distance_from_base < distance_from_applied
                                 ? current_height
                                 : current_height - previous_adjustment;
    }
    float adjustment =
        copysignf(fminf(inset, fmaxf(0.0f, fabsf(state->base_height) - 1.0f)), -state->base_height);
    float wanted_height = state->base_height + adjustment;
    if (fabsf(current_height - wanted_height) > 0.05f)
        mr_stf32(cpu, object + INTERFACE_HEIGHT_OFFSET, wanted_height);
    state->applied_height = wanted_height;
    return 1;
}

static int compensate_projected_2d(mr_cpu *cpu, uint32_t page, uint32_t object,
                                   int inside_body_part, float inset, unsigned depth,
                                   unsigned *remaining) {
    if (!object || !*remaining || depth > MAX_TREE_DEPTH ||
        !mr_mem_ok(cpu, object, INTERFACE_READ_SIZE))
        return 0;
    (*remaining)--;

    uint32_t vtable = mr_ld32(cpu, object);
    inside_body_part |= vtable == MR_GAME_INTERFACE_3D_BODY_PART_VTABLE + 8u;
    if (inside_body_part && is_2d_visual(cpu, object)) {
        object_state *state = find_object_state(page, object);
        return state ? apply_offset(cpu, state, object, -inset) : 0;
    }

    uint32_t first = mr_ld32(cpu, object + INTERFACE_CHILD_BEGIN_OFFSET);
    uint32_t last = mr_ld32(cpu, object + INTERFACE_CHILD_END_OFFSET);
    if (!first || last < first || (last - first) % sizeof(uint32_t)) return 0;
    unsigned child_count = (last - first) / sizeof(uint32_t);
    if (child_count > MAX_CHILDREN || !mr_mem_ok(cpu, first, child_count * sizeof(uint32_t)))
        return 0;

    int moved = 0;
    for (unsigned index = 0; index < child_count && *remaining; index++) {
        uint32_t child = mr_ld32(cpu, first + index * sizeof(uint32_t));
        moved += compensate_projected_2d(cpu, page, child, inside_body_part, inset, depth + 1u,
                                         remaining);
    }
    return moved;
}

static unsigned update_ui_subtree(mr_cpu *cpu, uint32_t page, uint32_t object,
                                  float parent_offset_y, float top_inset, float top_band,
                                  unsigned depth, unsigned *remaining) {
    if (!object || !*remaining || depth > MAX_TREE_DEPTH ||
        !mr_mem_ok(cpu, object, INTERFACE_READ_SIZE))
        return 0;
    (*remaining)--;

    object_state *state = find_object_state(page, object);
    if (!state) return 0;
    float current_offset = mr_ldf32(cpu, object + INTERFACE_OFFSET_Y_OFFSET);
    if (!isfinite(current_offset)) return 0;

    if (!state->initialized) {
        vertical_bounds bounds = {.remaining = MAX_TREE_OBJECTS};
        measure_subtree(cpu, object, parent_offset_y, 0u, &bounds);
        uint32_t object_first = mr_ld32(cpu, object + INTERFACE_CHILD_BEGIN_OFFSET);
        uint32_t object_last = mr_ld32(cpu, object + INTERFACE_CHILD_END_OFFSET);
        int has_children = object_first && object_last > object_first;
        float width = mr_ldf32(cpu, object + INTERFACE_WIDTH_OFFSET);
        float height = mr_ldf32(cpu, object + INTERFACE_HEIGHT_OFFSET);
        float y =
            parent_offset_y + current_offset + mr_ldf32(cpu, object + INTERFACE_POSITION_Y_OFFSET);
        float own_min_y = fminf(y, y + height);
        int has_extent =
            isfinite(width) && isfinite(height) && (fabsf(width) > 1.0f || fabsf(height) > 1.0f);
        int has_content = has_children || has_extent || is_2d_visual(cpu, object);
        int compact_upper_group =
            bounds.valid && bounds.min_y >= -top_band && bounds.max_y <= top_band;
        int extended_upper_container = is_vertical_container(cpu, object) && isfinite(own_min_y) &&
                                       own_min_y >= -top_band && own_min_y <= top_band;
        state->moves_with_safe_area =
            has_content && (compact_upper_group || extended_upper_container);
        state->resizes_with_safe_area =
            extended_upper_container && isfinite(height) && fabsf(height) > top_band;
        state->initialized = 1;
    }

    registered_object *registered = registered_ui_object(cpu, object);
    if (registered) {
        if (registered->role != MR_SAFE_AREA_LAYOUT_GROUP) return 0;
        float adjustment = state->moves_with_safe_area ? top_inset : 0.0f;
        if (!registered->applied && getenv("MR_DIAGNOSTICS")) {
            printf("[Safe area] layout group %08x: y %.1f, height %.1f, adjustment %.1f\n", object,
                   mr_ldf32(cpu, object + INTERFACE_POSITION_Y_OFFSET),
                   mr_ldf32(cpu, object + INTERFACE_HEIGHT_OFFSET), adjustment);
            registered->applied = 1;
        }
        return (unsigned)apply_offset(cpu, state, object, adjustment);
    }

    if (state->moves_with_safe_area) {
        unsigned moved = (unsigned)apply_offset(cpu, state, object, top_inset);
        if (state->resizes_with_safe_area)
            moved += (unsigned)apply_height(cpu, state, object, top_inset);
        if (mr_ld32(cpu, object) == MR_GAME_INTERFACE_3D_TOUCH_VTABLE + 8u) {
            unsigned compensation_remaining = MAX_TREE_OBJECTS;
            moved += (unsigned)compensate_projected_2d(cpu, page, object, 0, top_inset, 0u,
                                                       &compensation_remaining);
        }
        return moved;
    }
    if (is_projected_3d_container(cpu, object)) return 0;

    uint32_t first = mr_ld32(cpu, object + INTERFACE_CHILD_BEGIN_OFFSET);
    uint32_t last = mr_ld32(cpu, object + INTERFACE_CHILD_END_OFFSET);
    if (!first || last < first || (last - first) % sizeof(uint32_t)) return 0;
    unsigned child_count = (last - first) / sizeof(uint32_t);
    if (child_count > MAX_CHILDREN || !mr_mem_ok(cpu, first, child_count * sizeof(uint32_t)))
        return 0;

    unsigned moved = 0;
    float child_parent_offset_y = parent_offset_y + current_offset;
    for (unsigned index = 0; index < child_count && *remaining; index++) {
        uint32_t child = mr_ld32(cpu, first + index * sizeof(uint32_t));
        moved += update_ui_subtree(cpu, page, child, child_parent_offset_y, top_inset, top_band,
                                   depth + 1u, remaining);
    }
    return moved;
}

static void update_top_level_ui(mr_cpu *cpu, uint32_t page) {
    if (!page || !mr_mem_ok(cpu, page, INTERFACE_READ_SIZE) ||
        mr_ld32(cpu, page + INTERFACE_PARENT_OFFSET))
        return;

    mr_win_insets normalized = mr_win_safe_area();
    normalized.top = fmax(0.0, fmin(0.25, normalized.top));
    normalized.bottom = fmax(0.0, fmin(0.25, normalized.bottom));
    if (normalized.top <= 0.0 && normalized.bottom <= 0.0 && !SAFE_AREA_ADAPTATION_ACTIVE) return;
    if (normalized.top > 0.0 || normalized.bottom > 0.0) SAFE_AREA_ADAPTATION_ACTIVE = 1;

    uint32_t surface_width = 0;
    uint32_t surface_height = 0;
    mr_win_surface_size(&surface_width, &surface_height);
    if (!surface_width || !surface_height) return;

    // Aurora layouts use a 1024-unit reference width before UI scaling.
    float ui_scale = REFERENCE_UI_WIDTH / (float)surface_width;
    float top_inset = (float)(normalized.top * surface_height) * ui_scale;
    float bottom_inset = (float)(normalized.bottom * surface_height) * ui_scale;
    float ui_height = surface_height * ui_scale;
    float top_band = fminf(ui_height * 0.25f, REFERENCE_UI_WIDTH * 0.55f);
    uint32_t first = mr_ld32(cpu, page + INTERFACE_CHILD_BEGIN_OFFSET);
    uint32_t last = mr_ld32(cpu, page + INTERFACE_CHILD_END_OFFSET);
    if (!first || last < first || (last - first) % sizeof(uint32_t)) return;
    unsigned child_count = (last - first) / sizeof(uint32_t);
    if (child_count > MAX_CHILDREN || !mr_mem_ok(cpu, first, child_count * sizeof(uint32_t)))
        return;

    unsigned moved = update_registered_objects(cpu, bottom_inset);
    unsigned remaining = MAX_TREE_OBJECTS;
    for (unsigned index = 0; index < child_count && remaining; index++) {
        uint32_t object = mr_ld32(cpu, first + index * sizeof(uint32_t));
        moved += update_ui_subtree(cpu, page, object, 0.0f, top_inset, top_band, 0u, &remaining);
    }
    if (moved && getenv("MR_DIAGNOSTICS") && !page_was_logged(page))
        printf("[Safe area] page %08x: %u UI transforms adjusted (top %.1f, bottom %.1f UI "
               "units)\n",
               page, moved, top_inset, bottom_inset);
}

int mr_safe_area_before_block(mr_cpu *cpu) {
    if (!cpu || cpu->r[MR_R_PC] != MR_GAME_BASIC_PAGE_RENDER) return 0;
    update_top_level_ui(cpu, cpu->r[0]);
    return 0;
}
