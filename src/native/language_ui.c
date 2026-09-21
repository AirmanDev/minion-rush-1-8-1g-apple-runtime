#include "language_ui.h"

#include "game_bindings.h"
#include "guest_runtime.h"
#include "localization.h"
#include "localization_config.h"
#include "shim_libc.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INTERFACE_BUTTON_SIZE 592u
#define INTERFACE_FILL_RECT_SIZE 432u
#define INTERFACE_PARENT_OFFSET 0x08u
#define INTERFACE_POSITION_OFFSET 0x174u
#define INTERFACE_SIZE_OFFSET 0x184u
#define MAP_VALUE_OFFSET 0x0cu
#define OPTIONS_BUTTON_MAP_OFFSET 0x1c0u
#define INITIAL_BUTTON_MAP_OFFSET 0x124u
#define FLAG_BORDER_COLOR 0x396070u
#define FLAG_SELECTED_COLOR 0xff7100u
#define FLAG_INACTIVE_TINT 0xaaaaaau

typedef struct {
    const char *name;
    uint32_t localization;
} engine_language_button;

typedef struct {
    uint32_t object;
    float x;
    float y;
    float width;
    float height;
} button_geometry;

typedef enum {
    FLAG_RECTANGLE_BAND,
    FLAG_RECTANGLE_BORDER,
} flag_rectangle_kind;

typedef struct {
    uint32_t object;
    button_geometry bounds;
    uint32_t source_color;
    unsigned localization_index;
    flag_rectangle_kind kind;
} flag_rectangle;

typedef struct {
    uint32_t page;
    uint32_t buttons[MR_LOCALIZATION_CONFIG_COUNT ? MR_LOCALIZATION_CONFIG_COUNT : 1u];
    flag_rectangle rectangles[MR_LOCALIZATION_FLAG_RECTANGLE_CAPACITY
                                  ? MR_LOCALIZATION_FLAG_RECTANGLE_CAPACITY
                                  : 1u];
    unsigned button_count;
    unsigned rectangle_count;
    int style_dirty;
    int pending;
    int injected;
    int cleanup;
} language_page_state;

static language_page_state PAGES[MR_LANGUAGE_PAGE_COUNT];

static const engine_language_button ENGINE_BUTTONS[] = {
    {"en", MR_GAME_LOCALIZATION_EN},           {"fr", MR_GAME_LOCALIZATION_FR},
    {"de", MR_GAME_LOCALIZATION_DE},           {"it", MR_GAME_LOCALIZATION_IT},
    {"es", MR_GAME_LOCALIZATION_ES},           {"pt", MR_GAME_LOCALIZATION_PT},
    {"ar", MR_GAME_LOCALIZATION_AR},           {"ja", MR_GAME_LOCALIZATION_JA},
    {"ko", MR_GAME_LOCALIZATION_KO},           {"zh", MR_GAME_LOCALIZATION_ZH},
    {"ru", MR_GAME_LOCALIZATION_RU},           {"tr", MR_GAME_LOCALIZATION_TR},
    {"th", MR_GAME_LOCALIZATION_TH},           {"id", MR_GAME_LOCALIZATION_ID},
    {"zh-Hant", MR_GAME_LOCALIZATION_ZH_HANT},
};

static uint32_t button_map_offset(mr_language_page kind) {
    return kind == MR_LANGUAGE_PAGE_OPTIONS ? OPTIONS_BUTTON_MAP_OFFSET : INITIAL_BUTTON_MAP_OFFSET;
}

static uint32_t refresh_function(mr_language_page kind) {
    return kind == MR_LANGUAGE_PAGE_OPTIONS ? MR_GAME_OPTIONS_SETTINGS_REFRESH
                                            : MR_GAME_INITIAL_LANGUAGE_REFRESH;
}

static const char *page_name(mr_language_page kind) {
    return kind == MR_LANGUAGE_PAGE_OPTIONS ? "settings" : "initial-language";
}

static void reset_cpu(mr_cpu *cpu, uint32_t stack_top) {
    memset(cpu->r, 0, sizeof cpu->r);
    memset(&cpu->f, 0, sizeof cpu->f);
    cpu->halted = 0;
    cpu->fault = NULL;
    cpu->fault_addr = 0;
    cpu->a64_slow_pc = 0;
    cpu->a64_slow_addr = 0;
    cpu->thumb = 0;
    cpu->itstate = 0;
    cpu->r[MR_R_SP] = stack_top;
}

static int call_guest(mr_cpu *cpu, uint32_t stack_top, const char *label, uint32_t function,
                      int count, const uint32_t *arguments, uint32_t *result) {
    reset_cpu(cpu, stack_top);
    uint32_t value = mr_guest_call(cpu, function, count, arguments);
    if (cpu->fault) {
        fprintf(stderr, "ERROR: %s failed at guest address 0x%08x: %s\n", label, cpu->fault_addr,
                cpu->fault);
        return -1;
    }
    if (result) *result = value;
    return 0;
}

static int map_button(mr_cpu *cpu, uint32_t stack_top, uint32_t map, uint32_t localization,
                      uint32_t *node) {
    const uint32_t arguments[2] = {map, localization};
    return call_guest(cpu, stack_top, "language button map", MR_GAME_LANGUAGE_BUTTON_MAP_INDEX, 2,
                      arguments, node);
}

static int valid_interface(mr_cpu *cpu, uint32_t object) {
    return object && mr_mem_ok(cpu, object, INTERFACE_BUTTON_SIZE);
}

static int read_button_geometry(mr_cpu *cpu, uint32_t object, button_geometry *geometry) {
    if (!valid_interface(cpu, object) || !geometry) return 0;
    geometry->object = object;
    geometry->x = mr_ldf32(cpu, object + INTERFACE_POSITION_OFFSET);
    geometry->y = mr_ldf32(cpu, object + INTERFACE_POSITION_OFFSET + 4u);
    geometry->width = mr_ldf32(cpu, object + INTERFACE_SIZE_OFFSET);
    geometry->height = mr_ldf32(cpu, object + INTERFACE_SIZE_OFFSET + 4u);
    return isfinite(geometry->x) && isfinite(geometry->y) && geometry->width > 0.0f &&
           geometry->height > 0.0f;
}

static unsigned collect_buttons(mr_cpu *cpu, uint32_t stack_top, uint32_t map,
                                button_geometry *buttons, unsigned capacity) {
    unsigned count = 0;
    int diagnostics = getenv("MR_DIAGNOSTICS") != NULL;
    for (unsigned index = 0; index < sizeof ENGINE_BUTTONS / sizeof ENGINE_BUTTONS[0]; index++) {
        uint32_t node = 0;
        if (map_button(cpu, stack_top, map, ENGINE_BUTTONS[index].localization, &node) != 0)
            return 0;
        uint32_t object = node && mr_mem_ok(cpu, node + MAP_VALUE_OFFSET, 4)
                              ? mr_ld32(cpu, node + MAP_VALUE_OFFSET)
                              : 0;
        button_geometry geometry;
        if (!read_button_geometry(cpu, object, &geometry)) continue;
        if (diagnostics)
            printf("[Localization UI] %s: pos %.1f,%.1f size %.1fx%.1f\n",
                   ENGINE_BUTTONS[index].name, geometry.x, geometry.y, geometry.width,
                   geometry.height);
        if (count < capacity) buttons[count++] = geometry;
    }
    return count;
}

static int compare_float(const void *left, const void *right) {
    float a = *(const float *)left;
    float b = *(const float *)right;
    return a < b ? -1 : a > b;
}

static unsigned unique_axis(const button_geometry *buttons, unsigned count, int horizontal,
                            float *values, unsigned capacity) {
    unsigned used = 0;
    for (unsigned index = 0; index < count && used < capacity; index++) {
        float value = horizontal ? buttons[index].x : buttons[index].y;
        unsigned match = 0;
        for (; match < used; match++)
            if (fabsf(values[match] - value) < 1.0f) break;
        if (match == used) values[used++] = value;
    }
    qsort(values, used, sizeof values[0], compare_float);
    return used;
}

static unsigned recurring_x_axis(const button_geometry *buttons, unsigned count, float *values,
                                 unsigned capacity) {
    float candidates[sizeof ENGINE_BUTTONS / sizeof ENGINE_BUTTONS[0]];
    unsigned candidate_count =
        unique_axis(buttons, count, 1, candidates, sizeof candidates / sizeof candidates[0]);
    unsigned used = 0;
    for (unsigned candidate = 0; candidate < candidate_count && used < capacity; candidate++) {
        unsigned occurrences = 0;
        for (unsigned index = 0; index < count; index++)
            if (fabsf(buttons[index].x - candidates[candidate]) < 1.0f) occurrences++;
        if (occurrences > 1u) values[used++] = candidates[candidate];
    }
    if (used >= 2u) return used;
    unsigned fallback = candidate_count < capacity ? candidate_count : capacity;
    memcpy(values, candidates, fallback * sizeof values[0]);
    return fallback;
}

static int compare_geometry_x(const void *left, const void *right) {
    const button_geometry *a = left;
    const button_geometry *b = right;
    return a->x < b->x ? -1 : a->x > b->x;
}

static float centered_column(float center, float spacing, unsigned count, unsigned index) {
    return center - spacing * ((float)count - 1.0f) * 0.5f + spacing * (float)index;
}

static unsigned row_size(unsigned total, unsigned columns, unsigned row) {
    unsigned consumed = row * columns;
    unsigned remaining = total - consumed;
    return remaining < columns ? remaining : columns;
}

static int arrange_trailing_row(mr_cpu *cpu, button_geometry *buttons, unsigned count,
                                button_geometry *added, unsigned added_count) {
    if (!count || !added || !added_count) return 0;
    float xs[sizeof ENGINE_BUTTONS / sizeof ENGINE_BUTTONS[0]];
    float ys[sizeof ENGINE_BUTTONS / sizeof ENGINE_BUTTONS[0]];
    unsigned nx = recurring_x_axis(buttons, count, xs, sizeof xs / sizeof xs[0]);
    unsigned ny = unique_axis(buttons, count, 0, ys, sizeof ys / sizeof ys[0]);
    if (!nx || !ny) return -1;

    float last_y = ys[ny - 1u];
    button_geometry trailing[sizeof ENGINE_BUTTONS / sizeof ENGINE_BUTTONS[0]];
    unsigned trailing_count = 0;
    float visible_margin =
        nx > 1u ? (xs[nx - 1u] - xs[0]) / (float)(nx - 1u) * 0.5f : buttons[0].width * 0.5f;
    for (unsigned index = 0; index < count; index++) {
        if (fabsf(buttons[index].y - last_y) < 1.0f && buttons[index].x >= xs[0] - visible_margin &&
            buttons[index].x <= xs[nx - 1u] + visible_margin)
            trailing[trailing_count++] = buttons[index];
    }
    qsort(trailing, trailing_count, sizeof trailing[0], compare_geometry_x);

    float spacing = nx > 1u ? (xs[nx - 1u] - xs[0]) / (float)(nx - 1u) : buttons[0].width * 1.15f;
    float center = (xs[0] + xs[nx - 1u]) * 0.5f;
    float dy = ny > 1u ? ys[ny - 1u] - ys[ny - 2u] : buttons[0].height * 1.15f;
    if (dy <= 0.0f) dy = buttons[0].height * 1.15f;

    float first_y = last_y;
    if (trailing_count >= nx) {
        trailing_count = 0;
        first_y += dy;
    }
    unsigned total = trailing_count + added_count;

    for (unsigned ordinal = 0; ordinal < total; ordinal++) {
        unsigned row = ordinal / nx;
        unsigned column = ordinal % nx;
        unsigned columns = row_size(total, nx, row);
        float x = centered_column(center, spacing, columns, column);
        float y = first_y + dy * (float)row;
        if (ordinal < trailing_count) {
            mr_stf32(cpu, trailing[ordinal].object + INTERFACE_POSITION_OFFSET, x);
            mr_stf32(cpu, trailing[ordinal].object + INTERFACE_POSITION_OFFSET + 4u, y);
            if (getenv("MR_DIAGNOSTICS"))
                printf("[Localization UI] trailing engine button moved to %.1f,%.1f\n", x, y);
            continue;
        }
        button_geometry *geometry = &added[ordinal - trailing_count];
        *geometry = buttons[0];
        geometry->x = x;
        geometry->y = y;
    }
    return 0;
}

static uint32_t color_value(uint32_t rgb) {
    return 0xff000000u | rgb;
}

static uint32_t mix_color(uint32_t left, uint32_t right) {
    uint32_t red = ((left >> 16u & 0xffu) + (right >> 16u & 0xffu)) / 2u;
    uint32_t green = ((left >> 8u & 0xffu) + (right >> 8u & 0xffu)) / 2u;
    uint32_t blue = ((left & 0xffu) + (right & 0xffu)) / 2u;
    return red << 16u | green << 8u | blue;
}

static uint32_t rectangle_color(const flag_rectangle *rectangle) {
    int selected = mr_localization_current() == (int)rectangle->localization_index;
    if (rectangle->kind == FLAG_RECTANGLE_BORDER)
        return selected ? FLAG_SELECTED_COLOR : FLAG_BORDER_COLOR;
    return selected ? rectangle->source_color
                    : mix_color(rectangle->source_color, FLAG_INACTIVE_TINT);
}

static int initialize_fill_rect(mr_cpu *cpu, uint32_t stack_top, uint32_t object, uint32_t scratch,
                                float x, float y, float width, float height, uint32_t rgb) {
    mr_stf32(cpu, scratch, x);
    mr_stf32(cpu, scratch + 4u, y);
    mr_stf32(cpu, scratch + 8u, width);
    mr_stf32(cpu, scratch + 12u, height);
    const uint32_t initialize[4] = {object, scratch, scratch + 8u, color_value(rgb)};
    return call_guest(cpu, stack_top, "initialize language flag", MR_GAME_FILL_RECT_INIT, 4,
                      initialize, NULL);
}

static int create_fill_rect(mr_cpu *cpu, uint32_t stack_top, uint32_t button, uint32_t scratch,
                            float x, float y, float width, float height, uint32_t rgb,
                            uint32_t *result) {
    uint32_t object = mr_guest_alloc(cpu, INTERFACE_FILL_RECT_SIZE);
    if (!object) return -1;
    const uint32_t construct[1] = {object};
    if (call_guest(cpu, stack_top, "construct language flag", MR_GAME_FILL_RECT_CTOR, 1, construct,
                   NULL) != 0 ||
        initialize_fill_rect(cpu, stack_top, object, scratch, x, y, width, height, rgb) != 0)
        return -1;
    const uint32_t parent[2] = {object, button};
    if (call_guest(cpu, stack_top, "attach language flag", MR_GAME_INTERFACE_SET_PARENT, 2, parent,
                   NULL) != 0)
        return -1;
    *result = object;
    return 0;
}

static button_geometry geometry(float x, float y, float width, float height) {
    return (button_geometry){0, x, y, width, height};
}

static void rounded_slices(const button_geometry *bounds, float radius, int vertical,
                           button_geometry slices[MR_LOCALIZATION_FLAG_SLICE_COUNT]) {
    unsigned steps = (MR_LOCALIZATION_FLAG_SLICE_COUNT - 1u) / 2u;
    float depth = radius / (float)steps;
    for (unsigned index = 0; index < steps; index++) {
        float offset = depth * ((float)index + 0.5f);
        float curve = radius - offset;
        float inset = radius - sqrtf(fmaxf(0.0f, radius * radius - curve * curve));
        if (vertical) {
            slices[index] = geometry(bounds->x + depth * (float)index, bounds->y + inset, depth,
                                     bounds->height - inset * 2.0f);
            slices[MR_LOCALIZATION_FLAG_SLICE_COUNT - index - 1u] =
                geometry(bounds->x + bounds->width - depth * (float)(index + 1u), bounds->y + inset,
                         depth, bounds->height - inset * 2.0f);
        } else {
            slices[index] = geometry(bounds->x + inset, bounds->y + depth * (float)index,
                                     bounds->width - inset * 2.0f, depth);
            slices[MR_LOCALIZATION_FLAG_SLICE_COUNT - index - 1u] = geometry(
                bounds->x + inset, bounds->y + bounds->height - depth * (float)(index + 1u),
                bounds->width - inset * 2.0f, depth);
        }
    }
    slices[steps] = vertical ? geometry(bounds->x + radius, bounds->y,
                                        bounds->width - radius * 2.0f, bounds->height)
                             : geometry(bounds->x, bounds->y + radius, bounds->width,
                                        bounds->height - radius * 2.0f);
}

static int intersect_geometry(const button_geometry *left, const button_geometry *right,
                              button_geometry *result) {
    float x = fmaxf(left->x, right->x);
    float y = fmaxf(left->y, right->y);
    float right_edge = fminf(left->x + left->width, right->x + right->width);
    float bottom_edge = fminf(left->y + left->height, right->y + right->height);
    if (right_edge <= x || bottom_edge <= y) return 0;
    *result = geometry(x, y, right_edge - x, bottom_edge - y);
    return 1;
}

static int append_rectangle(mr_cpu *cpu, uint32_t stack_top, language_page_state *state,
                            uint32_t button, uint32_t scratch, const button_geometry *bounds,
                            unsigned localization_index, flag_rectangle_kind kind,
                            uint32_t source_color, uint32_t *result) {
    if (state->rectangle_count >= MR_LOCALIZATION_FLAG_RECTANGLE_CAPACITY) return -1;
    flag_rectangle rectangle = {
        .bounds = *bounds,
        .source_color = source_color,
        .localization_index = localization_index,
        .kind = kind,
    };
    if (create_fill_rect(cpu, stack_top, button, scratch, bounds->x, bounds->y, bounds->width,
                         bounds->height, rectangle_color(&rectangle), &rectangle.object) != 0)
        return -1;
    state->rectangles[state->rectangle_count++] = rectangle;
    if (result) *result = rectangle.object;
    return 0;
}

static int create_flag(mr_cpu *cpu, uint32_t stack_top, language_page_state *state, uint32_t button,
                       uint32_t scratch, unsigned localization_index,
                       const mr_localization_definition *definition,
                       const button_geometry *bounds) {
    float corner = fminf(bounds->width, bounds->height) * 0.12f;
    float border = fmaxf(2.0f, fminf(bounds->width, bounds->height) * 0.05f);
    button_geometry outline[MR_LOCALIZATION_FLAG_SLICE_COUNT];
    rounded_slices(bounds, corner, 0, outline);
    for (unsigned index = 0; index < MR_LOCALIZATION_FLAG_SLICE_COUNT; index++) {
        if (append_rectangle(cpu, stack_top, state, button, scratch, &outline[index],
                             localization_index, FLAG_RECTANGLE_BORDER, FLAG_BORDER_COLOR,
                             NULL) != 0)
            return -1;
    }

    button_geometry inner = geometry(bounds->x + border, bounds->y + border,
                                     bounds->width - border * 2.0f, bounds->height - border * 2.0f);
    float inner_corner = fmaxf(1.0f, corner - border);
    int vertical = definition->flag_layout == MR_FLAG_BANDS_VERTICAL;
    button_geometry clipping[MR_LOCALIZATION_FLAG_SLICE_COUNT];
    rounded_slices(&inner, inner_corner, vertical, clipping);
    for (unsigned band = 0; band < definition->flag_color_count; band++) {
        float begin = (float)band / (float)definition->flag_color_count;
        float end = (float)(band + 1u) / (float)definition->flag_color_count;
        button_geometry stripe = vertical ? geometry(inner.x + inner.width * begin, inner.y,
                                                     inner.width * (end - begin), inner.height)
                                          : geometry(inner.x, inner.y + inner.height * begin,
                                                     inner.width, inner.height * (end - begin));
        for (unsigned slice = 0; slice < MR_LOCALIZATION_FLAG_SLICE_COUNT; slice++) {
            button_geometry fragment;
            if (!intersect_geometry(&stripe, &clipping[slice], &fragment)) continue;
            if (append_rectangle(cpu, stack_top, state, button, scratch, &fragment,
                                 localization_index, FLAG_RECTANGLE_BAND,
                                 definition->flag_colors[band], NULL) != 0)
                return -1;
        }
    }
    return 0;
}

static int create_button(mr_cpu *cpu, uint32_t stack_top, language_page_state *state, uint32_t map,
                         unsigned localization_index, const mr_localization_definition *definition,
                         const button_geometry *geometry) {
    uint32_t scratch = mr_guest_alloc(cpu, 28u);
    uint32_t localization = scratch ? scratch + 16u : 0;
    uint32_t button = mr_guest_alloc(cpu, INTERFACE_BUTTON_SIZE);
    if (!scratch || !button) return -1;

    mr_st32(cpu, localization,
            (uint32_t)(uint8_t)definition->engine_code[0] << 8 |
                (uint32_t)(uint8_t)definition->engine_code[1]);
    mr_st32(cpu, localization + 4u, 0x2d2d2d2du);
    mr_st32(cpu, localization + 8u, 0x00002d2du);

    uint32_t node = 0;
    if (map_button(cpu, stack_top, map, localization, &node) != 0 || !node ||
        !mr_mem_ok(cpu, node + MAP_VALUE_OFFSET, 4))
        return -1;

    const uint32_t construct[1] = {button};
    if (call_guest(cpu, stack_top, "construct language button", MR_GAME_INTERFACE_BUTTON_CTOR, 1,
                   construct, NULL) != 0)
        return -1;
    mr_stf32(cpu, scratch, geometry->x);
    mr_stf32(cpu, scratch + 4u, geometry->y);
    mr_stf32(cpu, scratch + 8u, geometry->width);
    mr_stf32(cpu, scratch + 12u, geometry->height);
    uint32_t parent = mr_ld32(cpu, geometry->object + INTERFACE_PARENT_OFFSET);
    const uint32_t initialize[4] = {button, parent, scratch, scratch + 8u};
    if (!parent || call_guest(cpu, stack_top, "initialize language button",
                              MR_GAME_INTERFACE_BUTTON_INIT_FROM, 4, initialize, NULL) != 0)
        return -1;

    if (!definition->flag_color_count || create_flag(cpu, stack_top, state, button, scratch,
                                                     localization_index, definition, geometry) != 0)
        return -1;
    mr_st32(cpu, node + MAP_VALUE_OFFSET, button);
    state->buttons[state->button_count++] = button;
    mr_guest_free(cpu, scratch);
    return 0;
}

static int update_flag_styles(mr_cpu *cpu, uint32_t stack_top, language_page_state *state) {
    uint32_t scratch = mr_guest_alloc(cpu, 16u);
    if (!scratch) return -1;
    for (unsigned index = 0; index < state->rectangle_count; index++) {
        flag_rectangle *rectangle = &state->rectangles[index];
        if (initialize_fill_rect(cpu, stack_top, rectangle->object, scratch, rectangle->bounds.x,
                                 rectangle->bounds.y, rectangle->bounds.width,
                                 rectangle->bounds.height, rectangle_color(rectangle)) != 0) {
            mr_guest_free(cpu, scratch);
            return -1;
        }
    }
    mr_guest_free(cpu, scratch);
    state->style_dirty = 0;
    return 0;
}

static int destroy_objects(mr_cpu *cpu, uint32_t stack_top, language_page_state *state) {
    for (unsigned index = 0; index < state->rectangle_count; index++) {
        const uint32_t arguments[1] = {state->rectangles[index].object};
        if (call_guest(cpu, stack_top, "destroy language flag", MR_GAME_FILL_RECT_DTOR, 1,
                       arguments, NULL) != 0)
            return -1;
    }
    for (unsigned index = 0; index < state->button_count; index++) {
        const uint32_t arguments[1] = {state->buttons[index]};
        if (call_guest(cpu, stack_top, "destroy language button", MR_GAME_INTERFACE_BUTTON_DTOR, 1,
                       arguments, NULL) != 0)
            return -1;
    }
    memset(state, 0, sizeof *state);
    return 0;
}

static int inject_page(mr_cpu *cpu, uint32_t stack_top, mr_language_page kind,
                       language_page_state *state) {
    uint32_t map = state->page + button_map_offset(kind);
    button_geometry existing[sizeof ENGINE_BUTTONS / sizeof ENGINE_BUTTONS[0]];
    unsigned count =
        collect_buttons(cpu, stack_top, map, existing, sizeof existing / sizeof existing[0]);
    if (!count) {
        fprintf(stderr, "ERROR: %s language buttons are unavailable\n", page_name(kind));
        return -1;
    }

    button_geometry added[MR_LOCALIZATION_CONFIG_COUNT ? MR_LOCALIZATION_CONFIG_COUNT : 1u];
    unsigned added_count = mr_localization_count();
    if (arrange_trailing_row(cpu, existing, count, added, added_count) != 0) {
        fprintf(stderr, "ERROR: %s language-button grid is invalid\n", page_name(kind));
        return -1;
    }

    for (unsigned index = 0; index < added_count; index++) {
        button_geometry geometry = added[index];
        const mr_localization_definition *definition = mr_localization_get(index);
        if (!definition ||
            create_button(cpu, stack_top, state, map, index, definition, &geometry) != 0)
            return -1;
        printf("[Localization UI] %s added to %s at %.1f,%.1f\n", definition->code, page_name(kind),
               geometry.x, geometry.y);
    }
    state->pending = 0;
    state->injected = 1;

    const uint32_t arguments[1] = {state->page};
    return call_guest(cpu, stack_top, "refresh language page", refresh_function(kind), 1, arguments,
                      NULL);
}

void mr_language_ui_enter(mr_language_page kind, uint32_t page) {
    if ((unsigned)kind >= MR_LANGUAGE_PAGE_COUNT || !page || !mr_localization_count()) return;
    language_page_state *state = &PAGES[kind];
    if (state->page == page && state->injected) return;
    state->page = page;
    state->pending = 1;
}

void mr_language_ui_leave(mr_language_page kind, uint32_t page, int destroyed) {
    if ((unsigned)kind >= MR_LANGUAGE_PAGE_COUNT) return;
    language_page_state *state = &PAGES[kind];
    if (state->page != page) return;
    state->pending = 0;
    if (destroyed) state->cleanup = 1;
}

int mr_language_ui_defer_refresh(mr_language_page kind, uint32_t page) {
    if ((unsigned)kind >= MR_LANGUAGE_PAGE_COUNT || !mr_localization_count()) return 0;
    language_page_state *state = &PAGES[kind];
    if (state->page != page) mr_language_ui_enter(kind, page);
    return !state->injected;
}

int mr_language_ui_selection(mr_language_page kind, uint32_t page, uint32_t button) {
    if ((unsigned)kind >= MR_LANGUAGE_PAGE_COUNT) return MR_LOCALIZATION_ORIGINAL;
    language_page_state *state = &PAGES[kind];
    if (state->page != page) return MR_LOCALIZATION_ORIGINAL;
    state->style_dirty = 1;
    for (unsigned index = 0; index < state->button_count; index++)
        if (state->buttons[index] == button) return (int)index;
    return MR_LOCALIZATION_ORIGINAL;
}

int mr_language_ui_apply(mr_cpu *cpu, uint32_t stack_top) {
    for (unsigned kind = 0; kind < MR_LANGUAGE_PAGE_COUNT; kind++) {
        language_page_state *state = &PAGES[kind];
        if (state->cleanup && destroy_objects(cpu, stack_top, state) != 0) return -1;
        if (state->injected && state->style_dirty && update_flag_styles(cpu, stack_top, state) != 0)
            return -1;
        if (state->pending && !state->injected &&
            inject_page(cpu, stack_top, (mr_language_page)kind, state) != 0)
            return -1;
    }
    return 0;
}
