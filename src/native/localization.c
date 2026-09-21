#include "localization.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "localization_config.h"

#define LOCALIZATION_PATH_CAPACITY 1024u

static char PACK_ROOT[LOCALIZATION_PATH_CAPACITY];
static char PREFERENCE_PATH[LOCALIZATION_PATH_CAPACITY];
static atomic_int ACTIVE = ATOMIC_VAR_INIT(MR_LOCALIZATION_ORIGINAL);
static atomic_int REQUEST = ATOMIC_VAR_INIT(MR_LOCALIZATION_NO_REQUEST);
static atomic_int ENGINE_BOOTSTRAP = ATOMIC_VAR_INIT(1);

static int language_matches(const char *candidate, const char *configured) {
    if (!candidate || !configured) return 0;
    size_t length = strlen(configured);
    if (strlen(candidate) < length) return 0;
    return strncasecmp(candidate, configured, length) == 0 &&
           (candidate[length] == '\0' || candidate[length] == '-' || candidate[length] == '_');
}

static int find_language(const char *code) {
    for (unsigned index = 0; index < MR_LOCALIZATION_CONFIG_COUNT; index++)
        if (language_matches(code, MR_LOCALIZATION_CONFIGS[index].code)) return (int)index;
    return MR_LOCALIZATION_ORIGINAL;
}

static int read_preference(char *output, size_t capacity) {
    if (!PREFERENCE_PATH[0] || !output || capacity < 2) return 0;
    FILE *file = fopen(PREFERENCE_PATH, "r");
    if (!file) return 0;
    int found = fgets(output, (int)capacity, file) != NULL;
    fclose(file);
    if (!found) return 0;
    output[strcspn(output, "\r\n")] = '\0';
    return output[0] != '\0';
}

static void write_preference(const char *code) {
    if (!PREFERENCE_PATH[0] || !code) return;
    char temporary[LOCALIZATION_PATH_CAPACITY];
    if (snprintf(temporary, sizeof temporary, "%s.tmp", PREFERENCE_PATH) >= (int)sizeof temporary)
        return;
    FILE *file = fopen(temporary, "w");
    if (!file) return;
    int ok = fprintf(file, "%s\n", code) > 0 && fflush(file) == 0;
    if (fclose(file) != 0) ok = 0;
    if (!ok || rename(temporary, PREFERENCE_PATH) != 0) unlink(temporary);
}

void mr_localization_init(const char *pack_root, const char *data_root, const char *system_language,
                          const char *override_language) {
    PACK_ROOT[0] = '\0';
    PREFERENCE_PATH[0] = '\0';
    if (pack_root &&
        snprintf(PACK_ROOT, sizeof PACK_ROOT, "%s", pack_root) >= (int)sizeof PACK_ROOT)
        PACK_ROOT[0] = '\0';
    if (data_root && snprintf(PREFERENCE_PATH, sizeof PREFERENCE_PATH, "%s/language", data_root) >=
                         (int)sizeof PREFERENCE_PATH)
        PREFERENCE_PATH[0] = '\0';

    char saved[64];
    const char *wanted = override_language;
    if (!wanted || !*wanted)
        wanted = read_preference(saved, sizeof saved) ? saved : system_language;
    atomic_store_explicit(&ACTIVE, find_language(wanted), memory_order_release);
    atomic_store_explicit(&REQUEST, MR_LOCALIZATION_NO_REQUEST, memory_order_release);
    atomic_store_explicit(&ENGINE_BOOTSTRAP, 1, memory_order_release);
}

unsigned mr_localization_count(void) {
    return MR_LOCALIZATION_CONFIG_COUNT;
}

const mr_localization_definition *mr_localization_get(unsigned index) {
    return index < MR_LOCALIZATION_CONFIG_COUNT ? &MR_LOCALIZATION_CONFIGS[index] : NULL;
}

int mr_localization_current(void) {
    return atomic_load_explicit(&ACTIVE, memory_order_acquire);
}

const char *mr_localization_code(void) {
    int index = mr_localization_current();
    return index >= 0 ? MR_LOCALIZATION_CONFIGS[index].code : "en";
}

const char *mr_localization_engine_code(void) {
    int index = mr_localization_current();
    return index >= 0 ? MR_LOCALIZATION_CONFIGS[index].engine_code : "en";
}

const char *mr_localization_engine_query_code(void) {
    if (atomic_load_explicit(&ENGINE_BOOTSTRAP, memory_order_acquire) &&
        mr_localization_current() >= 0)
        return "en";
    return mr_localization_engine_code();
}

void mr_localization_finish_engine_bootstrap(void) {
    atomic_store_explicit(&ENGINE_BOOTSTRAP, 0, memory_order_release);
}

const char *mr_localization_country_code(void) {
    int index = mr_localization_current();
    return index >= 0 ? MR_LOCALIZATION_CONFIGS[index].country : "US";
}

void mr_localization_request(int index) {
    if (index < MR_LOCALIZATION_ORIGINAL || index >= (int)MR_LOCALIZATION_CONFIG_COUNT) return;
    atomic_store_explicit(&REQUEST, index, memory_order_release);
}

int mr_localization_take_request(void) {
    return atomic_exchange_explicit(&REQUEST, MR_LOCALIZATION_NO_REQUEST, memory_order_acq_rel);
}

int mr_localization_activate(int index) {
    if (index < MR_LOCALIZATION_ORIGINAL || index >= (int)MR_LOCALIZATION_CONFIG_COUNT) return 0;
    int previous = atomic_exchange_explicit(&ACTIVE, index, memory_order_acq_rel);
    if (previous == index) return 0;
    write_preference(index >= 0 ? MR_LOCALIZATION_CONFIGS[index].code : "en");
    return 1;
}

int mr_localization_override_path(const char *relative, char *output, size_t capacity) {
    if (!PACK_ROOT[0] || !relative || !output || !capacity) return 0;
    if (strcmp(relative, "localizationText/langs.json") != 0 &&
        strcmp(relative, "datalibs/gui_fonts_info") != 0) {
        return 0;
    }

    int length = snprintf(output, capacity, "%s/runtime/%s", PACK_ROOT, relative);
    if (length < 0 || (size_t)length >= capacity || access(output, R_OK) != 0) {
        output[0] = '\0';
        return 0;
    }
    return 1;
}
