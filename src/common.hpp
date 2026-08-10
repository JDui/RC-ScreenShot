#pragma once

#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace rc {

using Microsoft::WRL::ComPtr;

inline std::wstring HResultMessage(HRESULT hr) {
  wchar_t* raw = nullptr;
  const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                      FORMAT_MESSAGE_IGNORE_INSERTS;
  FormatMessageW(flags, nullptr, static_cast<DWORD>(hr), 0,
                 reinterpret_cast<wchar_t*>(&raw), 0, nullptr);
  std::wstring message = raw ? raw : L"Unknown error";
  if (raw) LocalFree(raw);
  while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n')) {
    message.pop_back();
  }
  return message;
}

inline std::string ToUtf8(std::wstring_view value) {
  if (value.empty()) return {};
  const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                       nullptr, 0, nullptr, nullptr);
  std::string result(static_cast<size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size,
                      nullptr, nullptr);
  return result;
}

inline std::wstring FromUtf8(std::string_view value) {
  if (value.empty()) return {};
  const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                       static_cast<int>(value.size()), nullptr, 0);
  if (size <= 0) return {};
  std::wstring result(static_cast<size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                      static_cast<int>(value.size()), result.data(), size);
  return result;
}

inline RECT NormalizeRect(POINT a, POINT b) {
  return {std::min(a.x, b.x), std::min(a.y, b.y), std::max(a.x, b.x), std::max(a.y, b.y)};
}

inline bool IsEmptyRect(const RECT& rect) {
  return rect.right <= rect.left || rect.bottom <= rect.top;
}

inline bool Contains(const RECT& rect, POINT point) {
  return point.x >= rect.left && point.x < rect.right && point.y >= rect.top && point.y < rect.bottom;
}

inline uint8_t ClampByte(float value) {
  return static_cast<uint8_t>(std::clamp(std::lround(value), 0L, 255L));
}

struct ScopeExit {
  std::function<void()> callback;
  ~ScopeExit() { if (callback) callback(); }
};

}  // namespace rc
