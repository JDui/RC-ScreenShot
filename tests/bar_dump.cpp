// Temporary diagnostic: renders the overlay's floating bars off-screen and
// dumps the toolbar regions as ASCII luminance maps so bar icons can be
// inspected without a live capture session.
#include "overlay.hpp"

#include <windows.h>

#include <cstdio>
#include <vector>

namespace {

void Pump() {
  MSG msg{};
  while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
}

// PrintWindow the overlay into a BGRA buffer covering virtualBounds.
std::vector<uint8_t> Grab(rc::CaptureOverlay& overlay, int width, int height) {
  std::vector<uint8_t> pixels(static_cast<size_t>(width * height) * 4, 0);
  HDC windowDC = GetWindowDC(overlay.hwnd());
  HDC memDC = CreateCompatibleDC(windowDC);
  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = width;
  info.bmiHeader.biHeight = -height;  // top-down
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  void* bits = nullptr;
  HBITMAP bitmap = CreateDIBSection(memDC, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
  HGDIOBJ old = SelectObject(memDC, bitmap);
  BOOL ok = PrintWindow(overlay.hwnd(), memDC, PW_RENDERFULLCONTENT);
  if (!ok && GetLastError() != 0) {
    // Fall back to a plain BitBlt of the window DC.
    BitBlt(memDC, 0, 0, width, height, windowDC, 0, 0, SRCCOPY);
  }
  SelectObject(memDC, old);
  DeleteDC(memDC);
  ReleaseDC(overlay.hwnd(), windowDC);
  if (bits && ok) memcpy(pixels.data(), bits, pixels.size());
  DeleteObject(bitmap);
  return pixels;
}

void DumpRegion(const char* label, const std::vector<uint8_t>& pixels, int width, int height,
                int left, int top, int right, int bottom, int stepX = 4, int stepY = 4) {
  std::printf("-- %s (%dx%d @ %d,%d) --\n", label, right - left, bottom - top, left, top);
  for (int y = top; y < bottom; y += stepY) {
    for (int x = left; x < right; x += stepX) {
      size_t index = (static_cast<size_t>(y) * width + x) * 4;
      const int b = pixels[index], g = pixels[index + 1], r = pixels[index + 2];
      // luminance ramp: ' ' < . < : < - < = < + < * < # < %
      const int lum = (r * 30 + g * 59 + b * 11) / 100;
      const char c = lum < 24 ? ' ' : lum < 48 ? '.' : lum < 72 ? ':' : lum < 96 ? '-' :
                     lum < 120 ? '=' : lum < 144 ? '+' : lum < 168 ? '*' : lum < 192 ? '#' : '%';
      std::putchar(c);
    }
    std::putchar('\n');
  }
  std::printf("-- end %s --\n", label);
}

}  // namespace

int wmain() {
  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(com)) return 91;
  rc::ScopeExit uninitialize{[&] { CoUninitialize(); }};
  constexpr int kWidth = 1280;
  constexpr int kHeight = 800;
  rc::DesktopSnapshot snapshot;
  snapshot.virtualBounds = {0, 0, kWidth, kHeight};
  snapshot.width = kWidth; snapshot.height = kHeight; snapshot.bgraStride = kWidth * 4;
  snapshot.bgra.resize(static_cast<size_t>(snapshot.bgraStride * snapshot.height));
  snapshot.hdrRgba.resize(static_cast<size_t>(snapshot.width * snapshot.height * 4), rc::FloatToHalf(1.0f));
  for (int y = 0; y < kHeight; ++y)
    for (int x = 0; x < kWidth; ++x) {
      uint8_t* pixel = snapshot.bgra.data() + static_cast<size_t>(y * snapshot.bgraStride + x * 4);
      pixel[0] = static_cast<uint8_t>(60 + x / 24);
      pixel[1] = static_cast<uint8_t>(60 + y / 16);
      pixel[2] = 90;
      pixel[3] = 255;
    }
  rc::AppConfig config;
  rc::CaptureOverlay overlay(GetModuleHandleW(nullptr), std::move(snapshot), config,
      [](rc::OverlayResult) {}, [] {},
      std::optional<RECT>{RECT{320, 120, 960, 640}});  // starts in edit mode
  std::wstring error;
  if (!overlay.Show(error)) {
    std::printf("showError=%s\n", rc::ToUtf8(error).c_str());
    return 92;
  }
  // Commit the window-hover target, then draw a manual selection so the
  // overlay enters edit mode (mirrors overlay_smoke.cpp).
  SendMessageW(overlay.hwnd(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(524, 284));
  Sleep(360);
  Pump();
  SendMessageW(overlay.hwnd(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(40, 40));
  SendMessageW(overlay.hwnd(), WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(520, 250));
  SendMessageW(overlay.hwnd(), WM_LBUTTONUP, 0, MAKELPARAM(520, 250));
  if (!overlay.EditingForTest()) std::printf("EDIT_MODE_ENTER_FAILED\n");
  Pump();
  auto grabBar = [&](const char* stage) {
    Pump();
    InvalidateRect(overlay.hwnd(), nullptr, FALSE);
    Pump();
    Sleep(60);
    std::vector<uint8_t> frame = Grab(overlay, kWidth, kHeight);
    char name[64];
    std::snprintf(name, sizeof(name), "bar_dump_%s.bmp", stage);
    // Write a minimal 32bpp BMP for later inspection.
    FILE* file = nullptr;
    if (fopen_s(&file, name, "wb") == 0 && file) {
      BITMAPFILEHEADER fh{};
      BITMAPINFOHEADER ih{};
      ih.biSize = sizeof(ih); ih.biWidth = kWidth; ih.biHeight = -kHeight;
      ih.biPlanes = 1; ih.biBitCount = 32; ih.biCompression = BI_RGB;
      ih.biSizeImage = static_cast<DWORD>(frame.size());
      fh.bfType = 0x4D42;
      fh.bfSize = sizeof(fh) + sizeof(ih) + frame.size();
      fh.bfOffBits = sizeof(fh) + sizeof(ih);
      fwrite(&fh, 1, sizeof(fh), file);
      fwrite(&ih, 1, sizeof(ih), file);
      fwrite(frame.data(), 1, frame.size(), file);
      fclose(file);
    }
    const RECT bar = overlay.ToolbarRectForTest();
    const LONG leftX = std::clamp(static_cast<LONG>(bar.left - 8), 0L, static_cast<LONG>(kWidth));
    const LONG topY = std::clamp(static_cast<LONG>(bar.top - 2), 0L, static_cast<LONG>(kHeight));
    const LONG rightX = std::clamp(static_cast<LONG>(bar.right + 8), 0L, static_cast<LONG>(kWidth));
    const LONG bottomY = std::clamp(static_cast<LONG>(bar.bottom + 2), 0L, static_cast<LONG>(kHeight));
    DumpRegion(stage, frame, kWidth, kHeight, leftX, topY, rightX, bottomY);
    return frame;
  };

  std::vector<uint8_t> frame = grabBar("edit");
  // Fine-grained view of the long-capture entry glyph and the two utility
  // buttons so a ghost/stroke can be distinguished from a missing draw.
  {
    const RECT bar = overlay.ToolbarRectForTest();
    const RECT entry = overlay.ToolbarLongEntryRectForTest();
    DumpRegion("entry-closeup", frame, kWidth, kHeight,
               entry.left - 8, entry.top - 2, entry.right + 8, entry.bottom + 2, 1, 1);
    DumpRegion("utility-closeup", frame, kWidth, kHeight,
               bar.right - 76, bar.top - 2, bar.right + 4, bar.top + 30, 1, 1);
  }
  // Click the long-capture entry (rainbow button) and dump the direction bar.
  const RECT entry = overlay.ToolbarLongEntryRectForTest();
  const POINT entryPoint{(entry.left + entry.right) / 2, (entry.top + entry.bottom) / 2};
  SendMessageW(overlay.hwnd(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(entryPoint.x, entryPoint.y));
  SendMessageW(overlay.hwnd(), WM_LBUTTONUP, 0, MAKELPARAM(entryPoint.x, entryPoint.y));
  frame = grabBar("longmode");
  if (!overlay.LongCaptureModeForTest()) std::printf("LONG_ENTRY_CLICK_FAILED\n");
  // Fine-grained view of the three direction buttons' chevron glyphs.
  {
    const RECT bar = overlay.ToolbarRectForTest();
    DumpRegion("directions-closeup", frame, kWidth, kHeight,
               bar.left - 8, bar.top + 33, bar.right + 8, bar.bottom + 2, 1, 1);
  }
  Pump();
  return 0;
}