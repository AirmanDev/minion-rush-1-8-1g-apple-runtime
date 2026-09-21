// Local replacements and UI policy for services retired from release 1.8.1g.

#include "offline_mode.h"

#include "a64_runtime.h"
#include "offline_events.h"
#include "game_bindings.h"
#include "guest_runtime.h"
#include "safe_area.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    MAX_UI_OBJECTS = 512,
    UI_VISIBLE_OFFSET = 4,
    UI_ACTIVE_OFFSET = 5,
    SERVER_TIME_SYNCHRONIZED = 2,
};

typedef enum {
    UI_NORMAL,
    UI_BLOCKED,
    UI_FORCED_EVENT,
} ui_policy;

typedef struct {
    uint32_t return_pc;
    uint32_t page;
    ui_policy pending;
    mr_safe_area_role safe_area_role;
    int active;
} ui_call_state;

typedef struct {
    uint32_t object;
    ui_policy policy;
} ui_object;

typedef struct {
    uint32_t ui_set_visible;
    uint32_t ui_set_active;
    uint32_t server_time_seconds;
    uint32_t server_time_status;
    uint32_t has_internet;
    uint32_t can_login;
    uint32_t is_logged_in;
} offline_api;

static const offline_api API = {
    .ui_set_visible = MR_GAME_UI_SET_VISIBLE,
    .ui_set_active = MR_GAME_UI_SET_ACTIVE,
    .server_time_seconds = MR_GAME_SERVER_TIME_SECONDS,
    .server_time_status = MR_GAME_SERVER_TIME_STATUS,
    .has_internet = MR_GAME_HAS_INTERNET,
    .can_login = MR_GAME_CAN_LOGIN,
    .is_logged_in = MR_GAME_IS_LOGGED_IN,
};
static const uint32_t UI_LOOKUP_FUNCTIONS[] = {
    MR_GAME_MENU_GET_OBJECT,       MR_GAME_MENU_GET_BUTTON,       MR_GAME_MENU_GET_GRAPH,
    MR_GAME_BASIC_PAGE_GET_OBJECT, MR_GAME_BASIC_PAGE_GET_BUTTON, MR_GAME_BASIC_PAGE_GET_GRAPH,
};
static ui_call_state UI_CALL;
static ui_object UI_OBJECTS[MAX_UI_OBJECTS];
static uint32_t UI_OBJECT_COUNT;
static uint64_t BLOCKED_SERVICE_CALLS;
static uint64_t HIDDEN_BUTTONS;
static uint64_t FORCED_EVENT_BUTTONS;
static int LOG_OFFLINE;
static int ENABLED;

static int sorted_contains(const uint32_t *list, size_t count, uint32_t value) {
    size_t low = 0, high = count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (list[middle] < value)
            low = middle + 1;
        else
            high = middle;
    }
    return low < count && list[low] == value;
}

static void set_pending_return(uint32_t return_pc, uint32_t page, ui_policy policy,
                               mr_safe_area_role safe_area_role) {
    UI_CALL.return_pc = return_pc & ~1u;
    UI_CALL.page = page;
    UI_CALL.pending = policy;
    UI_CALL.safe_area_role = safe_area_role;
    UI_CALL.active = 1;
    mr_a64_set_dynamic_hook(return_pc, 1);
}

static void clear_pending_return(void) {
    UI_CALL.pending = UI_NORMAL;
    UI_CALL.safe_area_role = MR_SAFE_AREA_DEFAULT;
    UI_CALL.active = 0;
    UI_CALL.page = 0;
    UI_CALL.return_pc = 0;
    mr_a64_set_dynamic_hook(0, 0);
}

static int blocked_function(uint32_t address) {
    return sorted_contains(MR_GAME_BLOCKED_FUNCTIONS, MR_GAME_BLOCKED_FUNCTION_COUNT,
                           address & ~1u);
}

static uint64_t BLOCKED_HITS[MR_GAME_BLOCKED_FUNCTION_COUNT];

static void blocked_note(uint32_t address) {
    uint32_t target = address & ~1u;
    for (unsigned i = 0; i < MR_GAME_BLOCKED_FUNCTION_COUNT; i++)
        if (MR_GAME_BLOCKED_FUNCTIONS[i] == target) {
            BLOCKED_HITS[i]++;
            return;
        }
}

static int ascii_contains(const char *text, const char *needle) {
    size_t length = strlen(needle);
    if (!length) return 1;
    for (; *text; text++) {
        size_t i = 0;
        while (i < length && text[i] &&
               tolower((unsigned char)text[i]) == tolower((unsigned char)needle[i]))
            i++;
        if (i == length) return 1;
    }
    return 0;
}

static int ascii_equal(const char *left, const char *right) {
    while (*left && *right) {
        if (tolower((unsigned char)*left++) != tolower((unsigned char)*right++)) return 0;
    }
    return *left == '\0' && *right == '\0';
}

static ui_policy classify_ui_object(const char *name) {
    if (ascii_equal(name, "Events_Button")) return UI_FORCED_EVENT;
    static const char *const BLOCKED_WORDS[] = {
        "iap",        "restore", "button_cash", "promo_cash", "gs_shop", "google",
        "gameapi",    "gplus",   "gs_login",    "gs_plus",    "glgames", "facebook",
        "gamecenter", "friend",  "social",      "support",
    };
    for (size_t i = 0; i < sizeof BLOCKED_WORDS / sizeof BLOCKED_WORDS[0]; i++)
        if (ascii_contains(name, BLOCKED_WORDS[i])) return UI_BLOCKED;
    return UI_NORMAL;
}

static int read_ui_policy(mr_cpu *cpu, uint32_t string_object, ui_policy *policy,
                          mr_safe_area_role *safe_area_role) {
    char name[160];
    if (mr_guest_read_jet_string(cpu, string_object, name, sizeof name) != 1) return 0;

    *policy = classify_ui_object(name);
    *safe_area_role = mr_safe_area_classify_ui_object(name);
    if (*safe_area_role != MR_SAFE_AREA_DEFAULT && getenv("MR_DIAGNOSTICS"))
        printf("[Safe area] lookup %s\n", name);
    if (LOG_OFFLINE && *policy != UI_NORMAL)
        fprintf(stderr, "[offline] %s: %s\n", name,
                *policy == UI_BLOCKED ? "hidden" : "event button active");
    return 1;
}

static ui_policy object_policy(uint32_t object) {
    for (uint32_t i = UI_OBJECT_COUNT; i > 0; i--)
        if (UI_OBJECTS[i - 1].object == object) return UI_OBJECTS[i - 1].policy;
    return UI_NORMAL;
}

static int is_ui_lookup(uint32_t address) {
    for (size_t index = 0; index < sizeof UI_LOOKUP_FUNCTIONS / sizeof UI_LOOKUP_FUNCTIONS[0];
         index++)
        if (address == UI_LOOKUP_FUNCTIONS[index]) return 1;
    return 0;
}

static void apply_object_policy(mr_cpu *cpu, uint32_t object, ui_policy policy) {
    if (!object || !mr_mem_ok(cpu, object, UI_ACTIVE_OFFSET + 1)) return;
    for (uint32_t i = 0; i < UI_OBJECT_COUNT; i++) {
        if (UI_OBJECTS[i].object == object) {
            UI_OBJECTS[i].policy = policy;
            goto apply;
        }
    }
    if (UI_OBJECT_COUNT >= MAX_UI_OBJECTS) return;
    UI_OBJECTS[UI_OBJECT_COUNT++] = (ui_object){object, policy};
    if (policy == UI_BLOCKED) HIDDEN_BUTTONS++;
    if (policy == UI_FORCED_EVENT) FORCED_EVENT_BUTTONS++;

apply:
    mr_st8(cpu, object + UI_VISIBLE_OFFSET, policy == UI_FORCED_EVENT);
    mr_st8(cpu, object + UI_ACTIVE_OFFSET, policy == UI_FORCED_EVENT);
}

int mr_offline_init(mr_cpu *cpu) {
    mr_offline_shutdown();
    if (!cpu || classify_ui_object("Shop_IAP") != UI_BLOCKED ||
        classify_ui_object("GoogleButton") != UI_BLOCKED ||
        classify_ui_object("Friends_Button") != UI_BLOCKED ||
        classify_ui_object("Map_social_button") != UI_BLOCKED ||
        classify_ui_object("Support_button") != UI_BLOCKED ||
        classify_ui_object("Support_loading") != UI_BLOCKED ||
        classify_ui_object("Events_Button") != UI_FORCED_EVENT ||
        classify_ui_object("Map_Shop_Button") != UI_NORMAL ||
        MR_GAME_BLOCKED_FUNCTION_COUNT <= MR_GAME_EXACT_BLOCKED_FUNCTION_COUNT ||
        MR_GAME_OFFLINE_HOOK_FUNCTION_COUNT < MR_GAME_BLOCKED_FUNCTION_COUNT)
        return -1;
    if (mr_offline_events_init() != 0) return -1;

    LOG_OFFLINE = getenv("MR_OFFLINE_LOG") != NULL;
    ENABLED = 1;
    return 0;
}

void mr_offline_shutdown(void) {
    mr_offline_events_shutdown();
    mr_a64_set_dynamic_hook(0, 0);
    memset(&UI_CALL, 0, sizeof UI_CALL);
    memset(UI_OBJECTS, 0, sizeof UI_OBJECTS);
    UI_OBJECT_COUNT = 0;
    BLOCKED_SERVICE_CALLS = HIDDEN_BUTTONS = FORCED_EVENT_BUTTONS = 0;
    LOG_OFFLINE = ENABLED = 0;
}

int mr_offline_before_block(mr_cpu *cpu) {
    if (!ENABLED || !cpu) return 0;
    uint32_t pc = cpu->r[MR_R_PC];

    if (UI_CALL.active && pc == UI_CALL.return_pc) {
        if (UI_CALL.safe_area_role != MR_SAFE_AREA_DEFAULT)
            mr_safe_area_register_ui_object(cpu, UI_CALL.page, cpu->r[0], UI_CALL.safe_area_role);
        if (UI_CALL.pending != UI_NORMAL) apply_object_policy(cpu, cpu->r[0], UI_CALL.pending);
        clear_pending_return();
    }

    uint32_t result = 0;
    if (mr_offline_events_before_block(cpu, pc, &result))
        return mr_guest_synthetic_return(cpu, result);

    if (is_ui_lookup(pc)) {
        ui_policy policy = UI_NORMAL;
        mr_safe_area_role safe_area_role = MR_SAFE_AREA_DEFAULT;
        if (read_ui_policy(cpu, cpu->r[1], &policy, &safe_area_role) &&
            (policy != UI_NORMAL || safe_area_role != MR_SAFE_AREA_DEFAULT))
            set_pending_return(cpu->r[MR_R_LR], cpu->r[0], policy, safe_area_role);
        return 0;
    }

    if (pc == API.ui_set_visible || pc == API.ui_set_active) {
        ui_policy policy = object_policy(cpu->r[0]);
        if (policy == UI_BLOCKED) cpu->r[1] = 0;
        if (policy == UI_FORCED_EVENT) cpu->r[1] = 1;
        return 0;
    }

    if (pc == API.server_time_seconds) return mr_guest_synthetic_return(cpu, (uint32_t)time(NULL));
    if (pc == API.server_time_status)
        return mr_guest_synthetic_return(cpu, SERVER_TIME_SYNCHRONIZED);
    if (pc == API.has_internet || pc == API.can_login || pc == API.is_logged_in)
        return mr_guest_synthetic_return(cpu, 0);

    if (blocked_function(pc)) {
        BLOCKED_SERVICE_CALLS++;
        blocked_note(pc);
        return mr_guest_synthetic_return(cpu, 0);
    }
    return 0;
}

int mr_offline_main_menu_ready(void) {
    return ENABLED && HIDDEN_BUTTONS > 0 && FORCED_EVENT_BUTTONS > 0;
}

void mr_offline_report(void) {
    printf("offline mode: %s", ENABLED ? "active" : "INACTIVE");
    if (ENABLED) {
        printf(" | %llu service calls blocked | %llu online/IAP buttons "
               "hidden | %llu event buttons active",
               (unsigned long long)BLOCKED_SERVICE_CALLS, (unsigned long long)HIDDEN_BUTTONS,
               (unsigned long long)FORCED_EVENT_BUTTONS);
    }
    printf("\n");
    if (ENABLED && getenv("MR_DIAGNOSTICS")) {
        for (unsigned round = 0; round < 5; round++) {
            unsigned best = 0;
            for (unsigned i = 1; i < MR_GAME_BLOCKED_FUNCTION_COUNT; i++)
                if (BLOCKED_HITS[i] > BLOCKED_HITS[best]) best = i;
            if (!BLOCKED_HITS[best]) break;
            printf("  blocked #%u: 0x%08x -- %llu calls\n", round + 1,
                   MR_GAME_BLOCKED_FUNCTIONS[best], (unsigned long long)BLOCKED_HITS[best]);
            BLOCKED_HITS[best] = 0;
        }
    }
    mr_offline_events_report();
}
