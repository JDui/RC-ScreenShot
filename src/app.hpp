#pragma once

#include "config.hpp"
#include "exporter.hpp"
#include "overlay.hpp"

#include <shellapi.h>

namespace rc {

class Application {
 public:
  explicit Application(HINSTANCE instance);
  ~Application();

  bool Initialize(std::span<wchar_t*> arguments, int showCommand, std::wstring& error);
  int Run();

  static constexpr wchar_t kWindowClass[] = L"RC-ScreenShot.MessageWindow";

 private:
  static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
  static LRESULT CALLBACK SettingsProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
  LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
  LRESULT HandleSettingsMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
  static LRESULT CALLBACK HotkeyCaptureProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
                                            UINT_PTR subclassId, DWORD_PTR referenceData);
  bool CreateMessageWindow(std::wstring& error);
  void AddTrayIcon();
  void RemoveTrayIcon();
  void ShowTrayMenu(POINT point);
  void RegisterConfiguredHotkeys();
  void UnregisterHotkeys();
  void StartCapture();
  void ProcessOverlayResult(std::unique_ptr<OverlayResult> result);
  void ShowSettings();
  void PopulateSettings(HWND hwnd);
  bool ReadSettings(HWND hwnd);
  bool UpdateAutoStart(std::wstring* error = nullptr);
  void SaveConfig();
  void Notify(std::wstring_view title, std::wstring_view message, DWORD flags = NIIF_INFO);
  bool HasArgument(std::span<wchar_t*> arguments, std::wstring_view name) const;

  HINSTANCE instance_ = nullptr;
  HWND hwnd_ = nullptr;
  HWND settingsWindow_ = nullptr;
  HANDLE mutex_ = nullptr;
  UINT taskbarCreated_ = 0;
  NOTIFYICONDATAW trayIcon_{};
  std::filesystem::path executablePath_;
  ConfigStore configStore_;
  AppConfig config_;
  std::vector<int> registeredHotkeyIds_;
  std::vector<std::wstring> hotkeyErrors_;
  std::unique_ptr<CaptureOverlay> overlay_;
  DesktopCapture desktopCapture_;
  ImageExporter exporter_;
  HBRUSH settingsBackgroundBrush_ = nullptr;
  HBRUSH settingsPanelBrush_ = nullptr;
  HBRUSH settingsControlBrush_ = nullptr;
  HFONT settingsFont_ = nullptr;
  HFONT settingsTitleFont_ = nullptr;
  HFONT settingsSmallFont_ = nullptr;
};

}  // namespace rc
