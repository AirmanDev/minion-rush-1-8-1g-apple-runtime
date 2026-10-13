#ifndef MR_WINDOWS_BUILDER_ARGS_H
#define MR_WINDOWS_BUILDER_ARGS_H

#include <windows.h>
#include <shellapi.h>
#include <wchar.h>

// Parse actual Windows argv rather than comparing the raw wWinMain command-line
// tail, which can retain trailing whitespace inserted by launchers.
static BOOL mr_builder_parse_args(BOOL *self_test) {
    int count = 0;
    LPWSTR *values = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!values) return FALSE;
    BOOL valid = count == 1 || (count == 2 && wcscmp(values[1], L"--self-test") == 0);
    if (valid) *self_test = count == 2;
    LocalFree(values);
    return valid;
}

#endif
