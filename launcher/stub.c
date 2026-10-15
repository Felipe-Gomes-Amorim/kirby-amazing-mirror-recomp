/* KirbyRecomp.exe - tiny GUI-subsystem stub: runs python\pythonw.exe app\launcher\launcher.py <args>.
 * Pass --console to use python.exe with a console window (diagnostics). The exit code is forwarded. */
#include <windows.h>
#include <wchar.h>
#include <stdlib.h>

int WINAPI wWinMain(HINSTANCE h, HINSTANCE p, LPWSTR cmd, int show) {
    wchar_t exe[MAX_PATH * 2], dir[MAX_PATH * 2];
    (void)h; (void)p; (void)show;
    GetModuleFileNameW(NULL, exe, MAX_PATH * 2);
    wcscpy(dir, exe);
    wchar_t *s = wcsrchr(dir, L'\\');
    if (s) *s = 0;
    int console = wcsstr(cmd, L"--console") != NULL;
    size_t cap = wcslen(cmd) + MAX_PATH * 6;
    wchar_t *line = (wchar_t *)malloc(cap * sizeof(wchar_t));
    swprintf(line, cap, L"\"%ls\\python\\%ls\" \"%ls\\app\\launcher\\launcher.py\" %ls",
             dir, console ? L"python.exe" : L"pythonw.exe", dir, cmd);
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    if (!CreateProcessW(NULL, line, NULL, NULL, FALSE, console ? 0 : CREATE_NO_WINDOW, NULL, dir, &si, &pi)) {
        MessageBoxW(NULL, L"Could not start the bundled Python (is the 'python' folder missing, or still being scanned by your antivirus?).",
                    L"Kirby Recomp", MB_ICONERROR);
        return 1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    return (int)code;
}
