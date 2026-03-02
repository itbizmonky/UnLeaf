#pragma once
#include <string>
#include <windows.h>

namespace unleaf {
    std::wstring Utf8ToWide(const char* s);
    std::string  WideToUtf8(const wchar_t* s);
}
