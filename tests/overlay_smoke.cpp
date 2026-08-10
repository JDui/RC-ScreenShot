#include "overlay.hpp"

#include <iostream>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  rc::ScopeExit uninitialize{[&] { if (SUCCEEDED(com)) CoUninitialize(); }};
  rc::DesktopSnapshot snapshot;
  snapshot.virtualBounds = {0, 0, 640, 360};
  snapshot.width = 640; snapshot.height = 360; snapshot.bgraStride = 640 * 4;
  snapshot.bgra.resize(static_cast<size_t>(snapshot.bgraStride * snapshot.height));
  snapshot.hdrRgba.resize(static_cast<size_t>(snapshot.width * snapshot.height * 4), rc::FloatToHalf(1.0f));
  for (int y = 0; y < snapshot.height; ++y) for (int x = 0; x < snapshot.width; ++x) {
    uint8_t* pixel = snapshot.bgra.data() + static_cast<size_t>(y * snapshot.bgraStride + x * 4);
    pixel[0] = static_cast<uint8_t>(80 + x / 8); pixel[1] = static_cast<uint8_t>(80 + y / 4);
    pixel[2] = 120; pixel[3] = 255;
  }
  rc::AppConfig config;
  bool completed = false;
  bool copied = false;
  bool textAdded = false;
  bool mosaicAdded = false;
  rc::CaptureOverlay overlay(instance, std::move(snapshot), config,
      [&](rc::OverlayResult result) {
        completed = true;
        copied = result.completion == rc::CaptureCompletion::Copy;
        for (const auto& command : result.document.Commands())
          if (const auto* text = std::get_if<rc::TextCommand>(&command))
            textAdded = text->text == L"横竖文字";
        for (const auto& command : result.document.Commands())
          if (const auto* mosaic = std::get_if<rc::MosaicCommand>(&command))
            mosaicAdded = mosaic->brush && !mosaic->points.empty();
      }, [] {});
  std::wstring error;
  if (!overlay.Show(error)) {
    std::cerr << "overlayError=" << rc::ToUtf8(error) << '\n';
    return 1;
  }
  SendMessageW(overlay.hwnd(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(40, 40));
  SendMessageW(overlay.hwnd(), WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(520, 250));
  SendMessageW(overlay.hwnd(), WM_LBUTTONUP, 0, MAKELPARAM(520, 250));
  SendMessageW(overlay.hwnd(), WM_KEYDOWN, 'M', 0);
  SendMessageW(overlay.hwnd(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(180, 140));
  SendMessageW(overlay.hwnd(), WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(260, 170));
  SendMessageW(overlay.hwnd(), WM_LBUTTONUP, 0, MAKELPARAM(260, 170));
  SendMessageW(overlay.hwnd(), WM_KEYDOWN, 'T', 0);
  SendMessageW(overlay.hwnd(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(100, 100));
  HWND edit = FindWindowExW(overlay.hwnd(), nullptr, L"Edit", nullptr);
  if (!edit) return 3;
  SendMessageW(edit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(L"横竖文字"));
  SendMessageW(edit, WM_KEYDOWN, VK_RETURN, 0);
  SendMessageW(overlay.hwnd(), WM_KEYDOWN, VK_RETURN, 0);
  MSG message{};
  for (int i = 0; i < 100 && !completed; ++i) {
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message); DispatchMessageW(&message);
    }
    Sleep(5);
  }
  std::cout << "overlayCreated=1 completed=" << completed
            << " copied=" << copied << " textAdded=" << textAdded
            << " mosaicAdded=" << mosaicAdded << '\n';
  return completed && copied && textAdded && mosaicAdded ? 0 : 2;
}
