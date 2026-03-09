#include "win_string_utils.h"

namespace unleaf {

// UTF-8 → UTF-16 変換
//
// 設計メモ:
//   第1呼び出し(cchWideChar=0) は null 終端込みの必要サイズ len を返す。
//   len<=0 は変換不能（無効入力）として即座に失敗させる。
//   w(len) で null 終端込みのサイズを確保し、w.data() へ書き込む。
//   cbMultiByte=-1: null 終端文字列として扱い UTF-8 シーケンスの途中切断を防ぐ。
//   MB_ERR_INVALID_CHARS: 不正な UTF-8 シーケンスを検出して失敗させる。
//   cchWideChar=len: null 終端を含む書き込みサイズを渡す（len-1 では不足して失敗する）。
//   変換後 resize(len-1) で API が書いた null 終端を除去する。
//   変換失敗時(written<=0) は安全フォールバック文字列を返す。
std::wstring Utf8ToWide(const char* s)
{
    if (!s) return L"(null)";
    int len = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (len <= 0) return L"(conv_error)";
    if (len == 1) return L"";                          // 空文字列（null のみ）
    std::wstring w(static_cast<size_t>(len), L'\0');
    int written = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1,
                                      w.data(),
                                      static_cast<int>(len));
    if (written <= 0) return L"(conv_error)";
    w.resize(static_cast<size_t>(len - 1));            // API が書いた null 終端を除去
    return w;
}

// UTF-16 → UTF-8 変換
//   cbMultiByte = -1: null 終端文字列として扱う
//   utf8(len) で null 終端込みサイズ確保、resize(len-1) で除去
std::string WideToUtf8(const wchar_t* s)
{
    if (!s) return "(null)";
    int len = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "(conv_error)";
    if (len == 1) return "";
    std::string utf8(static_cast<size_t>(len), '\0');
    int written = WideCharToMultiByte(CP_UTF8, 0, s, -1,
                                      utf8.data(),
                                      static_cast<int>(len),
                                      nullptr, nullptr);
    if (written <= 0) return "(conv_error)";
    utf8.resize(static_cast<size_t>(len - 1));
    return utf8;
}

} // namespace unleaf
