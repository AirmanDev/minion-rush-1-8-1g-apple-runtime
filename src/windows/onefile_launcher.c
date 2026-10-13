#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <wincrypt.h>
#include <shellapi.h>
#include "builder_args.h"
#include <strsafe.h>
#include <stdint.h>
#include <stdio.h>
#include <wchar.h>
#include <string.h>

#pragma pack(push, 1)
typedef struct {
    char magic[8];
    uint64_t offset;
    uint64_t length;
    BYTE sha256[32];
} BundleFooter;
#pragma pack(pop)
_Static_assert(sizeof(BundleFooter) == 56, "Unexpected bundle footer");

#define PATH_CAP 32768
#define CHUNK_SIZE (1024u * 1024u)
#define MAX_BUNDLE (256u * 1024u * 1024u)

static void error_message(const WCHAR *message) {
    MessageBoxW(NULL, message, L"Minion Rush Builder", MB_OK | MB_ICONERROR);
}

static BOOL file_is_present(const WCHAR *path) {
    DWORD attrs = GetFileAttributesW(path);
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

static BOOL copy_bundle(HANDLE source, uint64_t length, LPCWSTR destination, const BYTE expected[32]) {
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    HANDLE output = INVALID_HANDLE_VALUE;
    BYTE *block = NULL;
    BYTE actual[32];
    DWORD size = sizeof actual;
    BOOL ok = FALSE;
    if (!CryptAcquireContextW(&provider, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT))
        goto done;
    if (!CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash)) goto done;
    output = CreateFileW(destination, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                         FILE_ATTRIBUTE_NORMAL, NULL);
    if (output == INVALID_HANDLE_VALUE) goto done;
    block = HeapAlloc(GetProcessHeap(), 0, CHUNK_SIZE);
    if (!block) goto done;
    while (length) {
        DWORD request = length < CHUNK_SIZE ? (DWORD)length : CHUNK_SIZE;
        DWORD got = 0, written = 0;
        if (!ReadFile(source, block, request, &got, NULL) || got != request ||
            !CryptHashData(hash, block, got, 0) ||
            !WriteFile(output, block, got, &written, NULL) || written != got) goto done;
        length -= got;
    }
    if (!CryptGetHashParam(hash, HP_HASHVAL, actual, &size, 0) || size != sizeof actual)
        goto done;
    ok = memcmp(actual, expected, sizeof actual) == 0;
done:
    if (block) HeapFree(GetProcessHeap(), 0, block);
    if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
    if (hash) CryptDestroyHash(hash);
    if (provider) CryptReleaseContext(provider, 0);
    if (!ok) DeleteFileW(destination);
    return ok;
}

static BOOL run_child(LPCWSTR program, LPCWSTR arguments, LPCWSTR directory, DWORD *status) {
    WCHAR *command = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, PATH_CAP * sizeof(WCHAR));
    if (!command) return FALSE;
    BOOL ok = FALSE;
    if (SUCCEEDED(StringCchPrintfW(command, PATH_CAP, L"\"%s\" %s", program, arguments))) {
        STARTUPINFOW startup = {0};
        PROCESS_INFORMATION process = {0};
        startup.cb = sizeof startup;
        if (CreateProcessW(program, command, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                           NULL, directory, &startup, &process)) {
            CloseHandle(process.hThread);
            if (WaitForSingleObject(process.hProcess, INFINITE) == WAIT_OBJECT_0 &&
                GetExitCodeProcess(process.hProcess, status)) ok = TRUE;
            CloseHandle(process.hProcess);
        }
    }
    HeapFree(GetProcessHeap(), 0, command);
    return ok;
}

static BOOL private_directory(WCHAR *path, size_t capacity) {
    WCHAR temp[PATH_CAP];
    DWORD length = GetTempPathW(PATH_CAP, temp);
    if (!length || length >= PATH_CAP) return FALSE;
    GUID id;
    if (CoCreateGuid(&id) != S_OK) return FALSE;
    if (FAILED(StringCchPrintfW(path, capacity, L"%sMRB-%08lX-%04X-%04X",
                                temp, (unsigned long)id.Data1, id.Data2, id.Data3)))
        return FALSE;
    return CreateDirectoryW(path, NULL);
}

static BOOL remove_directory_tree(LPCWSTR path) {
    WCHAR *double_null = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                   (wcslen(path) + 2) * sizeof(WCHAR));
    if (!double_null) return FALSE;
    wcscpy(double_null, path);
    SHFILEOPSTRUCTW op = {0};
    op.wFunc = FO_DELETE;
    op.pFrom = double_null;
    op.fFlags = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    BOOL ok = SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
    HeapFree(GetProcessHeap(), 0, double_null);
    return ok;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR arguments, int display) {
    (void)instance; (void)previous; (void)arguments; (void)display;
    BOOL self_test = FALSE;
    if (!mr_builder_parse_args(&self_test)) {
        error_message(L"Unsupported command-line arguments.");
        return 2;
    }

    WCHAR application[PATH_CAP] = {0}, temp[PATH_CAP] = {0};
    WCHAR archive[PATH_CAP] = {0}, kit[PATH_CAP] = {0};
    WCHAR inner[PATH_CAP] = {0}, tar[PATH_CAP] = {0};
    DWORD exit_code = 1;
    HANDLE self = INVALID_HANDLE_VALUE;
    BOOL created = FALSE;
    BOOL launched = FALSE;
    if (!GetModuleFileNameW(NULL, application, PATH_CAP)) goto failure;
    self = CreateFileW(application, GENERIC_READ, FILE_SHARE_READ, NULL,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (self == INVALID_HANDLE_VALUE) goto failure;
    LARGE_INTEGER size;
    if (!GetFileSizeEx(self, &size) || size.QuadPart < (LONGLONG)sizeof(BundleFooter))
        goto failure;
    LARGE_INTEGER offset;
    offset.QuadPart = size.QuadPart - (LONGLONG)sizeof(BundleFooter);
    if (!SetFilePointerEx(self, offset, NULL, FILE_BEGIN)) goto failure;
    BundleFooter footer;
    DWORD received = 0;
    if (!ReadFile(self, &footer, sizeof footer, &received, NULL) ||
        received != sizeof footer || memcmp(footer.magic, "MR1FILE!", 8) != 0 ||
        footer.offset < 4096 || footer.length == 0 || footer.length > MAX_BUNDLE ||
        footer.offset + footer.length != (uint64_t)offset.QuadPart)
        goto failure;

    if (!private_directory(temp, PATH_CAP)) goto failure;
    created = TRUE;
    if (FAILED(StringCchPrintfW(archive, PATH_CAP, L"%s\\package.zip", temp)) ||
        FAILED(StringCchPrintfW(kit, PATH_CAP, L"%s\\MinionRush-Portable-Windows", temp)) ||
        FAILED(StringCchPrintfW(inner, PATH_CAP, L"%s\\MinionRush Builder.exe", kit)))
        goto failure;
    offset.QuadPart = (LONGLONG)footer.offset;
    if (!SetFilePointerEx(self, offset, NULL, FILE_BEGIN) ||
        !copy_bundle(self, footer.length, archive, footer.sha256))
        goto failure;
    CloseHandle(self);
    self = INVALID_HANDLE_VALUE;

    WCHAR system[PATH_CAP] = {0};
    UINT system_length = GetSystemDirectoryW(system, PATH_CAP);
    if (!system_length || system_length >= PATH_CAP ||
        FAILED(StringCchPrintfW(tar, PATH_CAP, L"%s\\tar.exe", system)) ||
        !file_is_present(tar))
        goto failure;
    WCHAR *tar_arguments = HeapAlloc(GetProcessHeap(), 0, PATH_CAP * sizeof(WCHAR));
    if (!tar_arguments) goto failure;
    HRESULT formatted = StringCchPrintfW(tar_arguments, PATH_CAP,
                                           L"-xf \"%s\" -C \"%s\"", archive, temp);
    BOOL extracted = SUCCEEDED(formatted) &&
                     run_child(tar, tar_arguments, temp, &exit_code) && exit_code == 0;
    HeapFree(GetProcessHeap(), 0, tar_arguments);
    if (!extracted || !file_is_present(inner)) goto failure;

    WCHAR *last_slash = wcsrchr(application, L'\\');
    if (last_slash) {
        *last_slash = 0;
        SetEnvironmentVariableW(L"MR_SOURCE_DIR", application);
    }
    launched = run_child(inner, self_test ? L"--self-test" : L"", kit, &exit_code);
    if (!launched) goto failure;
    goto done;

failure:
    error_message(L"The embedded Windows Builder could not start or its package failed verification.\n"
                  L"Check that Windows 11 is supported and the EXE is complete.");
    exit_code = 1;
done:
    if (self != INVALID_HANDLE_VALUE) CloseHandle(self);
    if (created && !remove_directory_tree(temp))
        error_message(L"Unable to remove temporary Builder files. Check your Windows TEMP folder.");
    return (int)exit_code;
}
