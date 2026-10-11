#pragma once
#include "text-correction.h"
#include <string>
#include <vector>

namespace yanflow {
enum class DictionaryProfile { Automatic, General, Development };
std::wstring applicationName(const std::wstring& executablePath);
bool developmentApplication(const std::wstring& application);
std::vector<std::wstring> dictionaryOverlays(const std::wstring& userDirectory,
    const std::wstring& packageDirectory, const std::wstring& application, DictionaryProfile profile);
bool rememberDictionaryRule(const std::wstring& path, const DictionaryRule& rule, std::wstring& error);
bool validateDictionaryRule(const DictionaryRule& rule, std::wstring& error);
// Compare under the same file lock used by quick corrections, then replace atomically.
bool saveDictionaryDocument(const std::wstring& path, bool expectedExists,
    const std::wstring& expected, const std::wstring& updated, std::wstring& error);
int runDictionaryStoreSmoke();
}
