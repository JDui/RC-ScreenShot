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
  rc::DesktopSnapshot secondSnapshot = snapshot;
  secondSnapshot.bgra[0] = 180;
  std::vector<rc::DesktopSnapshot> burstSnapshots;
  burstSnapshots.push_back(std::move(snapshot));
  burstSnapshots.push_back(std::move(secondSnapshot));
  rc::AppConfig config;
  bool completed = false;
  bool copied = false;
  bool textAdded = false;
  bool textResized = false;
  bool textRecolored = false;
  bool mosaicAdded = false;
  rc::CaptureOverlay overlay(instance, std::move(burstSnapshots), config,
      [&](rc::OverlayResult result) {
        completed = true;
        copied = result.completion == rc::CaptureCompletion::Copy;
        for (const auto& command : result.document.Commands())
          if (const auto* text = std::get_if<rc::TextCommand>(&command))
            if (text->text == L"横竖文字") {
              textAdded = true;
              textResized = text->style.size > config.text.size + 1.0f;
              textRecolored = text->style.color.rgba != config.text.color.rgba;
            }
        for (const auto& command : result.document.Commands())
          if (const auto* mosaic = std::get_if<rc::MosaicCommand>(&command))
            mosaicAdded = mosaic->brush && !mosaic->points.empty();
      }, [] {}, std::optional<RECT>{RECT{100, 40, 560, 320}});
  std::wstring error;
  if (!overlay.Show(error)) {
    std::cerr << "overlayError=" << rc::ToUtf8(error) << '\n';
    return 1;
  }
  SendMessageW(overlay.hwnd(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(524, 284));
  // Expansion uses the 280 ms ease-out transition; leave a little headroom so
  // this smoke path verifies the settled hit-test geometry as well as the
  // in-flight animation timer cleanup.
  Sleep(360);
  MSG burstMessage{};
  while (PeekMessageW(&burstMessage, nullptr, 0, 0, PM_REMOVE)) {
    TranslateMessage(&burstMessage); DispatchMessageW(&burstMessage);
  }
  // The target work area is an inset monitor rectangle.  The dynamic
  // two-column layout places frame 2 at the right-hand thumbnail; exercise
  // hover and commit through that path without relying on the virtual desktop
  // origin.
  SendMessageW(overlay.hwnd(), WM_MOUSEMOVE, 0, MAKELPARAM(490, 220));
  SendMessageW(overlay.hwnd(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(490, 220));
  SendMessageW(overlay.hwnd(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(40, 40));
  SendMessageW(overlay.hwnd(), WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(520, 250));
  SendMessageW(overlay.hwnd(), WM_LBUTTONUP, 0, MAKELPARAM(520, 250));
  const RECT cornerIcon = overlay.SnapshotIconRectForTest();
  bool dockTimerSettled = false;
  for (int i = 0; i < 80; ++i) {
    MSG dockMessage{};
    while (PeekMessageW(&dockMessage, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&dockMessage); DispatchMessageW(&dockMessage);
    }
    Sleep(8);
    const RECT current = overlay.SnapshotIconRectForTest();
    if (current.left != cornerIcon.left || current.top != cornerIcon.top) dockTimerSettled = true;
  }
  const RECT dockIcon = overlay.SnapshotIconRectForTest();
  if (!dockTimerSettled || (dockIcon.left == cornerIcon.left && dockIcon.top == cornerIcon.top)) return 4;
  // The old work-area corner must no longer consume the switcher click once
  // the dock animation has settled; the dynamically derived dock point does.
  const POINT oldCorner{cornerIcon.left + 2, cornerIcon.top + 2};
  SendMessageW(overlay.hwnd(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(oldCorner.x, oldCorner.y));
  SendMessageW(overlay.hwnd(), WM_LBUTTONUP, 0, MAKELPARAM(oldCorner.x, oldCorner.y));
  if (overlay.SnapshotSwitcherExpandedForTest()) return 5;
  const POINT dockPoint{(dockIcon.left + dockIcon.right) / 2, (dockIcon.top + dockIcon.bottom) / 2};
  SendMessageW(overlay.hwnd(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(dockPoint.x, dockPoint.y));
  if (!overlay.SnapshotSwitcherExpandedForTest()) return 6;
  Sleep(340);
  MSG previewMessage{};
  while (PeekMessageW(&previewMessage, nullptr, 0, 0, PM_REMOVE)) {
    TranslateMessage(&previewMessage); DispatchMessageW(&previewMessage);
  }
  const RECT thumb0 = overlay.SnapshotThumbnailRectForTest(0);
  const RECT thumb1 = overlay.SnapshotThumbnailRectForTest(1);
  const POINT previewPoints[2]{
      {(thumb0.left + thumb0.right) / 2, (thumb0.top + thumb0.bottom) / 2},
      {(thumb1.left + thumb1.right) / 2, (thumb1.top + thumb1.bottom) / 2}};
  bool previewResponsive = true;
  for (int cycle = 0; cycle < 20 && previewResponsive; ++cycle) {
    for (const POINT point : previewPoints) {
      const ULONGLONG started = GetTickCount64();
      SendMessageW(overlay.hwnd(), WM_MOUSEMOVE, 0, MAKELPARAM(point.x, point.y));
      if (GetTickCount64() - started >= 100) previewResponsive = false;
    }
  }
  if (!previewResponsive) return 7;
  const RECT originalToolbar = overlay.ToolbarRectForTest();
  SendMessageW(overlay.hwnd(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(dockPoint.x, dockPoint.y));
  const POINT toolbarDragPoint{(originalToolbar.left + originalToolbar.right) / 2,
                               originalToolbar.bottom - 2};
  const POINT toolbarTopPoint{toolbarDragPoint.x,
                              40 + (originalToolbar.bottom - originalToolbar.top) - 2};
  SendMessageW(overlay.hwnd(), WM_LBUTTONDOWN, MK_LBUTTON,
               MAKELPARAM(toolbarDragPoint.x, toolbarDragPoint.y));
  SendMessageW(overlay.hwnd(), WM_MOUSEMOVE, MK_LBUTTON,
               MAKELPARAM(toolbarTopPoint.x, toolbarTopPoint.y));
  SendMessageW(overlay.hwnd(), WM_LBUTTONUP, 0,
               MAKELPARAM(toolbarTopPoint.x, toolbarTopPoint.y));
  const RECT topIcon = overlay.SnapshotIconRectForTest();
  const POINT topIconPoint{(topIcon.left + topIcon.right) / 2,
                           (topIcon.top + topIcon.bottom) / 2};
  SendMessageW(overlay.hwnd(), WM_LBUTTONDOWN, MK_LBUTTON,
               MAKELPARAM(topIconPoint.x, topIconPoint.y));
  Sleep(340);
  while (PeekMessageW(&previewMessage, nullptr, 0, 0, PM_REMOVE)) {
    TranslateMessage(&previewMessage); DispatchMessageW(&previewMessage);
  }
  const RECT topPanel = overlay.SnapshotPanelRectForTest();
  const RECT topThumb = overlay.SnapshotThumbnailRectForTest(0);
  if (topPanel.top < topIcon.bottom + 7 || topPanel.bottom > 320) return 8;
  if (topThumb.right - topThumb.left < 24 || topThumb.bottom - topThumb.top < 18) return 9;
  SendMessageW(overlay.hwnd(), WM_LBUTTONDOWN, MK_LBUTTON,
               MAKELPARAM(topIconPoint.x, topIconPoint.y));
  const RECT topToolbar = overlay.ToolbarRectForTest();
  const POINT restoreDragPoint{(topToolbar.left + topToolbar.right) / 2,
                               topToolbar.bottom - 2};
  SendMessageW(overlay.hwnd(), WM_LBUTTONDOWN, MK_LBUTTON,
               MAKELPARAM(restoreDragPoint.x, restoreDragPoint.y));
  SendMessageW(overlay.hwnd(), WM_MOUSEMOVE, MK_LBUTTON,
               MAKELPARAM(toolbarDragPoint.x, toolbarDragPoint.y));
  SendMessageW(overlay.hwnd(), WM_LBUTTONUP, 0,
               MAKELPARAM(toolbarDragPoint.x, toolbarDragPoint.y));
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
  // The text tool can select its own existing text.  The size slider and the
  // first color swatch then edit that command instead of changing defaults.
  SendMessageW(overlay.hwnd(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(100, 100));
  SendMessageW(overlay.hwnd(), WM_LBUTTONUP, 0, MAKELPARAM(100, 100));
  // The narrow 640x360 test desktop clamps the 620px toolbar to the lower
  // work-area edge: top=222, size slider y=279..301, first swatch y=279..299.
  SendMessageW(overlay.hwnd(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(170, 290));
  SendMessageW(overlay.hwnd(), WM_LBUTTONUP, 0, MAKELPARAM(170, 290));
  SendMessageW(overlay.hwnd(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(236, 290));
  SendMessageW(overlay.hwnd(), WM_LBUTTONUP, 0, MAKELPARAM(236, 290));
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
            << " textResized=" << textResized << " textRecolored=" << textRecolored
            << " mosaicAdded=" << mosaicAdded
            << " previewResponsive=" << previewResponsive << '\n';
  return completed && copied && textAdded && textResized && textRecolored && mosaicAdded && previewResponsive ? 0 : 2;
}
