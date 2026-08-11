#include "capture.hpp"

#include <chrono>
#include <iostream>

int wmain() {
  const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  rc::ScopeExit uninitialize{[&] { if (SUCCEEDED(com)) CoUninitialize(); }};
  rc::DesktopCapture capture;
  std::vector<rc::DesktopSnapshot> snapshots;
  std::wstring error;
  const auto start = std::chrono::steady_clock::now();
  const bool success = capture.CaptureBurst(5, 0.05, snapshots, error);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start);
  const size_t validFrames = snapshots.size();
  const rc::DesktopSnapshot* snapshot = validFrames ? &snapshots.front() : nullptr;
  std::cout << "success=" << success << " requestedIntervalMs=50"
            << " totalElapsedMs=" << elapsed.count()
            << " validFrames=" << validFrames
            << " size=" << (snapshot ? snapshot->width : 0) << 'x' << (snapshot ? snapshot->height : 0)
            << " hdr=" << (snapshot ? snapshot->hasHdr : false)
            << " gdi=" << (snapshot ? snapshot->usedGdiFallback : false) << '\n';
  if (!error.empty()) std::cout << "error=" << rc::ToUtf8(error) << '\n';
  return success ? 0 : 1;
}
