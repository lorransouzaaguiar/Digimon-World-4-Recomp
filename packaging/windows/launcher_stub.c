/**
 * DigimonWorld4-Recomp.exe: a entrada do pacote de Windows.
 *
 * Abre o launcher (launcher\dw4_launcher.py) com o Python embutido do pacote
 * (python\pythonw.exe, sem janela de console) e repassa os argumentos. Define
 * DW4_PACKAGED (dados do usuario em %APPDATA%\dw4-recomp) e DW4_APP_DIR (onde
 * esta o executavel do jogo). Sai com o codigo de saida do launcher.
 *
 * Compilado pelo scripts/package_win.sh (MinGW-w64, -municode -mwindows).
 */
#include <windows.h>
#include <wchar.h>

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR args, int show)
{
    static wchar_t dir[32768], path[32768], cmd[32768 + 1024];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD rc = 1, n;
    wchar_t *slash;

    (void)inst; (void)prev; (void)show;
    n = GetModuleFileNameW(NULL, dir, 32767);
    slash = n ? wcsrchr(dir, L'\\') : NULL;
    if (!slash)
        return 1;
    *slash = 0;

    swprintf(path, 32768, L"%ls\\game", dir);
    SetEnvironmentVariableW(L"DW4_APP_DIR", path);
    SetEnvironmentVariableW(L"DW4_PACKAGED", L"1");
    /* O pacote usa so o que traz: nada do Python instalado na maquina. */
    SetEnvironmentVariableW(L"PYTHONHOME", NULL);
    SetEnvironmentVariableW(L"PYTHONPATH", NULL);

    swprintf(path, 32768, L"%ls\\python\\pythonw.exe", dir);
    swprintf(cmd, 32768 + 1024, L"\"%ls\" \"%ls\\launcher\\dw4_launcher.py\" %ls",
             path, dir, args ? args : L"");

    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    if (!CreateProcessW(path, cmd, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) {
        MessageBoxW(NULL,
                    L"Não foi possível abrir o launcher (python\\pythonw.exe).\n"
                    L"Extraia o .zip inteiro antes de abrir.\n\n"
                    L"Could not start the launcher (python\\pythonw.exe).\n"
                    L"Extract the whole .zip before opening.",
                    L"Digimon World 4 - Recomp", MB_ICONERROR);
        return 1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)rc;
}
