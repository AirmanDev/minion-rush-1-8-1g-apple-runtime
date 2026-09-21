// Deterministic weekly-event support built from validated 1.8.1g data.

#include "offline_events.h"

#include "game_bindings.h"
#include "guest_runtime.h"
#include "host_time.h"
#include "shim_libc.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

enum {
    JSON_READER_SIZE = 112,
    JSON_VALUE_SIZE = 16,
    SOCIAL_EVENT_SIZE = 400,
    EVENT_TYPE_WEEKLY = 0,
    EVENTS_TYPE_COUNT = 3,
    EVENTS_LOADING_FLAGS = 0x28,
    EVENTS_ERROR_FLAGS = 0x2b,
};

#define EVENTS_LIST_BEGIN(type) (60u + 12u * (type))
#define EVENTS_LIST_END(type) (64u + 12u * (type))
#define RETRY_DELAY_MS 250.0

enum { WEEKLY_GOAL_TIERS = 3 };

#define WEEKLY_GOAL_AWARD "OnlineAward_CURRENCY_Banana"
static const int WEEKLY_GOAL_BANANAS[WEEKLY_GOAL_TIERS] = {100, 250, 500};

typedef struct {
    const char *mission;
    const char *title;
    const char *description;
    int goal[WEEKLY_GOAL_TIERS];
} weekly_event;

static const weekly_event WEEKLY_EVENTS[] = {
    {"OnlineMission_Bananas", "OM_BANANAS", "OM_BANANAS_LOCATION", {250, 500, 900}},
    // Bananas collected with the vacuum power-up.
    {"OnlineMission_Bananas_Vacuum",
     "OM_BANANAS_VACUUM",
     "OM_BANANAS_VACUUM_LOCATION",
     {60, 150, 300}},
    {"OnlineMission_Bananas_Splitter",
     "OM_BANANAS_SPLITTER",
     "OM_BANANAS_SPLITTER_LOCATION",
     {200, 400, 700}},
    {"OnlineMission_Bananas_Fluffy",
     "OM_BANANAS_FLUFFY",
     "OM_BANANAS_FLUFFY_LOCATION",
     {200, 400, 700}},
    // Near misses.
    {"OnlineMission_NearMiss", "OM_NEARMISS", "OM_NEARMISS_LOCATION", {15, 30, 50}},
    // Turns.
    {"OnlineMission_Turns", "OM_TURNS", "OM_TURNS_LOCATION", {20, 45, 75}},
    // Collected power-ups.
    {"OnlineMission_Powerups", "OM_POWERUPS", "OM_POWERUPS_LOCATION", {4, 8, 14}},
    // Objects destroyed with the freeze ray.
    {"OnlineMission_Freeze_Smash", "OM_FREEZE_SMASH", "OM_FREEZE_SMASH_LOCATION", {8, 18, 30}},
    {"OnlineMission_Rolls", "OM_ROLLS", "OM_ROLLS_LOCATION", {15, 35, 60}},
    {"OnlineMission_Jumps", "OM_JUMPS", "OM_JUMPS_LOCATION", {15, 35, 60}},
    // Despicable actions.
    {"OnlineMission_Despicable", "OM_DESPICABLE", "OM_DESPICABLE_LOCATION", {8, 18, 30}},
    // Score multiplier reached.
    {"OnlineMission_Multiplier", "OM_MULTIPLIER", "OM_MULTIPLIER_LOCATION", {4, 6, 8}},
    // Boss defeats in one run.
    {"OnlineMission_Vector", "OM_VECTOR", "OM_VECTOR", {1, 2, 3}},
    {"OnlineMission_Meena", "OM_MEENA", "OM_MEENA", {1, 2, 3}},
    {"OnlineMission_Macho", "OM_MACHO", "OM_MACHO", {1, 2, 3}},
    // Vehicle time in milliseconds.
    {"OnlineMission_Time_Rocket",
     "OM_TIME_ROCKET",
     "OM_TIME_ROCKET_LOCATION",
     {10000, 20000, 35000}},
    {"OnlineMission_Time_MegaMinion",
     "OM_TIME_MEGAMINION",
     "OM_TIME_MEGAMINION_LOCATION",
     {10000, 20000, 35000}},
    // Score.
    {"OnlineMission_Score", "OM_SCORE", "OM_SCORE_LOCATION", {40000, 120000, 250000}},
    // Distance in meters.
    {"OnlineMission_Distance", "OM_DISTANCE", "OM_DISTANCE_LOCATION", {1200, 2500, 4000}},
    {"OnlineMission_Villaintroquist", "OM_VILLAINTROQUIST", "OM_VILLAINTROQUIST", {1, 2, 3}},
};

typedef enum {
    EVENT_EMPTY,
    EVENT_ADDED,
    EVENT_READY,
    EVENT_FAILED,
} event_phase;

typedef struct {
    uint32_t manager;
    uint32_t retained_event;
    event_phase phase;
    double retry_after_ms;
} event_manager;

typedef struct {
    uint32_t start_loading;
    uint32_t update;
    uint32_t is_loading;
    uint32_t reader_ctor;
    uint32_t reader_dtor;
    uint32_t reader_parse;
    uint32_t value_ctor;
    uint32_t value_dtor;
    uint32_t event_ctor;
    uint32_t event_dtor;
    uint32_t add_event;
    uint32_t mission_singleton;
    uint32_t weekly_category;
    uint32_t event_version;
    uint32_t mission_sync;
    uint32_t mission_first_ending;
    uint32_t mission_remaining;
    uint32_t mission_find;
    uint32_t claim_start;
    uint32_t claim_process;
} event_api;

static const event_api API = {
    .start_loading = MR_GAME_EVENTS_START_LOADING,
    .update = MR_GAME_EVENTS_UPDATE,
    .is_loading = MR_GAME_EVENTS_IS_LOADING,
    .reader_ctor = MR_GAME_JSON_READER_CTOR,
    .reader_dtor = MR_GAME_JSON_READER_DTOR,
    .reader_parse = MR_GAME_JSON_READER_PARSE,
    .value_ctor = MR_GAME_JSON_VALUE_CTOR,
    .value_dtor = MR_GAME_JSON_VALUE_DTOR,
    .event_ctor = MR_GAME_SOCIAL_EVENT_CTOR,
    .event_dtor = MR_GAME_SOCIAL_EVENT_DTOR,
    .add_event = MR_GAME_EVENTS_ADD_EVENT,
    .mission_singleton = MR_GAME_MISSION_SINGLETON,
    .weekly_category = MR_GAME_WEEKLY_CATEGORY,
    .event_version = MR_GAME_EVENT_VERSION,
    .mission_sync = MR_GAME_MISSION_SYNC,
    .mission_first_ending = MR_GAME_MISSION_FIRST_ENDING,
    .mission_remaining = MR_GAME_MISSION_REMAINING,
    .mission_find = MR_GAME_MISSION_FIND,
    .claim_start = MR_GAME_CLAIM_START,
    .claim_process = MR_GAME_CLAIM_PROCESS,
};
static event_manager MANAGER;
static uint64_t ACTIVE_EVENTS;
static uint64_t START_CALLS;
static uint64_t UPDATE_FALLBACKS;
static uint64_t SYNC_RETRIES;
static uint64_t PRIZE_CHECKS;
static uint64_t FAILURES;
static int32_t LAST_REMAINING;
static char ACTIVE_ID[96];
static char ACTIVE_WINDOW[80];
static char ACTIVE_VERSION[96];
static char LAST_FAILURE[96];
static int INITIALIZED;

static void set_failure(const char *stage) {
    FAILURES++;
    snprintf(LAST_FAILURE, sizeof LAST_FAILURE, "%s", stage);
}

static event_manager *manager_state(uint32_t address) {
    if (!address) return NULL;
    if (!MANAGER.manager) MANAGER.manager = address;
    if (MANAGER.manager != address) {
        MANAGER.phase = EVENT_FAILED;
        set_failure("multiple EventsMgr instances");
        return NULL;
    }
    return &MANAGER;
}

static void clear_loading_state(mr_cpu *cpu, uint32_t manager) {
    if (!mr_mem_ok(cpu, manager + EVENTS_ERROR_FLAGS, EVENTS_TYPE_COUNT)) return;
    for (uint32_t i = 0; i < EVENTS_TYPE_COUNT; i++) {
        mr_st8(cpu, manager + EVENTS_LOADING_FLAGS + i, 0);
        mr_st8(cpu, manager + EVENTS_ERROR_FLAGS + i, 0);
    }
}

static int32_t event_list_count(mr_cpu *cpu, uint32_t manager) {
    uint32_t begin_slot = manager + EVENTS_LIST_BEGIN(EVENT_TYPE_WEEKLY);
    if (!mr_mem_ok(cpu, begin_slot, 8)) return -1;
    uint32_t begin = mr_ld32(cpu, begin_slot);
    uint32_t end = mr_ld32(cpu, manager + EVENTS_LIST_END(EVENT_TYPE_WEEKLY));
    if (end < begin || ((end - begin) & 3u)) return -1;
    return (int32_t)((end - begin) / 4u);
}

static int json_escape(const char *input, char *output, size_t capacity) {
    if (!input || !output || !capacity) return -1;
    size_t used = 0;
    for (const unsigned char *p = (const unsigned char *)input; *p; p++) {
        const char *escape = NULL;
        char unicode[7];
        switch (*p) {
        case '"':
            escape = "\\\"";
            break;
        case '\\':
            escape = "\\\\";
            break;
        case '\b':
            escape = "\\b";
            break;
        case '\f':
            escape = "\\f";
            break;
        case '\n':
            escape = "\\n";
            break;
        case '\r':
            escape = "\\r";
            break;
        case '\t':
            escape = "\\t";
            break;
        default:
            if (*p < 0x20) {
                snprintf(unicode, sizeof unicode, "\\u%04x", *p);
                escape = unicode;
            }
            break;
        }
        if (escape) {
            size_t length = strlen(escape);
            if (used + length >= capacity) return -1;
            memcpy(output + used, escape, length);
            used += length;
        } else {
            if (used + 1 >= capacity) return -1;
            output[used++] = (char)*p;
        }
    }
    output[used] = '\0';
    return 0;
}

static void engine_time_string(time_t value, char out[32]) {
    struct tm tm_value;
    if (!gmtime_r(&value, &tm_value) || !strftime(out, 32, "%Y-%m-%d %H:%M:%SZ", &tm_value))
        snprintf(out, 32, "2014-07-28 00:00:00Z");
}

static uint64_t weekly_window(time_t now, time_t *start, time_t *end) {
    int64_t days = (int64_t)now / 86400;
    int64_t weekday_from_monday = (days + 3) % 7; // 1970-01-01 was a Thursday.
    if (weekday_from_monday < 0) weekday_from_monday += 7;
    int64_t monday = days - weekday_from_monday;
    if (start) *start = (time_t)(monday * 86400);
    if (end) *end = (time_t)(monday * 86400) + 7 * 86400;
    return (uint64_t)(monday / 7);
}

// The week number selects the event definition.
static const weekly_event *weekly_event_of(uint64_t week) {
    return &WEEKLY_EVENTS[week % (sizeof WEEKLY_EVENTS / sizeof WEEKLY_EVENTS[0])];
}

typedef struct {
    uint32_t text;
    uint32_t reader;
    uint32_t value;
} guest_json;

static void guest_json_free(mr_cpu *cpu, guest_json *document) {
    mr_guest_free(cpu, document->value);
    mr_guest_free(cpu, document->reader);
    mr_guest_free(cpu, document->text);
    *document = (guest_json){0};
}

static void guest_json_release(mr_cpu *cpu, guest_json *document) {
    uint32_t args[1];
    if (!cpu->fault && document->value) {
        args[0] = document->value;
        mr_guest_call(cpu, API.value_dtor, 1, args);
    }
    if (!cpu->fault && document->reader) {
        args[0] = document->reader;
        mr_guest_call(cpu, API.reader_dtor, 1, args);
    }
    guest_json_free(cpu, document);
}

static int guest_json_parse(mr_cpu *cpu, const char *text, size_t length, guest_json *out,
                            const char *stage) {
    *out = (guest_json){
        .text = mr_guest_alloc(cpu, (uint32_t)length + 1),
        .reader = mr_guest_alloc(cpu, JSON_READER_SIZE),
        .value = mr_guest_alloc(cpu, JSON_VALUE_SIZE),
    };
    if (!out->text || !out->reader || !out->value) {
        guest_json_free(cpu, out);
        set_failure("guest memory");
        return -1;
    }
    memcpy(mr_mem(cpu, out->text), text, length + 1);

    uint32_t args[5] = {out->reader, 0, 0, 0, 0};
    mr_guest_call(cpu, API.reader_ctor, 1, args);
    uint32_t reader = out->reader;
    if (cpu->fault) {
        out->reader = 0;
        set_failure("Json::Reader ctor");
        goto fail;
    }

    args[0] = out->value;
    args[1] = 0;
    mr_guest_call(cpu, API.value_ctor, 2, args);
    if (cpu->fault) {
        out->value = 0;
        set_failure("Json::Value ctor");
        goto fail;
    }

    args[0] = reader;
    args[1] = out->text;
    args[2] = out->text + (uint32_t)length;
    args[3] = out->value;
    args[4] = 0;
    if (!mr_guest_call(cpu, API.reader_parse, 5, args) || cpu->fault) {
        set_failure(stage);
        goto fail;
    }
    return 0;

fail:
    guest_json_release(cpu, out);
    return -1;
}

#define WEEK_KEY_LIMIT 999999u

static void weekly_event_id(char *out, size_t size, uint64_t week) {
    snprintf(out, size, "OFFLINE_%06llu_%s", (unsigned long long)(WEEK_KEY_LIMIT - week),
             weekly_event_of(week)->mission);
}

static int add_week_event(mr_cpu *cpu, event_manager *state, uint32_t category_string,
                          uint64_t week, time_t start_time, time_t end_time,
                          const char *escaped_version) {
    int32_t previous_count = event_list_count(cpu, state->manager);
    if (previous_count < 0) return 1;

    const weekly_event *event = weekly_event_of(week);

    char start[32], end[32], id[96], goals[512], json[2048];
    engine_time_string(start_time, start);
    engine_time_string(end_time, end);
    weekly_event_id(id, sizeof id, week);

    size_t used = 0;
    for (int tier = 0; tier < WEEKLY_GOAL_TIERS; tier++) {
        int written = snprintf(goals + used, sizeof goals - used,
                               "%s{\"target_score\":%d,\"prize_type\":\"%s\","
                               "\"prize_amount\":%d}",
                               tier ? "," : "", event->goal[tier], WEEKLY_GOAL_AWARD,
                               WEEKLY_GOAL_BANANAS[tier]);
        if (written <= 0 || (size_t)written >= sizeof goals - used) {
            set_failure("weekly goal construction");
            return -1;
        }
        used += (size_t)written;
    }

    int json_len = snprintf(json, sizeof json,
                            "{"
                            "\"_version\":\"%s\","
                            "\"id\":\"%s\","
                            "\"_mission\":\"%s\","
                            "\"name\":\"%s\","
                            "\"description\":\"%s\","
                            "\"category\":\"weekly\","
                            "\"start_date\":\"%s\","
                            "\"end_date\":\"%s\","
                            "\"_consolation_price_set\":[%s],"
                            "\"tournament\":{"
                            "\"type\":\"weekly\","
                            "\"leaderboard\":{\"name\":\"%s\"},"
                            "\"order\":\"desc\","
                            "\"delivery\":\"hermes\","
                            "\"awards\":[]"
                            "}"
                            "}",
                            escaped_version, id, event->mission, event->title, event->description,
                            start, end, goals, event->mission);
    if (json_len <= 0 || (size_t)json_len >= sizeof json) {
        set_failure("JSON construction");
        return -1;
    }

    guest_json document;
    if (guest_json_parse(cpu, json, (size_t)json_len, &document, "event") != 0) return -1;

    uint32_t social_event = mr_guest_alloc(cpu, SOCIAL_EVENT_SIZE);
    if (!social_event) {
        guest_json_release(cpu, &document);
        set_failure("guest memory");
        return -1;
    }

    int event_live = 0, transferred = 0;
    uint32_t args[4] = {0};

    args[0] = social_event;
    args[1] = document.value;
    mr_guest_call(cpu, API.event_ctor, 2, args);
    if (cpu->fault) {
        set_failure("social::Event ctor");
        goto cleanup;
    }
    event_live = 1;

    args[0] = state->manager;
    args[1] = EVENT_TYPE_WEEKLY;
    args[2] = social_event;
    args[3] = category_string;
    mr_guest_call(cpu, API.add_event, 4, args);
    if (cpu->fault) {
        set_failure("EventsMgr::AddEvent");
        goto cleanup;
    }
    transferred = 1;

    if (event_list_count(cpu, state->manager) <= previous_count) {
        state->retained_event = social_event;
        state->phase = EVENT_FAILED;
        set_failure("event list did not grow");
        goto cleanup;
    }

    state->retained_event = social_event;
    state->phase = EVENT_ADDED;
    guest_json_release(cpu, &document);
    snprintf(ACTIVE_ID, sizeof ACTIVE_ID, "%s", id);
    snprintf(ACTIVE_WINDOW, sizeof ACTIVE_WINDOW, "%s .. %s", start, end);
    return 0;

cleanup:
    if (!cpu->fault && event_live && !transferred) {
        args[0] = social_event;
        mr_guest_call(cpu, API.event_dtor, 1, args);
    }
    guest_json_release(cpu, &document);
    if (!transferred)
        mr_guest_free(cpu, social_event);
    else if (state->phase == EVENT_EMPTY)
        state->phase = EVENT_FAILED;
    return -1;
}

static int add_current_event(mr_cpu *cpu, event_manager *state, uint32_t category_string) {
    if (!category_string || !mr_mem_ok(cpu, category_string, 4)) return 1;

    char engine_version[96], escaped_version[192];
    int version_status =
        mr_guest_read_std_string(cpu, API.event_version, engine_version, sizeof engine_version);
    if (version_status == 0 || !engine_version[0]) return 1;
    if (version_status < 0 ||
        json_escape(engine_version, escaped_version, sizeof escaped_version) != 0) {
        set_failure("invalid event version");
        return -1;
    }
    snprintf(ACTIVE_VERSION, sizeof ACTIVE_VERSION, "%s", engine_version);

    time_t now = time(NULL), start_time, end_time;
    uint64_t week = weekly_window(now, &start_time, &end_time);
    if (week >= WEEK_KEY_LIMIT) {
        set_failure("week number exceeded key range");
        return -1;
    }

    char live_id[96], closed_id[96];
    weekly_event_id(live_id, sizeof live_id, week);
    weekly_event_id(closed_id, sizeof closed_id, week - 1);
    if (strcmp(live_id, closed_id) >= 0) {
        set_failure("event ID ordering");
        return -1;
    }

    int status = add_week_event(cpu, state, category_string, week - 1, start_time - 7 * 86400,
                                start_time, escaped_version);
    if (status != 0) return status;
    return add_week_event(cpu, state, category_string, week, start_time, end_time, escaped_version);
}

// 0 synchronized, 1 singleton not ready, -1 failure.
static int sync_current_mission(mr_cpu *cpu, event_manager *state) {
    if (!mr_mem_ok(cpu, API.mission_singleton, 4)) return -1;
    uint32_t mission_manager = mr_ld32(cpu, API.mission_singleton);
    if (!mission_manager || !mr_mem_ok(cpu, mission_manager, 4)) return 1;

    uint32_t args[3] = {mission_manager, EVENT_TYPE_WEEKLY, 0};
    mr_guest_call(cpu, API.mission_sync, 2, args);
    if (cpu->fault) {
        set_failure("OnlineMissionMgr::SyncMissionsWithEvents");
        return -1;
    }

    args[0] = mission_manager;
    mr_guest_call(cpu, API.mission_first_ending, 1, args);
    if (cpu->fault) {
        set_failure("CalculateFirstEndingEvent");
        return -1;
    }

    LAST_REMAINING = 0;
    uint32_t scratch = mr_guest_alloc(cpu, 8);
    if (scratch) {
        mr_st32(cpu, scratch, 0);
        mr_st32(cpu, scratch + 4, 0);
        args[0] = mission_manager;
        args[1] = scratch;
        args[2] = scratch + 4;
        mr_guest_call(cpu, API.mission_remaining, 3, args);
        if (!cpu->fault) LAST_REMAINING = (int32_t)mr_ld32(cpu, scratch);
        mr_guest_free(cpu, scratch);
        if (cpu->fault) {
            set_failure("GetFirstEndingMissionRemainingTime");
            return -1;
        }
    }

    state->phase = EVENT_READY;
    ACTIVE_EVENTS++;
    fprintf(stderr, "[offline-event] active: %s", ACTIVE_ID);
    if (LAST_REMAINING > 0) fprintf(stderr, ", %d s remaining", LAST_REMAINING);
    fprintf(stderr, "\n");
    return 0;
}

static void service_manager(mr_cpu *cpu, event_manager *state, uint32_t category_string,
                            int allow_add) {
    if (!state || state->phase == EVENT_READY || state->phase == EVENT_FAILED ||
        mr_monotonic_ms() < state->retry_after_ms)
        return;

    int status = 0;
    if (state->phase == EVENT_EMPTY) {
        if (!allow_add) return;
        status = add_current_event(cpu, state, category_string);
    }
    if (!cpu->fault && status == 0 && state->phase == EVENT_ADDED)
        status = sync_current_mission(cpu, state);

    if (status != 0) {
        state->retry_after_ms = mr_monotonic_ms() + RETRY_DELAY_MS;
        if (status > 0) SYNC_RETRIES++;
    }
}

int mr_offline_events_init(void) {
    mr_offline_events_shutdown();
    INITIALIZED = 1;
    return 0;
}

void mr_offline_events_shutdown(void) {
    memset(&MANAGER, 0, sizeof MANAGER);
    ACTIVE_EVENTS = START_CALLS = UPDATE_FALLBACKS = SYNC_RETRIES = FAILURES = 0;
    PRIZE_CHECKS = 0;
    LAST_REMAINING = 0;
    ACTIVE_ID[0] = ACTIVE_WINDOW[0] = ACTIVE_VERSION[0] = LAST_FAILURE[0] = '\0';
    INITIALIZED = 0;
}

enum {
    // OnlineMissionInfo stores completed, unclaimed weekly goals.
    CONSOLATION_LIST_BEGIN = 0x8c,
    CONSOLATION_LIST_END = 0x90,
    // OnlineMissionConsolationInfo stores the integer goal threshold.
    CONSOLATION_TARGET = 0x04,
};

static int earned_goal_prizes(mr_cpu *cpu, uint32_t data, char *out, size_t size) {
    if (!mr_mem_ok(cpu, API.mission_singleton, 4)) return -1;
    uint32_t mission_manager = mr_ld32(cpu, API.mission_singleton);
    if (!mission_manager) return -1;

    uint32_t args[2] = {mission_manager, data};
    uint32_t info = mr_guest_call(cpu, API.mission_find, 2, args);
    if (cpu->fault || !info || !mr_mem_ok(cpu, info + CONSOLATION_LIST_END, 4)) return -1;

    uint64_t closed_week = weekly_window(time(NULL), NULL, NULL) - 1;
    const weekly_event *closed = weekly_event_of(closed_week);

    uint32_t begin = mr_ld32(cpu, info + CONSOLATION_LIST_BEGIN);
    uint32_t end = mr_ld32(cpu, info + CONSOLATION_LIST_END);
    size_t used = 0;
    for (uint32_t slot = begin; slot && slot + 8 <= end; slot += 8) {
        if (!mr_mem_ok(cpu, slot, 4)) return -1;
        uint32_t entry = mr_ld32(cpu, slot);
        if (!entry || !mr_mem_ok(cpu, entry + CONSOLATION_TARGET, 4)) continue;
        int32_t target = (int32_t)mr_ld32(cpu, entry + CONSOLATION_TARGET);
        for (int tier = 0; tier < WEEKLY_GOAL_TIERS; tier++) {
            if (closed->goal[tier] != target) continue;
            int written = snprintf(out + used, size - used, "%s{\"name\":\"%s\",\"value\":%d}",
                                   used ? "," : "", WEEKLY_GOAL_AWARD, WEEKLY_GOAL_BANANAS[tier]);
            if (written <= 0 || (size_t)written >= size - used) return -1;
            used += (size_t)written;
            break;
        }
    }
    return 0;
}

static void resolve_prize_check(mr_cpu *cpu, uint32_t manager, uint32_t data) {
    char prizes[384] = {0}, list[512];
    if (earned_goal_prizes(cpu, data, prizes, sizeof prizes) != 0) {
        set_failure("completed-goal extraction");
        return;
    }
    int length = snprintf(list, sizeof list, "[%s]", prizes);
    if (length <= 0 || (size_t)length >= sizeof list) {
        set_failure("award-list construction");
        return;
    }

    guest_json document;
    if (guest_json_parse(cpu, list, (size_t)length, &document, "award-list JSON parse") != 0)
        return;

    uint32_t args[3] = {manager, data, document.value};
    mr_guest_call(cpu, API.claim_process, 3, args);
    if (cpu->fault)
        set_failure("ProcessPrizeCheckedJson");
    else
        PRIZE_CHECKS++;
    guest_json_release(cpu, &document);
}

int mr_offline_events_before_block(mr_cpu *cpu, uint32_t pc, uint32_t *result) {
    if (!INITIALIZED || !cpu || !result) return 0;

    if (pc == API.is_loading) {
        *result = 0;
        return 1;
    }

    if (pc == API.claim_start) {
        resolve_prize_check(cpu, cpu->r[0], cpu->r[1]);
        *result = 0;
        return 1;
    }

    if (pc == API.start_loading) {
        uint32_t manager = cpu->r[0];
        uint32_t type = cpu->r[1];
        uint32_t category = cpu->r[2];
        START_CALLS++;
        clear_loading_state(cpu, manager);
        if (type == EVENT_TYPE_WEEKLY && manager) {
            event_manager *state = manager_state(manager);
            service_manager(cpu, state, category, 1);
        }
        *result = 0;
        return 1;
    }

    if (pc != API.update) return 0;

    uint32_t manager = cpu->r[0];
    clear_loading_state(cpu, manager);
    event_manager *state = manager ? manager_state(manager) : NULL;
    if (state && state->phase == EVENT_EMPTY) UPDATE_FALLBACKS++;
    service_manager(cpu, state, API.weekly_category, 1);
    *result = 0;
    return 1;
}

int mr_offline_events_ready(void) {
    return ACTIVE_EVENTS > 0;
}

void mr_offline_events_report(void) {
    printf("local weekly event: %s", mr_offline_events_ready() ? "active" : "INACTIVE");
    if (*ACTIVE_ID) printf(" | %s | %s", ACTIVE_ID, ACTIVE_WINDOW);
    if (*ACTIVE_VERSION) printf(" | engine version %s", ACTIVE_VERSION);
    if (LAST_REMAINING > 0) printf(" | %d s remaining", LAST_REMAINING);
    printf(" | start %llu | update fallbacks %llu", (unsigned long long)START_CALLS,
           (unsigned long long)UPDATE_FALLBACKS);
    if (SYNC_RETRIES) printf(" | %llu sync retries", (unsigned long long)SYNC_RETRIES);
    if (PRIZE_CHECKS) printf(" | %llu local award calculations", (unsigned long long)PRIZE_CHECKS);
    if (FAILURES)
        printf(" | %llu failures | last: %s", (unsigned long long)FAILURES,
               *LAST_FAILURE ? LAST_FAILURE : "unknown");
    printf("\n");
}
