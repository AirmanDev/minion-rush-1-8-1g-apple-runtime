#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include "builder_args.h"
#include <shobjidl.h>
#include <strsafe.h>
#include <stdio.h>
#include <wchar.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

enum {
    ID_TITLE = 101, ID_SUBTITLE, ID_UI_EN, ID_UI_HU,
    ID_SOURCE_HINT, ID_SOURCE_LABEL, ID_SOURCE, ID_PICK_SOURCE,
    ID_DESTINATION_LABEL, ID_DESTINATION, ID_PICK_DESTINATION,
    ID_PACK_HINT, ID_PACK_LABEL, ID_HU_PACK,
    ID_BUILD, ID_CANCEL, ID_OPEN_FOLDER,
    ID_STATE, ID_DETAIL, ID_DETAILS_TOGGLE,
    ID_DIAGNOSTICS, ID_COPY_ERROR, ID_COPY_LOG, ID_SAVE_LOG, ID_OPEN_TEMP,
    ID_LOG, ID_LAST,
    TIMER_ANIMATION = 1,
    WM_APPEND_LOG = WM_APP + 1, WM_BUILD_DONE = WM_APP + 2
};

#include "builder_palette.h"

typedef enum {
    ROLE_TITLE, ROLE_SUBTITLE, ROLE_LABEL, ROLE_HINT, ROLE_FIELD,
    ROLE_PRIMARY, ROLE_SECONDARY, ROLE_BROWSE, ROLE_LINK, ROLE_LANG, ROLE_CHECK,
    ROLE_STATUS, ROLE_DETAIL, ROLE_LOG
} Role;

typedef enum { FONT_TITLE, FONT_BODY, FONT_LABEL, FONT_SMALL, FONT_STATUS, FONT_MONO, FONT_COUNT } FontSlot;
typedef enum { BRUSH_NAVY, BRUSH_CARD, BRUSH_FIELD, BRUSH_FIELD_OFF, BRUSH_COUNT } BrushSlot;
typedef enum { STATE_IDLE, STATE_BUSY, STATE_OK, STATE_FAILED, STATE_STOPPED } UiState;

typedef struct { int id; LPCWSTR kind; LPCWSTR text; DWORD style; Role role; } Spec;
typedef struct { int label, hint, field, pick; } Row;

#define LABEL_STYLE (WS_CHILD | WS_VISIBLE | SS_NOPREFIX | SS_ENDELLIPSIS)
#define BUTTON_STYLE (WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW)
#define FIELD_STYLE (WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_READONLY)
#define SOURCE_STYLE (WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL)

/* Creation order is also the z-order and keyboard order. Each field is
   preceded by its label so assistive technology can name it. */
static const Spec specs[] = {
    {ID_TITLE, L"STATIC", L"Minion Rush IPA Builder", LABEL_STYLE, ROLE_TITLE},
    {ID_SUBTITLE, L"STATIC",
     L"Build an unsigned iPhone IPA from your own original 1.8.1g ZIP.",
     LABEL_STYLE, ROLE_SUBTITLE},
    {ID_UI_EN, L"BUTTON", L"English", BUTTON_STYLE, ROLE_LANG},
    {ID_UI_HU, L"BUTTON", L"Magyar", BUTTON_STYLE, ROLE_LANG},
    {ID_SOURCE_HINT, L"STATIC", L"Drop or paste a ZIP path", LABEL_STYLE | SS_RIGHT, ROLE_HINT},
    {ID_SOURCE_LABEL, L"STATIC", L"Original game ZIP", LABEL_STYLE, ROLE_LABEL},
    {ID_SOURCE, L"EDIT", L"", SOURCE_STYLE, ROLE_FIELD},
    {ID_PICK_SOURCE, L"BUTTON", L"Browse ZIP...", BUTTON_STYLE, ROLE_BROWSE},
    {ID_DESTINATION_LABEL, L"STATIC", L"Save unsigned IPA to", LABEL_STYLE, ROLE_LABEL},
    {ID_DESTINATION, L"EDIT", L"", FIELD_STYLE, ROLE_FIELD},
    {ID_PICK_DESTINATION, L"BUTTON", L"Save As...", BUTTON_STYLE, ROLE_BROWSE},
    {ID_PACK_HINT, L"STATIC", L"Original languages are always included", LABEL_STYLE | SS_RIGHT, ROLE_HINT},
    {ID_PACK_LABEL, L"STATIC", L"Additional languages in IPA", LABEL_STYLE, ROLE_LABEL},
    {ID_HU_PACK, L"BUTTON", L"Magyar (Hungarian)", BUTTON_STYLE, ROLE_CHECK},
    {ID_BUILD, L"BUTTON", L"Create IPA", BUTTON_STYLE, ROLE_PRIMARY},
    {ID_CANCEL, L"BUTTON", L"Cancel", BUTTON_STYLE, ROLE_SECONDARY},
    {ID_OPEN_FOLDER, L"BUTTON", L"Open output folder", BUTTON_STYLE, ROLE_SECONDARY},
    {ID_STATE, L"STATIC", L"Ready. The output is unsigned and requires Sideloadly.", LABEL_STYLE, ROLE_STATUS},
    {ID_DETAIL, L"STATIC", L"", LABEL_STYLE, ROLE_DETAIL},
    {ID_DETAILS_TOGGLE, L"BUTTON", L"Show details", BUTTON_STYLE, ROLE_LINK},
    {ID_DIAGNOSTICS, L"EDIT", L"", WS_CHILD | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE |
     ES_AUTOVSCROLL | ES_READONLY, ROLE_LOG},
    {ID_COPY_ERROR, L"BUTTON", L"Copy error", BUTTON_STYLE, ROLE_BROWSE},
    {ID_COPY_LOG, L"BUTTON", L"Copy full log", BUTTON_STYLE, ROLE_BROWSE},
    {ID_SAVE_LOG, L"BUTTON", L"Save log...", BUTTON_STYLE, ROLE_BROWSE},
    {ID_OPEN_TEMP, L"BUTTON", L"Open temp folder", BUTTON_STYLE, ROLE_BROWSE},
    {ID_LOG, L"EDIT", L"", WS_CHILD | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY, ROLE_LOG}
};

static const Row rows[] = {
    {ID_SOURCE_LABEL, ID_SOURCE_HINT, ID_SOURCE, ID_PICK_SOURCE},
    {ID_DESTINATION_LABEL, 0, ID_DESTINATION, ID_PICK_DESTINATION}
};

/* Additional language packs use this one UI/packaging contract. */
typedef struct { int control; const WCHAR *file; const WCHAR *code; } PackOption;
static const PackOption pack_options[] = {
    {ID_HU_PACK, L"private-localizations\\hu.xlf", L"hu"}
};

typedef struct {
    RECT slot[ID_LAST];
    RECT header, card, well[2], status_card, dot, track, details_card, log_well;
} Geometry;

typedef struct { HDC dc, target; HBITMAP bitmap, previous; RECT area; } Canvas;

typedef struct {
    WCHAR python[32768], tool[32768], kit[32768];
    WCHAR source[32768], output[32768], scratch[32768];
    BOOL include_hungarian, overwrite_output;
    HWND window;
    HANDLE job; /* owned and closed by the UI thread */
} Build;

static HWND main_window, hot_control, remembered_focus;
static HWND ctl[ID_LAST];
static HFONT fonts[FONT_COUNT];
static HBRUSH brushes[BRUSH_COUNT];
static Geometry geo;
static UINT ui_dpi = 96;
static int body_height;
static WCHAR kit_path[MAX_PATH * 4], last_output[32768], last_scratch[32768];
static WCHAR approved_overwrite[32768];
static WCHAR last_error[4096], phase_text[192], transport_text[192], pending_detail[512];
static DWORD last_exit_code;
static BOOL diagnostics_dirty, log_truncated;
static HANDLE active_job;
static volatile LONG cancel_requested;
static BOOL running, aborted, log_open, detail_dirty;
static BOOL hu_available, hu_selected = TRUE;
static int interface_language; /* 0 = English, 1 = Magyar */
static LPCWSTR status_key = L"Ready. The output is unsigned and requires Sideloadly.";
static UiState ui_state = STATE_IDLE;

typedef BOOL (WINAPI *DpiAwarenessSetter)(HANDLE);
typedef UINT (WINAPI *WindowDpiGetter)(HWND);
typedef HRESULT (WINAPI *WindowAttributeSetter)(HWND, DWORD, LPCVOID, DWORD);

#define S(value) MulDiv((value), (int)ui_dpi, 96)

static void bind_symbol(void *target, size_t size, HMODULE module, const char *name) {
    FARPROC address = module ? GetProcAddress(module, name) : NULL;
    memcpy(target, &address, size);
}

/* Text stays in one table; future GUI locales only extend this mapping. */
typedef struct { LPCWSTR english, hungarian; } UiText;
static const UiText ui_texts[] = {
    {L"Build an unsigned iPhone IPA from your own original 1.8.1g ZIP.", L"K\u00e9sz\u00edts al\u00e1\u00edratlan iPhone IPA-t az eredeti 1.8.1g ZIP-b\u0151l."},
    {L"Drop or paste a ZIP path", L"ZIP beh\u00faz\u00e1sa vagy \u00fatvonal beilleszt\u00e9se"},
    {L"Original game ZIP", L"Eredeti j\u00e1t\u00e9k ZIP-f\u00e1jl"},
    {L"Browse ZIP...", L"ZIP kiv\u00e1laszt\u00e1sa..."},
    {L"Save unsigned IPA to", L"IPA ment\u00e9si helye"},
    {L"Save As...", L"Ment\u00e9s m\u00e1sk\u00e9nt..."},
    {L"Original languages are always included", L"Az eredeti nyelvek mindig el\u00e9rhet\u0151k"},
    {L"Additional languages in IPA", L"Tov\u00e1bbi nyelvek az IPA-ban"},
    {L"Magyar (Hungarian)", L"Magyar"},
    {L"Create IPA", L"IPA k\u00e9sz\u00edt\u00e9se"},
    {L"Cancel", L"Megszak\u00edt\u00e1s"},
    {L"Open output folder", L"Kimeneti mappa megnyit\u00e1sa"},
    {L"Show details", L"R\u00e9szletek"},
    {L"Hide details", L"R\u00e9szletek elrejt\u00e9se"},
    {L"Copy error", L"Hiba m\u00e1sol\u00e1sa"},
    {L"Copy full log", L"Teljes napl\u00f3 m\u00e1sol\u00e1sa"},
    {L"Save log...", L"Napl\u00f3 ment\u00e9se..."},
    {L"Open temp folder", L"Ideiglenes mappa megnyit\u00e1sa"},
    {L"Ready. The output is unsigned and requires Sideloadly.", L"K\u00e9sz. Az IPA al\u00e1\u00edr\u00e1s\u00e1hoz Sideloadly sz\u00fcks\u00e9ges."},
    {L"Building native ARM64 IPA. Keep this window open.", L"Nat\u00edv ARM64 IPA k\u00e9sz\u00fcl. Hagyd nyitva az ablakot."},
    {L"Could not start build thread.", L"Nem siker\u00fclt elind\u00edtani az \u00f6ssze\u00e1ll\u00edt\u00e1st."},
    {L"Cancelling...", L"Megszak\u00edt\u00e1s folyamatban..."},
    {L"Unsigned IPA created. Sign with Sideloadly before installing.", L"Az IPA elk\u00e9sz\u00fclt. Telep\u00edt\u00e9s el\u0151tt \u00edrd al\u00e1 a Sideloadlyban."},
    {L"Build cancelled.", L"Az \u00f6ssze\u00e1ll\u00edt\u00e1s megszak\u00edtva."},
    {L"Build failed. Review the diagnostic details below.", L"Az \u00f6ssze\u00e1ll\u00edt\u00e1s hib\u00e1ra futott. N\u00e9zd meg a r\u00e9szleteket."},
    {L"Choose your original ZIP...", L"V\u00e1laszd ki az eredeti ZIP-et..."},
    {L"Choose where to save...", L"V\u00e1lassz ment\u00e9si helyet..."},
    {L"Save unsigned IPA", L"Al\u00e1\u00edratlan IPA ment\u00e9se"},
    {L"Choose original Minion Rush 1.8.1g ZIP", L"Az eredeti Minion Rush 1.8.1g ZIP kiv\u00e1laszt\u00e1sa"},
    {L"Unsigned iOS package (*.ipa)", L"Al\u00e1\u00edratlan iOS-csomag (*.ipa)"},
    {L"Original ZIP (*.zip)", L"Eredeti ZIP (*.zip)"},
    {L"Select an existing original .zip file.", L"V\u00e1lassz ki egy l\u00e9tez\u0151 eredeti .zip f\u00e1jlt."},
    {L"Source ZIP required", L"Eredeti ZIP sz\u00fcks\u00e9ges"},
    {L"Choose an output .ipa path.", L"Add meg az IPA ment\u00e9si hely\u00e9t."},
    {L"Output required", L"Ment\u00e9si hely sz\u00fcks\u00e9ges"},
    {L"The output filename must end in .ipa.", L"A kimeneti f\u00e1jl nev\u00e9nek .ipa v\u00e9gz\u0151d\u00e9s\u0171nek kell lennie."},
    {L"Invalid filename", L"\u00c9rv\u00e9nytelen f\u00e1jln\u00e9v"},
    {L"This file already exists. Replace it with the newly built IPA?",
     L"Ez a f\u00e1jl m\u00e1r l\u00e9tezik. Fel\u00fcl\u00edrod az \u00faj IPA-val?"},
    {L"The selected output is not a regular file.", L"A c\u00e9l nem hagyom\u00e1nyos f\u00e1jl."},
    {L"Existing output", L"L\u00e9tez\u0151 kimeneti f\u00e1jl"},
    {L"The bundled Hungarian language file is missing.", L"Hi\u00e1nyzik a be\u00e9p\u00edtett magyar nyelvi f\u00e1jl."},
    {L"Missing translation", L"Hi\u00e1nyz\u00f3 ford\u00edt\u00e1s"},
    {L"The portable runtime is incomplete. Extract the full release ZIP.", L"A hordozhat\u00f3 futtat\u00f3k\u00f6rnyezet hi\u00e1nyos."},
    {L"Missing application files", L"Hi\u00e1nyz\u00f3 alkalmaz\u00e1sf\u00e1jlok"},
    {L"Cannot create a private temporary workspace.", L"Nem siker\u00fclt l\u00e9trehozni az ideiglenes munkamapp\u00e1t."},
    {L"Temporary storage unavailable", L"Az ideiglenes t\u00e1rhely nem \u00e9rhet\u0151 el"},
    {L"Unable to protect child process tree.", L"A h\u00e1tt\u00e9rfolyamatok v\u00e9delme sikertelen."},
    {L"Build cannot start", L"Nem ind\u00edthat\u00f3 az \u00f6ssze\u00e1ll\u00edt\u00e1s"},
    {L"Stop creating the IPA? The unfinished temporary files may need to be removed.", L"Megszak\u00edtod az IPA k\u00e9sz\u00edt\u00e9s\u00e9t? A f\u00e9lk\u00e9sz f\u00e1jlokat esetleg t\u00f6r\u00f6lni kell."},
    {L"Cancel build", L"\u00d6ssze\u00e1ll\u00edt\u00e1s megszak\u00edt\u00e1sa"},
    {L"Unsigned IPA created successfully. It is ready for signing with Sideloadly.", L"Az IPA sikeresen elk\u00e9sz\u00fclt. Al\u00e1\u00edrhatod a Sideloadly alkalmaz\u00e1sban."},
    {L"Build complete", L"\u00d6ssze\u00e1ll\u00edt\u00e1s k\u00e9sz"},
    {L"Drop exactly one original .zip file.", L"Pontosan egy eredeti .zip f\u00e1jlt h\u00fazz ide."},
    {L"ZIP required", L"ZIP sz\u00fcks\u00e9ges"},
    {L"Could not copy the report to the clipboard.", L"Nem siker\u00fclt a v\u00e1g\u00f3lapra m\u00e1solni a jelent\u00e9st."},
    {L"Clipboard unavailable", L"A v\u00e1g\u00f3lap nem \u00e9rhet\u0151 el"},
    {L"The diagnostic log could not be saved.", L"Nem siker\u00fclt menteni a napl\u00f3t."},
    {L"Save failed", L"A ment\u00e9s sikertelen"},
    {L"Save local build diagnostics", L"\u00d6ssze\u00e1ll\u00edt\u00e1si napl\u00f3 ment\u00e9se"},
    {L"Text log (*.txt)", L"Sz\u00f6veges napl\u00f3 (*.txt)"},
    {L"Running", L"Folyamatban"},
    {L"Completed", L"Befejezve"},
    {L"Failed", L"Hiba"},
    {L"Cancelled", L"Megszak\u00edtva"},
    {L"Ready", L"K\u00e9sz"},
    {L"Not started", L"M\u00e9g nem indult el"},
    {L"Not reported", L"Nincs adat"},
    {L"None reported", L"Nincs hiba"},
    {L"English", L"English"},
    {L"Magyar", L"Magyar"},
};

static LPCWSTR tr(LPCWSTR english) {
    if (!interface_language || !english) return english;
    for (size_t i = 0; i < sizeof(ui_texts) / sizeof(ui_texts[0]); i++)
        if (wcscmp(english, ui_texts[i].english) == 0) return ui_texts[i].hungarian;
    return english;
}

static void relayout(void);

static void update_interface(void) {
#ifdef MR_UTM_TEST
    SetWindowTextW(main_window, interface_language ?
        L"Minion Rush IPA-k\u00e9sz\u00edt\u0151 - UTM teszt" :
        L"Minion Rush IPA Builder - UTM test build");
#else
    SetWindowTextW(main_window, interface_language ?
        L"Minion Rush IPA-k\u00e9sz\u00edt\u0151" : L"Minion Rush IPA Builder");
#endif
    for (size_t i = 0; i < sizeof(specs) / sizeof(specs[0]); i++) {
        int id = specs[i].id;
        if (id == ID_SOURCE || id == ID_DESTINATION || id == ID_STATE ||
            id == ID_DETAIL || id == ID_DIAGNOSTICS || id == ID_LOG ||
            id == ID_DETAILS_TOGGLE) continue;
        SetWindowTextW(ctl[id], tr(specs[i].text));
    }
    SetWindowTextW(ctl[ID_DETAILS_TOGGLE], tr(log_open ? L"Hide details" : L"Show details"));
    SetWindowTextW(ctl[ID_STATE], tr(status_key));
    InvalidateRect(ctl[ID_UI_EN], NULL, FALSE);
    InvalidateRect(ctl[ID_UI_HU], NULL, FALSE);
    InvalidateRect(ctl[ID_HU_PACK], NULL, FALSE);
    relayout();
}

/* ---- Small utilities ------------------------------------------------ */

static Role role_of(int id) {
    for (size_t index = 0; index < sizeof(specs) / sizeof(specs[0]); index++)
        if (specs[index].id == id) return specs[index].role;
    return ROLE_LABEL;
}

static BOOL is_button_role(Role role) {
    return role == ROLE_PRIMARY || role == ROLE_SECONDARY || role == ROLE_BROWSE ||
           role == ROLE_LINK || role == ROLE_LANG || role == ROLE_CHECK;
}

static HFONT font_for(Role role) {
    switch (role) {
    case ROLE_TITLE: return fonts[FONT_TITLE];
    case ROLE_LABEL: case ROLE_PRIMARY: case ROLE_SECONDARY:
    case ROLE_BROWSE: case ROLE_LINK: case ROLE_LANG: case ROLE_CHECK: return fonts[FONT_LABEL];
    case ROLE_HINT: case ROLE_DETAIL: return fonts[FONT_SMALL];
    case ROLE_STATUS: return fonts[FONT_STATUS];
    case ROLE_LOG: return fonts[FONT_MONO];
    default: return fonts[FONT_BODY];
    }
}

static RECT make_rect(int left, int top, int right, int bottom) {
    RECT rect;
    rect.left = left; rect.top = top; rect.right = right; rect.bottom = bottom;
    return rect;
}

static BOOL get_text(HWND field_window, WCHAR *buffer, size_t capacity) {
    return GetWindowTextW(field_window, buffer, (int)capacity) > 0;
}

/* Show the end of a long path, where the file name is. */
static void set_path(HWND field_window, LPCWSTR text) {
    SetWindowTextW(field_window, text);
    int length = GetWindowTextLengthW(field_window);
    SendMessageW(field_window, EM_SETSEL, (WPARAM)length, (LPARAM)length);
    SendMessageW(field_window, EM_SCROLLCARET, 0, 0);
}

static BOOL is_zip(LPCWSTR filename) {
    size_t n = wcslen(filename);
    return n >= 4 && _wcsicmp(filename + n - 4, L".zip") == 0;
}

/* Query directory metadata only. Windows WebDAV rejects opening large ZIPs,
   but FindFirstFileW can retrieve their names without downloading them. */
static BOOL regular_file(LPCWSTR file) {
    DWORD attrs = GetFileAttributesW(file);
    if (attrs != INVALID_FILE_ATTRIBUTES)
        return (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
    WIN32_FIND_DATAW entry;
    HANDLE found = FindFirstFileW(file, &entry);
    if (found == INVALID_HANDLE_VALUE) return FALSE;
    FindClose(found);
    const WCHAR *slash = wcsrchr(file, L'\\');
    const WCHAR *name = slash ? slash + 1 : file;
    return _wcsicmp(name, entry.cFileName) == 0 &&
           (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

#ifdef MR_UTM_TEST
/* The private original ZIP is normally next to the extracted GUI directory
   in UTM's shared folder. Look it up by version, without opening the ZIP via
   the WebDAV-backed shell file picker. Only prefill when unambiguous. */
static BOOL nearby_original_zip(WCHAR *path, size_t capacity) {
    WCHAR pattern[32768];
    WCHAR source_root[32768] = {0};
    DWORD from_parent = GetEnvironmentVariableW(L"MR_SOURCE_DIR", source_root, 32768);
    LPCWSTR search_root = (from_parent > 0 && from_parent < 32768)
                           ? source_root : kit_path;
    const WCHAR *end = wcsrchr(search_root, L'\\');
    size_t prefix = (from_parent > 0 && from_parent < 32768)
                    ? wcslen(search_root)
                    : (end ? (size_t)(end - search_root) : 0);
    if (!prefix) return FALSE;
    if (FAILED(StringCchPrintfW(pattern, 32768, L"%.*s\\*1.8.1g*.zip",
                                (int)prefix, search_root)))
        return FALSE;
    WIN32_FIND_DATAW entry;
    HANDLE search = FindFirstFileW(pattern, &entry);
    if (search == INVALID_HANDLE_VALUE) return FALSE;
    BOOL found_one = FALSE, ambiguous = FALSE;
    do {
        if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            !is_zip(entry.cFileName))
            continue;
        if (found_one) { ambiguous = TRUE; break; }
        found_one = SUCCEEDED(StringCchPrintfW(
            path, capacity, L"%.*s\\%s", (int)prefix, search_root, entry.cFileName));
    } while (FindNextFileW(search, &entry));
    FindClose(search);
    return found_one && !ambiguous;
}

#endif

static void unquote_path(WCHAR *path) {
    size_t length = wcslen(path);
    if (length >= 2 && path[0] == L'"' && path[length - 1] == L'"') {
        memmove(path, path + 1, (length - 1) * sizeof(WCHAR));
        path[length - 2] = 0;
    }
}

static int text_width(Role role, LPCWSTR text) {
    HDC dc = GetDC(main_window);
    HFONT previous = (HFONT)SelectObject(dc, font_for(role));
    SIZE extent = {0, 0};
    GetTextExtentPoint32W(dc, text, (int)wcslen(text), &extent);
    SelectObject(dc, previous);
    ReleaseDC(main_window, dc);
    return extent.cx;
}

static int button_width(int id, int minimum) {
    WCHAR caption[64] = {0};
    GetWindowTextW(ctl[id], caption, 64);
    int wanted = text_width(role_of(id), caption) + S(40);
    return wanted > S(minimum) ? wanted : S(minimum);
}

/* ---- GDI drawing primitives ---------------------------------------- */

static void solid(HDC dc, int left, int top, int right, int bottom, COLORREF color) {
    if (right <= left || bottom <= top) return;
    RECT rect = make_rect(left, top, right, bottom);
    SetBkColor(dc, color);
    ExtTextOutW(dc, 0, 0, ETO_OPAQUE, &rect, NULL, 0, NULL);
}

static COLORREF blend(COLORREF below, COLORREF above, double alpha) {
    int red = (int)(GetRValue(below) + (GetRValue(above) - GetRValue(below)) * alpha + 0.5);
    int green = (int)(GetGValue(below) + (GetGValue(above) - GetGValue(below)) * alpha + 0.5);
    int blue = (int)(GetBValue(below) + (GetBValue(above) - GetBValue(below)) * alpha + 0.5);
    return RGB(red, green, blue);
}

static double coverage(double distance, double radius) {
    double value = radius - distance + 0.5;
    return value < 0.0 ? 0.0 : value > 1.0 ? 1.0 : value;
}

/* Rounded rectangle with anti-aliased corners. Every surface in this UI sits
   on a flat colour, so corners are blended against that backdrop. */
static void round_box(HDC dc, RECT box, int radius, COLORREF backdrop,
                      COLORREF fill, COLORREF edge, int edge_width) {
    int width = box.right - box.left, height = box.bottom - box.top;
    if (radius * 2 > width) radius = width / 2;
    if (radius * 2 > height) radius = height / 2;
    if (edge_width < 0) edge_width = 0;
    if (edge_width > 0) {
        solid(dc, box.left + radius, box.top, box.right - radius, box.bottom, edge);
        solid(dc, box.left, box.top + radius, box.right, box.bottom - radius, edge);
    }
    solid(dc, box.left + radius, box.top + edge_width, box.right - radius, box.bottom - edge_width, fill);
    solid(dc, box.left + edge_width, box.top + radius, box.left + radius, box.bottom - radius, fill);
    solid(dc, box.right - radius, box.top + radius, box.right - edge_width, box.bottom - radius, fill);
    for (int corner = 0; corner < 4; corner++) {
        int right_side = corner & 1, bottom_side = corner >> 1;
        int x0 = right_side ? box.right - radius : box.left;
        int y0 = bottom_side ? box.bottom - radius : box.top;
        double cx = right_side ? box.right - radius : box.left + radius;
        double cy = bottom_side ? box.bottom - radius : box.top + radius;
        for (int y = y0; y < y0 + radius; y++) {
            for (int x = x0; x < x0 + radius; x++) {
                double dx = x + 0.5 - cx, dy = y + 0.5 - cy;
                double distance = sqrt(dx * dx + dy * dy);
                COLORREF color = blend(backdrop, edge_width > 0 ? edge : fill,
                                       coverage(distance, radius));
                if (edge_width > 0)
                    color = blend(color, fill, coverage(distance, radius - edge_width));
                SetPixelV(dc, x, y, color);
            }
        }
    }
}

/* Double-buffered drawing in the target's own coordinates. */
static void canvas_open(Canvas *canvas, HDC target, RECT area) {
    int width = area.right - area.left, height = area.bottom - area.top;
    canvas->target = target;
    canvas->area = area;
    canvas->bitmap = NULL;
    canvas->previous = NULL;
    canvas->dc = CreateCompatibleDC(target);
    if (canvas->dc) canvas->bitmap = CreateCompatibleBitmap(target, width, height);
    if (!canvas->dc || !canvas->bitmap) {
        if (canvas->bitmap) DeleteObject(canvas->bitmap);
        if (canvas->dc) DeleteDC(canvas->dc);
        canvas->bitmap = NULL;
        canvas->dc = target; /* unbuffered fallback */
        return;
    }
    canvas->previous = (HBITMAP)SelectObject(canvas->dc, canvas->bitmap);
    SetViewportOrgEx(canvas->dc, -area.left, -area.top, NULL);
}

static void canvas_close(Canvas *canvas) {
    if (!canvas->bitmap) return;
    BitBlt(canvas->target, canvas->area.left, canvas->area.top,
           canvas->area.right - canvas->area.left, canvas->area.bottom - canvas->area.top,
           canvas->dc, canvas->area.left, canvas->area.top, SRCCOPY);
    SelectObject(canvas->dc, canvas->previous);
    DeleteObject(canvas->bitmap);
    DeleteDC(canvas->dc);
}

/* ---- Geometry ------------------------------------------------------- */

static void place_slot(Geometry *g, int id, int left, int top, int width, int height) {
    g->slot[id] = make_rect(left, top, left + width, top + height);
}

/* Fills g for a client area and returns the natural (log hidden) height. */
static int compute_geometry(Geometry *g, int width, int height) {
    memset(g, 0, sizeof(*g));
    int margin = S(28), pad = S(20), field_height = S(36), label_height = S(20);
    int row_gap = S(12), inner = width - 2 * margin;
    if (inner < S(320)) inner = S(320);
    int right = margin + inner;
    g->header = make_rect(0, 0, width, S(88));
    place_slot(g, ID_TITLE, margin, S(14), inner - S(160), S(38));
    place_slot(g, ID_SUBTITLE, margin, S(54), inner, S(22));
    place_slot(g, ID_UI_EN, right - S(158), S(16), S(74), S(30));
    place_slot(g, ID_UI_HU, right - S(78), S(16), S(74), S(30));

    int row_height = label_height + S(4) + field_height;
    int card_top = g->header.bottom + S(20);
    int card_height = 2 * S(18) + 3 * row_height + 2 * row_gap;
    g->card = make_rect(margin, card_top, right, card_top + card_height);
    int pick_width = button_width(ID_PICK_SOURCE, 112);
    if (button_width(ID_PICK_DESTINATION, 112) > pick_width) pick_width = button_width(ID_PICK_DESTINATION, 112);

    int card_inner = inner - 2 * pad, label_width = card_inner * 45 / 100;
    for (size_t index = 0; index < sizeof(rows) / sizeof(rows[0]); index++) {
        int top = card_top + S(18) + (int)index * (row_height + row_gap);
        int field_top = top + label_height + S(4);
        int left = margin + pad;
        place_slot(g, rows[index].label, left, top, label_width, label_height);
        if (rows[index].hint)
            place_slot(g, rows[index].hint, left + label_width, top, card_inner - label_width, label_height);
        g->well[index] = make_rect(left, field_top, right - pad - pick_width - S(10), field_top + field_height);
        place_slot(g, rows[index].pick, right - pad - pick_width, field_top, pick_width, field_height);
        int edit_left = g->well[index].left + S(12);
        int edit_width = g->well[index].right - S(12) - edit_left;
        place_slot(g, rows[index].field, edit_left, field_top + (field_height - body_height) / 2,
             edit_width, body_height);
    }

    int option_top = card_top + S(18) + 2 * (row_height + row_gap);
    place_slot(g, ID_PACK_LABEL, margin + pad, option_top, label_width, label_height);
    place_slot(g, ID_PACK_HINT, margin + pad + label_width, option_top,
               card_inner - label_width, label_height);
    place_slot(g, ID_HU_PACK, margin + pad, option_top + label_height + S(4),
               card_inner, field_height);

    int actions_top = g->card.bottom + S(16), actions_height = S(40);
    int build_width = button_width(ID_BUILD, 132), cancel_width = button_width(ID_CANCEL, 96);
    int open_width = button_width(ID_OPEN_FOLDER, 150);
    place_slot(g, ID_BUILD, margin, actions_top, build_width, actions_height);
    place_slot(g, ID_CANCEL, margin + build_width + S(10), actions_top, cancel_width, actions_height);
    place_slot(g, ID_OPEN_FOLDER, right - open_width, actions_top, open_width, actions_height);

    int status_top = actions_top + actions_height + S(16), status_pad = S(18);
    int line_top = status_top + S(16), dot_size = S(10);
    int toggle_width = text_width(ROLE_LINK, tr(L"Show details"));
    if (text_width(ROLE_LINK, tr(L"Hide details")) > toggle_width)
        toggle_width = text_width(ROLE_LINK, tr(L"Hide details"));
    toggle_width += S(28);
    int dot_left = margin + status_pad;
    g->dot = make_rect(dot_left, line_top + (S(24) - dot_size) / 2,
                       dot_left + dot_size, line_top + (S(24) - dot_size) / 2 + dot_size);
    int state_left = g->dot.right + S(10);
    place_slot(g, ID_DETAILS_TOGGLE, right - status_pad - toggle_width, line_top - S(2), toggle_width, S(28));
    place_slot(g, ID_STATE, state_left, line_top, g->slot[ID_DETAILS_TOGGLE].left - S(8) - state_left, S(24));
    g->track = make_rect(dot_left, line_top + S(24) + S(12), right - status_pad, line_top + S(24) + S(12) + S(6));
    place_slot(g, ID_DETAIL, dot_left, g->track.bottom + S(10), right - status_pad - dot_left, S(20));
    g->status_card = make_rect(margin, status_top, right, g->slot[ID_DETAIL].bottom + S(16));
    int natural = g->status_card.bottom + margin;

    if (log_open) {
        int top = g->status_card.bottom + S(14);
        int inner_left = margin + S(14), inner_right = right - S(14);
        int diag_top = top + S(14);
        place_slot(g, ID_DIAGNOSTICS, inner_left, diag_top,
                   inner_right - inner_left, S(126));
        int toolbar_top = diag_top + S(136);
        int x = inner_left;
        const int actions[] = {ID_COPY_ERROR, ID_COPY_LOG, ID_SAVE_LOG, ID_OPEN_TEMP};
        for (size_t i = 0; i < sizeof(actions) / sizeof(actions[0]); i++) {
            int button = button_width(actions[i], 92);
            place_slot(g, actions[i], x, toolbar_top, button, S(34));
            x += button + S(8);
        }
        int log_top = toolbar_top + S(46);
        int log_bottom = height - margin - S(14);
        if (log_bottom < log_top + S(60)) log_bottom = log_top + S(60);
        g->details_card = make_rect(margin, top, right, log_bottom + S(14));
        g->log_well = make_rect(inner_left, log_top, inner_right, log_bottom);
        g->slot[ID_LOG] = make_rect(inner_left + S(10), log_top + S(8),
                                   inner_right - S(8), log_bottom - S(8));
    }
    return natural;
}

static void relayout(void) {
    RECT client;
    GetClientRect(main_window, &client);
    compute_geometry(&geo, client.right, client.bottom);
    HDWP batch = BeginDeferWindowPos(ID_LAST);
    for (int id = ID_TITLE; id < ID_LAST && batch; id++) {
        RECT box = geo.slot[id];
        if (box.right <= box.left) continue;
        batch = DeferWindowPos(batch, ctl[id], NULL, box.left, box.top, box.right - box.left,
                               box.bottom - box.top, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (batch) EndDeferWindowPos(batch);
    InvalidateRect(main_window, NULL, FALSE);
}

static void window_frame(int *extra_width, int *extra_height) {
    RECT outer, client;
    GetWindowRect(main_window, &outer);
    GetClientRect(main_window, &client);
    *extra_width = (outer.right - outer.left) - client.right;
    *extra_height = (outer.bottom - outer.top) - client.bottom;
}

static RECT work_area(void) {
    MONITORINFO monitor;
    monitor.cbSize = sizeof(monitor);
    GetMonitorInfoW(MonitorFromWindow(main_window, MONITOR_DEFAULTTONEAREST), &monitor);
    return monitor.rcWork;
}

/* Resizes to a client size, kept inside the work area. */
static void resize_client(int client_width, int client_height, BOOL centre) {
    RECT outer, area = work_area();
    int extra_width, extra_height;
    GetWindowRect(main_window, &outer);
    window_frame(&extra_width, &extra_height);
    int width = client_width < 0 ? outer.right - outer.left : client_width + extra_width;
    int height = client_height + extra_height;
    if (width > area.right - area.left) width = area.right - area.left;
    if (height > area.bottom - area.top) height = area.bottom - area.top;
    int left = centre ? area.left + (area.right - area.left - width) / 2 : outer.left;
    int top = centre ? area.top + (area.bottom - area.top - height) / 3 : outer.top;
    if (left + width > area.right) left = area.right - width;
    if (top + height > area.bottom) top = area.bottom - height;
    if (left < area.left) left = area.left;
    if (top < area.top) top = area.top;
    SetWindowPos(main_window, NULL, left, top, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
}

#define LOG_MINIMUM 60
#define LOG_PREFERRED 205
#define DETAILS_EXTRA (14 + 14 + 126 + 10 + 34 + 12 + 14)

static void set_log_visible(BOOL visible) {
    if (log_open == visible) return;
    log_open = visible;
    SetWindowTextW(ctl[ID_DETAILS_TOGGLE], tr(visible ? L"Hide details" : L"Show details"));
    if (!visible && (GetFocus() == ctl[ID_LOG] || GetFocus() == ctl[ID_DIAGNOSTICS] ||
                      GetFocus() == ctl[ID_COPY_ERROR] || GetFocus() == ctl[ID_COPY_LOG] ||
                      GetFocus() == ctl[ID_SAVE_LOG] || GetFocus() == ctl[ID_OPEN_TEMP]))
        SetFocus(ctl[ID_DETAILS_TOGGLE]);
    const int details[] = {ID_DIAGNOSTICS, ID_COPY_ERROR, ID_COPY_LOG,
                           ID_SAVE_LOG, ID_OPEN_TEMP};
    for (size_t i = 0; i < sizeof(details) / sizeof(details[0]); i++)
        ShowWindow(ctl[details[i]], visible ? SW_SHOW : SW_HIDE);
    ShowWindow(ctl[ID_LOG], visible ? SW_SHOW : SW_HIDE);
    if (!IsZoomed(main_window)) {
        RECT client;
        Geometry probe;
        GetClientRect(main_window, &client);
        int natural = compute_geometry(&probe, client.right, 0);
        resize_client(-1, visible ? natural + S(DETAILS_EXTRA + LOG_PREFERRED) : natural, FALSE);
    }
    relayout();
    diagnostics_dirty = TRUE;
}

/* ---- Painting ------------------------------------------------------- */

static COLORREF state_color(void) {
    switch (ui_state) {
    case STATE_BUSY: return COL_GOLD;
    case STATE_OK: return COL_GOOD;
    case STATE_FAILED: return COL_BAD;
    default: return COL_IDLE;
    }
}

static void paint_progress(HDC dc) {
    int height = geo.track.bottom - geo.track.top;
    round_box(dc, geo.track, height / 2, COL_CARD, COL_TRACK, COL_TRACK, 0);
    if (ui_state == STATE_OK || ui_state == STATE_FAILED) {
        round_box(dc, geo.track, height / 2, COL_TRACK, state_color(), state_color(), 0);
    } else if (ui_state == STATE_BUSY) {
        int width = geo.track.right - geo.track.left, length = width * 30 / 100;
        double phase = (double)(GetTickCount64() % 1600) / 1600.0;
        int start = geo.track.left - length + (int)(phase * (width + length));
        int left = start < geo.track.left ? geo.track.left : start;
        int right = start + length > geo.track.right ? geo.track.right : start + length;
        if (right - left >= 2)
            round_box(dc, make_rect(left, geo.track.top, right, geo.track.bottom), height / 2,
                      COL_TRACK, COL_GOLD, COL_GOLD, 0);
    }
}

static void paint_scene(HDC dc) {
    RECT client;
    GetClientRect(main_window, &client);
    solid(dc, 0, 0, client.right, client.bottom, COL_BACKGROUND);
    solid(dc, 0, geo.header.top, geo.header.right, geo.header.bottom, COL_NAVY);
    solid(dc, 0, geo.header.bottom - S(3), geo.header.right, geo.header.bottom, COL_GOLD);
    round_box(dc, geo.card, S(12), COL_BACKGROUND, COL_CARD, COL_CARD_EDGE, 1);
    for (size_t index = 0; index < sizeof(rows) / sizeof(rows[0]); index++) {
        HWND field_window = ctl[rows[index].field];
        BOOL focused = GetFocus() == field_window;
        round_box(dc, geo.well[index], S(8), COL_CARD,
                  IsWindowEnabled(field_window) ? COL_FIELD : COL_FIELD_OFF,
                  focused ? COL_NAVY : COL_FIELD_EDGE, focused ? 2 : 1);
    }
    round_box(dc, geo.status_card, S(12), COL_BACKGROUND, COL_CARD, COL_CARD_EDGE, 1);
    round_box(dc, geo.dot, S(5), COL_CARD, state_color(), state_color(), 0);
    paint_progress(dc);
    if (log_open) {
        round_box(dc, geo.details_card, S(12), COL_BACKGROUND, COL_CARD, COL_CARD_EDGE, 1);
        round_box(dc, geo.log_well, S(8), COL_CARD, COL_FIELD, COL_FIELD_EDGE, 1);
    }
}

static void paint_window(HWND window) {
    PAINTSTRUCT paint;
    HDC dc = BeginPaint(window, &paint);
    Canvas canvas;
    canvas_open(&canvas, dc, paint.rcPaint);
    paint_scene(canvas.dc);
    canvas_close(&canvas);
    EndPaint(window, &paint);
}

static void draw_button(const DRAWITEMSTRUCT *item) {
    Role role = role_of((int)item->CtlID);
    BOOL disabled = (item->itemState & ODS_DISABLED) != 0;
    BOOL pressed = (item->itemState & ODS_SELECTED) != 0;
    BOOL focused = (item->itemState & ODS_FOCUS) != 0;
    BOOL hot = hot_control == item->hwndItem && !disabled;
    BOOL checked = role == ROLE_CHECK && hu_selected;
    BOOL chosen = role == ROLE_LANG &&
                  ((item->CtlID == ID_UI_HU) == (interface_language == 1));
    COLORREF backdrop = (role == ROLE_BROWSE || role == ROLE_LINK || role == ROLE_CHECK)
                        ? COL_CARD : role == ROLE_LANG ? COL_NAVY : COL_BACKGROUND;
    COLORREF fill = COL_CARD, edge = COL_FIELD_EDGE, text = COL_TEXT;
    if (role == ROLE_LANG) {
        fill = chosen ? COL_GOLD : COL_NAVY;
        edge = chosen ? COL_GOLD : COL_NAVY_MUTED;
        text = chosen ? COL_NAVY : RGB(255, 255, 255);
    } else if (role == ROLE_CHECK) {
        fill = edge = COL_CARD;
    } else if (role == ROLE_PRIMARY) {
        fill = pressed ? COL_GOLD_DOWN : hot ? COL_GOLD_HOT : COL_GOLD;
        edge = fill;
        text = COL_NAVY;
    } else if (role == ROLE_LINK) {
        fill = pressed ? COL_PRESSED : hot ? COL_HOVER : backdrop;
        edge = fill;
    } else {
        fill = pressed ? COL_PRESSED : hot ? COL_HOVER : COL_CARD;
        edge = hot ? COL_EDGE_HOT : COL_FIELD_EDGE;
    }
    if (disabled && role != ROLE_CHECK) {
        fill = edge = COL_FIELD_OFF;
        text = COL_DISABLED;
    }
    int edge_width = 1;
    if (focused) {
        edge = COL_NAVY;
        edge_width = 2;
    }
    Canvas canvas;
    canvas_open(&canvas, item->hDC, item->rcItem);
    solid(canvas.dc, item->rcItem.left, item->rcItem.top, item->rcItem.right, item->rcItem.bottom, backdrop);
    if (role == ROLE_CHECK) {
        RECT square = make_rect(item->rcItem.left + S(2), item->rcItem.top + S(7),
                                item->rcItem.left + S(24), item->rcItem.top + S(29));
        COLORREF mark = checked ? COL_GOOD : COL_CARD;
        round_box(canvas.dc, square, S(5), COL_CARD, mark,
                  focused ? COL_NAVY : checked ? COL_GOOD : COL_FIELD_EDGE, focused ? 2 : 1);
    } else {
        round_box(canvas.dc, item->rcItem, S(8), backdrop, fill, edge, edge_width);
    }
    WCHAR caption[64] = {0};
    GetWindowTextW(item->hwndItem, caption, 64);
    HFONT previous = (HFONT)SelectObject(canvas.dc, font_for(role));
    SetBkMode(canvas.dc, TRANSPARENT);
    SetTextColor(canvas.dc, text);
    RECT label = item->rcItem;
    if (pressed) OffsetRect(&label, 0, 1);
    if (role == ROLE_CHECK) label.left += S(38);
    DrawTextW(canvas.dc, caption, -1, &label,
              DT_SINGLELINE | DT_VCENTER |
              (role == ROLE_CHECK ? DT_LEFT : DT_CENTER) | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(canvas.dc, previous);
    canvas_close(&canvas);
}

static void paint_placeholder(HWND field_window, int id) {
    if (GetWindowTextLengthW(field_window) > 0) return;
    LPCWSTR hint = id == ID_SOURCE ? L"Choose your original ZIP..." :
                   id == ID_DESTINATION ? L"Choose where to save..." : NULL;
    if (!hint) return;
    HDC dc = GetDC(field_window);
    RECT area;
    GetClientRect(field_window, &area);
    HFONT previous = (HFONT)SelectObject(dc, fonts[FONT_BODY]);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, COL_MUTED);
    DrawTextW(dc, tr(hint), -1, &area, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, previous);
    ReleaseDC(field_window, dc);
}

static LRESULT control_colors(HDC dc, HWND control) {
    int id = GetDlgCtrlID(control);
    Role role = role_of(id);
    COLORREF color = COL_TEXT, text = COL_TEXT;
    BrushSlot brush_slot = BRUSH_CARD;
    switch (role) {
    case ROLE_TITLE: text = RGB(255, 255, 255); color = COL_NAVY; brush_slot = BRUSH_NAVY; break;
    case ROLE_SUBTITLE: text = COL_NAVY_MUTED; color = COL_NAVY; brush_slot = BRUSH_NAVY; break;
    case ROLE_HINT: case ROLE_DETAIL: text = COL_MUTED; color = COL_CARD; break;
    case ROLE_FIELD:
        text = IsWindowEnabled(control) ? COL_TEXT : COL_DISABLED;
        color = IsWindowEnabled(control) ? COL_FIELD : COL_FIELD_OFF;
        brush_slot = IsWindowEnabled(control) ? BRUSH_FIELD : BRUSH_FIELD_OFF;
        break;
    case ROLE_LOG: color = COL_FIELD; brush_slot = BRUSH_FIELD; break;
    default: color = COL_CARD; break;
    }
    SetTextColor(dc, text);
    SetBkColor(dc, color);
    return (LRESULT)brushes[brush_slot];
}

/* ---- Fonts, DPI and controls --------------------------------------- */

static HFONT make_font(int points, int weight, LPCWSTR face) {
    return CreateFontW(-MulDiv(points, (int)ui_dpi, 72), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
}

static void rebuild_fonts(void) {
    HFONT previous[FONT_COUNT];
    memcpy(previous, fonts, sizeof(previous));
    fonts[FONT_TITLE] = make_font(20, FW_SEMIBOLD, L"Segoe UI");
    fonts[FONT_BODY] = make_font(10, FW_NORMAL, L"Segoe UI");
    fonts[FONT_LABEL] = make_font(10, FW_SEMIBOLD, L"Segoe UI");
    fonts[FONT_SMALL] = make_font(9, FW_NORMAL, L"Segoe UI");
    fonts[FONT_STATUS] = make_font(11, FW_SEMIBOLD, L"Segoe UI");
    fonts[FONT_MONO] = make_font(9, FW_NORMAL, L"Consolas");
    for (size_t index = 0; index < sizeof(specs) / sizeof(specs[0]); index++)
        if (ctl[specs[index].id])
            SendMessageW(ctl[specs[index].id], WM_SETFONT, (WPARAM)font_for(specs[index].role), TRUE);
    for (int index = 0; index < FONT_COUNT; index++)
        if (previous[index]) DeleteObject(previous[index]);
    HDC dc = GetDC(main_window);
    HFONT old = (HFONT)SelectObject(dc, fonts[FONT_BODY]);
    TEXTMETRICW metrics;
    GetTextMetricsW(dc, &metrics);
    body_height = metrics.tmHeight;
    SelectObject(dc, old);
    ReleaseDC(main_window, dc);
}

static UINT query_dpi(HWND window) {
    WindowDpiGetter getter;
    bind_symbol(&getter, sizeof(getter), GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    if (getter) return getter(window);
    HDC dc = GetDC(window);
    UINT value = (UINT)GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(window, dc);
    return value;
}

static void declare_dpi_awareness(void) {
    DpiAwarenessSetter setter;
    bind_symbol(&setter, sizeof(setter), GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext");
    /* -4 is DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */
    if (setter && setter((HANDLE)(INT_PTR)-4)) return;
    SetProcessDPIAware();
}

/* Windows 11 honours these to colour the title bar like the header. */
static void tint_title_bar(HWND window) {
    HMODULE dwm = LoadLibraryExW(L"dwmapi.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dwm) return;
    WindowAttributeSetter setter;
    bind_symbol(&setter, sizeof(setter), dwm, "DwmSetWindowAttribute");
    if (setter) {
        COLORREF caption = COL_NAVY, border = COL_NAVY, text = RGB(255, 255, 255);
        setter(window, 35, &caption, sizeof(caption));
        setter(window, 34, &border, sizeof(border));
        setter(window, 36, &text, sizeof(text));
    }
    FreeLibrary(dwm);
}

static LRESULT CALLBACK control_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                     UINT_PTR id, DWORD_PTR data) {
    (void)data;
    Role role = role_of((int)id);
    switch (message) {
    case WM_MOUSEMOVE:
        if (is_button_role(role) && hot_control != window) {
            HWND previous = hot_control;
            TRACKMOUSEEVENT track;
            hot_control = window;
            if (previous) InvalidateRect(previous, NULL, FALSE);
            InvalidateRect(window, NULL, FALSE);
            track.cbSize = sizeof(track);
            track.dwFlags = TME_LEAVE;
            track.hwndTrack = window;
            track.dwHoverTime = 0;
            TrackMouseEvent(&track);
        }
        break;
    case WM_MOUSELEAVE:
        if (hot_control == window) {
            hot_control = NULL;
            InvalidateRect(window, NULL, FALSE);
        }
        break;
    case WM_DROPFILES:
        return SendMessageW(main_window, WM_DROPFILES, wparam, lparam);
    case WM_PAINT: {
        LRESULT result = DefSubclassProc(window, message, wparam, lparam);
        if (role == ROLE_FIELD) paint_placeholder(window, (int)id);
        return result;
    }
    case WM_NCDESTROY:
        RemoveWindowSubclass(window, control_proc, id);
        break;
    }
    return DefSubclassProc(window, message, wparam, lparam);
}

static void create_controls(HWND window) {
    for (size_t index = 0; index < sizeof(specs) / sizeof(specs[0]); index++) {
        const Spec *spec = &specs[index];
        HWND created = CreateWindowExW(0, spec->kind, spec->text, spec->style, 0, 0, 10, 10,
                                       window, (HMENU)(INT_PTR)spec->id,
                                       GetModuleHandleW(NULL), NULL);
        ctl[spec->id] = created;
        if (!created) continue;
        SetWindowSubclass(created, control_proc, (UINT_PTR)spec->id, 0);
        DragAcceptFiles(created, TRUE);
        if (spec->role == ROLE_FIELD)
            SendMessageW(created, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
    }
    EnableWindow(ctl[ID_CANCEL], FALSE);
    for (size_t index = 0; index < sizeof(pack_options) / sizeof(pack_options[0]); index++) {
        const PackOption *option = &pack_options[index];
        WCHAR bundled[32768] = {0};
        BOOL available = SUCCEEDED(StringCchPrintfW(bundled, 32768,
                                                  L"%s\\%s", kit_path, option->file)) &&
                         regular_file(bundled);
        EnableWindow(ctl[option->control], available);
        if (wcscmp(option->code, L"hu") == 0) {
            hu_available = available;
            hu_selected = available;
        }
    }
    EnableWindow(ctl[ID_OPEN_FOLDER], FALSE);
    EnableWindow(ctl[ID_OPEN_TEMP], FALSE);
    EnableWindow(ctl[ID_COPY_ERROR], FALSE);
    const int details[] = {ID_DIAGNOSTICS, ID_COPY_ERROR, ID_COPY_LOG,
                           ID_SAVE_LOG, ID_OPEN_TEMP};
    for (size_t i = 0; i < sizeof(details) / sizeof(details[0]); i++)
        ShowWindow(ctl[details[i]], SW_HIDE);
    SendMessageW(ctl[ID_LOG], EM_SETLIMITTEXT, 4 * 1024 * 1024, 0);
    SendMessageW(ctl[ID_DIAGNOSTICS], EM_SETLIMITTEXT, 16384, 0);
}

/* ---- File dialogs --------------------------------------------------- */

static BOOL choose_file(BOOL is_save, WCHAR *path, size_t capacity) {
    IFileDialog *dialog = NULL;
    CLSID const *clsid = is_save ? &CLSID_FileSaveDialog : &CLSID_FileOpenDialog;
    HRESULT hr = CoCreateInstance(clsid, NULL, CLSCTX_INPROC_SERVER,
                                  &IID_IFileDialog, (void **)&dialog);
    if (FAILED(hr)) return FALSE;
    COMDLG_FILTERSPEC filters[1] = {
        {tr(is_save ? L"Unsigned iOS package (*.ipa)" : L"Original ZIP (*.zip)"),
         is_save ? L"*.ipa" : L"*.zip"}
    };
    IFileDialog_SetFileTypes(dialog, 1, filters);
    IFileDialog_SetFileTypeIndex(dialog, 1);
    DWORD options = 0;
    IFileDialog_GetOptions(dialog, &options);
    options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
    if (is_save) options |= FOS_OVERWRITEPROMPT;
    else options |= FOS_FILEMUSTEXIST;
    IFileDialog_SetOptions(dialog, options);
    IFileDialog_SetTitle(dialog, tr(is_save ? L"Save unsigned IPA" :
                                    L"Choose original Minion Rush 1.8.1g ZIP"));
    if (is_save) {
        IFileDialog_SetDefaultExtension(dialog, L"ipa");
        IFileDialog_SetFileName(dialog, L"MinionRush-unsigned.ipa");
    }
    BOOL selected = FALSE;
    if (SUCCEEDED(IFileDialog_Show(dialog, main_window))) {
        IShellItem *item = NULL;
        if (SUCCEEDED(IFileDialog_GetResult(dialog, &item))) {
            PWSTR result = NULL;
            if (SUCCEEDED(IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &result))) {
                if (SUCCEEDED(StringCchCopyW(path, capacity, result))) selected = TRUE;
                CoTaskMemFree(result);
            }
            IShellItem_Release(item);
        }
    }
    IFileDialog_Release(dialog);
    return selected;
}

/* ---- Status and logging -------------------------------------------- */

static void set_status(UiState state, LPCWSTR text) {
    ui_state = state;
    status_key = text;
    SetWindowTextW(ctl[ID_STATE], tr(text));
    InvalidateRect(main_window, &geo.dot, FALSE);
    InvalidateRect(main_window, &geo.track, FALSE);
}

static void show_detail(LPCWSTR text) {
    SetWindowTextW(ctl[ID_DETAIL], text);
    detail_dirty = FALSE;
}

static void append_log(WCHAR *line) {
    HWND log_field = ctl[ID_LOG];
    int size = GetWindowTextLengthW(log_field);
    if (size + (int)wcslen(line) > 4 * 1024 * 1024 - 128) {
        if (!log_truncated) {
            log_truncated = TRUE;
            const WCHAR *notice = L"\r\n[Log display reached 4 MB. Additional output omitted.]\r\n";
            SendMessageW(log_field, EM_SETSEL, (WPARAM)size, (LPARAM)size);
            SendMessageW(log_field, EM_REPLACESEL, FALSE, (LPARAM)notice);
        }
    } else {
        SendMessageW(log_field, EM_SETSEL, (WPARAM)size, (LPARAM)size);
        SendMessageW(log_field, EM_REPLACESEL, FALSE, (LPARAM)line);
        SendMessageW(log_field, EM_SCROLLCARET, 0, 0);
    }
    if (SUCCEEDED(StringCchCopyW(pending_detail, 512, line))) {
        size_t length = wcslen(pending_detail);
        while (length && (pending_detail[length - 1] == L'\r' || pending_detail[length - 1] == L'\n'))
            pending_detail[--length] = 0;
        detail_dirty = TRUE;
    }
    if (wcsncmp(line, L"1/3:", 4) == 0 || wcsncmp(line, L"2/3:", 4) == 0 ||
        wcsncmp(line, L"3/3:", 4) == 0) {
        StringCchCopyW(phase_text, 192, line);
        diagnostics_dirty = TRUE;
    }
    if (wcsncmp(line, L"Import transport:", 17) == 0) {
        StringCchCopyW(transport_text, 192, line + 17);
        diagnostics_dirty = TRUE;
    }
    if (wcsstr(line, L"Windows native IPA build failed:") ||
        wcsncmp(line, L"Error:", 6) == 0) {
        StringCchCopyW(last_error, 4096, line);
        EnableWindow(ctl[ID_COPY_ERROR], TRUE);
        diagnostics_dirty = TRUE;
    }
}

static void send_log(HWND window, const char *bytes, int length) {
    if (!length) return;
    int count = MultiByteToWideChar(CP_UTF8, 0, bytes, length, NULL, 0);
    if (count <= 0) return;
    WCHAR *line = HeapAlloc(GetProcessHeap(), 0, (count + 3) * sizeof(WCHAR));
    if (!line) return;
    if (!MultiByteToWideChar(CP_UTF8, 0, bytes, length, line, count)) {
        HeapFree(GetProcessHeap(), 0, line);
        return;
    }
    line[count] = L'\r';
    line[count + 1] = L'\n';
    line[count + 2] = 0;
    if (!PostMessageW(window, WM_APPEND_LOG, 0, (LPARAM)line))
        HeapFree(GetProcessHeap(), 0, line);
}


/* ---- Diagnostic report and export ---------------------------------- */

static void refresh_diagnostics(void) {
    WCHAR source[2048] = {0}, output[2048] = {0};
    WCHAR summary[16384] = {0};
    GetWindowTextW(ctl[ID_SOURCE], source, 2048);
    GetWindowTextW(ctl[ID_DESTINATION], output, 2048);

    LPCWSTR status = tr(ui_state == STATE_BUSY ? L"Running" :
                         ui_state == STATE_OK ? L"Completed" :
                         ui_state == STATE_FAILED ? L"Failed" :
                         ui_state == STATE_STOPPED ? L"Cancelled" : L"Ready");
    StringCchPrintfW(summary, 16384,
        interface_language ?
        L"\u00c1llapot: %s    Kil\u00e9p\u00e9si k\u00f3d: %lu\r\n"
        L"F\u00e1zis: %.180s\r\n"
        L"Adat\u00e1tvitel: %.170s\r\n"
        L"Forr\u00e1s ZIP: %.1900s\r\n"
        L"Kimeneti IPA: %.1900s\r\n"
        L"Tov\u00e1bbi nyelvek: %.1900s\r\n"
        L"Ideiglenes munkamappa: %.1900s\r\n"
        L"Hiba: %.3800s" :
        L"Status: %s    Exit code: %lu\r\n"
        L"Phase: %.180s\r\n"
        L"Transport: %.170s\r\n"
        L"Source ZIP: %.1900s\r\n"
        L"Output IPA: %.1900s\r\n"
        L"Additional languages: %.1900s\r\n"
        L"Private workspace: %.1900s\r\n"
        L"Error: %.3800s",
        status, (unsigned long)last_exit_code,
        phase_text[0] ? phase_text : tr(L"Not started"),
        transport_text[0] ? transport_text : tr(L"Not reported"),
        source, output,
        hu_available && hu_selected ? L"hu (Magyar)" :
        interface_language ? L"Csak eredeti nyelvek" : L"Original only",
        last_scratch[0] ? last_scratch : tr(L"Not started"),
        last_error[0] ? last_error : tr(L"None reported"));
    SetWindowTextW(ctl[ID_DIAGNOSTICS], summary);
    diagnostics_dirty = FALSE;
}

static WCHAR *get_report(BOOL include_log) {
    size_t max_log = include_log ? (size_t)GetWindowTextLengthW(ctl[ID_LOG]) : 0;
    size_t capacity = max_log + 20000;
    WCHAR *report = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, capacity * sizeof(WCHAR));
    if (!report) return NULL;
    refresh_diagnostics();
    GetWindowTextW(ctl[ID_DIAGNOSTICS], report, 16000);
    if (include_log) {
        WCHAR *log = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                               (max_log + 1) * sizeof(WCHAR));
        if (!log) { HeapFree(GetProcessHeap(), 0, report); return NULL; }
        GetWindowTextW(ctl[ID_LOG], log, (int)max_log + 1);
        StringCchCatW(report, capacity, L"\r\n\r\n--- Complete available build output ---\r\n");
        StringCchCatW(report, capacity, log);
        HeapFree(GetProcessHeap(), 0, log);
    } else {
        StringCchCatW(report, capacity, L"\r\n\r\nError details:\r\n");
        StringCchCatW(report, capacity, last_error[0] ? last_error : L"No error recorded.");
    }
    return report;
}

static BOOL copy_to_clipboard(LPCWSTR value) {
    size_t bytes = (wcslen(value) + 1) * sizeof(WCHAR);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) return FALSE;
    void *target = GlobalLock(memory);
    if (!target) { GlobalFree(memory); return FALSE; }
    memcpy(target, value, bytes);
    GlobalUnlock(memory);
    if (!OpenClipboard(main_window)) { GlobalFree(memory); return FALSE; }
    EmptyClipboard();
    BOOL success = SetClipboardData(CF_UNICODETEXT, memory) != NULL;
    CloseClipboard();
    if (!success) GlobalFree(memory);
    return success;
}

static void copy_report(BOOL error_only) {
    WCHAR *report = get_report(!error_only);
    if (!report) return;
    if (!copy_to_clipboard(report))
        MessageBoxW(main_window, tr(L"Could not copy the report to the clipboard."),
                    tr(L"Clipboard unavailable"), MB_OK | MB_ICONWARNING);
    HeapFree(GetProcessHeap(), 0, report);
}

static void save_report(void) {
    WCHAR path[32768] = {0};
    IFileDialog *dialog = NULL;
    if (FAILED(CoCreateInstance(&CLSID_FileSaveDialog, NULL, CLSCTX_INPROC_SERVER,
                                &IID_IFileDialog, (void **)&dialog))) return;
    COMDLG_FILTERSPEC filter = {tr(L"Text log (*.txt)"), L"*.txt"};
    IFileDialog_SetFileTypes(dialog, 1, &filter);
    IFileDialog_SetDefaultExtension(dialog, L"txt");
    IFileDialog_SetFileName(dialog, L"MinionRush-build-log.txt");
    IFileDialog_SetTitle(dialog, tr(L"Save local build diagnostics"));
    BOOL selected = FALSE;
    if (SUCCEEDED(IFileDialog_Show(dialog, main_window))) {
        IShellItem *item = NULL;
        if (SUCCEEDED(IFileDialog_GetResult(dialog, &item))) {
            PWSTR name = NULL;
            if (SUCCEEDED(IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &name))) {
                selected = SUCCEEDED(StringCchCopyW(path, 32768, name));
                CoTaskMemFree(name);
            }
            IShellItem_Release(item);
        }
    }
    IFileDialog_Release(dialog);
    if (!selected) return;
    WCHAR *report = get_report(TRUE);
    if (!report) return;
    int bytes = WideCharToMultiByte(CP_UTF8, 0, report, -1, NULL, 0, NULL, NULL);
    char *utf8 = bytes > 0 ? HeapAlloc(GetProcessHeap(), 0, (size_t)bytes) : NULL;
    BOOL ok = FALSE;
    if (utf8 && WideCharToMultiByte(CP_UTF8, 0, report, -1, utf8, bytes, NULL, NULL)) {
        HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, NULL);
        if (file != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            static const BYTE bom[] = {0xEF, 0xBB, 0xBF};
            ok = WriteFile(file, bom, 3, &written, NULL) && written == 3;
            if (ok) ok = WriteFile(file, utf8, (DWORD)(bytes - 1), &written, NULL) &&
                         written == (DWORD)(bytes - 1);
            if (!CloseHandle(file)) ok = FALSE;
        }
    }
    if (!ok) MessageBoxW(main_window, tr(L"The diagnostic log could not be saved."),
                    tr(L"Save failed"), MB_OK | MB_ICONERROR);
    if (utf8) HeapFree(GetProcessHeap(), 0, utf8);
    HeapFree(GetProcessHeap(), 0, report);
}

static void open_private_workspace(void) {
    if (last_scratch[0] && GetFileAttributesW(last_scratch) != INVALID_FILE_ATTRIBUTES)
        ShellExecuteW(main_window, L"open", last_scratch, NULL, NULL, SW_SHOWNORMAL);
}

/* ---- Build job ------------------------------------------------------ */

static void remove_job_scratch(HWND window, LPCWSTR path) {
    WCHAR items[32768] = {0};
    if (!path[0] || FAILED(StringCchCopyW(items, 32768, path))) return;
    // SHFileOperation expects a double-NUL terminated, absolute path.
    SHFILEOPSTRUCTW operation = {0};
    operation.wFunc = FO_DELETE;
    operation.pFrom = items;
    operation.fFlags = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    int error = SHFileOperationW(&operation);
    if (error || operation.fAnyOperationsAborted) {
        const char *message = "Private temporary directory could not be cleaned automatically.";
        send_log(window, message, (int)strlen(message));
    }
}

static BOOL create_job_scratch(WCHAR *destination, size_t capacity) {
    WCHAR temp[MAX_PATH * 4] = {0};
    GUID guid;
    DWORD length = GetTempPathW(MAX_PATH * 4, temp);
    if (!length || length >= MAX_PATH * 4 || FAILED(CoCreateGuid(&guid)))
        return FALSE;
    if (FAILED(StringCchPrintfW(destination, capacity,
                                 L"%sMR-%08lX%04X", temp,
                                 (unsigned long)guid.Data1, (unsigned int)guid.Data2)))
        return FALSE;
    return CreateDirectoryW(destination, NULL) != FALSE;
}

/* The UI thread creates, terminates and closes the job object, so the handle
   it cancels with can never be stale or reused. */
static HANDLE create_kill_job(void) {
    HANDLE job = CreateJobObjectW(NULL, NULL);
    if (!job) return NULL;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        CloseHandle(job);
        return NULL;
    }
    return job;
}

static DWORD WINAPI run_build(LPVOID opaque) {
    Build *job = (Build *)opaque;
    HANDLE read_pipe = NULL, write_pipe = NULL;
    SECURITY_ATTRIBUTES security = {sizeof(security), NULL, TRUE};
    PROCESS_INFORMATION process = {0};
    DWORD result = 1;
    if (!CreatePipe(&read_pipe, &write_pipe, &security, 0)) goto done;
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);
    HANDLE null_in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ, &security,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (null_in == INVALID_HANDLE_VALUE) goto done;
    STARTUPINFOW startup = {0};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = null_in;
    startup.hStdOutput = write_pipe;
    startup.hStdError = write_pipe;
    size_t cap = 160000;
    WCHAR *command = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, cap * sizeof(WCHAR));
    if (!command) {
        CloseHandle(null_in);
        goto done;
    }
    HRESULT hr = StringCchPrintfW(
        command, cap, L"\"%s\" -X utf8 \"%s\" \"%s\" --kit \"%s\" --output \"%s\"",
        job->python, job->tool, job->source, job->kit, job->output
    );
    if (SUCCEEDED(hr) && !job->include_hungarian)
        hr = StringCchCatW(command, cap, L" --no-hungarian");
    if (SUCCEEDED(hr) && job->overwrite_output)
        hr = StringCchCatW(command, cap, L" --overwrite");
    if (SUCCEEDED(hr)) {
        hr = StringCchCatW(command, cap, L" --scratch-root \"");
        if (SUCCEEDED(hr)) hr = StringCchCatW(command, cap, job->scratch);
        if (SUCCEEDED(hr)) hr = StringCchCatW(command, cap, L"\"");
    }
    if (SUCCEEDED(hr) && CreateProcessW(
            job->python, command, NULL, NULL, TRUE,
            CREATE_NO_WINDOW | CREATE_SUSPENDED,
            NULL, job->kit, &startup, &process)) {
        BOOL assigned = AssignProcessToJobObject(job->job, process.hProcess);
        if (!assigned) {
            const char *message = "Unable to protect child process tree.";
            send_log(job->window, message, (int)strlen(message));
        }
        // A cancel that arrived before assignment could not reach the process.
        if (!assigned || InterlockedCompareExchange(&cancel_requested, 0, 0))
            TerminateProcess(process.hProcess, 1);
        else
            ResumeThread(process.hThread);
        CloseHandle(process.hThread);
        CloseHandle(null_in);
        CloseHandle(write_pipe);
        write_pipe = NULL;
        char bytes[2048], line[16384];
        int used = 0;
        DWORD got = 0;
        while (ReadFile(read_pipe, bytes, sizeof(bytes), &got, NULL) && got > 0) {
            for (DWORD index = 0; index < got; index++) {
                char byte = bytes[index];
                if (byte == '\n' || used == (int)sizeof(line)) {
                    send_log(job->window, line, used);
                    used = 0;
                }
                if (byte != '\r' && byte != '\n') line[used++] = byte;
            }
        }
        if (used) send_log(job->window, line, used);
        WaitForSingleObject(process.hProcess, INFINITE);
        GetExitCodeProcess(process.hProcess, &result);
        CloseHandle(process.hProcess);
    } else {
        const char *message = "Could not start the embedded Python runtime.";
        send_log(job->window, message, (int)strlen(message));
        CloseHandle(null_in);
    }
    HeapFree(GetProcessHeap(), 0, command);
done:
    if (write_pipe) CloseHandle(write_pipe);
    if (read_pipe) CloseHandle(read_pipe);
    if (result == 0 || InterlockedCompareExchange(&cancel_requested, 0, 0))
        remove_job_scratch(job->window, job->scratch);
    else {
        const char *note = "Build failed. Private workspace retained for inspection. Delete it after troubleshooting.";
        send_log(job->window, note, (int)strlen(note));
    }
    PostMessageW(job->window, WM_BUILD_DONE, result, 0);
    HeapFree(GetProcessHeap(), 0, job);
    return 0;
}

static void set_busy(BOOL busy) {
    static const int locked[] = {ID_SOURCE, ID_DESTINATION, ID_HU_PACK, ID_PICK_SOURCE,
                                 ID_PICK_DESTINATION, ID_BUILD};
    for (size_t index = 0; index < sizeof(locked) / sizeof(locked[0]); index++)
        EnableWindow(ctl[locked[index]], !busy &&
                     (locked[index] != ID_HU_PACK || hu_available));
    EnableWindow(ctl[ID_CANCEL], busy);
    if (busy) {
        EnableWindow(ctl[ID_OPEN_FOLDER], FALSE);
        SetTimer(main_window, TIMER_ANIMATION, 25, NULL);
    } else {
        KillTimer(main_window, TIMER_ANIMATION);
    }
    // Disabling the focused control drops focus; keep keyboard users oriented.
    HWND focus = GetFocus();
    if (!focus || focus == main_window) SetFocus(ctl[busy ? ID_CANCEL : ID_BUILD]);
    InvalidateRect(main_window, NULL, FALSE);
}

static void start_build(void) {
    if (running) return;
    Build *job = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(Build));
    if (!job) return;
    job->window = main_window;
    StringCchCopyW(job->kit, 32768, kit_path);
    StringCchPrintfW(job->python, 32768, L"%s\\python\\python.exe", kit_path);
    StringCchPrintfW(job->tool, 32768,
                     L"%s\\runtime\\tools\\windows_native_package.py", kit_path);
    if (!get_text(ctl[ID_SOURCE], job->source, 32768)) {
        MessageBoxW(main_window, tr(L"Select an existing original .zip file."),
                    tr(L"Source ZIP required"), MB_OK | MB_ICONWARNING);
        goto failure;
    }
    unquote_path(job->source);
    if (!is_zip(job->source) || !regular_file(job->source)) {
        MessageBoxW(main_window, tr(L"Select an existing original .zip file."),
                    tr(L"Source ZIP required"), MB_OK | MB_ICONWARNING);
        goto failure;
    }
    if (!get_text(ctl[ID_DESTINATION], job->output, 32768) ||
            wcslen(job->output) < 5) {
        MessageBoxW(main_window, tr(L"Choose an output .ipa path."),
                    tr(L"Output required"), MB_OK | MB_ICONWARNING);
        goto failure;
    }
    size_t output_len = wcslen(job->output);
    if (output_len < 4 || _wcsicmp(job->output + output_len - 4, L".ipa") != 0) {
        MessageBoxW(main_window, tr(L"The output filename must end in .ipa."),
                    tr(L"Invalid filename"), MB_OK | MB_ICONWARNING);
        goto failure;
    }
    DWORD attrs = GetFileAttributesW(job->output);
    if (attrs != INVALID_FILE_ATTRIBUTES) {
        if (attrs & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) {
            MessageBoxW(main_window, tr(L"The selected output is not a regular file."),
                        tr(L"Invalid filename"), MB_OK | MB_ICONWARNING);
            goto failure;
        }
        if (_wcsicmp(approved_overwrite, job->output) != 0 &&
                MessageBoxW(main_window,
                            tr(L"This file already exists. Replace it with the newly built IPA?"),
                            tr(L"Existing output"),
                            MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2) != IDYES)
            goto failure;
        job->overwrite_output = TRUE;
    }
    approved_overwrite[0] = 0;  /* Consent is for one build only. */
    job->include_hungarian = hu_selected && hu_available;
    if (!regular_file(job->python) || !regular_file(job->tool)) {
        MessageBoxW(main_window, tr(L"The portable runtime is incomplete. Extract the full release ZIP."),
                    tr(L"Missing application files"), MB_OK | MB_ICONERROR);
        goto failure;
    }
    if (!create_job_scratch(job->scratch, 32768)) {
        MessageBoxW(main_window, tr(L"Cannot create a private temporary workspace."),
                    tr(L"Temporary storage unavailable"), MB_OK | MB_ICONERROR);
        goto failure;
    }
    active_job = create_kill_job();
    if (!active_job) {
        MessageBoxW(main_window, tr(L"Unable to protect child process tree."),
                    tr(L"Build cannot start"), MB_OK | MB_ICONERROR);
        RemoveDirectoryW(job->scratch);
        goto failure;
    }
    job->job = active_job;
    SetWindowTextW(ctl[ID_LOG], L"");
    SetWindowTextW(ctl[ID_DIAGNOSTICS], L"");
    last_error[0] = phase_text[0] = transport_text[0] = 0;
    log_truncated = FALSE;
    last_exit_code = 0;
    StringCchCopyW(last_scratch, 32768, job->scratch);
    diagnostics_dirty = TRUE;
    EnableWindow(ctl[ID_COPY_ERROR], FALSE);
    EnableWindow(ctl[ID_OPEN_TEMP], FALSE);
    show_detail(L"");
    set_status(STATE_BUSY, L"Building native ARM64 IPA. Keep this window open.");
    running = TRUE;
    aborted = FALSE;
    InterlockedExchange(&cancel_requested, 0);
    set_busy(TRUE);
    StringCchCopyW(last_output, 32768, job->output);
    HANDLE thread = CreateThread(NULL, 0, run_build, job, 0, NULL);
    if (thread) {
        CloseHandle(thread);
        return;
    }
    running = FALSE;
    CloseHandle(active_job);
    active_job = NULL;
    set_busy(FALSE);
    set_status(STATE_FAILED, L"Could not start build thread.");
    RemoveDirectoryW(job->scratch);
failure:
    HeapFree(GetProcessHeap(), 0, job);
}

static void stop_build(void) {
    if (!running || !active_job) return;
    int choice = MessageBoxW(main_window,
        tr(L"Stop creating the IPA? The unfinished temporary files may need to be removed."),
        tr(L"Cancel build"), MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2);
    // The build can finish while the question is open.
    if (choice != IDYES || !running || !active_job) return;
    aborted = TRUE;
    InterlockedExchange(&cancel_requested, 1);
    TerminateJobObject(active_job, 1);
    set_status(STATE_BUSY, L"Cancelling...");
    EnableWindow(ctl[ID_CANCEL], FALSE);
    if (!GetFocus() || GetFocus() == main_window) SetFocus(ctl[ID_DETAILS_TOGGLE]);
}

static void finish_build(DWORD exit_code) {
    running = FALSE;
    last_exit_code = exit_code;
    diagnostics_dirty = TRUE;
    if (active_job) {
        CloseHandle(active_job);
        active_job = NULL;
    }
    set_busy(FALSE);
    if (detail_dirty) show_detail(pending_detail);
    BOOL completed = exit_code == 0 && regular_file(last_output);
    if (completed) {
        set_status(STATE_OK, L"Unsigned IPA created. Sign with Sideloadly before installing.");
        show_detail(last_output);
        EnableWindow(ctl[ID_OPEN_FOLDER], TRUE);
        SetFocus(ctl[ID_OPEN_FOLDER]);
    } else if (aborted) {
        set_status(STATE_STOPPED, L"Build cancelled.");
    } else {
        set_status(STATE_FAILED, L"Build failed. Review the diagnostic details below.");
        EnableWindow(ctl[ID_OPEN_TEMP], last_scratch[0] &&
                    GetFileAttributesW(last_scratch) != INVALID_FILE_ATTRIBUTES);
        set_log_visible(TRUE);
    }
    /* The success dialog pumps its own event loop. Publish the final
       diagnostic state before showing it, not when the dialog closes. */
    refresh_diagnostics();
    if (completed)
        MessageBoxW(main_window, tr(L"Unsigned IPA created successfully. It is ready for signing with Sideloadly."),
                    tr(L"Build complete"), MB_OK | MB_ICONINFORMATION);
}

static void pick_into(int target, BOOL is_save) {
    WCHAR selection[32768];
    if (!choose_file(is_save, selection, 32768)) return;
    set_path(ctl[target], selection);
    if (is_save) {
        /* FOS_OVERWRITEPROMPT has already obtained consent when the chosen
           path exists. Never reuse that consent for another destination. */
        DWORD attrs = GetFileAttributesW(selection);
        if (attrs != INVALID_FILE_ATTRIBUTES &&
                !(attrs & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
            StringCchCopyW(approved_overwrite, 32768, selection);
        else
            approved_overwrite[0] = 0;
    }
}

static void accept_drop(HDROP drop) {
    WCHAR selected[32768] = {0};
    if (!running && DragQueryFileW(drop, 0xFFFFFFFF, NULL, 0) == 1 &&
        DragQueryFileW(drop, 0, selected, 32768) && is_zip(selected) && regular_file(selected))
        set_path(ctl[ID_SOURCE], selected);
    else if (!running) MessageBoxW(main_window, tr(L"Drop exactly one original .zip file."),
                    tr(L"ZIP required"), MB_OK | MB_ICONWARNING);
    DragFinish(drop);
}

/* Enter activates the focused button, otherwise the default action. */
static void press_enter(void) {
    HWND focus = GetFocus();
    HWND target = ctl[ID_BUILD];
    for (int id = ID_TITLE; id < ID_LAST; id++)
        if (focus == ctl[id] && is_button_role(role_of(id))) target = focus;
    if (IsWindowEnabled(target)) SendMessageW(target, BM_CLICK, 0, 0);
}

static LRESULT CALLBACK window_proc(HWND window, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_CREATE: {
        main_window = window;
        static const COLORREF brush_colors[BRUSH_COUNT] = {COL_NAVY, COL_CARD, COL_FIELD, COL_FIELD_OFF};
        for (int index = 0; index < BRUSH_COUNT; index++)
            brushes[index] = CreateSolidBrush(brush_colors[index]);
        ui_dpi = query_dpi(window);
        create_controls(window);
        rebuild_fonts();
        interface_language = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_HUNGARIAN;
        update_interface();
#ifdef MR_UTM_TEST
        WCHAR nearby[32768] = {0};
        if (nearby_original_zip(nearby, 32768))
            set_path(ctl[ID_SOURCE], nearby);
#endif
        tint_title_bar(window);
        DragAcceptFiles(window, TRUE);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paint_window(window);
        return 0;
    case WM_DRAWITEM:
        if (((DRAWITEMSTRUCT *)lparam)->CtlType == ODT_BUTTON) {
            draw_button((DRAWITEMSTRUCT *)lparam);
            return TRUE;
        }
        break;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
        return control_colors((HDC)wparam, (HWND)lparam);
    case WM_GETMINMAXINFO: {
        MINMAXINFO *limits = (MINMAXINFO *)lparam;
        if (!ctl[ID_LOG]) break;
        Geometry probe;
        int extra_width, extra_height;
        int natural = compute_geometry(&probe, S(640), 0);
        window_frame(&extra_width, &extra_height);
        limits->ptMinTrackSize.x = S(640) + extra_width;
        limits->ptMinTrackSize.y = natural + (log_open ? S(DETAILS_EXTRA + LOG_MINIMUM) : 0) + extra_height;
        return 0;
    }
    case WM_SIZE:
        if (wparam != SIZE_MINIMIZED && ctl[ID_LOG]) relayout();
        return 0;
    case WM_DPICHANGED: {
        const RECT *suggested = (const RECT *)lparam;
        ui_dpi = HIWORD(wparam);
        rebuild_fonts();
        SetWindowPos(window, NULL, suggested->left, suggested->top,
                     suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        relayout();
        return 0;
    }
    case WM_ACTIVATE:
        if (LOWORD(wparam) == WA_INACTIVE) {
            HWND focus = GetFocus();
            if (focus && IsChild(window, focus)) remembered_focus = focus;
        } else {
            HWND target = remembered_focus && IsWindowEnabled(remembered_focus) ?
                          remembered_focus : ctl[running ? ID_CANCEL : ID_PICK_SOURCE];
            if (IsWindowEnabled(target)) SetFocus(target);
        }
        return 0;
    case WM_TIMER:
        if (wparam == TIMER_ANIMATION) {
            InvalidateRect(window, &geo.track, FALSE);
            if (detail_dirty) show_detail(pending_detail);
            if (diagnostics_dirty && log_open) refresh_diagnostics();
        }
        return 0;
    case WM_DROPFILES:
        accept_drop((HDROP)wparam);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case ID_SOURCE: case ID_DESTINATION:
            if (HIWORD(wparam) == EN_SETFOCUS || HIWORD(wparam) == EN_KILLFOCUS)
                InvalidateRect(window, NULL, FALSE);
            return 0;
        case ID_PICK_SOURCE: {
#ifdef MR_UTM_TEST
            WCHAR nearby[32768] = {0};
            if (nearby_original_zip(nearby, 32768)) {
                set_path(ctl[ID_SOURCE], nearby);
                return 0;
            }
#endif
            pick_into(ID_SOURCE, FALSE);
            return 0;
        }
        case ID_PICK_DESTINATION: pick_into(ID_DESTINATION, TRUE); return 0;
        case ID_HU_PACK:
            if (!running && hu_available) {
                hu_selected = !hu_selected;
                InvalidateRect(ctl[ID_HU_PACK], NULL, FALSE);
                diagnostics_dirty = TRUE;
            }
            return 0;
        case ID_UI_EN: case ID_UI_HU:
            interface_language = LOWORD(wparam) == ID_UI_HU;
            update_interface();
            diagnostics_dirty = TRUE;
            if (log_open) refresh_diagnostics();
            return 0;
        case ID_BUILD: start_build(); return 0;
        case ID_CANCEL: stop_build(); return 0;
        case ID_DETAILS_TOGGLE:
            set_log_visible(!log_open);
            if (log_open) refresh_diagnostics();
            return 0;
        case ID_COPY_ERROR: copy_report(TRUE); return 0;
        case ID_COPY_LOG: copy_report(FALSE); return 0;
        case ID_SAVE_LOG: save_report(); return 0;
        case ID_OPEN_TEMP: open_private_workspace(); return 0;
        case ID_OPEN_FOLDER:
            if (last_output[0]) {
                WCHAR explorer[32768];
                StringCchPrintfW(explorer, 32768, L"/select,\"%s\"", last_output);
                ShellExecuteW(window, L"open", L"explorer.exe", explorer, NULL, SW_SHOWNORMAL);
            }
            return 0;
        case IDOK: press_enter(); return 0;
        case IDCANCEL: return 0;
        }
        break;
    case WM_APPEND_LOG:
        append_log((WCHAR *)lparam);
        HeapFree(GetProcessHeap(), 0, (LPVOID)lparam);
        return 0;
    case WM_BUILD_DONE:
        finish_build((DWORD)wparam);
        return 0;
    case WM_CLOSE:
        if (running) {
            stop_build();
            return 0;
        }
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        DragAcceptFiles(window, FALSE);
        KillTimer(window, TIMER_ANIMATION);
        for (int index = 0; index < FONT_COUNT; index++)
            if (fonts[index]) DeleteObject(fonts[index]);
        for (int index = 0; index < BRUSH_COUNT; index++)
            if (brushes[index]) DeleteObject(brushes[index]);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, msg, wparam, lparam);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE unused, PWSTR arguments, int show) {
    (void)unused;
    (void)arguments;
    BOOL self_test = FALSE;
    if (!mr_builder_parse_args(&self_test)) return 2;
    if (!GetModuleFileNameW(NULL, kit_path, MAX_PATH * 4)) return 1;
    WCHAR *separator = wcsrchr(kit_path, L'\\');
    if (!separator) return 1;
    *separator = 0;
    if (self_test) {
        WCHAR location[32768];
        StringCchPrintfW(location, 32768, L"%s\\python\\python.exe", kit_path);
        if (!regular_file(location)) return 2;
        StringCchPrintfW(location, 32768, L"%s\\runtime\\tools\\windows_native_package.py", kit_path);
        if (!regular_file(location)) return 3;
        StringCchPrintfW(location, 32768, L"%s\\kit-manifest.json", kit_path);
        if (!regular_file(location)) return 4;
        return 0;
    }
    declare_dpi_awareness();
    if (FAILED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED))) return 1;
    WNDCLASSEXW cls = {0};
    cls.cbSize = sizeof(cls);
    cls.hInstance = instance;
    cls.lpfnWndProc = window_proc;
    cls.lpszClassName = L"MinionRushNativeIpaBuilder";
    cls.hCursor = LoadCursorW(NULL, IDC_ARROW);
    cls.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(101));
    cls.hIconSm = (HICON)LoadImageW(instance, MAKEINTRESOURCEW(101),
                                    IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                    GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    if (!cls.hIcon || !cls.hIconSm || !RegisterClassExW(&cls)) return 1;
#ifdef MR_UTM_TEST
    LPCWSTR title = L"Minion Rush IPA Builder - UTM test build";
#else
    LPCWSTR title = L"Minion Rush IPA Builder";
#endif
    HWND window = CreateWindowExW(0, cls.lpszClassName, title,
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 800, 640,
        NULL, NULL, instance, NULL);
    if (!window) return 1;
    {
        Geometry probe;
        resize_client(S(760), compute_geometry(&probe, S(760), 0), TRUE);
    }
    ShowWindow(window, show);
    UpdateWindow(window);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (IsDialogMessageW(window, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CoUninitialize();
    return (int)msg.wParam;
}
