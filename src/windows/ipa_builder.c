#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <strsafe.h>
#include <stdio.h>
#include <wchar.h>
#include <stdlib.h>
#include <string.h>

enum {
    ID_SOURCE = 101, ID_PICK_SOURCE, ID_DESTINATION, ID_PICK_DESTINATION,
    ID_CATALOG, ID_PICK_CATALOG, ID_BUILD, ID_CANCEL, ID_OPEN_FOLDER, ID_LOG,
    ID_PROGRESS, ID_STATE,
    WM_APPEND_LOG = WM_APP + 1, WM_BUILD_DONE = WM_APP + 2,
    WM_JOB_READY = WM_APP + 3
};

static HWND main_window;
static HWND source_field, destination_field, catalog_field;
static HWND browse_source, browse_destination, browse_catalog;
static HWND build_button, cancel_button, open_button, log_field, status_field;
static HWND progress_bar;
static HFONT ui_font, heading_font;
static HBRUSH white_brush;
static WCHAR kit_path[MAX_PATH * 4], last_output[32768];
static HANDLE active_job;
static BOOL running, aborted;

typedef struct {
    WCHAR python[32768], tool[32768], kit[32768];
    WCHAR source[32768], output[32768], catalog[32768], scratch[32768];
    HWND window;
    HANDLE job;
} Build;

static void font(HWND window, HFONT face) {
    SendMessageW(window, WM_SETFONT, (WPARAM)face, TRUE);
}

static HWND child(LPCWSTR kind, LPCWSTR text, DWORD style, int id) {
    HWND window = CreateWindowExW(
        0, kind, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 100, 26,
        main_window, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL
    );
    font(window, ui_font);
    return window;
}

static void label(LPCWSTR title, int left, int top, int width, int height) {
    HWND widget = child(L"STATIC", title, SS_LEFT, 0);
    SetWindowPos(widget, NULL, left, top, width, height, SWP_NOZORDER);
}

static void field(HWND *target, int id, LPCWSTR prompt) {
    *target = child(L"EDIT", prompt, WS_BORDER | ES_AUTOHSCROLL | ES_READONLY | WS_TABSTOP, id);
}

static void place(HWND child_window, int x, int y, int w, int h) {
    MoveWindow(child_window, x, y, w, h, TRUE);
}

static BOOL get_text(HWND field_window, WCHAR *buffer, size_t capacity) {
    return GetWindowTextW(field_window, buffer, (int)capacity) > 0;
}

static BOOL is_zip(LPCWSTR filename) {
    size_t n = wcslen(filename);
    return n >= 4 && _wcsicmp(filename + n - 4, L".zip") == 0;
}

static BOOL regular_file(LPCWSTR file) {
    DWORD attrs = GetFileAttributesW(file);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static BOOL choose_file(BOOL is_save, BOOL translation, WCHAR *path, size_t capacity) {
    IFileDialog *dialog = NULL;
    CLSID const *clsid = is_save ? &CLSID_FileSaveDialog : &CLSID_FileOpenDialog;
    HRESULT hr = CoCreateInstance(clsid, NULL, CLSCTX_INPROC_SERVER,
                                  &IID_IFileDialog, (void **)&dialog);
    if (FAILED(hr)) return FALSE;
    COMDLG_FILTERSPEC filters[1] = {
        {translation ? L"XLIFF catalog (*.xlf)" :
                       is_save ? L"Unsigned iOS package (*.ipa)" : L"Original ZIP (*.zip)",
         translation ? L"*.xlf" : is_save ? L"*.ipa" : L"*.zip"}
    };
    IFileDialog_SetFileTypes(dialog, 1, filters);
    IFileDialog_SetFileTypeIndex(dialog, 1);
    DWORD options = 0;
    IFileDialog_GetOptions(dialog, &options);
    options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
    if (!is_save) options |= FOS_FILEMUSTEXIST;
    IFileDialog_SetOptions(dialog, options);
    IFileDialog_SetTitle(dialog,
        translation ? L"Select private Hungarian hu.xlf" :
        is_save ? L"Save unsigned IPA" : L"Choose original Minion Rush 1.8.1g ZIP");
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

static void layout(int width, int height) {
    int margin = 30;
    int content = width - 2 * margin;
    int button_width = 148;
    int h = 32;
    int w = content - button_width - 10;
    if (w < 150) w = 150;
    place(source_field, margin, 128, w, h);
    place(browse_source, margin + w + 10, 128, button_width, h);
    place(destination_field, margin, 202, w, h);
    place(browse_destination, margin + w + 10, 202, button_width, h);
    place(catalog_field, margin, 276, w, h);
    place(browse_catalog, margin + w + 10, 276, button_width, h);
    place(build_button, margin, 336, 170, 39);
    place(cancel_button, margin + 184, 336, 100, 39);
    place(open_button, margin + 297, 336, 166, 39);
    place(progress_bar, margin, 387, content, 8);
    place(status_field, margin, 405, content, 28);
    place(log_field, margin, 435, content, height - 455);
}

static void append_log(WCHAR *line) {
    int size = GetWindowTextLengthW(log_field);
    if (size > 50000) {
        SetWindowTextW(log_field, L"Recent output:\r\n");
        size = GetWindowTextLengthW(log_field);
    }
    SendMessageW(log_field, EM_SETSEL, size, size);
    SendMessageW(log_field, EM_REPLACESEL, FALSE, (LPARAM)line);
    SendMessageW(log_field, EM_SCROLLCARET, 0, 0);
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
    WCHAR identifier[64] = {0};
    GUID guid;
    DWORD length = GetTempPathW(MAX_PATH * 4, temp);
    if (!length || length >= MAX_PATH * 4 || FAILED(CoCreateGuid(&guid)))
        return FALSE;
    if (!StringFromGUID2(&guid, identifier, 64)) return FALSE;
    if (FAILED(StringCchPrintfW(destination, capacity,
                                 L"%sMinionRush-private-%s", temp, identifier)))
        return FALSE;
    return CreateDirectoryW(destination, NULL) != FALSE;
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
    if (SUCCEEDED(hr) && job->catalog[0]) {
        hr = StringCchCatW(command, cap, L" --hungarian-xlf \"");
        if (SUCCEEDED(hr)) hr = StringCchCatW(command, cap, job->catalog);
        if (SUCCEEDED(hr)) hr = StringCchCatW(command, cap, L"\"");
    }
    if (SUCCEEDED(hr)) {
        hr = StringCchCatW(command, cap, L" --scratch-root \"");
        if (SUCCEEDED(hr)) hr = StringCchCatW(command, cap, job->scratch);
        if (SUCCEEDED(hr)) hr = StringCchCatW(command, cap, L"\"");
    }
    if (SUCCEEDED(hr) && CreateProcessW(
            job->python, command, NULL, NULL, TRUE,
            CREATE_NO_WINDOW | CREATE_SUSPENDED,
            NULL, job->kit, &startup, &process)) {
        job->job = CreateJobObjectW(NULL, NULL);
        if (job->job) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(job->job, JobObjectExtendedLimitInformation,
                                    &limits, sizeof(limits));
            if (!AssignProcessToJobObject(job->job, process.hProcess)) {
                const char *message = "Unable to protect child process tree.";
                send_log(job->window, message, (int)strlen(message));
                TerminateProcess(process.hProcess, 1);
            } else {
                PostMessageW(job->window, WM_JOB_READY, (WPARAM)job->job, 0);
                ResumeThread(process.hThread);
            }
        } else {
            TerminateProcess(process.hProcess, 1);
        }
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
    if (job->job) {
        CloseHandle(job->job);
        job->job = NULL;
    }
    remove_job_scratch(job->window, job->scratch);
    PostMessageW(job->window, WM_BUILD_DONE, result, 0);
    HeapFree(GetProcessHeap(), 0, job);
    return 0;
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
    if (!get_text(source_field, job->source, 32768) || !is_zip(job->source) ||
            !regular_file(job->source)) {
        MessageBoxW(main_window, L"Select an existing original .zip file.",
                    L"Source ZIP required", MB_OK | MB_ICONWARNING);
        goto failure;
    }
    if (!get_text(destination_field, job->output, 32768) ||
            wcslen(job->output) < 5) {
        MessageBoxW(main_window, L"Choose an output .ipa path.",
                    L"Output required", MB_OK | MB_ICONWARNING);
        goto failure;
    }
    size_t output_len = wcslen(job->output);
    if (output_len < 4 || _wcsicmp(job->output + output_len - 4, L".ipa") != 0) {
        MessageBoxW(main_window, L"The output filename must end in .ipa.",
                    L"Invalid filename", MB_OK | MB_ICONWARNING);
        goto failure;
    }
    if (GetFileAttributesW(job->output) != INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(main_window, L"This output already exists. Choose a different filename.",
                    L"Existing output", MB_OK | MB_ICONWARNING);
        goto failure;
    }
    get_text(catalog_field, job->catalog, 32768);
    if (job->catalog[0] && !regular_file(job->catalog)) {
        MessageBoxW(main_window, L"The selected hu.xlf catalog was not found.",
                    L"Missing translation", MB_OK | MB_ICONWARNING);
        goto failure;
    }
    if (!regular_file(job->python) || !regular_file(job->tool)) {
        MessageBoxW(main_window, L"The portable runtime is incomplete. Extract the full release ZIP.",
                    L"Missing application files", MB_OK | MB_ICONERROR);
        goto failure;
    }
    if (!create_job_scratch(job->scratch, 32768)) {
        MessageBoxW(main_window, L"Cannot create a private temporary workspace.",
                    L"Temporary storage unavailable", MB_OK | MB_ICONERROR);
        goto failure;
    }
    SetWindowTextW(log_field, L"");
    SetWindowTextW(status_field, L"Building native ARM64 IPA. Keep this window open.");
    EnableWindow(source_field, FALSE);
    EnableWindow(destination_field, FALSE);
    EnableWindow(catalog_field, FALSE);
    EnableWindow(browse_source, FALSE);
    EnableWindow(browse_destination, FALSE);
    EnableWindow(browse_catalog, FALSE);
    EnableWindow(build_button, FALSE);
    EnableWindow(open_button, FALSE);
    EnableWindow(cancel_button, TRUE);
    SendMessageW(progress_bar, PBM_SETMARQUEE, TRUE, 0);
    running = TRUE;
    aborted = FALSE;
    StringCchCopyW(last_output, 32768, job->output);
    HANDLE thread = CreateThread(NULL, 0, run_build, job, 0, NULL);
    if (thread) {
        CloseHandle(thread);
        return;
    }
    running = FALSE;
    SendMessageW(progress_bar, PBM_SETMARQUEE, FALSE, 0);
    EnableWindow(source_field, TRUE);
    EnableWindow(destination_field, TRUE);
    EnableWindow(catalog_field, TRUE);
    EnableWindow(browse_source, TRUE);
    EnableWindow(browse_destination, TRUE);
    EnableWindow(browse_catalog, TRUE);
    EnableWindow(build_button, TRUE);
    EnableWindow(cancel_button, FALSE);
    SetWindowTextW(status_field, L"Could not start build thread.");
    RemoveDirectoryW(job->scratch);
failure:
    HeapFree(GetProcessHeap(), 0, job);
}

static void stop_build(void) {
    if (!running || !active_job) return;
    int choice = MessageBoxW(main_window,
        L"Stop creating the IPA? The unfinished temporary files may need to be removed.",
        L"Cancel build", MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2);
    if (choice == IDYES) {
        aborted = TRUE;
        TerminateJobObject(active_job, 1);
        SetWindowTextW(status_field, L"Cancelling...");
        EnableWindow(cancel_button, FALSE);
    }
}

static LRESULT CALLBACK window_proc(HWND window, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_CREATE: {
        main_window = window;
        white_brush = CreateSolidBrush(RGB(255, 255, 255));
        HDC dc = GetDC(window);
        int dpi = GetDeviceCaps(dc, LOGPIXELSY);
        ReleaseDC(window, dc);
        ui_font = CreateFontW(-MulDiv(10, dpi, 72), 0, 0, 0,
                              FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH, L"Segoe UI");
        heading_font = CreateFontW(-MulDiv(19, dpi, 72), 0, 0, 0,
                                   FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                   OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        HWND title = child(L"STATIC", L"Minion Rush - native IPA builder", SS_LEFT, 0);
        font(title, heading_font);
        place(title, 30, 23, 650, 38);
        label(L"Build an unsigned iPhone IPA from your own original 1.8.1g ZIP.",30,70,690,23);
        label(L"Original game ZIP (drag a ZIP onto this window or browse)", 30, 106, 690, 22);
        label(L"Save unsigned IPA to",30,180,400,22);
        label(L"Hungarian translation - optional hu.xlf (can be auto-detected)",30,254,700,22);
        field(&source_field, ID_SOURCE, L"Choose your original ZIP...");
        field(&destination_field, ID_DESTINATION, L"Choose where to save...");
        field(&catalog_field, ID_CATALOG, L"");
        browse_source = child(L"BUTTON",L"Browse ZIP...", BS_PUSHBUTTON | WS_TABSTOP, ID_PICK_SOURCE);
        browse_destination = child(L"BUTTON",L"Save As...", BS_PUSHBUTTON | WS_TABSTOP, ID_PICK_DESTINATION);
        browse_catalog = child(L"BUTTON",L"Choose hu.xlf...", BS_PUSHBUTTON | WS_TABSTOP, ID_PICK_CATALOG);
        build_button = child(L"BUTTON",L"Create IPA", BS_DEFPUSHBUTTON | WS_TABSTOP, ID_BUILD);
        cancel_button = child(L"BUTTON",L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, ID_CANCEL);
        open_button = child(L"BUTTON",L"Open output folder", BS_PUSHBUTTON | WS_TABSTOP, ID_OPEN_FOLDER);
        EnableWindow(cancel_button, FALSE);
        EnableWindow(open_button, FALSE);
        progress_bar = child(PROGRESS_CLASSW, L"", PBS_MARQUEE, ID_PROGRESS);
        status_field = child(L"STATIC", L"Ready. The output is unsigned and requires Sideloadly.", SS_LEFT, ID_STATE);
        log_field = child(L"EDIT", L"", WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL |
                          ES_READONLY | WS_VSCROLL, ID_LOG);
        DragAcceptFiles(window, TRUE);
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wparam;
        SetBkColor(dc, RGB(255, 255, 255));
        SetTextColor(dc, RGB(30, 39, 55));
        return (LRESULT)white_brush;
    }
    case WM_ERASEBKGND: {
        RECT r;
        GetClientRect(window, &r);
        FillRect((HDC)wparam, &r, white_brush);
        return 1;
    }
    case WM_GETMINMAXINFO: {
        MINMAXINFO *limits = (MINMAXINFO *)lparam;
        limits->ptMinTrackSize.x = 790;
        limits->ptMinTrackSize.y = 610;
        return 0;
    }
    case WM_SIZE:
        if (source_field) layout(LOWORD(lparam), HIWORD(lparam));
        return 0;
    case WM_DROPFILES: {
        HDROP drop = (HDROP)wparam;
        WCHAR selected[32768] = {0};
        if (!running && DragQueryFileW(drop, 0xFFFFFFFF, NULL, 0) == 1 &&
            DragQueryFileW(drop, 0, selected, 32768) && is_zip(selected) && regular_file(selected))
            SetWindowTextW(source_field, selected);
        else if (!running) MessageBoxW(window,L"Drop exactly one original .zip file.",
                                       L"ZIP required", MB_OK | MB_ICONWARNING);
        DragFinish(drop);
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case ID_PICK_SOURCE: {
            WCHAR selection[32768];
            if (choose_file(FALSE, FALSE, selection, 32768))
                SetWindowTextW(source_field, selection);
            return 0;
        }
        case ID_PICK_DESTINATION: {
            WCHAR selection[32768];
            if (choose_file(TRUE, FALSE, selection, 32768))
                SetWindowTextW(destination_field, selection);
            return 0;
        }
        case ID_PICK_CATALOG: {
            WCHAR selection[32768];
            if (choose_file(FALSE, TRUE, selection, 32768))
                SetWindowTextW(catalog_field, selection);
            return 0;
        }
        case ID_BUILD: start_build(); return 0;
        case ID_CANCEL: stop_build(); return 0;
        case ID_OPEN_FOLDER:
            if (last_output[0]) {
                WCHAR explorer[32768];
                StringCchPrintfW(explorer, 32768, L"/select,\"%s\"", last_output);
                ShellExecuteW(window, L"open", L"explorer.exe", explorer, NULL, SW_SHOWNORMAL);
            }
            return 0;
        }
        break;
    case WM_JOB_READY:
        active_job = (HANDLE)wparam;
        return 0;
    case WM_APPEND_LOG:
        append_log((WCHAR *)lparam);
        HeapFree(GetProcessHeap(), 0, (LPVOID)lparam);
        return 0;
    case WM_BUILD_DONE:
        running = FALSE;
        active_job = NULL;
        SendMessageW(progress_bar, PBM_SETMARQUEE, FALSE, 0);
        EnableWindow(source_field, TRUE);
        EnableWindow(destination_field, TRUE);
        EnableWindow(catalog_field, TRUE);
        EnableWindow(browse_source, TRUE);
        EnableWindow(browse_destination, TRUE);
        EnableWindow(browse_catalog, TRUE);
        EnableWindow(build_button, TRUE);
        EnableWindow(cancel_button, FALSE);
        if (wparam == 0 && regular_file(last_output)) {
            SetWindowTextW(status_field, L"Unsigned IPA created. Sign with Sideloadly before installing.");
            EnableWindow(open_button, TRUE);
            MessageBoxW(window, L"Unsigned IPA created successfully. It is ready for signing with Sideloadly.",
                        L"Build complete", MB_OK | MB_ICONINFORMATION);
        } else {
            SetWindowTextW(status_field, aborted ? L"Build cancelled." :
                            L"Build failed. Read the details above.");
        }
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
        if (ui_font) DeleteObject(ui_font);
        if (heading_font) DeleteObject(heading_font);
        if (white_brush) DeleteObject(white_brush);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, msg, wparam, lparam);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE unused, PWSTR arguments, int show) {
    (void)unused;
    BOOL self_test = wcscmp(arguments, L"--self-test") == 0;
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
    if (FAILED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED))) return 1;
    INITCOMMONCONTROLSEX controls = {sizeof(controls), ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&controls);
    WNDCLASSEXW cls = {0};
    cls.cbSize = sizeof(cls);
    cls.hInstance = instance;
    cls.lpfnWndProc = window_proc;
    cls.lpszClassName = L"MinionRushNativeIpaBuilder";
    cls.hCursor = LoadCursorW(NULL, IDC_ARROW);
    cls.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    cls.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    if (!RegisterClassExW(&cls)) return 1;
    HWND window = CreateWindowExW(0, cls.lpszClassName, L"Minion Rush IPA Builder",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 810, 650,
        NULL, NULL, instance, NULL);
    if (!window) return 1;
    ShowWindow(window, show);
    UpdateWindow(window);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CoUninitialize();
    return (int)msg.wParam;
}
