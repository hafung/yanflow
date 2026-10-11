#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include <cwchar>

#include "yanflow.h"
#include "text-correction.h"
#include "text-pipeline.h"
#include "audio-segmentation.h"
#include "dictionary-store.h"
#include "text-delivery.h"

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    int argumentCount = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (arguments == nullptr) return 2;
    if (argumentCount == 2 && std::wcscmp(arguments[1], L"--csc-pipeline-smoke") == 0) {
        LocalFree(arguments); return yanflow::runCscPipelineSmoke();
    }
    if (argumentCount == 2 && std::wcscmp(arguments[1], L"--text-delivery-smoke") == 0) {
        LocalFree(arguments); return yanflow::runTextDeliverySmoke();
    }
    if (argumentCount >= 2 && std::wcscmp(arguments[1], L"--correct-text") == 0) {
        const int result = argumentCount == 4 || (argumentCount == 5 && std::wcscmp(arguments[4], L"--macbert") == 0)
            ? yanflow::correctTextFile(arguments[2], arguments[3], argumentCount == 5) : 2;
        LocalFree(arguments);
        return result;
    }

    int smokeMilliseconds = 0;
    bool asrSmoke = false;
    bool textSmoke = false;
    bool accuracySmoke = false;
    if (argumentCount > 1) {
        const wchar_t* option = arguments[1];
        if (std::wcscmp(option, L"--asr-smoke") == 0) asrSmoke = true;
        else if (std::wcscmp(option, L"--text-smoke") == 0) textSmoke = true;
        else if (std::wcscmp(option, L"--accuracy-smoke") == 0) accuracySmoke = true;
        else if (std::wcscmp(option, L"--smoke") == 0) smokeMilliseconds = 800;
        else if (std::wcscmp(option, L"--e2e-smoke") == 0) smokeMilliseconds = -1;
        else if (std::wcscmp(option, L"--fallback-smoke") == 0) smokeMilliseconds = -2;
        else if (std::wcscmp(option, L"--capture-smoke") == 0) smokeMilliseconds = -3;
        else if (std::wcscmp(option, L"--pipeline-smoke") == 0) smokeMilliseconds = -4;
        else if (std::wcscmp(option, L"--readme-demo") == 0) smokeMilliseconds = -5;
        else if (std::wcscmp(option, L"--idle-smoke") == 0) smokeMilliseconds = -6;
        else if (std::wcscmp(option, L"--hold-pipeline-smoke") == 0) smokeMilliseconds = -7;
        else if (std::wcscmp(option, L"--learn-smoke") == 0) smokeMilliseconds = -8;
        else if (std::wcscmp(option, L"--short-hold-smoke") == 0) smokeMilliseconds = -9;
        else if (std::wcscmp(option, L"--long-hold-smoke") == 0) smokeMilliseconds = -10;
        else if (std::wcscmp(option, L"--long-stream-smoke") == 0) smokeMilliseconds = -11;
        else if (std::wcscmp(option, L"--ui-preview-smoke") == 0) smokeMilliseconds = -12;
        else if (std::wcscmp(option, L"--ui-state-smoke") == 0) smokeMilliseconds = -13;
    }
    LocalFree(arguments);

    if (accuracySmoke) {
        const int result = yanflow::runAudioSegmentationSmoke();
        return result ? result : yanflow::runDictionaryStoreSmoke();
    }

    return textSmoke ? yanflow::runTextCorrectionSmoke() : asrSmoke ? runAsrSmoke() : runYanFlow(smokeMilliseconds);
}
