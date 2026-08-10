#include "app.hpp"

#include <shellapi.h>
#include <commctrl.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  if (FAILED(com)) return static_cast<int>(com);
  rc::ScopeExit uninitialize{[] { CoUninitialize(); }};
  INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES};
  InitCommonControlsEx(&controls);
  int count = 0; wchar_t** values = CommandLineToArgvW(GetCommandLineW(), &count);
  rc::ScopeExit releaseArguments{[&] { if (values) LocalFree(values); }};
  rc::Application app(instance); std::wstring error;
  if (!app.Initialize({values, static_cast<size_t>(count)}, showCommand, error)) {
    if (!error.empty()) MessageBoxW(nullptr, error.c_str(), L"RC-ScreenShot", MB_ICONERROR);
    return error.empty() ? 0 : 1;
  }
  return app.Run();
}
