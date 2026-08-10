#include "app.hpp"

#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>

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

constexpr int IDC_HOTKEYS = 2001;
constexpr int IDC_OUTPUT = 2002;
constexpr int IDC_BROWSE = 2003;
constexpr int IDC_QUALITY = 2004;
constexpr int IDC_AUTOSAVE = 2005;
constexpr int IDC_AUTOSTART = 2006;
constexpr int IDC_SILENT = 2007;
constexpr int IDC_SHADOW = 2008;
constexpr int IDC_FRAME = 2009;
constexpr int IDC_DEFAULT_ACTION = 2010;
constexpr int IDC_SAVE_SETTINGS = 2011;
constexpr int IDC_CANCEL_SETTINGS = 2012;
constexpr int IDC_HOTKEY_CAPTURE = 2013;
constexpr int IDC_HOTKEY_ADD = 2014;
constexpr int IDC_HOTKEY_REMOVE = 2015;

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
  if (!std::filesystem::exists(configStore_.path())) SaveConfig();
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
          std::wstring text = L"RC-ScreenShot 0.2.0\n\n原生 C++20 / DXGI / Direct2D 截图工具\n\n";
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
  if (!settingsBackgroundBrush_) settingsBackgroundBrush_ = CreateSolidBrush(RGB(15, 18, 24));
  if (!settingsPanelBrush_) settingsPanelBrush_ = CreateSolidBrush(RGB(25, 30, 40));
  if (!settingsControlBrush_) settingsControlBrush_ = CreateSolidBrush(RGB(32, 38, 50));
  if (!settingsFont_) {
    settingsFont_ = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH, L"Microsoft YaHei UI");
    settingsTitleFont_ = CreateFontW(-27, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                     DEFAULT_PITCH, L"Microsoft YaHei UI");
    settingsSmallFont_ = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                     DEFAULT_PITCH, L"Microsoft YaHei UI");
  }
  WNDCLASSEXW windowClass{sizeof(windowClass)}; windowClass.lpfnWndProc = SettingsProc;
  windowClass.hInstance = instance_; windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  windowClass.hIcon = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(102), IMAGE_ICON,
                                                    0, 0, LR_DEFAULTSIZE | LR_SHARED));
  windowClass.hIconSm = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(102), IMAGE_ICON,
                                                      16, 16, LR_SHARED));
  windowClass.hbrBackground = settingsBackgroundBrush_; windowClass.lpszClassName = L"RC-ScreenShot.Settings";
  RegisterClassExW(&windowClass);
  settingsWindow_ = CreateWindowExW(WS_EX_APPWINDOW | WS_EX_CONTROLPARENT, windowClass.lpszClassName, L"RC-ScreenShot 设置",
                                    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                    CW_USEDEFAULT, CW_USEDEFAULT, 760, 640, nullptr, nullptr, instance_, this);
  if (!settingsWindow_) return;
  const auto setFont = [&](HWND control, HFONT font = nullptr) {
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font ? font : settingsFont_), TRUE);
  };
  const auto label = [&](const wchar_t* text, int x, int y, int w, int h, HFONT font = nullptr) {
    HWND control = CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE, x, y, w, h,
                                 settingsWindow_, nullptr, instance_, nullptr);
    setFont(control, font);
    return control;
  };
  const auto edit = [&](DWORD exStyle, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id) {
    HWND control = CreateWindowExW(exStyle, L"EDIT", text, style, x, y, w, h, settingsWindow_,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
    setFont(control);
    return control;
  };
  const auto button = [&](const wchar_t* text, int x, int y, int w, int h, int id) {
    HWND control = CreateWindowW(L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                 x, y, w, h, settingsWindow_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                 instance_, nullptr);
    setFont(control);
    return control;
  };
  const auto check = [&](const wchar_t* text, int x, int y, int w, int h, int id) {
    HWND control = CreateWindowW(L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                 x, y, w, h, settingsWindow_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                 instance_, nullptr);
    setFont(control);
    return control;
  };

  label(L"RC-ScreenShot", 32, 22, 400, 38, settingsTitleFont_);
  label(L"截图、标注与输出都可以在这里一次配置好", 34, 59, 450, 22, settingsSmallFont_);
  label(L"快捷键", 32, 101, 150, 26);
  label(L"按下组合键即可采集，保存后立即生效", 32, 126, 260, 20, settingsSmallFont_);
  edit(WS_EX_STATICEDGE, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
       32, 151, 246, 32, IDC_HOTKEY_CAPTURE);
  HWND capture = GetDlgItem(settingsWindow_, IDC_HOTKEY_CAPTURE);
  SetWindowSubclass(capture, HotkeyCaptureProc, 1, reinterpret_cast<DWORD_PTR>(this));
  button(L"添加", 288, 151, 82, 32, IDC_HOTKEY_ADD);
  button(L"移除所选", 380, 151, 94, 32, IDC_HOTKEY_REMOVE);
  label(L"当前快捷键", 492, 145, 150, 20, settingsSmallFont_);
  edit(WS_EX_STATICEDGE, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL |
       ES_READONLY | WS_VSCROLL, 492, 166, 236, 50, IDC_HOTKEYS);

  label(L"输出", 32, 266, 150, 26);
  label(L"截图目录", 32, 300, 86, 24);
  edit(WS_EX_STATICEDGE, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
       128, 296, 488, 32, IDC_OUTPUT);
  button(L"浏览", 628, 296, 100, 32, IDC_BROWSE);
  label(L"JPEG 质量", 32, 342, 86, 24);
  edit(WS_EX_STATICEDGE, L"95", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER,
       128, 338, 72, 32, IDC_QUALITY);
  label(L"Enter 默认动作", 246, 342, 120, 24);
  HWND combo = CreateWindowW(WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
                             376, 338, 150, 180, settingsWindow_,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_DEFAULT_ACTION)), instance_, nullptr);
  setFont(combo);
  SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"复制到剪贴板"));
  SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"保存到本地"));

  label(L"行为与外观", 32, 412, 180, 26);
  check(L"复制时自动本地保存", 32, 447, 220, 28, IDC_AUTOSAVE);
  check(L"登录时启动", 280, 447, 180, 28, IDC_AUTOSTART);
  check(L"自启时静默", 492, 447, 180, 28, IDC_SILENT);
  check(L"窗口截图添加阴影", 32, 480, 220, 28, IDC_SHADOW);
  check(L"启用截图外框", 280, 480, 180, 28, IDC_FRAME);
  label(L"外框样式仅在设置页控制，不占用截图时的浮动工具栏。", 492, 481, 236, 38, settingsSmallFont_);
  label(L"截图时可用：空格切换模式 · V 选择对象 · Ctrl+Z / Ctrl+Y 撤销与重做 · Esc 取消", 32, 545, 650, 22, settingsSmallFont_);
  button(L"保存设置", 548, 577, 100, 34, IDC_SAVE_SETTINGS);
  button(L"取消", 660, 577, 68, 34, IDC_CANCEL_SETTINGS);
  BOOL darkTitle = TRUE;
  DwmSetWindowAttribute(settingsWindow_, 20, &darkTitle, sizeof(darkTitle));
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
    PAINTSTRUCT paint{};
    BeginPaint(hwnd, &paint);
    HDC dc = paint.hdc;
    RECT client{};
    GetClientRect(hwnd, &client);
    FillRect(dc, &client, settingsBackgroundBrush_);
    const auto panel = [&](int top, int bottom) {
      RECT rect{24, top, client.right - 24, bottom};
      HGDIOBJ oldBrush = SelectObject(dc, settingsPanelBrush_);
      HPEN pen = CreatePen(PS_SOLID, 1, RGB(43, 50, 64));
      HGDIOBJ oldPen = SelectObject(dc, pen);
      RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, 16, 16);
      SelectObject(dc, oldPen); SelectObject(dc, oldBrush); DeleteObject(pen);
    };
    panel(88, 238); panel(250, 386); panel(400, 530);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(116, 171, 245));
    SelectObject(dc, settingsSmallFont_);
    const std::wstring badge = L"CONFIGURATION";
    TextOutW(dc, 32, 83, badge.c_str(), static_cast<int>(badge.size()));
    EndPaint(hwnd, &paint);
    return 0;
  }
  if (message == WM_CTLCOLORSTATIC) {
    HDC dc = reinterpret_cast<HDC>(wParam);
    SetTextColor(dc, RGB(224, 230, 241));
    SetBkMode(dc, TRANSPARENT);
    return reinterpret_cast<LRESULT>(settingsBackgroundBrush_);
  }
  if (message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX) {
    HDC dc = reinterpret_cast<HDC>(wParam);
    SetTextColor(dc, RGB(235, 240, 248));
    SetBkColor(dc, RGB(32, 38, 50));
    return reinterpret_cast<LRESULT>(settingsControlBrush_);
  }
  if (message == WM_CTLCOLORBTN) {
    HDC dc = reinterpret_cast<HDC>(wParam);
    SetTextColor(dc, RGB(224, 230, 241));
    SetBkMode(dc, TRANSPARENT);
    return reinterpret_cast<LRESULT>(settingsPanelBrush_);
  }
  if (message == WM_DRAWITEM) {
    const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
    if (item && item->CtlType == ODT_BUTTON) {
      const bool primary = GetDlgCtrlID(item->hwndItem) == IDC_SAVE_SETTINGS;
      const bool pressed = (item->itemState & ODS_SELECTED) != 0;
      const COLORREF fill = primary ? (pressed ? RGB(54, 123, 214) : RGB(70, 143, 235))
                                    : (pressed ? RGB(48, 57, 73) : RGB(37, 45, 59));
      HBRUSH brush = CreateSolidBrush(fill);
      HPEN pen = CreatePen(PS_SOLID, 1, primary ? RGB(92, 165, 247) : RGB(62, 73, 92));
      HGDIOBJ oldBrush = SelectObject(item->hDC, brush);
      HGDIOBJ oldPen = SelectObject(item->hDC, pen);
      RoundRect(item->hDC, item->rcItem.left, item->rcItem.top, item->rcItem.right, item->rcItem.bottom, 10, 10);
      SelectObject(item->hDC, oldPen); SelectObject(item->hDC, oldBrush);
      DeleteObject(pen); DeleteObject(brush);
      wchar_t text[128]{};
      GetWindowTextW(item->hwndItem, text, _countof(text));
      SetBkMode(item->hDC, TRANSPARENT);
      SetTextColor(item->hDC, RGB(242, 247, 255));
      SelectObject(item->hDC, settingsFont_);
      RECT textRect = item->rcItem;
      DrawTextW(item->hDC, text, -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      if (item->itemState & ODS_FOCUS) DrawFocusRect(item->hDC, &textRect);
      return TRUE;
    }
  }
  if (message == WM_COMMAND) {
    if (LOWORD(wParam) == IDC_SAVE_SETTINGS) {
      if (ReadSettings(hwnd)) { SaveConfig(); RegisterConfiguredHotkeys(); UpdateAutoStart(); DestroyWindow(hwnd); }
      return 0;
    }
    if (LOWORD(wParam) == IDC_CANCEL_SETTINGS) { DestroyWindow(hwnd); return 0; }
    if (LOWORD(wParam) == IDC_HOTKEY_ADD) {
      const std::wstring captured = GetWindowString(hwnd, IDC_HOTKEY_CAPTURE);
      if (captured.empty()) return 0;
      HotkeySetting key;
      if (!ParseHotkeyText(captured, key)) {
        MessageBoxW(hwnd, L"请先按下包含 Ctrl、Alt、Shift 或 Win 的快捷键组合。", L"快捷键", MB_ICONWARNING);
        return 0;
      }
      std::wstring list = GetWindowString(hwnd, IDC_HOTKEYS);
      if (!list.empty()) list += L"\r\n";
      list += FormatHotkey(key);
      SetWindowString(hwnd, IDC_HOTKEYS, list);
      SetWindowString(hwnd, IDC_HOTKEY_CAPTURE, L"");
      SetFocus(GetDlgItem(hwnd, IDC_HOTKEY_CAPTURE));
      return 0;
    }
    if (LOWORD(wParam) == IDC_HOTKEY_REMOVE) {
      HWND list = GetDlgItem(hwnd, IDC_HOTKEYS);
      DWORD selectionStart = 0, selectionEnd = 0;
      SendMessageW(list, EM_GETSEL, reinterpret_cast<WPARAM>(&selectionStart), reinterpret_cast<LPARAM>(&selectionEnd));
      const int lineIndex = static_cast<int>(SendMessageW(list, EM_LINEFROMCHAR, selectionStart, 0));
      std::wstringstream lines(GetWindowString(hwnd, IDC_HOTKEYS));
      std::vector<std::wstring> values;
      std::wstring line;
      while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (!line.empty()) values.push_back(line);
      }
      if (lineIndex >= 0 && lineIndex < static_cast<int>(values.size())) values.erase(values.begin() + lineIndex);
      std::wstring updated;
      for (const auto& value : values) { if (!updated.empty()) updated += L"\r\n"; updated += value; }
      SetWindowString(hwnd, IDC_HOTKEYS, updated);
      return 0;
    }
    if (LOWORD(wParam) == IDC_BROWSE) {
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

LRESULT CALLBACK Application::HotkeyCaptureProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
                                                UINT_PTR subclassId, DWORD_PTR referenceData) {
  (void)subclassId;
  (void)referenceData;
  if (message == WM_GETDLGCODE) return DLGC_WANTALLKEYS;
  if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) {
    const UINT key = static_cast<UINT>(wParam);
    if (key == VK_CONTROL || key == VK_LCONTROL || key == VK_RCONTROL ||
        key == VK_SHIFT || key == VK_LSHIFT || key == VK_RSHIFT ||
        key == VK_MENU || key == VK_LMENU || key == VK_RMENU ||
        key == VK_LWIN || key == VK_RWIN) return 0;
    HotkeySetting hotkey;
    hotkey.modifiers = MOD_NOREPEAT;
    if (GetKeyState(VK_CONTROL) & 0x8000) hotkey.modifiers |= MOD_CONTROL;
    if (GetKeyState(VK_MENU) & 0x8000) hotkey.modifiers |= MOD_ALT;
    if (GetKeyState(VK_SHIFT) & 0x8000) hotkey.modifiers |= MOD_SHIFT;
    if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) hotkey.modifiers |= MOD_WIN;
    hotkey.virtualKey = key;
    if (!(hotkey.modifiers & (MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN))) {
      MessageBeep(MB_ICONWARNING);
      return 0;
    }
    SetWindowTextW(hwnd, FormatHotkey(hotkey).c_str());
    SendMessageW(hwnd, EM_SETSEL, 0, -1);
    return 0;
  }
  if (message == WM_KEYUP || message == WM_SYSKEYUP || message == WM_CHAR) return 0;
  return DefSubclassProc(hwnd, message, wParam, lParam);
}

void Application::PopulateSettings(HWND hwnd) {
  std::wstring hotkeys;
  for (const auto& key : config_.hotkeys) if (key.enabled) { if (!hotkeys.empty()) hotkeys += L"\r\n"; hotkeys += FormatHotkey(key); }
  SetWindowString(hwnd, IDC_HOTKEYS, hotkeys);
  SetWindowString(hwnd, IDC_HOTKEY_CAPTURE, L"");
  SetWindowString(hwnd, IDC_OUTPUT, config_.outputDirectory);
  SetWindowString(hwnd, IDC_QUALITY, std::to_wstring(config_.jpegQuality));
  SendDlgItemMessageW(hwnd, IDC_DEFAULT_ACTION, CB_SETCURSEL, config_.defaultAction == DefaultAction::Save ? 1 : 0, 0);
  CheckDlgButton(hwnd, IDC_AUTOSAVE, config_.autoSaveOnCopy ? BST_CHECKED : BST_UNCHECKED);
  CheckDlgButton(hwnd, IDC_AUTOSTART, config_.launchAtLogin ? BST_CHECKED : BST_UNCHECKED);
  CheckDlgButton(hwnd, IDC_SILENT, config_.silentAtLogin ? BST_CHECKED : BST_UNCHECKED);
  CheckDlgButton(hwnd, IDC_SHADOW, config_.windowShadow ? BST_CHECKED : BST_UNCHECKED);
  CheckDlgButton(hwnd, IDC_FRAME, config_.frameEnabled ? BST_CHECKED : BST_UNCHECKED);
}

bool Application::ReadSettings(HWND hwnd) {
  std::wstring hotkeysText = GetWindowString(hwnd, IDC_HOTKEYS);
  const std::wstring captured = GetWindowString(hwnd, IDC_HOTKEY_CAPTURE);
  if (!captured.empty()) {
    if (!hotkeysText.empty()) hotkeysText += L"\r\n";
    hotkeysText += captured;
  }
  std::wstringstream lines(hotkeysText);
  std::vector<HotkeySetting> hotkeys; std::wstring line;
  while (std::getline(lines, line)) {
    if (!line.empty() && line.back() == L'\r') line.pop_back();
    while (!line.empty() && (line.front() == L' ' || line.front() == L'\t')) line.erase(line.begin());
    while (!line.empty() && (line.back() == L' ' || line.back() == L'\t')) line.pop_back();
    if (line.empty()) continue;
    HotkeySetting key; if (!ParseHotkeyText(line, key)) { MessageBoxW(hwnd, (L"无法识别快捷键：" + line).c_str(), L"RC-ScreenShot", MB_ICONWARNING); return false; }
    hotkeys.push_back(key);
  }
  if (hotkeys.empty()) { MessageBoxW(hwnd, L"至少需要一组快捷键。", L"RC-ScreenShot", MB_ICONWARNING); return false; }
  int quality = _wtoi(GetWindowString(hwnd, IDC_QUALITY).c_str());
  if (quality < 1 || quality > 100) { MessageBoxW(hwnd, L"JPEG 质量必须为 1–100。", L"RC-ScreenShot", MB_ICONWARNING); return false; }
  config_.hotkeys = std::move(hotkeys); config_.outputDirectory = GetWindowString(hwnd, IDC_OUTPUT);
  config_.jpegQuality = quality; config_.defaultAction = SendDlgItemMessageW(hwnd, IDC_DEFAULT_ACTION, CB_GETCURSEL, 0, 0) == 1 ? DefaultAction::Save : DefaultAction::Copy;
  config_.autoSaveOnCopy = IsDlgButtonChecked(hwnd, IDC_AUTOSAVE) == BST_CHECKED;
  config_.launchAtLogin = IsDlgButtonChecked(hwnd, IDC_AUTOSTART) == BST_CHECKED;
  config_.silentAtLogin = IsDlgButtonChecked(hwnd, IDC_SILENT) == BST_CHECKED;
  config_.windowShadow = IsDlgButtonChecked(hwnd, IDC_SHADOW) == BST_CHECKED;
  config_.frameEnabled = IsDlgButtonChecked(hwnd, IDC_FRAME) == BST_CHECKED;
  return true;
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

void Application::SaveConfig() { std::wstring error; if (!configStore_.Save(config_, &error)) Notify(L"配置无法保存", error, NIIF_WARNING); }

void Application::Notify(std::wstring_view title, std::wstring_view message, DWORD flags) {
  NOTIFYICONDATAW data = trayIcon_; data.uFlags = NIF_INFO; data.dwInfoFlags = flags;
  wcsncpy_s(data.szInfoTitle, title.data(), _TRUNCATE); wcsncpy_s(data.szInfo, message.data(), _TRUNCATE);
  Shell_NotifyIconW(NIM_MODIFY, &data);
}

bool Application::HasArgument(std::span<wchar_t*> arguments, std::wstring_view name) const {
  return std::any_of(arguments.begin(), arguments.end(), [&](const wchar_t* value) { return value && name == value; });
}

}  // namespace rc
