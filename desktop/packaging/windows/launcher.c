// SPDX-License-Identifier: GPL-3.0-only
// Local Windows GUI launcher; no shell, installation, registry or network changes.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <wchar.h>
#include <stdio.h>

static int fail(const wchar_t *message) {
    MessageBoxW(NULL, message, L"Quadband Card Studio", MB_OK | MB_ICONERROR);
    return 1;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, LPWSTR args, int show) {
    (void)instance; (void)previous; (void)show;
    wchar_t folder[32768], python[32768], script[32768], command[32768];
    DWORD size = GetModuleFileNameW(NULL, folder, 32768);
    if (!size || size >= 32768) return fail(L"The application path is too long.");
    wchar_t *slash = wcsrchr(folder, L'\\');
    if (!slash) return fail(L"Cannot locate the application folder.");
    *slash = 0;
    if (wcslen(folder) > 15000) return fail(L"Move the app to a shorter folder path.");
    swprintf(python, 32768, L"%ls\\runtime\\pythonw.exe", folder);
    swprintf(script, 32768, L"%ls\\app\\launch.py", folder);
    if (GetFileAttributesW(python) == INVALID_FILE_ATTRIBUTES || GetFileAttributesW(script) == INVALID_FILE_ATTRIBUTES)
        return fail(L"Extract the entire Quadband ZIP first. Keep the runtime and app folders beside this executable.");
    // Only the documented self-test flag is forwarded; no command shell is involved.
    int smoke = args && wcscmp(args, L"--smoke-test") == 0;
    int length = swprintf(command, 32768, L"\"%ls\" -I \"%ls\"%ls", python, script, smoke ? L" --smoke-test" : L"");
    if (length < 0 || length >= 32768) return fail(L"The application path is too long.");
    STARTUPINFOW start = {0};
    PROCESS_INFORMATION process = {0};
    start.cb = sizeof(start);
    if (!CreateProcessW(python, command, NULL, NULL, FALSE, 0, NULL, folder, &start, &process))
        return fail(L"Windows could not start the bundled runtime. Extract the entire ZIP to a local folder and try again.");
    CloseHandle(process.hThread);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD result = 1;
    GetExitCodeProcess(process.hProcess, &result);
    CloseHandle(process.hProcess);
    return (int)result;
}
