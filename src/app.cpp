#include "app.hpp"

#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>

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
constexpr int IDC_HOTKEY_PRIMARY = 2016;
constexpr int IDC_HOTKEY_SECONDARY = 2017;
constexpr int IDC_TOGGLE_AUTOSAVE = 2101;
constexpr int IDC_TOGGLE_AUTOSTART = 2102;
constexpr int IDC_TOGGLE_SILENT = 2103;
constexpr int IDC_TOGGLE_SHADOW = 2104;
constexpr int IDC_TOGGLE_FRAME = 2105;
constexpr int IDC_ACTION_COPY = 2106;
constexpr int IDC_ACTION_SAVE = 2107;

constexpr int kSettingsWidth = 1040;
constexpr int kSettingsHeight = 680;

RECT QualitySliderRect() {
  return {150, 420, 390, 434};
}

bool IsToggleId(int id) {
  return id == IDC_TOGGLE_AUTOSAVE || id == IDC_TOGGLE_AUTOSTART || id == IDC_TOGGLE_SILENT ||
         id == IDC_TOGGLE_SHADOW || id == IDC_TOGGLE_FRAME;
}

bool IsHotkeyId(int id) { return id == IDC_HOTKEY_PRIMARY || id == IDC_HOTKEY_SECONDARY; }

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
  return id == IDC_HOTKEY_PRIMARY || id == IDC_HOTKEY_SECONDARY || id == IDC_OUTPUT;
}

RECT SettingsInputFrameRect(int id) {
  switch (id) {
    case IDC_HOTKEY_PRIMARY: return {154, 166, 466, 216};
    case IDC_HOTKEY_SECONDARY: return {584, 166, 896, 216};
    case IDC_OUTPUT: return {40, 354, 416, 402};
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
  if (!std::filesystem::exists(configStore_.path()) || config_.schemaVersion < 3) SaveConfig();
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
    case WM_HOTKEY: StartCapture(); return 0;
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
          std::wstring text = L"RC-ScreenShot 0.3.3\n\n原生 C++20 / DXGI / Direct2D 截图工具\n\n";
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

void Application::ProcessOverlayResult(std::unique_ptr<OverlayResult> result) {
  if (!overlay_) return;
  if (result->completion != CaptureCompletion::Cancel) {
    RenderedImage image; std::wstring error;
    if (!exporter_.Render(overlay_->snapshot(), result->selection, result->document, config_,
                          result->windowSelection, image, error)) {
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
  if (!settingsBackgroundBrush_) settingsBackgroundBrush_ = CreateSolidBrush(RGB(10, 15, 23));
  if (!settingsPanelBrush_) settingsPanelBrush_ = CreateSolidBrush(RGB(17, 25, 36));
  if (!settingsControlBrush_) settingsControlBrush_ = CreateSolidBrush(RGB(25, 37, 52));
  if (!settingsFont_) {
    settingsFont_ = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH, L"Microsoft YaHei UI");
    settingsTitleFont_ = CreateFontW(-27, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                     DEFAULT_PITCH, L"Microsoft YaHei UI");
    settingsSectionFont_ = CreateFontW(-16, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                       DEFAULT_PITCH, L"Microsoft YaHei UI");
    settingsSmallFont_ = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
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
  const DWORD settingsStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
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
    setFont(control); return control;
  };
  const auto hotkeyButton = [&](int x, int y, int w, int h, int id) {
    HWND control = button(L"", x, y, w, h, id);
    SetWindowSubclass(control, HotkeyCaptureProc, 1, reinterpret_cast<DWORD_PTR>(this));
    return control;
  };
  const auto toggle = [&](int x, int y, int id) {
    HWND control = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                 x, y, 70, 30, settingsWindow_,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
    setFont(control, settingsSmallFont_); return control;
  };
  label(L"设置中心", 32, 24, 320, 38, settingsTitleFont_);
  label(L"调整截图、导出和启动行为", 32, 62, 420, 20, settingsSmallFont_);
    label(L"RCSS · v0.3.3", 900, 36, 110, 20, settingsSmallFont_);
  label(L"快捷键", 64, 116, 180, 24, settingsSectionFont_);
  label(L"主快捷键必填，副快捷键可选", 64, 144, 360, 18, settingsSmallFont_);
  label(L"主快捷键", 64, 180, 90, 18, settingsSmallFont_);
  hotkeyButton(160, 172, 300, 38, IDC_HOTKEY_PRIMARY);
  label(L"副快捷键", 490, 180, 90, 18, settingsSmallFont_);
  hotkeyButton(590, 172, 300, 38, IDC_HOTKEY_SECONDARY);
  label(L"重复快捷键会被拒绝", 64, 218, 360, 18, settingsSmallFont_);
  label(L"输出", 64, 278, 180, 24, settingsSectionFont_);
  label(L"保存位置与导出质量", 64, 306, 260, 18, settingsSmallFont_);
  label(L"截图目录", 48, 340, 90, 18, settingsSmallFont_);
  edit(L"", 48, 360, 360, 36, IDC_OUTPUT);
  button(L"浏览", 420, 360, 64, 36, IDC_BROWSE);
  label(L"JPEG 质量", 48, 412, 100, 18, settingsSmallFont_);
  label(L"Enter 默认动作", 48, 450, 100, 18, settingsSmallFont_);
  button(L"复制", 150, 448, 90, 30, IDC_ACTION_COPY);
  button(L"保存", 248, 448, 90, 30, IDC_ACTION_SAVE);
  label(L"编辑器", 556, 278, 180, 24, settingsSectionFont_);
  label(L"文字与截图层效果", 556, 306, 240, 18, settingsSmallFont_);
  label(L"窗口截图阴影", 556, 360, 160, 22, settingsFont_); toggle(900, 354, IDC_TOGGLE_SHADOW);
  label(L"截图外框", 556, 420, 120, 22, settingsFont_); toggle(900, 414, IDC_TOGGLE_FRAME);
  label(L"行为", 64, 520, 180, 24, settingsSectionFont_);
  label(L"高频选项，修改后保存即可生效", 64, 548, 300, 18, settingsSmallFont_);
  label(L"复制后自动保存", 64, 568, 130, 22, settingsFont_); toggle(200, 564, IDC_TOGGLE_AUTOSAVE);
  label(L"登录时启动", 392, 568, 120, 22, settingsFont_); toggle(528, 564, IDC_TOGGLE_AUTOSTART);
  label(L"自启时静默", 720, 568, 120, 22, settingsFont_); toggle(856, 564, IDC_TOGGLE_SILENT);
  label(L"截图提示：V 选择对象 · Ctrl+Z / Ctrl+Y 撤销 · Esc 取消", 64, 638, 680, 20,
        settingsSmallFont_);
  button(L"取消", 840, 630, 80, 36, IDC_CANCEL_SETTINGS);
  button(L"保存设置", 932, 630, 84, 36, IDC_SAVE_SETTINGS);
  BOOL darkTitle = TRUE; DwmSetWindowAttribute(settingsWindow_, 20, &darkTitle, sizeof(darkTitle));
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
    FillRect(dc, &client, settingsBackgroundBrush_); SetBkMode(dc, TRANSPARENT);
    const auto rounded = [&](RECT rect, COLORREF fill, COLORREF border, int radius = 14) {
      HBRUSH brush = CreateSolidBrush(fill); HPEN pen = CreatePen(PS_SOLID, 1, border);
      HGDIOBJ oldBrush = SelectObject(dc, brush); HGDIOBJ oldPen = SelectObject(dc, pen);
      RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
      SelectObject(dc, oldPen); SelectObject(dc, oldBrush); DeleteObject(pen); DeleteObject(brush);
    };
    rounded({24, 104, 1016, 250}, RGB(17, 25, 36), RGB(34, 48, 67));
    rounded({24, 266, 500, 490}, RGB(17, 25, 36), RGB(34, 48, 67));
    rounded({516, 266, 1016, 490}, RGB(17, 25, 36), RGB(34, 48, 67));
    rounded({24, 506, 1016, 610}, RGB(17, 25, 36), RGB(34, 48, 67));
    const auto drawIcon = [&](int type, int x, int y) {
      HPEN iconPen = CreatePen(PS_SOLID, 1, RGB(91, 160, 255));
      HGDIOBJ oldPen = SelectObject(dc, iconPen);
      HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
      if (type == 0) {  // keyboard
        RoundRect(dc, x, y, x + 22, y + 14, 3, 3);
        for (int key = 0; key < 4; ++key) {
          MoveToEx(dc, x + 4 + key * 4, y + 4, nullptr);
          LineTo(dc, x + 6 + key * 4, y + 4);
        }
        MoveToEx(dc, x + 7, y + 10, nullptr); LineTo(dc, x + 15, y + 10);
      } else if (type == 1) {  // folder
        MoveToEx(dc, x, y + 4, nullptr); LineTo(dc, x + 7, y + 4); LineTo(dc, x + 10, y + 1);
        LineTo(dc, x + 17, y + 1); LineTo(dc, x + 20, y + 4); LineTo(dc, x + 22, y + 14);
        LineTo(dc, x, y + 14); LineTo(dc, x, y + 4);
      } else if (type == 2) {  // pencil
        MoveToEx(dc, x + 3, y + 15, nullptr); LineTo(dc, x + 5, y + 10);
        LineTo(dc, x + 16, y - 1); LineTo(dc, x + 21, y + 4); LineTo(dc, x + 10, y + 15);
        LineTo(dc, x + 3, y + 15); MoveToEx(dc, x + 14, y + 1, nullptr); LineTo(dc, x + 19, y + 6);
      } else if (type == 3) {  // rocket
        Ellipse(dc, x + 7, y, x + 17, y + 12);
        MoveToEx(dc, x + 7, y + 8, nullptr); LineTo(dc, x + 2, y + 13); LineTo(dc, x + 8, y + 12);
        MoveToEx(dc, x + 17, y + 8, nullptr); LineTo(dc, x + 22, y + 13); LineTo(dc, x + 16, y + 12);
        MoveToEx(dc, x + 9, y + 12, nullptr); LineTo(dc, x + 9, y + 17); LineTo(dc, x + 12, y + 14);
        LineTo(dc, x + 15, y + 17); LineTo(dc, x + 15, y + 12);
      } else {  // information
        Ellipse(dc, x + 2, y, x + 18, y + 16);
        MoveToEx(dc, x + 10, y + 6, nullptr); LineTo(dc, x + 10, y + 12);
        Ellipse(dc, x + 9, y + 3, x + 11, y + 5);
      }
      SelectObject(dc, oldBrush); SelectObject(dc, oldPen); DeleteObject(iconPen);
    };
    drawIcon(0, 37, 131);
    drawIcon(1, 37, 293);
    drawIcon(2, 529, 293);
    drawIcon(3, 37, 531);
    drawIcon(4, 43, 639);
    const auto drawInputFrame = [&](RECT rect, HWND control) {
      const bool focused = control && GetFocus() == control;
      HBRUSH brush = CreateSolidBrush(RGB(25, 37, 52));
      HPEN pen = CreatePen(PS_SOLID, 1, focused ? RGB(91, 160, 255) : RGB(49, 70, 96));
      HGDIOBJ oldBrush = SelectObject(dc, brush); HGDIOBJ oldPen = SelectObject(dc, pen);
      RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, 12, 12);
      SelectObject(dc, oldPen); SelectObject(dc, oldBrush); DeleteObject(pen); DeleteObject(brush);
    };
    drawInputFrame({154, 166, 466, 216}, GetDlgItem(hwnd, IDC_HOTKEY_PRIMARY));
    drawInputFrame({584, 166, 896, 216}, GetDlgItem(hwnd, IDC_HOTKEY_SECONDARY));
    drawInputFrame({40, 354, 416, 402}, GetDlgItem(hwnd, IDC_OUTPUT));
    const RECT track = QualitySliderRect(); const int lineY = (track.top + track.bottom) / 2;
    HBRUSH trackBrush = CreateSolidBrush(RGB(38, 53, 73)); HGDIOBJ old = SelectObject(dc, trackBrush);
    RoundRect(dc, track.left, lineY - 2, track.right, lineY + 2, 2, 2); SelectObject(dc, old); DeleteObject(trackBrush);
    const int thumbX = track.left + (track.right - track.left) * (config_.jpegQuality - 1) / 99;
    HBRUSH fillBrush = CreateSolidBrush(RGB(91, 140, 255)); old = SelectObject(dc, fillBrush);
    RoundRect(dc, track.left, lineY - 2, thumbX, lineY + 2, 2, 2); SelectObject(dc, old); DeleteObject(fillBrush);
    HBRUSH thumbBrush = CreateSolidBrush(RGB(233, 241, 255)); old = SelectObject(dc, thumbBrush);
    Ellipse(dc, thumbX - 7, lineY - 7, thumbX + 7, lineY + 7); SelectObject(dc, old); DeleteObject(thumbBrush);
    EndPaint(hwnd, &paint); return 0;
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
    SetTextColor(dc, (font == settingsTitleFont_ || font == settingsSectionFont_) ? RGB(241, 246, 253)
                 : font == settingsFont_ ? RGB(220, 228, 240) : RGB(139, 160, 190));
    SetBkMode(dc, TRANSPARENT); return reinterpret_cast<LRESULT>(GetStockObject(NULL_BRUSH));
  }
  if (message == WM_CTLCOLOREDIT) {
    HDC dc = reinterpret_cast<HDC>(wParam);
    SetTextColor(dc, RGB(235, 240, 248));
    SetBkMode(dc, OPAQUE);
    SetBkColor(dc, RGB(25, 37, 52));
    return reinterpret_cast<LRESULT>(settingsControlBrush_);
  }
  if (message == WM_CTLCOLORBTN) {
    HDC dc = reinterpret_cast<HDC>(wParam); SetTextColor(dc, RGB(224, 232, 244));
    SetBkMode(dc, TRANSPARENT); return reinterpret_cast<LRESULT>(settingsPanelBrush_);
  }
  if (message == WM_DRAWITEM) {
    const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam); if (!item || item->CtlType != ODT_BUTTON) return 0;
    const int id = GetDlgCtrlID(item->hwndItem); const bool pressed = (item->itemState & ODS_SELECTED) != 0;
    if (IsHotkeyId(id)) {
      const auto& state = HotkeyStateFor(item->hwndItem);
      const RECT r = item->rcItem;
      const COLORREF fill = state.listening ? RGB(30, 55, 90) : RGB(25, 37, 52);
      const COLORREF border = state.listening ? RGB(91, 160, 255) : RGB(49, 70, 96);
      HBRUSH brush = CreateSolidBrush(fill);
      HPEN pen = CreatePen(PS_SOLID, state.listening ? 2 : 1, border);
      HGDIOBJ oldBrush = SelectObject(item->hDC, brush);
      HGDIOBJ oldPen = SelectObject(item->hDC, pen);
      RoundRect(item->hDC, r.left, r.top, r.right, r.bottom, 12, 12);
      SelectObject(item->hDC, oldPen); SelectObject(item->hDC, oldBrush);
      DeleteObject(pen); DeleteObject(brush);
      std::wstring display = GetWindowString(settingsWindow_, id);
      if (state.listening && !state.submitted) {
        display = state.modifiers ? HotkeyModifierPreview(state.modifiers) : L"按下组合键…";
      }
      SetBkMode(item->hDC, TRANSPARENT);
      SetTextColor(item->hDC, state.listening ? RGB(232, 242, 255) : RGB(220, 228, 240));
      SelectObject(item->hDC, settingsFont_);
      RECT textRect = r;
      DrawTextW(item->hDC, display.c_str(), -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      if (state.listening) {
        SetTextColor(item->hDC, RGB(130, 175, 235));
        RECT hintRect{r.right - 62, r.top, r.right - 8, r.bottom};
        DrawTextW(item->hDC, L"监听中", -1, &hintRect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
      }
      return TRUE;
    }
    if (IsToggleId(id)) {
      const bool on = ToggleValue(config_, id); const RECT r = item->rcItem; const int height = r.bottom - r.top;
      HBRUSH track = CreateSolidBrush(on ? (pressed ? RGB(48, 101, 205) : RGB(60, 124, 235))
                                         : (pressed ? RGB(50, 62, 80) : RGB(39, 50, 66)));
      HPEN border = CreatePen(PS_SOLID, 1, on ? RGB(123, 170, 255) : RGB(95, 113, 137));
      HGDIOBJ oldBrush = SelectObject(item->hDC, track); HGDIOBJ oldPen = SelectObject(item->hDC, border);
      RoundRect(item->hDC, r.left, r.top, r.right, r.bottom, height / 2, height / 2);
      SelectObject(item->hDC, oldPen); SelectObject(item->hDC, oldBrush); DeleteObject(border); DeleteObject(track);
      const int radius = height / 2 - 4; const int knobX = on ? r.right - radius - 4 : r.left + radius + 4;
      HBRUSH knob = CreateSolidBrush(on ? RGB(255, 255, 255) : RGB(220, 228, 240)); oldBrush = SelectObject(item->hDC, knob);
      Ellipse(item->hDC, knobX - radius, (r.top + r.bottom) / 2 - radius, knobX + radius, (r.top + r.bottom) / 2 + radius);
      SelectObject(item->hDC, oldBrush); DeleteObject(knob);
      SetBkMode(item->hDC, TRANSPARENT); SetTextColor(item->hDC, on ? RGB(255, 255, 255) : RGB(185, 198, 218));
      SelectObject(item->hDC, settingsSmallFont_);
      RECT stateRect = on ? RECT{r.left + 4, r.top, r.left + (r.right - r.left) / 2, r.bottom}
                          : RECT{r.left + (r.right - r.left) / 2, r.top, r.right - 4, r.bottom};
      DrawTextW(item->hDC, on ? L"开" : L"关", -1, &stateRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      return TRUE;
    }
    if (IsSegmentId(id)) {
      const bool active = (id == IDC_ACTION_COPY && config_.defaultAction == DefaultAction::Copy) ||
                          (id == IDC_ACTION_SAVE && config_.defaultAction == DefaultAction::Save);
      const COLORREF fill = active ? (pressed ? RGB(68, 111, 219) : RGB(91, 140, 255))
                                   : (pressed ? RGB(31, 49, 77) : RGB(24, 35, 50));
      const COLORREF borderColor = active ? RGB(123, 170, 255) : RGB(46, 65, 90);
      HBRUSH brush = CreateSolidBrush(fill);
      HPEN pen = CreatePen(PS_SOLID, 1, borderColor);
      HGDIOBJ oldBrush = SelectObject(item->hDC, brush);
      HGDIOBJ oldPen = SelectObject(item->hDC, pen);
      RoundRect(item->hDC, item->rcItem.left, item->rcItem.top, item->rcItem.right,
                item->rcItem.bottom, 10, 10);
      SelectObject(item->hDC, oldPen);
      SelectObject(item->hDC, oldBrush);
      DeleteObject(pen);
      DeleteObject(brush);
      wchar_t text[128]{};
      GetWindowTextW(item->hwndItem, text, _countof(text));
      SetBkMode(item->hDC, TRANSPARENT);
      SetTextColor(item->hDC, active ? RGB(255, 255, 255) : RGB(185, 198, 218));
      SelectObject(item->hDC, settingsFont_);
      RECT textRect = item->rcItem;
      DrawTextW(item->hDC, text, -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      return TRUE;
    }
    const bool primary = id == IDC_SAVE_SETTINGS; const bool accent = id == IDC_BROWSE;
    const COLORREF fill = primary ? (pressed ? RGB(68, 111, 219) : RGB(91, 140, 255))
                                  : accent ? (pressed ? RGB(44, 79, 137) : RGB(36, 61, 101))
                                           : (pressed ? RGB(31, 49, 77) : RGB(24, 35, 50));
    HBRUSH brush = CreateSolidBrush(fill); HPEN pen = CreatePen(PS_SOLID, 1, RGB(46, 65, 90));
    HGDIOBJ oldBrush = SelectObject(item->hDC, brush); HGDIOBJ oldPen = SelectObject(item->hDC, pen);
    RoundRect(item->hDC, item->rcItem.left, item->rcItem.top, item->rcItem.right, item->rcItem.bottom, 10, 10);
    SelectObject(item->hDC, oldPen); SelectObject(item->hDC, oldBrush); DeleteObject(pen); DeleteObject(brush);
    wchar_t text[128]{}; GetWindowTextW(item->hwndItem, text, _countof(text)); SetBkMode(item->hDC, TRANSPARENT);
    SetTextColor(item->hDC, RGB(242, 247, 255)); SelectObject(item->hDC, settingsFont_); RECT textRect = item->rcItem;
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
    MessageBoxW(hwnd, L"请设置有效的主快捷键（例如 Ctrl+\\）。", L"RC-ScreenShot", MB_ICONWARNING);
    SetFocus(GetDlgItem(hwnd, IDC_HOTKEY_PRIMARY)); return false;
  }
  std::vector<HotkeySetting> hotkeys{primary};
  if (!secondaryText.empty()) {
    if (!ParseHotkeyText(secondaryText, secondary)) {
      MessageBoxW(hwnd, L"副快捷键格式无效。", L"RC-ScreenShot", MB_ICONWARNING);
      SetFocus(GetDlgItem(hwnd, IDC_HOTKEY_SECONDARY)); return false;
    }
    if (secondary.modifiers == primary.modifiers && secondary.virtualKey == primary.virtualKey) {
      MessageBoxW(hwnd, L"主快捷键与副快捷键不能重复。", L"RC-ScreenShot", MB_ICONWARNING); return false;
    }
    hotkeys.push_back(secondary);
  }
  std::wstring outputDirectory = GetWindowString(hwnd, IDC_OUTPUT);
  while (!outputDirectory.empty() && iswspace(outputDirectory.front())) outputDirectory.erase(outputDirectory.begin());
  while (!outputDirectory.empty() && iswspace(outputDirectory.back())) outputDirectory.pop_back();
  if (outputDirectory.empty()) outputDirectory = DefaultOutputDirectory().wstring();
  config_.hotkeys = std::move(hotkeys); config_.outputDirectory = std::move(outputDirectory);
  return true;
}

void Application::UpdateQualitySlider(HWND hwnd, int x) {
  const RECT track = QualitySliderRect();
  const float t = std::clamp((x - track.left) / static_cast<float>(track.right - track.left), 0.0f, 1.0f);
  config_.jpegQuality = std::clamp(static_cast<int>(std::lround(1.0f + t * 99.0f)), 1, 100);
  RECT dirty = track;
  InflateRect(&dirty, 8, 8);
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
  else config_.schemaVersion = 3;
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
