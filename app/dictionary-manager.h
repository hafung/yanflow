#pragma once
#include <windows.h>
#include <string>

namespace yanflow {
enum class DictionaryScope { General, Development, Application };
void showDictionaryManager(HWND owner, HWND& activeWindow, const std::wstring& userDirectory,
    const std::wstring& packageDirectory, const std::wstring& application, DictionaryScope initialScope);
int runDictionaryManagerSmoke();
}
