#include "capture.hpp"

#include <chrono>
#include <iostream>

int wmain() {
  const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  rc::ScopeExit uninitialize{[&] { if (SUCCEEDED(com)) CoUninitialize(); }};
  rc::DesktopCapture capture;
  rc::DesktopSnapshot snapshot;
  std::wstring error;
  const auto start = std::chrono::steady_clock::now();
  const bool success = capture.Capture(snapshot, error);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start);
  std::cout << "success=" << success << " elapsedMs=" << elapsed.count()
            << " size=" << snapshot.width << 'x' << snapshot.height
            << " hdr=" << snapshot.hasHdr << " gdi=" << snapshot.usedGdiFallback << '\n';
  if (!error.empty()) std::cout << "error=" << rc::ToUtf8(error) << '\n';
  return success ? 0 : 1;
}
