#pragma once

#include "common.hpp"

namespace rc {

struct WindowCandidate {
  HWND hwnd = nullptr;
  RECT bounds{};  // virtual desktop coordinates
  std::wstring title;
};

struct DesktopSnapshot {
  RECT virtualBounds{};
  int width = 0;
  int height = 0;
  int bgraStride = 0;
  std::vector<uint8_t> bgra;
  // Linear RGBA half float. 1.0 equals 203 nits as expected by libultrahdr.
  std::vector<uint16_t> hdrRgba;
  bool hasHdr = false;
  bool usedGdiFallback = false;
  float peakLuminanceNits = 203.0f;
  std::vector<RECT> hdrRegions;
  std::vector<WindowCandidate> windows;

  bool IsValid() const {
    return width > 0 && height > 0 && bgraStride >= width * 4 &&
           bgra.size() >= static_cast<size_t>(bgraStride * height);
  }
};

class DesktopCapture {
 public:
  bool Capture(DesktopSnapshot& snapshot, std::wstring& error);
  // Captures a sequence while keeping the DXGI duplication sessions alive
  // between frames.  The first frame is immediate; subsequent frames are
  // scheduled from the same steady-clock origin at intervalSeconds.
  bool CaptureBurst(int frameCount, double intervalSeconds,
                    std::vector<DesktopSnapshot>& snapshots, std::wstring& error);

 private:
  bool CaptureDxgi(DesktopSnapshot& snapshot, std::wstring& error);
  bool CaptureGdi(DesktopSnapshot& snapshot, std::wstring& error);
};

// Rejects an uninitialized/empty capture before it reaches the overlay. Alpha is deliberately
// ignored because desktop duplication leaves it undefined on SDR outputs.
bool SnapshotIsBlank(const DesktopSnapshot& snapshot);

// Fills snapshot.windows with candidate windows that overlap the virtual desktop. Only needed
// for Window mode, so callers run it off the capture path (e.g. the overlay background thread).
void EnumerateWindows(DesktopSnapshot& snapshot);

float HalfToFloat(uint16_t half);
uint16_t FloatToHalf(float value);

}  // namespace rc
