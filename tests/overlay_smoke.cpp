#include "overlay.hpp"

#include <iostream>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int) {
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
  if (std::wstring_view(commandLine) == L"--shapes") {
    const HWND window = overlay.hwnd();
    BYTE originalKeys[256]{};
    GetKeyboardState(originalKeys);
    rc::ScopeExit restoreKeys{[&] { SetKeyboardState(originalKeys); }};
    const auto shift = [&](bool down) {
      BYTE keys[256]{};
      GetKeyboardState(keys);
      keys[VK_SHIFT] = down ? 0x80 : 0;
      SetKeyboardState(keys);
      SendMessageW(window, down ? WM_KEYDOWN : WM_KEYUP, VK_SHIFT, 0);
    };
    const auto mouse = [&](UINT message, POINT point) {
      SendMessageW(window, message, message == WM_LBUTTONUP ? 0 : MK_LBUTTON,
                   MAKELPARAM(point.x, point.y));
    };
    const auto drag = [&](POINT start, POINT end) {
      mouse(WM_LBUTTONDOWN, start);
      mouse(WM_MOUSEMOVE, end);
      mouse(WM_LBUTTONUP, end);
    };
    const auto pointsMatch = [](rc::PointF a, rc::PointF b) {
      return std::hypot(a.x - b.x, a.y - b.y) < 0.01f;
    };
    const auto shapeAt = [&](size_t index) -> const rc::ShapeCommand* {
      const auto* command = overlay.DocumentForTest().At(index);
      return command ? std::get_if<rc::ShapeCommand>(command) : nullptr;
    };
    const auto matches = [&](size_t index, rc::PointF start, rc::PointF end, float width) {
      const auto* shape = shapeAt(index);
      return shape && pointsMatch(shape->start, start) && pointsMatch(shape->end, end) &&
             std::abs(shape->style.stroke.width - width) < 0.01f;
    };
    shift(false);
    drag({40, 40}, {520, 250});
    if (!overlay.EditingForTest()) return 30;
    for (WPARAM tool : {'R', 'E'}) {
      SendMessageW(window, WM_KEYDOWN, tool, 0);
      shift(true);
      size_t index = overlay.DocumentForTest().Size();
      drag({180, 110}, {270, 150});
      const auto* shape = shapeAt(index);
      if (!shape || !pointsMatch(shape->start, {140, 70}) || !pointsMatch(shape->end, {230, 160})) return 31;
      shift(false);
      index = overlay.DocumentForTest().Size();
      drag({340, 110}, {430, 150});
      shape = shapeAt(index);
      if (!shape || !pointsMatch(shape->start, {300, 70}) || !pointsMatch(shape->end, {390, 110})) return 32;
    }
    const rc::RectF canvas{0, 0, 480, 210};
    for (WPARAM tool : {'L', 'A'}) {
      SendMessageW(window, WM_KEYDOWN, tool, 0);
      const size_t index = overlay.DocumentForTest().Size();
      shift(true);
      drag({180, 110}, {300, 150});
      const auto* shape = shapeAt(index);
      const rc::ShapeKind kind = tool == 'L' ? rc::ShapeKind::Line : rc::ShapeKind::Arrow;
      const rc::PointF snapped = rc::ConstrainShapeEnd(kind, {140, 70}, {260, 110}, canvas, true);
      if (!shape || shape->kind != kind || !pointsMatch(shape->end, snapped)) return 33;
      const float ticks = std::atan2(shape->end.y - shape->start.y, shape->end.x - shape->start.x)
                          * 12 / 3.14159265358979323846f;
      if (std::abs(ticks - 1.0f) > 0.001f) return 33;
      shift(false);
      // The first endpoint can cross the other endpoint without scaling the stroke.
      const float width = shape->style.stroke.width;
      drag({180, 110}, {170, 170});
      if (!matches(index, {130, 130}, snapped, width)) return 34;
      const POINT tip{static_cast<LONG>(std::lround(snapped.x + 40)),
                      static_cast<LONG>(std::lround(snapped.y + 40))};
      drag(tip, {160, 90});
      // Rounding the screen coordinate must not make the endpoint jump on grab.
      const rc::PointF movedTip{snapped.x + 160 - tip.x, snapped.y + 90 - tip.y};
      if (!matches(index, {130, 130}, movedTip, width)) return 35;
      drag({160, 90}, {290, 170});
      const rc::PointF horizontal{movedTip.x + 130, movedTip.y + 80};
      if (!matches(index, {130, 130}, horizontal, width)) return 36;
      // Select uses the same endpoints as the matching drawing tool.
      SendMessageW(window, WM_KEYDOWN, 'V', 0);
      drag({290, 170}, {170, 100});
      const rc::PointF vertical{horizontal.x - 120, horizontal.y - 70};
      if (!matches(index, {130, 130}, vertical, width)) return 37;
      mouse(WM_LBUTTONDOWN, {170, 100});
      mouse(WM_MOUSEMOVE, {275, 130});
      const rc::PointF freeEnd{vertical.x + 105, vertical.y + 30};
      // Shift press/release updates a stationary drag immediately, without drift.
      shift(true);
      const rc::PointF constrained = rc::ConstrainShapeEnd(kind, {130, 130}, freeEnd, canvas, true);
      if (!matches(index, {130, 130}, constrained, width)) return 38;
      shift(false);
      if (!matches(index, {130, 130}, freeEnd, width)) return 39;
      shift(true);
      mouse(WM_LBUTTONUP, {275, 130});
      if (!matches(index, {130, 130}, constrained, width)) return 40;
      shift(false);
      const POINT middle{static_cast<LONG>(std::lround((130 + constrained.x) / 2 + 40)),
                         static_cast<LONG>(std::lround((130 + constrained.y) / 2 + 40))};
      drag(middle, {middle.x + 10, middle.y + 10});
      if (!matches(index, {140, 140}, {constrained.x + 10, constrained.y + 10}, width)) return 41;
      SendMessageW(window, WM_KEYDOWN, tool, 0);
      // Empty space at the old bounding-box corner starts a new annotation.
      drag({180, 150}, {200, 140});
      if (overlay.DocumentForTest().Size() != index + 2 ||
          !matches(index, {140, 140}, {constrained.x + 10, constrained.y + 10}, width)) return 42;
      // Full-width lines can move along the edge without changing length or
      // yielding a selected endpoint to the screenshot's resize border.
      drag({180, 150}, {40, 150});
      drag({200, 140}, {520, 150});
      if (!matches(index + 1, {0, 110}, {480, 110}, width)) return 46;
      drag({300, 150}, {320, 160});
      if (!matches(index + 1, {0, 120}, {480, 120}, width)) return 47;
      drag({520, 160}, {500, 160});
      if (!matches(index + 1, {0, 120}, {460, 120}, width)) return 48;
      UpdateWindow(window);
    }
    // Opposite quadrants and canvas clipping preserve the requested constraint.
    const rc::PointF square = rc::ConstrainShapeEnd(rc::ShapeKind::Rectangle,
        {470, 200}, {400, 150}, canvas, true);
    if (!pointsMatch(square, {400, 130})) return 43;
    const rc::PointF circle = rc::ConstrainShapeEnd(rc::ShapeKind::Ellipse,
        {470, 200}, {480, 201}, canvas, true);
    if (!pointsMatch(circle, {480, 210})) return 44;
    const rc::PointF line = rc::ConstrainShapeEnd(rc::ShapeKind::Line,
        {470, 200}, {480, 205}, canvas, true);
    const float angleTicks = std::atan2(line.y - 200, line.x - 470) * 12 / 3.14159265358979323846f;
    if (line.x > 480 || line.y > 210 || std::abs(angleTicks - 2.0f) > 0.001f) return 45;
    return 0;
  }
  const bool mosaicBrushTest = std::wstring_view(commandLine) == L"--mosaic-brush";
  if (std::wstring_view(commandLine) == L"--pen" || mosaicBrushTest) {
    SendMessageW(overlay.hwnd(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(40, 40));
    SendMessageW(overlay.hwnd(), WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(520, 250));
    SendMessageW(overlay.hwnd(), WM_LBUTTONUP, 0, MAKELPARAM(520, 250));
    if (!overlay.EditingForTest()) return 22;
    const auto drag = [&](POINT start, POINT end) {
      SendMessageW(overlay.hwnd(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(start.x, start.y));
      SendMessageW(overlay.hwnd(), WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(end.x, end.y));
      SendMessageW(overlay.hwnd(), WM_LBUTTONUP, 0, MAKELPARAM(end.x, end.y));
    };
    const auto strokePointsMatch = [&](size_t index, rc::PointF first, rc::PointF last) {
      const auto* command = overlay.DocumentForTest().At(index);
      if (!command) return false;
      std::span<const rc::PointF> points;
      if (mosaicBrushTest) {
        const auto* mosaic = std::get_if<rc::MosaicCommand>(command);
        if (!mosaic || !mosaic->brush) return false;
        points = mosaic->points;
      } else {
        const auto* pen = std::get_if<rc::PenCommand>(command);
        if (!pen) return false;
        points = pen->points;
      }
      return points.size() >= 2 &&
             std::abs(points.front().x - first.x) < 0.01f &&
             std::abs(points.front().y - first.y) < 0.01f &&
             std::abs(points.back().x - last.x) < 0.01f &&
             std::abs(points.back().y - last.y) < 0.01f;
    };
    // Drawing over an existing stroke must add ink instead of moving/resizing it.
    const WPARAM brushKey = mosaicBrushTest ? 'M' : 'P';
    SendMessageW(overlay.hwnd(), WM_KEYDOWN, brushKey, 0);
    drag({180, 120}, {260, 180});
    drag({220, 150}, {240, 130});
    if (overlay.DocumentForTest().Size() != 2 ||
        !strokePointsMatch(0, {140, 80}, {220, 140}) ||
        !strokePointsMatch(1, {180, 110}, {200, 90})) return 19;
    // Select can still move the first stroke from an unoccluded interior point.
    SendMessageW(overlay.hwnd(), WM_KEYDOWN, 'V', 0);
    drag({200, 135}, {210, 145});
    if (overlay.DocumentForTest().Size() != 2 ||
        !strokePointsMatch(0, {150, 90}, {230, 150})) return 20;
    // Returning to either freehand tool releases the selection and its bounding box.
    SendMessageW(overlay.hwnd(), WM_KEYDOWN, brushKey, 0);
    drag({210, 145}, {230, 125});
    if (overlay.DocumentForTest().Size() != 3 ||
        !strokePointsMatch(0, {150, 90}, {230, 150}) ||
        !strokePointsMatch(2, {170, 105}, {190, 85})) return 21;
    // Select also retains corner-handle resizing for existing ink.
    SendMessageW(overlay.hwnd(), WM_KEYDOWN, 'V', 0);
    drag({250, 175}, {250, 175});
    const auto& beforeResize = *overlay.DocumentForTest().At(0);
    const float padding = mosaicBrushTest
        ? std::max(4.0f, std::get<rc::MosaicCommand>(beforeResize).brushSize * 0.5f)
        : std::max(4.0f, rc::PenMaximumWidth(std::get<rc::PenCommand>(beforeResize)) * 0.5f + 2.0f);
    const POINT corner{static_cast<LONG>(std::lround(270.0f + padding)),
                       static_cast<LONG>(std::lround(130.0f - padding))};
    drag(corner, {corner.x + 20, corner.y - 20});
    const auto& resized = *overlay.DocumentForTest().At(0);
    const auto& points = mosaicBrushTest ? std::get<rc::MosaicCommand>(resized).points
                                         : std::get<rc::PenCommand>(resized).points;
    if (overlay.DocumentForTest().Size() != 3 || points.front().y >= 90.0f ||
        points.back().x <= 230.0f ||
        !strokePointsMatch(1, {180, 110}, {200, 90}) ||
        !strokePointsMatch(2, {170, 105}, {190, 85})) return 23;
    return 0;
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
  if (!overlay.EditingForTest()) return 10;
  SendMessageW(overlay.hwnd(), WM_KEYDOWN, VK_ESCAPE, 0);
  if (overlay.EditingForTest()) return 11;
  // ESC from edit mode returns to selection instead of closing the overlay.
  SendMessageW(overlay.hwnd(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(40, 40));
  SendMessageW(overlay.hwnd(), WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(520, 250));
  SendMessageW(overlay.hwnd(), WM_LBUTTONUP, 0, MAKELPARAM(520, 250));
  if (!overlay.EditingForTest()) return 12;
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
  const auto editText = [](HWND target) {
    const int chars = GetWindowTextLengthW(target);
    std::wstring value(static_cast<size_t>(chars), L'\0');
    GetWindowTextW(target, value.data(), chars + 1);
    return value;
  };
  const auto pumpMessages = [] {
    MSG pending{};
    while (PeekMessageW(&pending, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&pending);
      DispatchMessageW(&pending);
    }
  };
  // Mirror TranslateMessage: the EDIT acts on the WM_CHAR following the
  // key-down (Enter inserts CRLF; Backspace deletes one character).
  const auto pressEnter = [](HWND target) {
    SendMessageW(target, WM_KEYDOWN, VK_RETURN, 0);
    SendMessageW(target, WM_CHAR, VK_RETURN, 0);
    SendMessageW(target, WM_KEYUP, VK_RETURN, 0);
  };
  const auto pressBackspace = [](HWND target) {
    SendMessageW(target, WM_KEYDOWN, VK_BACK, 0);
    SendMessageW(target, WM_CHAR, VK_BACK, 0);
    SendMessageW(target, WM_KEYUP, VK_BACK, 0);
  };
  // Ctrl+Enter commits.  The physical modifier must be synced into the thread
  // key-state array (which happens when a queued message is retrieved) before
  // the direct SendMessage reaches the edit subclass.
  const auto commitTextEdit = [&](HWND target) {
    keybd_event(VK_CONTROL, 0, 0, 0);
    pumpMessages();
    SendMessageW(target, WM_KEYDOWN, VK_RETURN, 0);
    keybd_event(VK_CONTROL, 0, KEYEVENTF_KEYUP, 0);
    pumpMessages();
  };
  SendMessageW(edit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(L"横竖文字"));
  // Plain Enter inserts a line break instead of committing.
  pressEnter(edit);
  if (editText(edit) != L"横竖文字\r\n") return 13;
  if (FindWindowExW(overlay.hwnd(), nullptr, L"Edit", nullptr) != edit) return 14;
  // Two backspaces remove the CRLF; Ctrl+Enter then finishes editing.
  pressBackspace(edit);
  pressBackspace(edit);
  if (editText(edit) != L"横竖文字") return 15;
  commitTextEdit(edit);
  if (FindWindowExW(overlay.hwnd(), nullptr, L"Edit", nullptr)) return 16;
  // Double-click re-opens the committed text for another editing pass.
  SendMessageW(overlay.hwnd(), WM_LBUTTONDBLCLK, 0, MAKELPARAM(100, 100));
  edit = FindWindowExW(overlay.hwnd(), nullptr, L"Edit", nullptr);
  if (!edit || editText(edit) != L"横竖文字") return 17;
  commitTextEdit(edit);
  if (FindWindowExW(overlay.hwnd(), nullptr, L"Edit", nullptr)) return 18;
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
