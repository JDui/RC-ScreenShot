#include "app.hpp"

#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>

#include <climits>
#include <cstdlib>
#include <cwctype>
#include <sstream>

namespace rc {
namespace {

constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kOverlayDone = WM_APP + 2;
constexpr UINT kDeferredCapture = WM_APP + 3;
constexpr UINT kCommandCapture = 100;
constexpr UINT kCommandSettings = 101;
constexpr UINT kCommandAutoStart = 102;
constexpr UINT kCommandExit = 103;
constexpr UINT kCommandAbout = 104;
constexpr int kHotkeyBase = 1000;

constexpr int IDC_OUTPUT = 2002;
constexpr int IDC_BROWSE = 2003;
constexpr int IDC_SAVE_SETTINGS = 2011;
constexpr int IDC_CANCEL_SETTINGS = 2012;
constexpr int IDC_RESET_SETTINGS = 2013;
constexpr int IDC_HOTKEY_PRIMARY = 2016;
constexpr int IDC_HOTKEY_SECONDARY = 2017;
constexpr int IDC_BURST_COUNT = 2018;
constexpr int IDC_BURST_INTERVAL = 2019;
constexpr int IDC_TOGGLE_AUTOSAVE = 2101;
constexpr int IDC_TOGGLE_AUTOSTART = 2102;
constexpr int IDC_TOGGLE_SILENT = 2103;
constexpr int IDC_TOGGLE_SHADOW = 2104;
constexpr int IDC_TOGGLE_FRAME = 2105;
constexpr int IDC_ACTION_COPY = 2106;
constexpr int IDC_ACTION_SAVE = 2107;
constexpr int IDC_BURST_COUNT_MINUS = 2020;
constexpr int IDC_BURST_COUNT_PLUS = 2021;
constexpr int IDC_BURST_INTERVAL_MINUS = 2022;
constexpr int IDC_BURST_INTERVAL_PLUS = 2023;

constexpr int kSettingsWidth = 820;
constexpr int kSettingsHeight = 690;

// Matte graphite-and-blue palette shared with the original floating toolbar.
constexpr COLORREF kBackground = RGB(11, 16, 26);
constexpr COLORREF kBackgroundEnd = RGB(14, 21, 34);
constexpr COLORREF kCardStart = RGB(23, 33, 51);
constexpr COLORREF kCardEnd = RGB(16, 24, 38);
constexpr COLORREF kCardBorder = RGB(41, 55, 80);
constexpr COLORREF kControlFill = RGB(27, 39, 59);
constexpr COLORREF kControlBorder = RGB(47, 65, 95);
constexpr COLORREF kControlHover = RGB(33, 47, 70);
constexpr COLORREF kControlHoverBorder = RGB(72, 97, 138);
constexpr COLORREF kAccent = RGB(76, 141, 255);
constexpr COLORREF kAccentHover = RGB(96, 155, 255);
constexpr COLORREF kAccentPressed = RGB(55, 110, 205);
constexpr COLORREF kAccentBorder = RGB(132, 176, 255);
constexpr COLORREF kTextBright = RGB(240, 246, 255);
constexpr COLORREF kTextNormal = RGB(206, 216, 232);
constexpr COLORREF kTextLabel = RGB(162, 182, 206);
constexpr COLORREF kTextDim = RGB(120, 140, 168);

// Card geometry used by both control placement and WM_PAINT chrome.
constexpr RECT kShortcutCard = {18, 92, 802, 276};
constexpr RECT kOutputCard = {18, 292, 402, 530};
constexpr RECT kEditorCard = {418, 292, 802, 530};
constexpr RECT kBehaviorCard = {18, 546, 802, 628};

RECT QualitySliderRect() {
  return {130, 437, 330, 449};
}

// COLOR16 channel for GradientFill vertices.  Extracts the channel without
// the narrowing intermediate casts that make GetXValue warn on constants.
constexpr COLOR16 Channel16(COLORREF value, int shift) {
  return static_cast<COLOR16>(((value >> shift) & 0xFF) << 8);
}

// Blend a flat tint through GDI's alpha compositor. A one-pixel 32-bit DIB is
// stretched over the destination for simple translucent control fills.
void BlendFill(HDC dc, const RECT& rect, COLORREF color, BYTE opacity) {
  const int width = rect.right - rect.left;
  const int height = rect.bottom - rect.top;
  if (!dc || width <= 0 || height <= 0 || opacity == 0) return;

  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(info.bmiHeader);
  info.bmiHeader.biWidth = 1;
  info.bmiHeader.biHeight = 1;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  void* pixels = nullptr;
  HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
  HDC source = CreateCompatibleDC(dc);
  if (!bitmap || !source || !pixels) {
    if (source) DeleteDC(source);
    if (bitmap) DeleteObject(bitmap);
    HBRUSH fallback = CreateSolidBrush(color);
    if (fallback) { FillRect(dc, &rect, fallback); DeleteObject(fallback); }
    return;
  }

  *static_cast<DWORD*>(pixels) = 0xFF000000u |
      (static_cast<DWORD>(GetRValue(color)) << 16) |
      (static_cast<DWORD>(GetGValue(color)) << 8) |
      static_cast<DWORD>(GetBValue(color));
  HGDIOBJ oldBitmap = SelectObject(source, bitmap);
  const BLENDFUNCTION blend{AC_SRC_OVER, 0, opacity, 0};
  AlphaBlend(dc, rect.left, rect.top, width, height, source, 0, 0, 1, 1, blend);
  SelectObject(source, oldBitmap);
  DeleteDC(source);
  DeleteObject(bitmap);
}

void DrawPanelButton(HDC dc, const RECT& rect, COLORREF fill, COLORREF border,
                     int cornerRadius, BYTE opacity = 232) {
  HRGN clip = CreateRoundRectRgn(rect.left, rect.top, rect.right + 1, rect.bottom + 1,
                                 cornerRadius, cornerRadius);
  const int saved = SaveDC(dc);
  SelectClipRgn(dc, clip);
  BlendFill(dc, rect, fill, opacity);
  RestoreDC(dc, saved);
  DeleteObject(clip);

  HPEN pen = CreatePen(PS_SOLID, 1, border);
  HGDIOBJ oldPen = SelectObject(dc, pen);
  HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
  RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, cornerRadius, cornerRadius);
  SelectObject(dc, oldPen);
  SelectObject(dc, oldBrush);
  DeleteObject(pen);
}

bool IsToggleId(int id) {
  return id == IDC_TOGGLE_AUTOSAVE || id == IDC_TOGGLE_AUTOSTART || id == IDC_TOGGLE_SILENT ||
         id == IDC_TOGGLE_SHADOW || id == IDC_TOGGLE_FRAME;
}

bool IsHotkeyId(int id) { return id == IDC_HOTKEY_PRIMARY || id == IDC_HOTKEY_SECONDARY; }

bool IsStepId(int id) {
  return id == IDC_BURST_COUNT_MINUS || id == IDC_BURST_COUNT_PLUS ||
         id == IDC_BURST_INTERVAL_MINUS || id == IDC_BURST_INTERVAL_PLUS;
}

UINT HotkeyModifierForKey(UINT key) {
  switch (key) {
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: return MOD_CONTROL;
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: return MOD_SHIFT;
    case VK_MENU: case VK_LMENU: case VK_RMENU: return MOD_ALT;
    case VK_LWIN: case VK_RWIN: return MOD_WIN;
    default: return 0;
  }
}

UINT CurrentHotkeyModifiers() {
  UINT modifiers = 0;
  if (GetKeyState(VK_CONTROL) & 0x8000) modifiers |= MOD_CONTROL;
  if (GetKeyState(VK_SHIFT) & 0x8000) modifiers |= MOD_SHIFT;
  if (GetKeyState(VK_MENU) & 0x8000) modifiers |= MOD_ALT;
  if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) modifiers |= MOD_WIN;
  return modifiers;
}

std::wstring HotkeyModifierPreview(UINT modifiers) {
  std::wstring text;
  if (modifiers & MOD_CONTROL) text += L"Ctrl+";
  if (modifiers & MOD_ALT) text += L"Alt+";
  if (modifiers & MOD_SHIFT) text += L"Shift+";
  if (modifiers & MOD_WIN) text += L"Win+";
  return text + L"…";
}

bool IsSegmentId(int id) {
  return id == IDC_ACTION_COPY || id == IDC_ACTION_SAVE;
}

bool IsSettingsEditId(int id) {
  return id == IDC_HOTKEY_PRIMARY || id == IDC_HOTKEY_SECONDARY || id == IDC_OUTPUT ||
         id == IDC_BURST_COUNT || id == IDC_BURST_INTERVAL;
}

RECT SettingsInputFrameRect(int id) {
  switch (id) {
    // Borderless edits get a 1px-outset rounded frame from WM_PAINT; the
    // owner-drawn hotkey buttons carry their own border so their entries just
    // describe the button bounds for focus invalidation.
    case IDC_HOTKEY_PRIMARY: return {42, 165, 388, 211};
    case IDC_HOTKEY_SECONDARY: return {426, 165, 772, 211};
    case IDC_BURST_COUNT: return {171, 224, 225, 256};
    case IDC_BURST_INTERVAL: return {551, 224, 605, 256};
    case IDC_OUTPUT: return {112, 370, 310, 404};
    default: return {};
  }
}

bool ToggleValue(const AppConfig& config, int id) {
  switch (id) {
    case IDC_TOGGLE_AUTOSAVE: return config.autoSaveOnCopy;
    case IDC_TOGGLE_AUTOSTART: return config.launchAtLogin;
    case IDC_TOGGLE_SILENT: return config.silentAtLogin;
    case IDC_TOGGLE_SHADOW: return config.windowShadow;
    case IDC_TOGGLE_FRAME: return config.frameEnabled;
  }
  return false;
}

void SetToggleValue(AppConfig& config, int id, bool value) {
  switch (id) {
    case IDC_TOGGLE_AUTOSAVE: config.autoSaveOnCopy = value; break;
    case IDC_TOGGLE_AUTOSTART: config.launchAtLogin = value; break;
    case IDC_TOGGLE_SILENT: config.silentAtLogin = value; break;
    case IDC_TOGGLE_SHADOW: config.windowShadow = value; break;
    case IDC_TOGGLE_FRAME: config.frameEnabled = value; break;
  }
}

std::wstring GetWindowString(HWND hwnd, int id) {
  HWND control = GetDlgItem(hwnd, id);
  const int length = GetWindowTextLengthW(control);
  std::wstring value(static_cast<size_t>(length), L'\0');
  GetWindowTextW(control, value.data(), length + 1);
  return value;
}

void SetWindowString(HWND hwnd, int id, std::wstring_view value) {
  SetWindowTextW(GetDlgItem(hwnd, id), std::wstring(value).c_str());
}

}  // namespace

Application::Application(HINSTANCE instance)
    : instance_(instance),
      executablePath_([] { wchar_t path[MAX_PATH]{}; GetModuleFileNameW(nullptr, path, MAX_PATH); return std::filesystem::path(path); }()),
      configStore_(executablePath_) {}

Application::~Application() {
  UnregisterHotkeys(); RemoveTrayIcon();
  if (settingsWindow_) DestroyWindow(settingsWindow_);
  if (settingsBackgroundBrush_) DeleteObject(settingsBackgroundBrush_);
  if (settingsPanelBrush_) DeleteObject(settingsPanelBrush_);
  if (settingsControlBrush_) DeleteObject(settingsControlBrush_);
  if (settingsFont_) DeleteObject(settingsFont_);
  if (settingsTitleFont_) DeleteObject(settingsTitleFont_);
  if (settingsSectionFont_) DeleteObject(settingsSectionFont_);
  if (settingsSmallFont_) DeleteObject(settingsSmallFont_);
  if (settingsHintFont_) DeleteObject(settingsHintFont_);
  overlay_.reset();
  if (hwnd_) DestroyWindow(hwnd_);
  if (mutex_) CloseHandle(mutex_);
}

bool Application::Initialize(std::span<wchar_t*> arguments, int, std::wstring& error) {
  mutex_ = CreateMutexW(nullptr, FALSE, L"Local\\RC-ScreenShot.Singleton.v1");
  if (!mutex_) { error = L"创建单实例互斥量失败。"; return false; }
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    HWND existing = FindWindowW(kWindowClass, nullptr);
    if (existing) {
      std::wstring command = HasArgument(arguments, L"--settings") ? L"--settings" : L"--capture";
      COPYDATASTRUCT data{1, static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)), command.data()};
      SendMessageTimeoutW(existing, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data),
                          SMTO_ABORTIFHUNG, 2000, nullptr);
    }
    return false;
  }
  std::wstring warning;
  config_ = configStore_.Load(&warning);
  if (!CreateMessageWindow(error)) return false;
  AddTrayIcon(); RegisterConfiguredHotkeys(); UpdateAutoStart();
  if (!std::filesystem::exists(configStore_.path()) || config_.schemaVersion < 4) SaveConfig();
  if (!warning.empty()) Notify(L"RC-ScreenShot 配置", warning, NIIF_WARNING);
  if (!hotkeyErrors_.empty()) Notify(L"快捷键注册失败", hotkeyErrors_.front(), NIIF_WARNING);
  if (HasArgument(arguments, L"--settings")) PostMessageW(hwnd_, WM_COMMAND, kCommandSettings, 0);
  else if (HasArgument(arguments, L"--capture")) PostMessageW(hwnd_, kDeferredCapture, 0, 0);
  return true;
}

int Application::Run() {
  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    if (!settingsWindow_ || !IsDialogMessageW(settingsWindow_, &message)) {
      TranslateMessage(&message); DispatchMessageW(&message);
    }
  }
  return static_cast<int>(message.wParam);
}

bool Application::CreateMessageWindow(std::wstring& error) {
  WNDCLASSEXW windowClass{sizeof(windowClass)};
  windowClass.lpfnWndProc = WindowProc; windowClass.hInstance = instance_;
  windowClass.lpszClassName = kWindowClass;
  windowClass.hIcon = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(102), IMAGE_ICON,
                                                    0, 0, LR_DEFAULTSIZE | LR_SHARED));
  windowClass.hIconSm = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(102), IMAGE_ICON,
                                                      16, 16, LR_SHARED));
  RegisterClassExW(&windowClass);
  hwnd_ = CreateWindowExW(0, kWindowClass, nullptr, 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance_, this);
  if (!hwnd_) { error = L"创建应用消息窗口失败。"; return false; }
  taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
  return true;
}

LRESULT CALLBACK Application::WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
  auto* self = reinterpret_cast<Application*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    self = static_cast<Application*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
    self->hwnd_ = hwnd; SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  }
  return self ? self->HandleMessage(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT Application::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
  if (taskbarCreated_ && message == taskbarCreated_) { AddTrayIcon(); return 0; }
  switch (message) {
    case WM_HOTKEY:
      if (static_cast<int>(wParam) == kHotkeyBase + 1) StartBurstCapture();
      else StartCapture();
      return 0;
    case kDeferredCapture: StartCapture(); return 0;
    case kOverlayDone: ProcessOverlayResult(std::unique_ptr<OverlayResult>(reinterpret_cast<OverlayResult*>(lParam))); return 0;
    case WM_COPYDATA: {
      const auto* data = reinterpret_cast<COPYDATASTRUCT*>(lParam);
      const std::wstring command(static_cast<const wchar_t*>(data->lpData));
      PostMessageW(hwnd_, command == L"--settings" ? WM_COMMAND : kDeferredCapture,
                   command == L"--settings" ? kCommandSettings : 0, 0); return TRUE;
    }
    case kTrayMessage:
      // NOTIFYICON_VERSION_4 packs the notification code into LOWORD(lParam) and
      // the anchor coordinates into wParam. Comparing the complete lParam makes
      // the context menu silently stop working on current Windows versions.
      if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU ||
          LOWORD(lParam) == NIN_KEYSELECT) {
        POINT point{GET_X_LPARAM(wParam), GET_Y_LPARAM(wParam)};
        if (point.x == 0 && point.y == 0) GetCursorPos(&point);
        ShowTrayMenu(point);
      } else if (LOWORD(lParam) == WM_LBUTTONDBLCLK || LOWORD(lParam) == NIN_SELECT) {
        StartCapture();
      }
      return 0;
    case WM_COMMAND:
      switch (LOWORD(wParam)) {
        case kCommandCapture: StartCapture(); break;
        case kCommandSettings: ShowSettings(); break;
        case kCommandAutoStart:
          config_.launchAtLogin = !config_.launchAtLogin; UpdateAutoStart(); SaveConfig(); break;
        case kCommandAbout: {
      std::wstring text = L"RC-ScreenShot 0.7.2\r\n\r\n原生 C++20 / DXGI / Direct2D 截图工具\r\n\r\n";
          HRSRC resource = FindResourceW(instance_, MAKEINTRESOURCEW(101), RT_RCDATA);
          if (resource) {
            HGLOBAL loaded = LoadResource(instance_, resource);
            const auto* bytes = static_cast<const char*>(LockResource(loaded));
            const DWORD size = SizeofResource(instance_, resource);
            if (bytes && size) text += FromUtf8(std::string_view(bytes, size));
          }
          MessageBoxW(nullptr, text.c_str(), L"关于 RC-ScreenShot", MB_OK | MB_ICONINFORMATION);
          break;
        }
        case kCommandExit: DestroyWindow(hwnd_); break;
      }
      return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
  }
  return DefWindowProcW(hwnd_, message, wParam, lParam);
}

void Application::AddTrayIcon() {
  trayIcon_ = {}; trayIcon_.cbSize = sizeof(trayIcon_); trayIcon_.hWnd = hwnd_; trayIcon_.uID = 1;
  trayIcon_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
  trayIcon_.uCallbackMessage = kTrayMessage;
  trayIcon_.hIcon = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(102), IMAGE_ICON,
                                                  0, 0, LR_DEFAULTSIZE | LR_SHARED));
  wcscpy_s(trayIcon_.szTip, L"RC-ScreenShot");
  Shell_NotifyIconW(NIM_ADD, &trayIcon_); trayIcon_.uVersion = NOTIFYICON_VERSION_4;
  Shell_NotifyIconW(NIM_SETVERSION, &trayIcon_);
}

void Application::RemoveTrayIcon() { if (trayIcon_.hWnd) Shell_NotifyIconW(NIM_DELETE, &trayIcon_); trayIcon_ = {}; }

void Application::ShowTrayMenu(POINT point) {
  HMENU menu = CreatePopupMenu();
  AppendMenuW(menu, MF_STRING, kCommandCapture, L"截图\tCtrl+\\");
  AppendMenuW(menu, MF_STRING, kCommandSettings, L"设置…");
  AppendMenuW(menu, MF_STRING | (config_.launchAtLogin ? MF_CHECKED : 0), kCommandAutoStart, L"登录时启动");
  AppendMenuW(menu, MF_STRING, kCommandAbout, L"关于…");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr); AppendMenuW(menu, MF_STRING, kCommandExit, L"退出");
  SetForegroundWindow(hwnd_); TrackPopupMenu(menu, TPM_RIGHTBUTTON, point.x, point.y, 0, hwnd_, nullptr);
  DestroyMenu(menu);
}

void Application::RegisterConfiguredHotkeys() {
  UnregisterHotkeys(); hotkeyErrors_.clear(); int id = kHotkeyBase;
  for (const auto& hotkey : config_.hotkeys) {
    if (!hotkey.enabled) { ++id; continue; }
    if (RegisterHotKey(hwnd_, id, hotkey.modifiers | MOD_NOREPEAT, hotkey.virtualKey)) registeredHotkeyIds_.push_back(id);
    else hotkeyErrors_.push_back(FormatHotkey(hotkey) + L" 已被其他程序占用。");
    ++id;
  }
}

void Application::UnregisterHotkeys() { for (int id : registeredHotkeyIds_) UnregisterHotKey(hwnd_, id); registeredHotkeyIds_.clear(); }

void Application::StartCapture() {
  if (overlay_) return;
  DesktopSnapshot snapshot; std::wstring error;
  if (!desktopCapture_.Capture(snapshot, error)) { Notify(L"截图失败", error, NIIF_ERROR); return; }
  overlay_ = std::make_unique<CaptureOverlay>(instance_, std::move(snapshot), config_,
      [this](OverlayResult result) { PostMessageW(hwnd_, kOverlayDone, 0, reinterpret_cast<LPARAM>(new OverlayResult(std::move(result)))); },
      [this] { SaveConfig(); });
  if (!overlay_->Show(error)) { overlay_.reset(); Notify(L"截图失败", error, NIIF_ERROR); }
}

void Application::StartBurstCapture() {
  if (overlay_) return;
  std::optional<RECT> targetWorkArea;
  POINT cursor{};
  if (GetCursorPos(&cursor)) {
    if (HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST)) {
      MONITORINFO monitorInfo{sizeof(monitorInfo)};
      if (GetMonitorInfoW(monitor, &monitorInfo)) targetWorkArea = monitorInfo.rcWork;
    }
  }
  const int requested = std::clamp(config_.burstCount, 2, 30);
  const double intervalSeconds = std::clamp(static_cast<double>(config_.burstIntervalSeconds), 0.05, 0.99);
  std::vector<DesktopSnapshot> snapshots;
  std::wstring captureError;
  if (!desktopCapture_.CaptureBurst(requested, intervalSeconds, snapshots, captureError)) {
    Notify(L"连拍失败", captureError.empty() ? L"未能获取首帧。" : captureError, NIIF_ERROR);
    return;
  }
  int skipped = std::max(0, requested - static_cast<int>(snapshots.size()));
  RECT expectedBounds{};
  int expectedWidth = 0;
  int expectedHeight = 0;
  if (!snapshots.empty()) {
    expectedBounds = snapshots.front().virtualBounds;
    expectedWidth = snapshots.front().width;
    expectedHeight = snapshots.front().height;
  }
  std::vector<DesktopSnapshot> validSnapshots;
  validSnapshots.reserve(snapshots.size());
  for (DesktopSnapshot& snapshot : snapshots) {
    if (snapshot.virtualBounds.left != expectedBounds.left || snapshot.virtualBounds.top != expectedBounds.top ||
        snapshot.virtualBounds.right != expectedBounds.right || snapshot.virtualBounds.bottom != expectedBounds.bottom ||
        snapshot.width != expectedWidth || snapshot.height != expectedHeight) {
      ++skipped;
      continue;
    }
    validSnapshots.push_back(std::move(snapshot));
  }
  snapshots.swap(validSnapshots);
  if (snapshots.empty()) {
    Notify(L"连拍失败", captureError.empty() ? L"未能获取首帧。" : captureError, NIIF_ERROR);
    return;
  }
  if (!captureError.empty()) Notify(L"连拍提示", captureError, NIIF_WARNING);
  if (snapshots.size() < 2) {
    Notify(L"连拍提示", L"连拍未获得足够帧，已打开首帧。", NIIF_WARNING);
  } else if (skipped > 0) {
    Notify(L"连拍提示", L"部分帧捕获失败或显示器布局变化，已跳过。", NIIF_WARNING);
  }
  if (snapshots.size() < 2) {
    DesktopSnapshot first = std::move(snapshots.front());
    overlay_ = std::make_unique<CaptureOverlay>(instance_, std::move(first), config_,
        [this](OverlayResult result) {
          PostMessageW(hwnd_, kOverlayDone, 0,
                       reinterpret_cast<LPARAM>(new OverlayResult(std::move(result))));
        }, [this] { SaveConfig(); }, targetWorkArea);
  } else {
    overlay_ = std::make_unique<CaptureOverlay>(instance_, std::move(snapshots), config_,
        [this](OverlayResult result) {
          PostMessageW(hwnd_, kOverlayDone, 0,
                       reinterpret_cast<LPARAM>(new OverlayResult(std::move(result))));
        }, [this] { SaveConfig(); }, targetWorkArea);
  }
  std::wstring error;
  if (!overlay_->Show(error)) {
    overlay_.reset();
    Notify(L"截图失败", error, NIIF_ERROR);
  }
}

void Application::ProcessOverlayResult(std::unique_ptr<OverlayResult> result) {
  if (!overlay_) return;
  if (result->completion != CaptureCompletion::Cancel) {
    RenderedImage image; std::wstring error;
    bool rendered = false;
    if (result->longImage) {
      // Long screenshot: the stitched buffer is the export source.
      DesktopSnapshot stitched;
      stitched.virtualBounds = {0, 0, static_cast<LONG>(result->longImage->width),
                                static_cast<LONG>(result->longImage->height)};
      stitched.width = result->longImage->width;
      stitched.height = result->longImage->height;
      stitched.bgraStride = result->longImage->stride;
      stitched.bgra = std::move(result->longImage->bgra);
      rendered = exporter_.Render(stitched, stitched.virtualBounds, result->document, config_,
                                  false, image, error);
    } else {
      rendered = exporter_.Render(overlay_->snapshot(), result->selection, result->document, config_,
                                  result->windowSelection, image, error);
    }
    if (!rendered) {
      Notify(L"处理截图失败", error, NIIF_ERROR);
    } else {
      bool copied = false, saved = false;
      std::filesystem::path savedPath;
      if (result->completion == CaptureCompletion::Copy) {
        copied = exporter_.CopyToClipboard(hwnd_, image, error);
        if (copied && config_.autoSaveOnCopy) {
          saved = exporter_.Save(image, configStore_.ResolveOutputDirectory(config_), config_.filenameTemplate,
                                 config_.jpegQuality, savedPath, error);
        }
      } else {
        saved = exporter_.Save(image, configStore_.ResolveOutputDirectory(config_), config_.filenameTemplate,
                               config_.jpegQuality, savedPath, error);
      }
      if (!copied && !saved) Notify(L"截图输出失败", error, NIIF_ERROR);
      else if (saved) Notify(L"截图已保存", savedPath.wstring());
    }
  }
  overlay_.reset();
}

void Application::ShowSettings() {
  if (settingsWindow_) { ShowWindow(settingsWindow_, SW_RESTORE); SetForegroundWindow(settingsWindow_); return; }
  if (!settingsBackgroundBrush_) settingsBackgroundBrush_ = CreateSolidBrush(kBackground);
  if (!settingsPanelBrush_) settingsPanelBrush_ = CreateSolidBrush(kCardEnd);
  if (!settingsControlBrush_) settingsControlBrush_ = CreateSolidBrush(kControlFill);
  if (!settingsFont_) {
    settingsFont_ = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH, L"Microsoft YaHei UI");
    settingsTitleFont_ = CreateFontW(-27, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                     DEFAULT_PITCH, L"Microsoft YaHei UI");
    settingsSectionFont_ = CreateFontW(-17, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                       DEFAULT_PITCH, L"Microsoft YaHei UI");
    settingsSmallFont_ = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                     DEFAULT_PITCH, L"Microsoft YaHei UI");
    settingsHintFont_ = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                    DEFAULT_PITCH, L"Microsoft YaHei UI");
  }
  WNDCLASSEXW windowClass{sizeof(windowClass)};
  windowClass.lpfnWndProc = SettingsProc; windowClass.hInstance = instance_;
  windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  windowClass.hIcon = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(102), IMAGE_ICON,
                                                     0, 0, LR_DEFAULTSIZE | LR_SHARED));
  windowClass.hIconSm = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(102), IMAGE_ICON,
                                                       16, 16, LR_SHARED));
  windowClass.hbrBackground = settingsBackgroundBrush_; windowClass.lpszClassName = L"RC-ScreenShot.Settings";
  RegisterClassExW(&windowClass);
  // No WS_CLIPCHILDREN: WM_PAINT paints the full card surfaces underneath the
  // child controls and then repaints the children on top (see the WM_PAINT
  // handler).  With WS_CLIPCHILDREN the cards would never be painted in the
  // areas under the controls, and the transparent labels would sit on bare
  // background instead of the card surface.
  const DWORD settingsStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
  const DWORD settingsExStyle = WS_EX_APPWINDOW | WS_EX_CONTROLPARENT;
  RECT outerRect{0, 0, kSettingsWidth, kSettingsHeight};
  UINT dpi = hwnd_ ? GetDpiForWindow(hwnd_) : 0;
  if (dpi == 0) dpi = GetDpiForSystem();
  if (dpi == 0) dpi = 96;
  if (!AdjustWindowRectExForDpi(&outerRect, settingsStyle, FALSE, settingsExStyle, dpi)) {
    AdjustWindowRectEx(&outerRect, settingsStyle, FALSE, settingsExStyle);
  }
  const int outerWidth = outerRect.right - outerRect.left;
  const int outerHeight = outerRect.bottom - outerRect.top;
  POINT cursor{};
  GetCursorPos(&cursor);
  HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
  MONITORINFO monitorInfo{sizeof(monitorInfo)};
  RECT workArea{0, 0, outerWidth, outerHeight};
  if (monitor && GetMonitorInfoW(monitor, &monitorInfo)) workArea = monitorInfo.rcWork;
  int left = workArea.left + (workArea.right - workArea.left - outerWidth) / 2;
  int top = workArea.top + (workArea.bottom - workArea.top - outerHeight) / 2;
  const int minLeft = static_cast<int>(workArea.left);
  const int minTop = static_cast<int>(workArea.top);
  const int maxLeft = std::max(minLeft, static_cast<int>(workArea.right) - outerWidth);
  const int maxTop = std::max(minTop, static_cast<int>(workArea.bottom) - outerHeight);
  left = std::clamp(left, minLeft, maxLeft);
  top = std::clamp(top, minTop, maxTop);
  settingsWindow_ = CreateWindowExW(settingsExStyle, windowClass.lpszClassName,
                                    L"RC-ScreenShot 设置",
                                    settingsStyle, left, top, outerWidth, outerHeight,
                                    nullptr, nullptr, instance_, this);
  if (!settingsWindow_) return;
  const auto setFont = [&](HWND control, HFONT font = nullptr) {
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font ? font : settingsFont_), TRUE);
  };
  const auto label = [&](const wchar_t* text, int x, int y, int w, int h, HFONT font = nullptr) {
    HWND control = CreateWindowExW(WS_EX_TRANSPARENT, L"STATIC", text,
                                   WS_CHILD | WS_VISIBLE | SS_LEFT, x, y, w, h,
                                   settingsWindow_, nullptr, instance_, nullptr);
    setFont(control, font); return control;
  };
  const auto edit = [&](const wchar_t* text, int x, int y, int w, int h, int id) {
    HWND control = CreateWindowExW(WS_EX_TRANSPARENT, L"EDIT", text,
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                   x, y, w, h, settingsWindow_,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
    setFont(control); SendMessageW(control, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELONG(10, 10));
    return control;
  };
  const auto button = [&](const wchar_t* text, int x, int y, int w, int h, int id) {
    HWND control = CreateWindowW(L"BUTTON", text,
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                 x, y, w, h, settingsWindow_,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
    setFont(control);
    SetWindowSubclass(control, ButtonHoverProc, 2, reinterpret_cast<DWORD_PTR>(this));
    return control;
  };
  const auto hotkeyButton = [&](int x, int y, int w, int h, int id) {
    HWND control = button(L"", x, y, w, h, id);
    SetWindowSubclass(control, HotkeyCaptureProc, 1, reinterpret_cast<DWORD_PTR>(this));
    return control;
  };
  const auto toggle = [&](int x, int y, int id) {
    HWND control = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                 x, y, 52, 28, settingsWindow_,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
    setFont(control, settingsSmallFont_);
    SetWindowSubclass(control, ButtonHoverProc, 2, reinterpret_cast<DWORD_PTR>(this));
    return control;
  };
  // Header: editorial title, one-line guidance, and a quiet secondary reset.
  label(L"偏好设置", 27, 17, 300, 38, settingsTitleFont_);
  label(L"精细调整捕捉、输出与工作流", 30, 58, 360, 20, settingsHintFont_);
  button(L"恢复默认", 675, 23, 126, 36, IDC_RESET_SETTINGS);

  // Capture card — shortcuts get the largest visual weight and a generous,
  // two-column layout, with compact steppers on a separate baseline.
  label(L"快捷键", 43, 108, 90, 24, settingsSectionFont_);
  label(L"快速调用截图与连拍", 132, 113, 220, 18, settingsHintFont_);
  label(L"区域截图", 43, 145, 180, 18, settingsSmallFont_);
  hotkeyButton(42, 165, 346, 46, IDC_HOTKEY_PRIMARY);
  label(L"连续捕捉", 427, 145, 180, 18, settingsSmallFont_);
  hotkeyButton(426, 165, 346, 46, IDC_HOTKEY_SECONDARY);
  label(L"连拍张数", 43, 230, 84, 20, settingsSmallFont_);
  button(L"−", 134, 224, 32, 32, IDC_BURST_COUNT_MINUS);
  edit(L"", 171, 224, 54, 32, IDC_BURST_COUNT);
  button(L"+", 230, 224, 32, 32, IDC_BURST_COUNT_PLUS);
  label(L"2–30 张 · 默认 6", 275, 231, 128, 18, settingsHintFont_);
  label(L"拍摄间隔", 427, 230, 84, 20, settingsSmallFont_);
  button(L"−", 514, 224, 32, 32, IDC_BURST_INTERVAL_MINUS);
  edit(L"", 551, 224, 54, 32, IDC_BURST_INTERVAL);
  button(L"+", 610, 224, 32, 32, IDC_BURST_INTERVAL_PLUS);
  label(L"0.05–0.99 秒 · 默认 0.08", 654, 231, 134, 18, settingsHintFont_);

  // Output card — a compact two-column form with clear affordances for path,
  // export quality and the default Enter action.
  label(L"输出", 43, 308, 90, 25, settingsSectionFont_);
  label(L"保存位置与图像质量", 43, 338, 250, 18, settingsHintFont_);
  label(L"保存位置", 43, 378, 72, 20, settingsSmallFont_);
  edit(L"", 112, 370, 198, 34, IDC_OUTPUT);
  button(L"浏览", 319, 370, 67, 34, IDC_BROWSE);
  label(L"JPEG 质量", 43, 432, 76, 20, settingsSmallFont_);
  label(L"1", 127, 451, 16, 17, settingsHintFont_);
  label(L"100", 307, 451, 32, 17, settingsHintFont_);
  label(L"回车默认", 43, 485, 76, 20, settingsSmallFont_);
  button(L"复制", 128, 476, 76, 34, IDC_ACTION_COPY);
  button(L"保存", 212, 476, 76, 34, IDC_ACTION_SAVE);
  label(L"截图后执行", 299, 484, 88, 18, settingsHintFont_);

  // Editor card — each preference is a self-contained row with an explanatory
  // note, a hairline divider and a tactile toggle aligned to the right edge.
  label(L"编辑器", 443, 308, 100, 25, settingsSectionFont_);
  label(L"让成品更贴近你的使用习惯", 443, 338, 310, 18, settingsHintFont_);
  label(L"窗口截图阴影", 443, 372, 180, 20, settingsSmallFont_);
  label(L"为窗口边缘添加轻柔层次", 443, 396, 240, 18, settingsHintFont_);
  toggle(738, 378, IDC_TOGGLE_SHADOW);
  label(L"截图外框", 443, 444, 180, 20, settingsSmallFont_);
  label(L"为导出图像添加清晰边界", 443, 468, 240, 18, settingsHintFont_);
  toggle(738, 450, IDC_TOGGLE_FRAME);

  // Workflow card — a concise title block plus three aligned preferences.
  label(L"自动化", 43, 558, 125, 24, settingsSectionFont_);
  label(L"少一点重复，多一点专注", 43, 584, 145, 17, settingsHintFont_);
  label(L"复制后自动保存", 213, 559, 142, 19, settingsSmallFont_);
  label(L"同步落盘", 213, 583, 100, 17, settingsHintFont_);
  toggle(326, 563, IDC_TOGGLE_AUTOSAVE);
  label(L"登录时启动", 407, 559, 108, 19, settingsSmallFont_);
  label(L"开机即用", 407, 583, 100, 17, settingsHintFont_);
  toggle(515, 563, IDC_TOGGLE_AUTOSTART);
  label(L"静默启动", 594, 559, 104, 19, settingsSmallFont_);
  label(L"后台待命", 594, 583, 100, 17, settingsHintFont_);
  toggle(711, 563, IDC_TOGGLE_SILENT);

  // Footer floats beneath the cards, rather than being boxed into another row.
  label(L"快捷键支持 Ctrl / Alt / Shift 组合", 27, 654, 380, 18, settingsHintFont_);
  button(L"取消", 620, 646, 78, 36, IDC_CANCEL_SETTINGS);
  button(L"保存设置", 708, 646, 94, 36, IDC_SAVE_SETTINGS);
  BOOL darkTitle = TRUE; DwmSetWindowAttribute(settingsWindow_, 20, &darkTitle, sizeof(darkTitle));
  // Windows 11: tint the caption, frame and caption text to match the dialog
  // chrome (attributes are ignored on older systems).
  const COLORREF captionColor = kBackground;
  const COLORREF frameColor = kCardBorder;
  const COLORREF captionText = kTextNormal;
  DwmSetWindowAttribute(settingsWindow_, 35, &captionColor, sizeof(captionColor));
  DwmSetWindowAttribute(settingsWindow_, 34, &frameColor, sizeof(frameColor));
  DwmSetWindowAttribute(settingsWindow_, 36, &captionText, sizeof(captionText));
  PopulateSettings(settingsWindow_); ShowWindow(settingsWindow_, SW_SHOW); UpdateWindow(settingsWindow_);
}

LRESULT CALLBACK Application::SettingsProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
  auto* self = reinterpret_cast<Application*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    self = static_cast<Application*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  }
  return self ? self->HandleSettingsMessage(hwnd, message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT Application::HandleSettingsMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
  if (message == WM_ERASEBKGND) return 1;
  if (message == WM_PAINT) {
    PAINTSTRUCT paint{}; BeginPaint(hwnd, &paint);
    HDC dc = paint.hdc; RECT client{}; GetClientRect(hwnd, &client);
    // Background: a quiet vertical wash instead of a flat fill so the window
    // reads as one lit surface behind the cards.
    TRIVERTEX backgroundVertices[2]{};
    backgroundVertices[0].x = client.left; backgroundVertices[0].y = client.top;
    backgroundVertices[0].Red = Channel16(kBackground, 0);
    backgroundVertices[0].Green = Channel16(kBackground, 8);
    backgroundVertices[0].Blue = Channel16(kBackground, 16);
    backgroundVertices[0].Alpha = 0xff00;
    backgroundVertices[1].x = client.right; backgroundVertices[1].y = client.bottom;
    backgroundVertices[1].Red = Channel16(kBackgroundEnd, 0);
    backgroundVertices[1].Green = Channel16(kBackgroundEnd, 8);
    backgroundVertices[1].Blue = Channel16(kBackgroundEnd, 16);
    backgroundVertices[1].Alpha = 0xff00;
    GRADIENT_RECT backgroundMesh{0, 1};
    if (!GradientFill(dc, backgroundVertices, 2, &backgroundMesh, 1, GRADIENT_FILL_RECT_V))
      FillRect(dc, &client, settingsBackgroundBrush_);
    SetBkMode(dc, TRANSPARENT);
    const auto flatCard = [&](RECT rect) {
      HRGN clip = CreateRoundRectRgn(rect.left, rect.top, rect.right + 1, rect.bottom + 1, 16, 16);
      const int saved = SaveDC(dc);
      SelectClipRgn(dc, clip);
      HBRUSH cardBrush = CreateSolidBrush(kCardStart);
      FillRect(dc, &rect, cardBrush);
      RestoreDC(dc, saved);
      DeleteObject(clip);
      DeleteObject(cardBrush);

      HPEN pen = CreatePen(PS_SOLID, 1, kCardBorder);
      HGDIOBJ oldPen = SelectObject(dc, pen);
      HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
      RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, 16, 16);
      SelectObject(dc, oldPen);
      SelectObject(dc, oldBrush);
      DeleteObject(pen);
    };
    flatCard(kShortcutCard);
    flatCard(kOutputCard);
    flatCard(kEditorCard);
    flatCard(kBehaviorCard);
    // Accent bars mark the four section titles at a glance.
    const auto sectionBar = [&](int x, int y) {
      HBRUSH brush = CreateSolidBrush(kAccent);
      HGDIOBJ oldBrush = SelectObject(dc, brush);
      HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
      RoundRect(dc, x, y, x + 4, y + 14, 2, 2);
      SelectObject(dc, oldPen); SelectObject(dc, oldBrush); DeleteObject(brush);
    };
    sectionBar(26, 111);
    sectionBar(26, 315);
    sectionBar(426, 315);
    sectionBar(26, 565);
    const auto drawInputFrame = [&](RECT rect, HWND control) {
      const bool focused = control && GetFocus() == control;
      HRGN clip = CreateRoundRectRgn(rect.left, rect.top, rect.right + 1, rect.bottom + 1, 8, 8);
      const int saved = SaveDC(dc);
      SelectClipRgn(dc, clip);
      BlendFill(dc, rect, kControlFill, 224);
      RestoreDC(dc, saved);
      DeleteObject(clip);
      HPEN pen = CreatePen(PS_SOLID, focused ? 2 : 1, focused ? kAccent : kControlBorder);
      HGDIOBJ oldPen = SelectObject(dc, pen);
      HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
      RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, 8, 8);
      SelectObject(dc, oldPen); SelectObject(dc, oldBrush); DeleteObject(pen);
    };
    drawInputFrame(SettingsInputFrameRect(IDC_HOTKEY_PRIMARY), GetDlgItem(hwnd, IDC_HOTKEY_PRIMARY));
    drawInputFrame(SettingsInputFrameRect(IDC_HOTKEY_SECONDARY), GetDlgItem(hwnd, IDC_HOTKEY_SECONDARY));
    drawInputFrame(SettingsInputFrameRect(IDC_BURST_COUNT), GetDlgItem(hwnd, IDC_BURST_COUNT));
    drawInputFrame(SettingsInputFrameRect(IDC_BURST_INTERVAL), GetDlgItem(hwnd, IDC_BURST_INTERVAL));
    drawInputFrame(SettingsInputFrameRect(IDC_OUTPUT), GetDlgItem(hwnd, IDC_OUTPUT));
    HPEN dividerPen = CreatePen(PS_SOLID, 1, RGB(75, 96, 132));
    HGDIOBJ previousPen = SelectObject(dc, dividerPen);
    MoveToEx(dc, 442, 427, nullptr); LineTo(dc, 778, 427);
    MoveToEx(dc, 383, 558, nullptr); LineTo(dc, 383, 616);
    MoveToEx(dc, 580, 558, nullptr); LineTo(dc, 580, 616);
    SelectObject(dc, previousPen); DeleteObject(dividerPen);
    const RECT track = QualitySliderRect(); const int lineY = (track.top + track.bottom) / 2;
    // Groove, accent fill and knob share rounded profiles so the slider reads
    // as one control; the knob gains an accent ring for precision.
    HPEN trackPen = CreatePen(PS_SOLID, 1, RGB(52, 70, 102));
    HBRUSH trackBrush = CreateSolidBrush(RGB(40, 55, 82));
    HGDIOBJ oldPen = SelectObject(dc, trackPen); HGDIOBJ oldBrush = SelectObject(dc, trackBrush);
    RoundRect(dc, track.left, lineY - 3, track.right, lineY + 3, 3, 3);
    const int thumbX = track.left + (track.right - track.left) * (config_.jpegQuality - 1) / 99;
    HBRUSH fillBrush = CreateSolidBrush(kAccent);
    SelectObject(dc, fillBrush);
    RoundRect(dc, track.left, lineY - 3, thumbX, lineY + 3, 3, 3);
    HPEN thumbPen = CreatePen(PS_SOLID, 2, kAccent);
    HBRUSH thumbBrush = CreateSolidBrush(kTextBright);
    SelectObject(dc, thumbPen); SelectObject(dc, thumbBrush);
    Ellipse(dc, thumbX - 6, lineY - 6, thumbX + 6, lineY + 6);
    SelectObject(dc, oldPen); SelectObject(dc, oldBrush);
    DeleteObject(trackPen); DeleteObject(trackBrush); DeleteObject(fillBrush);
    DeleteObject(thumbPen); DeleteObject(thumbBrush);
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, kTextNormal); SelectObject(dc, settingsSmallFont_);
    RECT qualityPill{344, 429, 386, 452};
    DrawPanelButton(dc, qualityPill, kControlFill, kControlBorder, 10, 232);
    const std::wstring qualityText = std::to_wstring(config_.jpegQuality) + L"%";
    RECT quality{qualityPill.left + 2, qualityPill.top, qualityPill.right - 2, qualityPill.bottom};
    DrawTextW(dc, qualityText.c_str(), -1, &quality, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    EndPaint(hwnd, &paint);
    // The cards were just painted over the child controls, so invalidate the
    // children to paint themselves again on top.  They repaint in the same
    // paint cycle, before the compositor presents the frame, so the text never
    // visibly disappears.  Only the children are invalidated — invalidating
    // the parent here would endlessly re-enter WM_PAINT.
    for (HWND child = GetWindow(hwnd, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
      InvalidateRect(child, nullptr, FALSE);
    return 0;
  }
  if (message == WM_LBUTTONDOWN) {
    POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    if (Contains(QualitySliderRect(), point)) {
      settingsSliderDragging_ = true; SetCapture(hwnd); UpdateQualitySlider(hwnd, point.x); return 0;
    }
  } else if (message == WM_MOUSEMOVE && settingsSliderDragging_) {
    UpdateQualitySlider(hwnd, GET_X_LPARAM(lParam)); return 0;
  } else if (message == WM_LBUTTONUP && settingsSliderDragging_) {
    UpdateQualitySlider(hwnd, GET_X_LPARAM(lParam)); settingsSliderDragging_ = false; ReleaseCapture(); return 0;
  } else if (message == WM_CAPTURECHANGED && settingsSliderDragging_) { settingsSliderDragging_ = false; return 0; }
  if (message == WM_CTLCOLORSTATIC) {
    HDC dc = reinterpret_cast<HDC>(wParam); HWND control = reinterpret_cast<HWND>(lParam);
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(control, WM_GETFONT, 0, 0));
    // Hierarchy: section titles read brightest, field captions mid-tone, and
    // helper hints are deliberately dimmed.
    SetTextColor(dc, (font == settingsTitleFont_ || font == settingsSectionFont_) ? kTextBright
                 : font == settingsHintFont_ ? kTextDim : kTextLabel);
    SetBkMode(dc, TRANSPARENT); return reinterpret_cast<LRESULT>(GetStockObject(NULL_BRUSH));
  }
  if (message == WM_CTLCOLOREDIT) {
    HDC dc = reinterpret_cast<HDC>(wParam);
    SetTextColor(dc, kTextNormal);
    SetBkMode(dc, OPAQUE);
    SetBkColor(dc, kControlFill);
    return reinterpret_cast<LRESULT>(settingsControlBrush_);
  }
  if (message == WM_CTLCOLORBTN) {
    HDC dc = reinterpret_cast<HDC>(wParam); SetTextColor(dc, kTextNormal);
    SetBkMode(dc, TRANSPARENT); return reinterpret_cast<LRESULT>(settingsPanelBrush_);
  }
  if (message == WM_DRAWITEM) {
    const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam); if (!item || item->CtlType != ODT_BUTTON) return 0;
    const int id = GetDlgCtrlID(item->hwndItem); const bool pressed = (item->itemState & ODS_SELECTED) != 0;
    const bool hovered = hoverButton_ == item->hwndItem;
    if (IsHotkeyId(id)) {
      const auto& state = HotkeyStateFor(item->hwndItem);
      const RECT r = item->rcItem;
      const COLORREF fill = state.listening ? RGB(38, 54, 92)
                            : hovered ? kControlHover : kControlFill;
      const COLORREF border = state.listening ? kAccent
                              : hovered ? kControlHoverBorder : kControlBorder;
      DrawPanelButton(item->hDC, r, fill, border, 8, 238);
      std::wstring display = GetWindowString(settingsWindow_, id);
      if (state.listening && !state.submitted) {
        display = state.modifiers ? HotkeyModifierPreview(state.modifiers) : L"按下组合键…";
      }
      SetBkMode(item->hDC, TRANSPARENT);
      SetTextColor(item->hDC, state.listening ? kTextBright : kTextNormal);
      SelectObject(item->hDC, settingsFont_);
      RECT textRect = r;
      DrawTextW(item->hDC, display.c_str(), -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      if (state.listening) {
        SetTextColor(item->hDC, kAccentBorder);
        RECT hintRect{r.right - 62, r.top, r.right - 8, r.bottom};
        DrawTextW(item->hDC, L"监听中", -1, &hintRect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
      }
      return TRUE;
    }
    if (IsToggleId(id)) {
      const bool on = ToggleValue(config_, id); const RECT r = item->rcItem; const int height = r.bottom - r.top;
      const COLORREF track = on ? (pressed ? kAccentPressed : kAccent)
                                : (pressed ? RGB(46, 58, 80)
                                           : hovered ? RGB(44, 56, 78) : RGB(40, 52, 72));
      DrawPanelButton(item->hDC, r, track, on ? kAccentBorder : RGB(82, 101, 132), height, 244);
      const int radius = std::max(3, height / 2 - 3); const int knobX = on ? r.right - radius - 3 : r.left + radius + 3;
      HBRUSH knob = CreateSolidBrush(on ? RGB(250, 253, 255) : RGB(196, 208, 226));
      HGDIOBJ oldBrush = SelectObject(item->hDC, knob);
      Ellipse(item->hDC, knobX - radius, (r.top + r.bottom) / 2 - radius, knobX + radius, (r.top + r.bottom) / 2 + radius);
      SelectObject(item->hDC, oldBrush); DeleteObject(knob);
      SetBkMode(item->hDC, TRANSPARENT); SetTextColor(item->hDC, on ? RGB(255, 255, 255) : RGB(160, 176, 198));
      SelectObject(item->hDC, settingsSmallFont_);
      RECT stateRect = on ? RECT{r.left + 4, r.top, r.left + (r.right - r.left) / 2, r.bottom}
                          : RECT{r.left + (r.right - r.left) / 2, r.top, r.right - 4, r.bottom};
      DrawTextW(item->hDC, on ? L"开" : L"关", -1, &stateRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      return TRUE;
    }
    if (IsSegmentId(id)) {
      const bool active = (id == IDC_ACTION_COPY && config_.defaultAction == DefaultAction::Copy) ||
                          (id == IDC_ACTION_SAVE && config_.defaultAction == DefaultAction::Save);
      const COLORREF fill = active ? (pressed ? kAccentPressed : kAccentHover)
                                   : (pressed ? RGB(34, 48, 72) : hovered ? RGB(38, 54, 80) : kControlFill);
      const COLORREF borderColor = active ? kAccentBorder : hovered ? kControlHoverBorder : kControlBorder;
      DrawPanelButton(item->hDC, item->rcItem, fill, borderColor, 8, active ? 244 : 232);
      wchar_t text[128]{};
      GetWindowTextW(item->hwndItem, text, _countof(text));
      SetBkMode(item->hDC, TRANSPARENT);
      SetTextColor(item->hDC, active ? RGB(255, 255, 255) : kTextLabel);
      SelectObject(item->hDC, settingsFont_);
      RECT textRect = item->rcItem;
      DrawTextW(item->hDC, text, -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      return TRUE;
    }
    if (IsStepId(id)) {
      const COLORREF fill = pressed ? kAccentPressed : hovered ? kControlHover : kControlFill;
      DrawPanelButton(item->hDC, item->rcItem, fill,
                      hovered ? kControlHoverBorder : kControlBorder, 8, 232);
      SetBkMode(item->hDC, TRANSPARENT); SetTextColor(item->hDC, kTextNormal); SelectObject(item->hDC, settingsFont_);
      wchar_t text[8]{}; GetWindowTextW(item->hwndItem, text, _countof(text)); RECT textRect = item->rcItem;
      DrawTextW(item->hDC, text, -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE); return TRUE;
    }
    const bool primary = id == IDC_SAVE_SETTINGS; const bool accent = id == IDC_BROWSE || id == IDC_RESET_SETTINGS;
    const COLORREF fill = primary ? (pressed ? kAccentPressed : kAccent)
                                  : accent ? (pressed ? RGB(38, 56, 86) : hovered ? RGB(40, 58, 88) : RGB(32, 48, 72))
                                           : (pressed ? RGB(34, 48, 72) : hovered ? kControlHover : kControlFill);
    DrawPanelButton(item->hDC, item->rcItem, fill,
                    primary ? kAccentBorder : hovered ? kControlHoverBorder : kControlBorder,
                    8, primary ? 244 : 232);
    wchar_t text[128]{}; GetWindowTextW(item->hwndItem, text, _countof(text)); SetBkMode(item->hDC, TRANSPARENT);
    SetTextColor(item->hDC, primary ? RGB(255, 255, 255) : kTextNormal); SelectObject(item->hDC, settingsFont_); RECT textRect = item->rcItem;
    DrawTextW(item->hDC, text, -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE); return TRUE;
  }
  if (message == WM_COMMAND) {
    const int id = LOWORD(wParam);
    const int notification = HIWORD(wParam);
    if (IsSettingsEditId(id) && (notification == EN_SETFOCUS || notification == EN_KILLFOCUS)) {
      RECT dirty = SettingsInputFrameRect(id);
      InflateRect(&dirty, 2, 2);
      InvalidateRect(hwnd, &dirty, FALSE);
      if (HWND control = GetDlgItem(hwnd, id)) InvalidateRect(control, nullptr, FALSE);
      return 0;
    }
    if (IsStepId(id)) {
      if (id == IDC_BURST_COUNT_MINUS || id == IDC_BURST_COUNT_PLUS) {
        int value = config_.burstCount;
        const std::wstring text = GetWindowString(hwnd, IDC_BURST_COUNT);
        wchar_t* end = nullptr;
        const long parsed = text.empty() ? 0L : wcstol(text.c_str(), &end, 10);
        while (end && iswspace(*end)) ++end;
        if (!text.empty() && end && *end == L'\0' && parsed >= INT_MIN && parsed <= INT_MAX)
          value = static_cast<int>(parsed);
        value = std::clamp(value + (id == IDC_BURST_COUNT_PLUS ? 1 : -1), 2, 30);
        SetWindowString(hwnd, IDC_BURST_COUNT, std::to_wstring(value));
        config_.burstCount = value;
      } else {
        const float delta = id == IDC_BURST_INTERVAL_PLUS ? 0.01f : -0.01f;
        float value = config_.burstIntervalSeconds;
        const std::wstring text = GetWindowString(hwnd, IDC_BURST_INTERVAL);
        wchar_t* end = nullptr;
        const float parsed = text.empty() ? 0.0f : wcstof(text.c_str(), &end);
        while (end && iswspace(*end)) ++end;
        if (!text.empty() && end && *end == L'\0' && std::isfinite(parsed)) value = parsed;
        value = std::clamp(value + delta, 0.05f, 0.99f);
        config_.burstIntervalSeconds = value;
        wchar_t interval[32]{}; swprintf_s(interval, L"%.2f", value);
        SetWindowString(hwnd, IDC_BURST_INTERVAL, interval);
      }
      InvalidateRect(hwnd, nullptr, FALSE); return 0;
    }
    if (id == IDC_RESET_SETTINGS) {
      config_ = AppConfig{};
      PopulateSettings(hwnd); InvalidateRect(hwnd, nullptr, FALSE); return 0;
    }
    if (id == IDC_SAVE_SETTINGS) {
      if (ReadSettings(hwnd)) { SaveConfig(); RegisterConfiguredHotkeys(); UpdateAutoStart(); DestroyWindow(hwnd); }
      return 0;
    }
    if (id == IDC_CANCEL_SETTINGS) { config_ = configStore_.Load(); DestroyWindow(hwnd); return 0; }
    if (IsToggleId(id)) { SetToggleValue(config_, id, !ToggleValue(config_, id)); InvalidateRect(GetDlgItem(hwnd, id), nullptr, FALSE); return 0; }
    if (id == IDC_ACTION_COPY || id == IDC_ACTION_SAVE) {
      config_.defaultAction = id == IDC_ACTION_SAVE ? DefaultAction::Save : DefaultAction::Copy;
      InvalidateRect(GetDlgItem(hwnd, IDC_ACTION_COPY), nullptr, FALSE); InvalidateRect(GetDlgItem(hwnd, IDC_ACTION_SAVE), nullptr, FALSE); return 0;
    }
    if (id == IDC_BROWSE) {
      IFileDialog* raw = nullptr;
      if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&raw)))) {
        ComPtr<IFileDialog> dialog; dialog.Attach(raw); DWORD options = 0; dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        if (SUCCEEDED(dialog->Show(hwnd))) { ComPtr<IShellItem> item; dialog->GetResult(&item); PWSTR path = nullptr;
          if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) { SetWindowString(hwnd, IDC_OUTPUT, path); CoTaskMemFree(path); }}
      }
      return 0;
    }
  }
  if (message == WM_CLOSE) { DestroyWindow(hwnd); return 0; }
  if (message == WM_DESTROY) { settingsWindow_ = nullptr; return 0; }
  return DefWindowProcW(hwnd, message, wParam, lParam);
}

Application::HotkeyCaptureState& Application::HotkeyStateFor(HWND hwnd) {
  return GetDlgCtrlID(hwnd) == IDC_HOTKEY_SECONDARY ? hotkeySecondaryState_ : hotkeyPrimaryState_;
}

const Application::HotkeyCaptureState& Application::HotkeyStateFor(HWND hwnd) const {
  return GetDlgCtrlID(hwnd) == IDC_HOTKEY_SECONDARY ? hotkeySecondaryState_ : hotkeyPrimaryState_;
}

LRESULT CALLBACK Application::HotkeyCaptureProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
                                                UINT_PTR subclassId, DWORD_PTR referenceData) {
  (void)subclassId;
  auto* self = reinterpret_cast<Application*>(referenceData);
  if (!self) return DefSubclassProc(hwnd, message, wParam, lParam);
  auto& state = self->HotkeyStateFor(hwnd);
  const auto refresh = [&]() {
    InvalidateRect(hwnd, nullptr, FALSE);
  };
  const auto moveFocusAfterFinish = [&]() {
    HWND parent = GetParent(hwnd);
    if (!parent) return;
    HWND next = GetNextDlgTabItem(parent, hwnd, FALSE);
    while (next && next != hwnd && IsHotkeyId(GetDlgCtrlID(next))) {
      HWND candidate = GetNextDlgTabItem(parent, next, FALSE);
      if (!candidate || candidate == next) break;
      next = candidate;
    }
    if (!next || next == hwnd) next = GetDlgItem(parent, IDC_OUTPUT);
    if (next && next != hwnd) SetFocus(next);
  };
  const auto finish = [&](bool restoreOriginal) {
    if (restoreOriginal) SetWindowTextW(hwnd, state.originalText.c_str());
    state.modifiers = 0;
    state.listening = false;
    state.submitted = !restoreOriginal;
    refresh();
    moveFocusAfterFinish();
  };
  if (message == WM_GETDLGCODE) return DLGC_WANTALLKEYS | DLGC_BUTTON;
  if (message == WM_SETFOCUS) {
    wchar_t text[256]{};
    GetWindowTextW(hwnd, text, _countof(text));
    state.originalText = text;
    state.modifiers = 0;
    state.listening = true;
    state.submitted = false;
    refresh();
    return DefSubclassProc(hwnd, message, wParam, lParam);
  }
  if (message == WM_LBUTTONDOWN && !state.listening) {
    wchar_t text[256]{};
    GetWindowTextW(hwnd, text, _countof(text));
    state.originalText = text;
    state.modifiers = 0;
    state.listening = true;
    state.submitted = false;
    refresh();
  }
  if (message == WM_KILLFOCUS) {
    if (state.listening && !state.submitted) SetWindowTextW(hwnd, state.originalText.c_str());
    state.modifiers = 0;
    state.listening = false;
    state.submitted = false;
    refresh();
    return DefSubclassProc(hwnd, message, wParam, lParam);
  }
  if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) {
    const UINT key = static_cast<UINT>(wParam);
    if (key == VK_ESCAPE) {
      finish(true);
      return 0;
    }
    if (key == VK_BACK || key == VK_DELETE) {
      SetWindowTextW(hwnd, L"");
      finish(false);
      return 0;
    }
    const UINT modifier = HotkeyModifierForKey(key);
    if (modifier) {
      state.listening = true;
      state.submitted = false;
      state.modifiers |= modifier;
      refresh();
      return 0;
    }
    const UINT modifiers = state.modifiers | CurrentHotkeyModifiers();
    if (!(modifiers & (MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN))) return 0;
    const HotkeySetting hotkey{modifiers | MOD_NOREPEAT, key, true};
    SetWindowTextW(hwnd, FormatHotkey(hotkey).c_str());
    state.modifiers = hotkey.modifiers;
    finish(false);
    return 0;
  }
  if (message == WM_KEYUP || message == WM_SYSKEYUP) {
    if (state.listening) state.modifiers &= ~HotkeyModifierForKey(static_cast<UINT>(wParam));
    refresh();
    return 0;
  }
  if (message == WM_CHAR || message == WM_SYSCHAR) return 0;
  if (message == WM_NCDESTROY) {
    state.modifiers = 0;
    state.listening = false;
    state.submitted = false;
    RemoveWindowSubclass(hwnd, HotkeyCaptureProc, subclassId);
  }
  return DefSubclassProc(hwnd, message, wParam, lParam);
}

// Tracks the mouse across owner-drawn buttons so WM_DRAWITEM can render a
// hover state; the parent's WM_DRAWITEM reads hoverButton_ per control.
LRESULT CALLBACK Application::ButtonHoverProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
                                              UINT_PTR, DWORD_PTR referenceData) {
  auto* self = reinterpret_cast<Application*>(referenceData);
  if (self) {
    if (message == WM_MOUSEMOVE) {
      if (self->hoverButton_ != hwnd) {
        HWND previous = self->hoverButton_;
        self->hoverButton_ = hwnd;
        if (previous) InvalidateRect(previous, nullptr, FALSE);
        InvalidateRect(hwnd, nullptr, FALSE);
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&track);
      }
    } else if (message == WM_MOUSELEAVE) {
      if (self->hoverButton_ == hwnd) self->hoverButton_ = nullptr;
      InvalidateRect(hwnd, nullptr, FALSE);
    }
  }
  return DefSubclassProc(hwnd, message, wParam, lParam);
}

void Application::PopulateSettings(HWND hwnd) {
  std::vector<HotkeySetting> enabled;
  for (const auto& key : config_.hotkeys) if (key.enabled && enabled.size() < 2) enabled.push_back(key);
  if (enabled.empty()) enabled.push_back({});
  SetWindowString(hwnd, IDC_HOTKEY_PRIMARY, FormatHotkey(enabled[0]));
  SetWindowString(hwnd, IDC_HOTKEY_SECONDARY, enabled.size() > 1 ? FormatHotkey(enabled[1]) : L"");
  hotkeyPrimaryState_ = {};
  hotkeySecondaryState_ = {};
  hotkeyPrimaryState_.originalText = GetWindowString(hwnd, IDC_HOTKEY_PRIMARY);
  hotkeySecondaryState_.originalText = GetWindowString(hwnd, IDC_HOTKEY_SECONDARY);
  SetWindowString(hwnd, IDC_BURST_COUNT, std::to_wstring(std::clamp(config_.burstCount, 2, 30)));
  {
    wchar_t interval[32]{};
    swprintf_s(interval, L"%.2f", std::clamp(config_.burstIntervalSeconds, 0.05f, 0.99f));
    SetWindowString(hwnd, IDC_BURST_INTERVAL, interval);
  }
  SetWindowString(hwnd, IDC_OUTPUT, config_.outputDirectory);
  CheckDlgButton(hwnd, IDC_TOGGLE_AUTOSAVE, config_.autoSaveOnCopy ? BST_CHECKED : BST_UNCHECKED);
  CheckDlgButton(hwnd, IDC_TOGGLE_AUTOSTART, config_.launchAtLogin ? BST_CHECKED : BST_UNCHECKED);
  CheckDlgButton(hwnd, IDC_TOGGLE_SILENT, config_.silentAtLogin ? BST_CHECKED : BST_UNCHECKED);
  CheckDlgButton(hwnd, IDC_TOGGLE_SHADOW, config_.windowShadow ? BST_CHECKED : BST_UNCHECKED);
  CheckDlgButton(hwnd, IDC_TOGGLE_FRAME, config_.frameEnabled ? BST_CHECKED : BST_UNCHECKED);
}

bool Application::ReadSettings(HWND hwnd) {
  auto trim = [](std::wstring value) {
    while (!value.empty() && iswspace(value.front())) value.erase(value.begin());
    while (!value.empty() && iswspace(value.back())) value.pop_back();
    return value;
  };
  const std::wstring primaryText = trim(GetWindowString(hwnd, IDC_HOTKEY_PRIMARY));
  const std::wstring secondaryText = trim(GetWindowString(hwnd, IDC_HOTKEY_SECONDARY));
  HotkeySetting primary{}, secondary{};
  if (!ParseHotkeyText(primaryText, primary)) {
    MessageBoxW(hwnd, L"请设置有效的截图快捷键（例如 Ctrl+\\）。", L"RC-ScreenShot", MB_ICONWARNING);
    SetFocus(GetDlgItem(hwnd, IDC_HOTKEY_PRIMARY)); return false;
  }
  std::vector<HotkeySetting> hotkeys{primary};
  if (!secondaryText.empty()) {
    if (!ParseHotkeyText(secondaryText, secondary)) {
      MessageBoxW(hwnd, L"连拍快捷键格式无效。", L"RC-ScreenShot", MB_ICONWARNING);
      SetFocus(GetDlgItem(hwnd, IDC_HOTKEY_SECONDARY)); return false;
    }
    if (secondary.modifiers == primary.modifiers && secondary.virtualKey == primary.virtualKey) {
      MessageBoxW(hwnd, L"截图快捷键与连拍快捷键不能重复。", L"RC-ScreenShot", MB_ICONWARNING); return false;
    }
    hotkeys.push_back(secondary);
  }
  std::wstring outputDirectory = GetWindowString(hwnd, IDC_OUTPUT);
  while (!outputDirectory.empty() && iswspace(outputDirectory.front())) outputDirectory.erase(outputDirectory.begin());
  while (!outputDirectory.empty() && iswspace(outputDirectory.back())) outputDirectory.pop_back();
  if (outputDirectory.empty()) outputDirectory = DefaultOutputDirectory().wstring();
  const auto parseInteger = [](const std::wstring& text, int& value) {
    if (text.empty()) return false;
    wchar_t* end = nullptr;
    const long parsed = wcstol(text.c_str(), &end, 10);
    while (end && iswspace(*end)) ++end;
    if (!end || *end != L'\0' || parsed < INT_MIN || parsed > INT_MAX) return false;
    value = static_cast<int>(parsed);
    return true;
  };
  const auto parseFloat = [](const std::wstring& text, float& value) {
    if (text.empty()) return false;
    wchar_t* end = nullptr;
    const float parsed = wcstof(text.c_str(), &end);
    while (end && iswspace(*end)) ++end;
    if (!end || *end != L'\0' || !std::isfinite(parsed)) return false;
    value = parsed;
    return true;
  };
  int burstCount = 0;
  if (!parseInteger(GetWindowString(hwnd, IDC_BURST_COUNT), burstCount) || burstCount < 2 || burstCount > 30) {
    MessageBoxW(hwnd, L"连拍张数必须是 2 到 30 之间的整数。", L"RC-ScreenShot", MB_ICONWARNING);
    SetFocus(GetDlgItem(hwnd, IDC_BURST_COUNT)); return false;
  }
  float burstInterval = 0.0f;
  if (!parseFloat(GetWindowString(hwnd, IDC_BURST_INTERVAL), burstInterval) ||
      burstInterval < 0.05f || burstInterval > 0.99f) {
    MessageBoxW(hwnd, L"连拍间隔必须是 0.05 到 0.99 秒之间的数字。", L"RC-ScreenShot", MB_ICONWARNING);
    SetFocus(GetDlgItem(hwnd, IDC_BURST_INTERVAL)); return false;
  }
  config_.hotkeys = std::move(hotkeys); config_.outputDirectory = std::move(outputDirectory);
  config_.burstCount = burstCount;
  config_.burstIntervalSeconds = burstInterval;
  return true;
}

void Application::UpdateQualitySlider(HWND hwnd, int x) {
  const RECT track = QualitySliderRect();
  const float t = std::clamp((x - track.left) / static_cast<float>(track.right - track.left), 0.0f, 1.0f);
  config_.jpegQuality = std::clamp(static_cast<int>(std::lround(1.0f + t * 99.0f)), 1, 100);
  RECT dirty = track;
  dirty.right = 390;  // extend past the numeric readout drawn by WM_PAINT
  InflateRect(&dirty, 6, 6);
  InvalidateRect(hwnd, &dirty, FALSE);
}

bool Application::UpdateAutoStart(std::wstring* error) {
  HKEY key = nullptr;
  LONG status = RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                                0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
  if (status != ERROR_SUCCESS) { if (error) *error = L"无法打开登录启动注册表项。"; return false; }
  ScopeExit close{[&] { RegCloseKey(key); }};
  if (!config_.launchAtLogin) { RegDeleteValueW(key, L"RC-ScreenShot"); return true; }
  std::wstring command = L"\"" + executablePath_.wstring() + L"\"" + (config_.silentAtLogin ? L" --silent" : L"");
  status = RegSetValueExW(key, L"RC-ScreenShot", 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()),
                          static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
  if (status != ERROR_SUCCESS && error) *error = L"写入登录启动注册表项失败。";
  return status == ERROR_SUCCESS;
}

void Application::SaveConfig() {
  std::wstring error;
  if (!configStore_.Save(config_, &error)) Notify(L"配置无法保存", error, NIIF_WARNING);
  else config_.schemaVersion = 4;
}

void Application::Notify(std::wstring_view title, std::wstring_view message, DWORD flags) {
  NOTIFYICONDATAW data = trayIcon_; data.uFlags = NIF_INFO; data.dwInfoFlags = flags;
  wcsncpy_s(data.szInfoTitle, title.data(), _TRUNCATE); wcsncpy_s(data.szInfo, message.data(), _TRUNCATE);
  Shell_NotifyIconW(NIM_MODIFY, &data);
}

bool Application::HasArgument(std::span<wchar_t*> arguments, std::wstring_view name) const {
  return std::any_of(arguments.begin(), arguments.end(), [&](const wchar_t* value) { return value && name == value; });
}

}  // namespace rc
