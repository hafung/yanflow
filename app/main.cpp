#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include <cwchar>

#include "yanflow.h"

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    int argumentCount = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (arguments == nullptr) return 2;

    int smokeMilliseconds = 0;
    bool asrSmoke = false;
    if (argumentCount > 1) {
        const wchar_t* option = arguments[1];
        if (std::wcscmp(option, L"--asr-smoke") == 0) asrSmoke = true;
        else if (std::wcscmp(option, L"--smoke") == 0) smokeMilliseconds = 800;
        else if (std::wcscmp(option, L"--e2e-smoke") == 0) smokeMilliseconds = -1;
        else if (std::wcscmp(option, L"--fallback-smoke") == 0) smokeMilliseconds = -2;
        else if (std::wcscmp(option, L"--capture-smoke") == 0) smokeMilliseconds = -3;
        else if (std::wcscmp(option, L"--pipeline-smoke") == 0) smokeMilliseconds = -4;
        else if (std::wcscmp(option, L"--readme-demo") == 0) smokeMilliseconds = -5;
        else if (std::wcscmp(option, L"--idle-smoke") == 0) smokeMilliseconds = -6;
    }
    LocalFree(arguments);

    return asrSmoke ? runAsrSmoke() : runYanFlow(smokeMilliseconds);
}
