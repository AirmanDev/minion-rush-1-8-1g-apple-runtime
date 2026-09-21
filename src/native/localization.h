#ifndef MR_LOCALIZATION_H
#define MR_LOCALIZATION_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    MR_FLAG_BANDS_HORIZONTAL,
    MR_FLAG_BANDS_VERTICAL,
} mr_flag_band_layout;

typedef struct {
    const char *code;
    const char *display_name;
    const char *engine_code;
    const char *country;
    mr_flag_band_layout flag_layout;
    const uint32_t *flag_colors;
    unsigned flag_color_count;
} mr_localization_definition;

enum {
    MR_LOCALIZATION_ORIGINAL = -1,
    MR_LOCALIZATION_NO_REQUEST = -2,
};

void mr_localization_init(const char *pack_root, const char *data_root, const char *system_language,
                          const char *override_language);

unsigned mr_localization_count(void);
const mr_localization_definition *mr_localization_get(unsigned index);
int mr_localization_current(void);
const char *mr_localization_code(void);
const char *mr_localization_engine_code(void);
const char *mr_localization_engine_query_code(void);
const char *mr_localization_country_code(void);
void mr_localization_finish_engine_bootstrap(void);

void mr_localization_request(int index);
int mr_localization_take_request(void);
int mr_localization_activate(int index);

int mr_localization_override_path(const char *relative, char *output, size_t capacity);

#endif
