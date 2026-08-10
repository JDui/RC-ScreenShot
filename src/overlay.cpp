#include "overlay.hpp"

#include <commdlg.h>
#include <commctrl.h>
#include <windowsx.h>

#include <cwctype>
#include <limits>

namespace rc {
namespace {

constexpr wchar_t kOverlayClass[] = L"RC-ScreenShot.Overlay";
constexpr UINT_PTR kUnitTimer = 1;
constexpr int kToolbarMargin = 8;
constexpr int kToolbarHeight = 94;
constexpr int kToolbarWidth = 540;
constexpr int kToolbarToolSize = 38;
constexpr int kToolbarToolGap = 4;
constexpr int kToolbarActionSize = 42;

constexpr std::array<uint32_t, 9> kPresetColors{{
    0xFF3B30FF, 0xFF9500FF, 0xFFCC00FF, 0x34C759FF, 0x32ADE6FF,
    0x007AFFFF, 0xAF52DEFF, 0xFFFFFFFF, 0x111111FF}};
constexpr std::array<const wchar_t*, 9> kPresetNames{{
    L"红色", L"橙色", L"黄色", L"绿色", L"青色", L"蓝色", L"紫色", L"白色", L"黑色"}};

enum ContextCommand : UINT {
  kContextToolBase = 5000,
  kContextColorBase = 5100,
  kContextMoreColor = 5199,
  kContextSizeBase = 5200,
  kContextOpacityBase = 5300,
  kContextFillOpacityBase = 5400,
  kContextMosaicPixel = 5500,
  kContextMosaicBlur,
  kContextTextHorizontal,
  kContextTextVertical,
  kContextUndo = 5600,
  kContextRedo,
  kContextCopy,
  kContextSave,
  kContextCancel
};

constexpr std::array<Tool, 9> kContextTools{{
    Tool::Pen, Tool::Rectangle, Tool::Ellipse, Tool::Line, Tool::Arrow, Tool::Text,
    Tool::MosaicBrush, Tool::MosaicRectangle, Tool::Select}};
constexpr std::array<const wchar_t*, 9> kContextToolNames{{
    L"画笔", L"矩形", L"圆形", L"直线", L"箭头", L"文字",
    L"马赛克笔", L"框选马赛克", L"选择/调整选区"}};
constexpr std::array<float, 7> kContextSizes{{2, 4, 8, 16, 32, 64, 96}};
constexpr std::array<float, 4> kContextOpacities{{0.25f, 0.5f, 0.75f, 1.0f}};

D2D1_COLOR_F ColorFromSetting(const ColorSetting& setting, float opacity = 1.0f) {
  const uint32_t value = setting.rgba;
  return D2D1::ColorF(((value >> 24) & 255) / 255.0f, ((value >> 16) & 255) / 255.0f,
                      ((value >> 8) & 255) / 255.0f, (value & 255) / 255.0f * opacity);
}

RECT ToLocal(const RECT& rect, const RECT& virtualBounds) {
  RECT local = rect;
  OffsetRect(&local, -virtualBounds.left, -virtualBounds.top);
  return local;
}

D2D1_RECT_F ToD2D(const RECT& rect) {
  return D2D1::RectF(static_cast<float>(rect.left), static_cast<float>(rect.top),
                     static_cast<float>(rect.right), static_cast<float>(rect.bottom));
}

bool HasArea(const RECT& rect, int minimum = 4) {
  return rect.right - rect.left >= minimum && rect.bottom - rect.top >= minimum;
}

RectF EmptyBounds() {
  return {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
          std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()};
}

void IncludePoint(RectF& bounds, PointF point) {
  bounds.left = std::min(bounds.left, point.x);
  bounds.top = std::min(bounds.top, point.y);
  bounds.right = std::max(bounds.right, point.x);
  bounds.bottom = std::max(bounds.bottom, point.y);
}

RectF Inflate(RectF bounds, float amount) {
  if (bounds.left > bounds.right || bounds.top > bounds.bottom) return {};
  return {bounds.left - amount, bounds.top - amount, bounds.right + amount, bounds.bottom + amount};
}

RectF CommandBounds(const EditCommand& command) {
  if (const auto* pen = std::get_if<PenCommand>(&command)) {
    if (pen->points.empty()) return {};
    RectF bounds = EmptyBounds();
    for (const PointF point : pen->points) IncludePoint(bounds, point);
    return Inflate(bounds, std::max(4.0f, pen->style.width * 0.5f + 2.0f));
  }
  if (const auto* shape = std::get_if<ShapeCommand>(&command)) {
    const RectF bounds = NormalizeRect(shape->start, shape->end);
    return Inflate(bounds, std::max(4.0f, shape->style.stroke.width * 0.5f + 2.0f));
  }
  if (const auto* mosaic = std::get_if<MosaicCommand>(&command)) {
    if (!mosaic->brush) return Inflate(mosaic->bounds, 2.0f);
    if (mosaic->points.empty()) return {};
    RectF bounds = EmptyBounds();
    for (const PointF point : mosaic->points) IncludePoint(bounds, point);
    return Inflate(bounds, std::max(4.0f, mosaic->brushSize * 0.5f));
  }
  const auto* text = std::get_if<TextCommand>(&command);
  if (!text || text->text.empty()) return {};
  const float advance = std::max(8.0f, text->style.size * 1.16f);
  float lineWidth = 0.0f;
  float maxLineWidth = 0.0f;
  int lines = 1;
  int currentColumnLength = 0;
  int maxColumnLength = 0;
  for (const wchar_t character : text->text) {
    if (character == L'\r') continue;
    if (character == L'\n') {
      maxLineWidth = std::max(maxLineWidth, lineWidth);
      lineWidth = 0.0f;
      ++lines;
      maxColumnLength = std::max(maxColumnLength, currentColumnLength);
      currentColumnLength = 0;
      continue;
    }
    lineWidth += text->style.size * (character >= 0x2E80 ? 1.0f : 0.62f);
    ++currentColumnLength;
  }
  maxLineWidth = std::max(maxLineWidth, lineWidth);
  maxColumnLength = std::max(maxColumnLength, currentColumnLength);
  if (text->style.vertical) {
    return {text->origin.x, text->origin.y,
            text->origin.x + std::max(advance, static_cast<float>(lines) * advance),
            text->origin.y + std::max(advance, static_cast<float>(maxColumnLength) * advance)};
  }
  return {text->origin.x, text->origin.y, text->origin.x + std::max(advance, maxLineWidth),
          text->origin.y + std::max(advance, static_cast<float>(lines) * text->style.size * 1.25f)};
}

bool NearRect(const RectF& bounds, PointF point, float radius) {
  return point.x >= bounds.left - radius && point.x <= bounds.right + radius &&
         point.y >= bounds.top - radius && point.y <= bounds.bottom + radius;
}

float DistanceToRectEdge(const RectF& bounds, PointF point) {
  const float left = std::abs(point.x - bounds.left);
  const float right = std::abs(point.x - bounds.right);
  const float top = std::abs(point.y - bounds.top);
  const float bottom = std::abs(point.y - bounds.bottom);
  return std::min({left, right, top, bottom});
}

bool HitCommandGeometry(const EditCommand& command, PointF point) {
  if (const auto* pen = std::get_if<PenCommand>(&command)) {
    const float radius = std::max(5.0f, pen->style.width * 0.5f + 4.0f);
    if (pen->points.size() == 1) return std::hypot(point.x - pen->points.front().x,
                                                    point.y - pen->points.front().y) <= radius;
    for (size_t i = 1; i < pen->points.size(); ++i) {
      if (DistanceToSegment(point, pen->points[i - 1], pen->points[i]) <= radius) return true;
    }
    return false;
  }
  if (const auto* shape = std::get_if<ShapeCommand>(&command)) {
    const RectF bounds = NormalizeRect(shape->start, shape->end);
    const float tolerance = std::max(5.0f, shape->style.stroke.width * 0.5f + 4.0f);
    if (shape->kind == ShapeKind::Line || shape->kind == ShapeKind::Arrow) {
      if (DistanceToSegment(point, shape->start, shape->end) <= tolerance) return true;
      if (shape->kind == ShapeKind::Arrow) {
        const float angle = std::atan2(shape->end.y - shape->start.y, shape->end.x - shape->start.x);
        const float size = std::max(10.0f, shape->style.stroke.width * 4.0f);
        const PointF left{shape->end.x - size * std::cos(angle - .55f),
                          shape->end.y - size * std::sin(angle - .55f)};
        const PointF right{shape->end.x - size * std::cos(angle + .55f),
                           shape->end.y - size * std::sin(angle + .55f)};
        return DistanceToSegment(point, shape->end, left) <= tolerance ||
               DistanceToSegment(point, shape->end, right) <= tolerance;
      }
      return false;
    }
    if (shape->style.fillOpacity > 0 && Contains(bounds, point)) return true;
    if (!NearRect(bounds, point, tolerance)) return false;
    if (shape->kind == ShapeKind::Rectangle)
      return DistanceToRectEdge(bounds, point) <= tolerance;
    const float rx = std::max(0.5f, (bounds.right - bounds.left) * 0.5f);
    const float ry = std::max(0.5f, (bounds.bottom - bounds.top) * 0.5f);
    const float nx = (point.x - (bounds.left + bounds.right) * 0.5f) / rx;
    const float ny = (point.y - (bounds.top + bounds.bottom) * 0.5f) / ry;
    return std::abs(std::hypot(nx, ny) - 1.0f) * std::min(rx, ry) <= tolerance;
  }
  if (const auto* mosaic = std::get_if<MosaicCommand>(&command)) {
    if (!mosaic->brush) return Contains(Inflate(mosaic->bounds, 2.0f), point);
    const float radius = std::max(5.0f, mosaic->brushSize * 0.5f + 2.0f);
    for (size_t i = 1; i < mosaic->points.size(); ++i) {
      if (DistanceToSegment(point, mosaic->points[i - 1], mosaic->points[i]) <= radius) return true;
    }
    return !mosaic->points.empty() &&
           std::hypot(point.x - mosaic->points.front().x, point.y - mosaic->points.front().y) <= radius;
  }
  return Contains(CommandBounds(command), point);
}

void TransformPoint(PointF& point, const RectF& before, const RectF& after) {
  const float width = std::max(1.0f, before.right - before.left);
  const float height = std::max(1.0f, before.bottom - before.top);
  point.x = after.left + (point.x - before.left) * (after.right - after.left) / width;
  point.y = after.top + (point.y - before.top) * (after.bottom - after.top) / height;
}

void TransformCommand(EditCommand& command, const RectF& before, const RectF& after) {
  if (auto* pen = std::get_if<PenCommand>(&command)) {
    for (PointF& point : pen->points) TransformPoint(point, before, after);
  } else if (auto* shape = std::get_if<ShapeCommand>(&command)) {
    TransformPoint(shape->start, before, after);
    TransformPoint(shape->end, before, after);
    shape->style.stroke.width = std::clamp(shape->style.stroke.width *
                                           std::sqrt(std::abs((after.right - after.left) /
                                                              std::max(1.0f, before.right - before.left)) *
                                                     std::abs((after.bottom - after.top) /
                                                              std::max(1.0f, before.bottom - before.top))),
                                           1.0f, 128.0f);
  } else if (auto* mosaic = std::get_if<MosaicCommand>(&command)) {
    for (PointF& point : mosaic->points) TransformPoint(point, before, after);
    PointF start{mosaic->bounds.left, mosaic->bounds.top};
    PointF end{mosaic->bounds.right, mosaic->bounds.bottom};
    TransformPoint(start, before, after);
    TransformPoint(end, before, after);
    mosaic->bounds = NormalizeRect(start, end);
    mosaic->brushSize = std::clamp(mosaic->brushSize *
                                   std::sqrt(std::abs((after.right - after.left) /
                                                      std::max(1.0f, before.right - before.left)) *
                                             std::abs((after.bottom - after.top) /
                                                      std::max(1.0f, before.bottom - before.top))),
                                   4.0f, 256.0f);
  } else if (auto* text = std::get_if<TextCommand>(&command)) {
    TransformPoint(text->origin, before, after);
    const float scale = std::sqrt(std::abs((after.right - after.left) /
                                           std::max(1.0f, before.right - before.left)) *
                                  std::abs((after.bottom - after.top) /
                                           std::max(1.0f, before.bottom - before.top)));
    text->style.size = std::clamp(text->style.size * scale, 8.0f, 256.0f);
  }
}

bool ChooseColorFor(HWND owner, ColorSetting& color) {
  static COLORREF custom[16]{};
  const uint32_t rgba = color.rgba;
  CHOOSECOLORW chooser{sizeof(chooser)};
  chooser.hwndOwner = owner; chooser.lpCustColors = custom;
  chooser.rgbResult = RGB((rgba >> 24) & 255, (rgba >> 16) & 255, (rgba >> 8) & 255);
  chooser.Flags = CC_FULLOPEN | CC_RGBINIT;
  if (!ChooseColorW(&chooser)) return false;
  color.rgba = (GetRValue(chooser.rgbResult) << 24) | (GetGValue(chooser.rgbResult) << 16) |
               (GetBValue(chooser.rgbResult) << 8) | (rgba & 255);
  return true;
}

}  // namespace

CaptureOverlay::CaptureOverlay(HINSTANCE instance, DesktopSnapshot snapshot, AppConfig& config,
                               CompletionCallback completion, ConfigChangedCallback configChanged)
    : instance_(instance), snapshot_(std::move(snapshot)), config_(config),
      completion_(std::move(completion)), configChanged_(std::move(configChanged)) {}

CaptureOverlay::~CaptureOverlay() {
  if (unitThread_.joinable()) unitThread_.request_stop();
  CancelTextInput();
  if (hwnd_) DestroyWindow(hwnd_);
  SetCursor(LoadCursorW(nullptr, IDC_ARROW));
}

bool CaptureOverlay::Show(std::wstring& error) {
  WNDCLASSEXW windowClass{sizeof(windowClass)};
  windowClass.style = CS_HREDRAW | CS_VREDRAW;
  windowClass.lpfnWndProc = WindowProc;
  windowClass.hInstance = instance_;
  windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  windowClass.hIcon = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(102), IMAGE_ICON,
                                                    0, 0, LR_DEFAULTSIZE | LR_SHARED));
  windowClass.hIconSm = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(102), IMAGE_ICON,
                                                      16, 16, LR_SHARED));
  windowClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
  windowClass.lpszClassName = kOverlayClass;
  RegisterClassExW(&windowClass);
  hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kOverlayClass, L"RC-ScreenShot",
                          WS_POPUP, snapshot_.virtualBounds.left, snapshot_.virtualBounds.top,
                          snapshot_.width, snapshot_.height, nullptr, nullptr, instance_, this);
  if (!hwnd_) { error = L"创建截图覆盖层失败：" + HResultMessage(HRESULT_FROM_WIN32(GetLastError())); return false; }
  if (!CreateDeviceResources()) { error = L"初始化截图覆盖层图形资源失败。"; return false; }
  ShowWindow(hwnd_, SW_SHOW);
  SetForegroundWindow(hwnd_);
  SetFocus(hwnd_);
  BeginUnitDetection();
  return true;
}

LRESULT CALLBACK CaptureOverlay::WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
  CaptureOverlay* self = reinterpret_cast<CaptureOverlay*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
    self = static_cast<CaptureOverlay*>(create->lpCreateParams);
    self->hwnd_ = hwnd;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  }
  return self ? self->HandleMessage(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT CaptureOverlay::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
  switch (message) {
    case WM_ERASEBKGND: return 1;
    case WM_CTLCOLOREDIT:
      if (textEdit_ && reinterpret_cast<HWND>(lParam) == textEdit_) {
        const uint32_t color = textInputStyle_.color.rgba;
        SetTextColor(reinterpret_cast<HDC>(wParam), RGB((color >> 24) & 0xFF,
                                                        (color >> 16) & 0xFF,
                                                        (color >> 8) & 0xFF));
        SetBkMode(reinterpret_cast<HDC>(wParam), TRANSPARENT);
        return reinterpret_cast<LRESULT>(GetStockObject(NULL_BRUSH));
      }
      break;
    case WM_PAINT: Paint(); return 0;
    case WM_SIZE:
      if (renderTarget_ && renderTarget_->Resize(D2D1::SizeU(LOWORD(lParam), HIWORD(lParam))) == D2DERR_RECREATE_TARGET)
        DiscardDeviceResources();
      InvalidateRect(hwnd_, nullptr, FALSE); return 0;
    case WM_DISPLAYCHANGE: Cancel(); return 0;
    case WM_TIMER:
      if (wParam == kUnitTimer && unitReady_) { KillTimer(hwnd_, kUnitTimer); InvalidateRect(hwnd_, nullptr, FALSE); }
      return 0;
    case WM_MOUSEMOVE: {
      POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
      currentPoint_ = point;
      if (Contains(selection_, point)) {
        lastCanvasPoint_ = point;
        if (settingPreview_) settingPreviewPoint_ = point;
      } else if (settingPreview_) {
        settingPreviewPoint_ = lastCanvasPoint_;
      }
      if (sizeSliderDragging_) SetSizeFromSlider(point);
      else if (selecting_) ContinueSelection(point);
      else if (drawing_) ContinueEditGesture(point);
      else UpdateHover(point);
      InvalidateRect(hwnd_, nullptr, FALSE); return 0;
    }
    case WM_LBUTTONDOWN: {
      SetFocus(hwnd_);
      POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
      if (editing_) {
        if (textEdit_) CommitTextInput();
        if (Contains(SizeSliderRect(), point) && tool_ != Tool::Select) {
          sizeSliderDragging_ = true; BeginSettingPreview(point); SetSizeFromSlider(point); return 0;
        }
        if (auto preset = HitTestColorPreset(point)) {
          SetActivePresetColor(*preset); BeginSettingPreview(point); return 0;
        }
        if (HitCopy(point)) { Complete(CaptureCompletion::Copy); return 0; }
        if (HitSave(point)) { Complete(CaptureCompletion::Save); return 0; }
        if (auto hit = HitTestTool(point)) { SelectTool(*hit); return 0; }
        if (auto property = HitTestProperty(point)) {
          ActivateProperty(*property); BeginSettingPreview(point); return 0;
        }
        BeginEditGesture(point);
      } else BeginSelection(point);
      return 0;
    }
    case WM_LBUTTONUP: {
      POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
      if (sizeSliderDragging_) {
        SetSizeFromSlider(point); sizeSliderDragging_ = false; EndSettingPreview(); return 0;
      }
      if (settingPreview_) { EndSettingPreview(); return 0; }
      if (selecting_) EndSelection(point);
      else if (drawing_) EndEditGesture(point);
      return 0;
    }
    case WM_CAPTURECHANGED:
      if (sizeSliderDragging_) sizeSliderDragging_ = false;
      if (settingPreview_ && !drawing_ && !selecting_) settingPreview_ = false;
      InvalidateRect(hwnd_, nullptr, FALSE); return 0;
    case WM_LBUTTONDBLCLK: {
      if (editing_ && tool_ == Tool::Select) {
        const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const auto hit = HitTestCommand(point);
        if (hit) {
          selectedCommand_ = *hit;
          drawing_ = false;
          commandAdjustment_ = SelectionAdjustment::None;
          ReleaseCapture();
          if (const auto* text = document_.At(*hit)) {
            if (std::holds_alternative<TextCommand>(*text)) BeginTextInput(point, *hit);
          }
          InvalidateRect(hwnd_, nullptr, FALSE);
        }
      }
      return 0;
    }
    case WM_RBUTTONUP:
      if (editing_) {
        ShowEditorContextMenu({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
        return 0;
      }
      return 0;
    case WM_MOUSEWHEEL:
      if (!editing_ && mode_ == SelectionMode::Unit && !hoverUnitChain_.empty()) {
        if (GET_WHEEL_DELTA_WPARAM(wParam) > 0) hoverUnitIndex_ = std::min(hoverUnitIndex_ + 1, hoverUnitChain_.size() - 1);
        else if (hoverUnitIndex_) --hoverUnitIndex_;
        std::scoped_lock lock(unitMutex_);
        hoverRect_ = unitCandidates_[hoverUnitChain_[hoverUnitIndex_]].bounds;
        InvalidateRect(hwnd_, nullptr, FALSE);
      }
      return 0;
    case WM_KEYDOWN: {
      const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
      const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
      if (wParam == VK_ESCAPE) { Cancel(); return 0; }
      if (wParam == VK_SPACE && !editing_) { CycleMode(); return 0; }
      if (control && wParam == 'Z') {
        document_.Undo();
        if (selectedCommand_ && !document_.At(*selectedCommand_)) selectedCommand_.reset();
        InvalidateRect(hwnd_, nullptr, FALSE); return 0;
      }
      if (control && wParam == 'Y') {
        document_.Redo();
        if (selectedCommand_ && !document_.At(*selectedCommand_)) selectedCommand_.reset();
        InvalidateRect(hwnd_, nullptr, FALSE); return 0;
      }
      if (editing_ && (wParam == VK_DELETE || wParam == VK_BACK) && selectedCommand_) {
        if (document_.Remove(*selectedCommand_)) {
          selectedCommand_.reset();
          InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
      }
      if (editing_ && control && wParam == 'C') { Complete(CaptureCompletion::Copy); return 0; }
      if (editing_ && control && wParam == 'S') { Complete(CaptureCompletion::Save); return 0; }
      if (editing_ && wParam == VK_RETURN) {
        Complete(config_.defaultAction == DefaultAction::Save ? CaptureCompletion::Save : CaptureCompletion::Copy); return 0;
      }
      if (editing_) {
        switch (wParam) {
          case 'P': SelectTool(Tool::Pen); break;
          case 'R': SelectTool(Tool::Rectangle); break;
          case 'E': SelectTool(Tool::Ellipse); break;
          case 'L': SelectTool(Tool::Line); break;
          case 'A': SelectTool(Tool::Arrow); break;
          case 'T': SelectTool(Tool::Text); break;
          case 'M': SelectTool(shift ? Tool::MosaicRectangle : Tool::MosaicBrush); break;
          case 'V': SelectTool(Tool::Select); break;
          case 'C': ChooseActiveColor(); break;
          case 'O': CycleActiveOpacity(); break;
          case VK_OEM_4: AdjustActiveSize(-1.0f); break;
          case VK_OEM_6: AdjustActiveSize(1.0f); break;
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
      }
      return 0;
    }
    case WM_DESTROY: hwnd_ = nullptr; return 0;
  }
  return DefWindowProcW(hwnd_, message, wParam, lParam);
}

bool CaptureOverlay::CreateDeviceResources() {
  if (!snapshot_.IsValid()) return false;
  if (!d2dFactory_ && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(&d2dFactory_)))) return false;
  if (!dwriteFactory_ && FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                                    reinterpret_cast<IUnknown**>(dwriteFactory_.GetAddressOf())))) return false;
  if (!renderTarget_) {
    const auto properties = D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(snapshot_.width, snapshot_.height),
                                                             D2D1_PRESENT_OPTIONS_IMMEDIATELY);
    if (FAILED(d2dFactory_->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                     D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                                       D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96),
        properties, &renderTarget_))) return false;
    renderTarget_->SetDpi(96.0f, 96.0f);
  }
  if (!desktopBitmap_) {
    const auto bitmapProperties = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
    if (FAILED(renderTarget_->CreateBitmap(D2D1::SizeU(static_cast<UINT32>(snapshot_.width),
                                                       static_cast<UINT32>(snapshot_.height)),
                                           snapshot_.bgra.data(), static_cast<UINT32>(snapshot_.bgraStride),
                                           bitmapProperties, &desktopBitmap_))) return false;
  }
  return true;
}

void CaptureOverlay::DiscardDeviceResources() { desktopBitmap_.Reset(); renderTarget_.Reset(); }

void CaptureOverlay::Paint() {
  PAINTSTRUCT paint{};
  BeginPaint(hwnd_, &paint);
  ScopeExit end{[&] { EndPaint(hwnd_, &paint); }};
  if (!CreateDeviceResources()) return;
  renderTarget_->BeginDraw();
  renderTarget_->SetTransform(D2D1::Matrix3x2F::Identity());
  renderTarget_->DrawBitmap(desktopBitmap_.Get(), D2D1::RectF(0, 0, static_cast<float>(snapshot_.width),
                                                              static_cast<float>(snapshot_.height)));
  ComPtr<ID2D1SolidColorBrush> dim;
  renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.48f), &dim);
  RECT active = editing_ || selecting_ ? selection_ : hoverRect_;
  if (!HasArea(active)) renderTarget_->FillRectangle(D2D1::RectF(0, 0, static_cast<float>(snapshot_.width), static_cast<float>(snapshot_.height)), dim.Get());
  else {
    renderTarget_->FillRectangle(D2D1::RectF(0, 0, static_cast<float>(snapshot_.width), static_cast<float>(active.top)), dim.Get());
    renderTarget_->FillRectangle(D2D1::RectF(0, static_cast<float>(active.bottom), static_cast<float>(snapshot_.width), static_cast<float>(snapshot_.height)), dim.Get());
    renderTarget_->FillRectangle(D2D1::RectF(0, static_cast<float>(active.top), static_cast<float>(active.left), static_cast<float>(active.bottom)), dim.Get());
    renderTarget_->FillRectangle(D2D1::RectF(static_cast<float>(active.right), static_cast<float>(active.top), static_cast<float>(snapshot_.width), static_cast<float>(active.bottom)), dim.Get());
    ComPtr<ID2D1SolidColorBrush> border;
    renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0.1f, 0.65f, 1.0f, 1.0f), &border);
    renderTarget_->DrawRectangle(ToD2D(active), border.Get(), 2.0f);
  }
  if (editing_) { DrawDocument(); DrawSettingPreview(); DrawToolbar(); }
  const wchar_t* modeName = mode_ == SelectionMode::Normal ? L"普通模式" : mode_ == SelectionMode::Window ? L"窗口模式" : L"单元模式";
  std::wstring status = std::wstring(L"空格切换  ·  ") + modeName;
  if (mode_ == SelectionMode::Unit && !unitReady_) status += L"（正在分析区域…）";
  DrawText(status, D2D1::RectF(16, 14, 330, 48), 15, D2D1::ColorF(D2D1::ColorF::White), DWRITE_TEXT_ALIGNMENT_LEADING);
  HRESULT hr = renderTarget_->EndDraw();
  if (hr == D2DERR_RECREATE_TARGET) DiscardDeviceResources();
}

void CaptureOverlay::BeginSettingPreview(POINT point) {
  settingPreview_ = true;
  settingPreviewPoint_ = Contains(selection_, point) ? point : lastCanvasPoint_;
  if (HasArea(selection_)) {
    settingPreviewPoint_.x = std::clamp(settingPreviewPoint_.x, selection_.left, selection_.right - 1);
    settingPreviewPoint_.y = std::clamp(settingPreviewPoint_.y, selection_.top, selection_.bottom - 1);
  } else {
    settingPreviewPoint_ = point;
  }
  SetCapture(hwnd_);
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void CaptureOverlay::EndSettingPreview() {
  settingPreview_ = false;
  if (GetCapture() == hwnd_) ReleaseCapture();
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void CaptureOverlay::DrawSettingPreview() {
  if (!settingPreview_ || tool_ == Tool::Select || !HasArea(selection_)) return;
  POINT point = settingPreviewPoint_;
  point.x = std::clamp(point.x, selection_.left, selection_.right - 1);
  point.y = std::clamp(point.y, selection_.top, selection_.bottom - 1);
  const float x = static_cast<float>(point.x), y = static_cast<float>(point.y);
  const float size = std::max(1.0f, ActiveSize());
  ComPtr<ID2D1SolidColorBrush> brush, outline;
  const auto createBrushes = [&](D2D1_COLOR_F color, D2D1_COLOR_F edge) {
    renderTarget_->CreateSolidColorBrush(color, &brush);
    renderTarget_->CreateSolidColorBrush(edge, &outline);
  };
  if (tool_ == Tool::Pen) {
    createBrushes(ColorFromSetting(config_.pen.color, config_.pen.opacity * .20f),
                  ColorFromSetting(config_.pen.color, std::min(1.0f, config_.pen.opacity + .2f)));
    const float radius = std::max(3.0f, size * .5f);
    renderTarget_->FillEllipse({{x, y}, radius, radius}, brush.Get());
    renderTarget_->DrawEllipse({{x, y}, radius, radius}, outline.Get(), 1.5f);
    return;
  }
  if (tool_ == Tool::MosaicBrush) {
    createBrushes(D2D1::ColorF(.20f, .75f, 1.0f, .16f), D2D1::ColorF(.48f, .88f, 1.0f, .92f));
    const float radius = std::max(5.0f, size * .5f);
    renderTarget_->FillEllipse({{x, y}, radius, radius}, brush.Get());
    renderTarget_->DrawEllipse({{x, y}, radius, radius}, outline.Get(), 1.5f);
    return;
  }
  if (tool_ == Tool::MosaicRectangle) {
    createBrushes(D2D1::ColorF(.20f, .75f, 1.0f, .16f), D2D1::ColorF(.48f, .88f, 1.0f, .92f));
    const float width = std::clamp(72.0f + size * 2.0f, 48.0f, 180.0f);
    const float height = std::clamp(44.0f + size * 1.5f, 32.0f, 120.0f);
    const float left = std::clamp(x - width * .5f, static_cast<float>(selection_.left),
                                  static_cast<float>(selection_.right) - width);
    const float top = std::clamp(y - height * .5f, static_cast<float>(selection_.top),
                                 static_cast<float>(selection_.bottom) - height);
    const D2D1_RECT_F rect{left, top, left + width, top + height};
    renderTarget_->FillRectangle(rect, brush.Get());
    renderTarget_->DrawRectangle(rect, outline.Get(), 1.5f);
    return;
  }
  if (tool_ == Tool::Text) {
    TextCommand sample{{static_cast<float>(point.x - selection_.left),
                        static_cast<float>(point.y - selection_.top)}, L"Aa", config_.text};
    DrawTextCommand(sample);
    return;
  }

  const auto shapeStyle = tool_ == Tool::Rectangle ? config_.rectangle :
                          tool_ == Tool::Ellipse ? config_.ellipse :
                          ShapeSetting{tool_ == Tool::Line ? config_.line : config_.arrow, {}, 0.0f};
  renderTarget_->CreateSolidColorBrush(ColorFromSetting(shapeStyle.stroke.color, shapeStyle.stroke.opacity), &brush);
  if (shapeStyle.fillOpacity > 0)
    renderTarget_->CreateSolidColorBrush(ColorFromSetting(shapeStyle.fill, shapeStyle.fillOpacity), &outline);
  const float width = std::clamp(56.0f + size * 2.0f, 42.0f, 150.0f);
  const float height = std::clamp(34.0f + size * 1.5f, 28.0f, 100.0f);
  const float left = std::clamp(x - width * .5f, static_cast<float>(selection_.left),
                                static_cast<float>(selection_.right) - width);
  const float top = std::clamp(y - height * .5f, static_cast<float>(selection_.top),
                               static_cast<float>(selection_.bottom) - height);
  const D2D1_RECT_F rect{left, top, left + width, top + height};
  if (tool_ == Tool::Rectangle) {
    if (outline) renderTarget_->FillRectangle(rect, outline.Get());
    renderTarget_->DrawRectangle(rect, brush.Get(), size);
  } else if (tool_ == Tool::Ellipse) {
    const D2D1_ELLIPSE ellipse{{left + width * .5f, top + height * .5f}, width * .5f, height * .5f};
    if (outline) renderTarget_->FillEllipse(ellipse, outline.Get());
    renderTarget_->DrawEllipse(ellipse, brush.Get(), size);
  } else {
    const D2D1_POINT_2F start{left, top + height * .5f};
    const D2D1_POINT_2F end{left + width, top + height * .5f};
    renderTarget_->DrawLine(start, end, brush.Get(), size);
    if (tool_ == Tool::Arrow) {
      const float head = std::max(10.0f, size * 4.0f);
      renderTarget_->DrawLine(end, {end.x - head, end.y - head * .55f}, brush.Get(), size);
      renderTarget_->DrawLine(end, {end.x - head, end.y + head * .55f}, brush.Get(), size);
    }
  }
}

void CaptureOverlay::BeginUnitDetection() {
  SetTimer(hwnd_, kUnitTimer, 50, nullptr);
  unitThread_ = std::jthread([this](std::stop_token token) {
    UnitDetector detector;
    auto candidates = detector.Detect(snapshot_.bgra, snapshot_.width, snapshot_.height, snapshot_.bgraStride);
    if (token.stop_requested()) return;
    { std::scoped_lock lock(unitMutex_); unitCandidates_ = std::move(candidates); }
    unitReady_ = true;
    if (hwnd_) PostMessageW(hwnd_, WM_TIMER, kUnitTimer, 0);
  });
}

void CaptureOverlay::UpdateHover(POINT point) {
  if (editing_ || selecting_) return;
  hoverRect_ = {};
  if (mode_ == SelectionMode::Window) {
    for (const auto& window : snapshot_.windows) {
      RECT local = ToLocal(window.bounds, snapshot_.virtualBounds);
      if (Contains(local, point)) { hoverRect_ = local; break; }
    }
  } else if (mode_ == SelectionMode::Unit && unitReady_) {
    UnitDetector detector;
    std::scoped_lock lock(unitMutex_);
    hoverUnitChain_ = detector.CandidatesAt(unitCandidates_, point);
    hoverUnitIndex_ = std::min(hoverUnitIndex_, hoverUnitChain_.empty() ? size_t{0} : hoverUnitChain_.size() - 1);
    if (!hoverUnitChain_.empty()) hoverRect_ = unitCandidates_[hoverUnitChain_[hoverUnitIndex_]].bounds;
  }
}

void CaptureOverlay::CycleMode() {
  mode_ = mode_ == SelectionMode::Normal ? SelectionMode::Window
         : mode_ == SelectionMode::Window ? SelectionMode::Unit : SelectionMode::Normal;
  hoverRect_ = {}; hoverUnitChain_.clear(); hoverUnitIndex_ = 0;
  UpdateHover(currentPoint_); InvalidateRect(hwnd_, nullptr, FALSE);
}

void CaptureOverlay::BeginSelection(POINT point) {
  windowSelection_ = false;
  if ((mode_ == SelectionMode::Window || mode_ == SelectionMode::Unit) && HasArea(hoverRect_)) {
    selection_ = hoverRect_;
    windowSelection_ = mode_ == SelectionMode::Window;
    editing_ = true;
    InvalidateRect(hwnd_, nullptr, FALSE);
    return;
  }
  dragStart_ = point; currentPoint_ = point; selection_ = NormalizeRect(point, point);
  selecting_ = true; SetCapture(hwnd_);
}

void CaptureOverlay::ContinueSelection(POINT point) {
  selection_ = NormalizeRect(dragStart_, point);
  selection_.left = std::clamp(selection_.left, 0L, static_cast<LONG>(snapshot_.width));
  selection_.right = std::clamp(selection_.right, 0L, static_cast<LONG>(snapshot_.width));
  selection_.top = std::clamp(selection_.top, 0L, static_cast<LONG>(snapshot_.height));
  selection_.bottom = std::clamp(selection_.bottom, 0L, static_cast<LONG>(snapshot_.height));
}

void CaptureOverlay::EndSelection(POINT point) {
  ContinueSelection(point); selecting_ = false; ReleaseCapture();
  if (HasArea(selection_)) editing_ = true;
  else selection_ = {};
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void CaptureOverlay::BeginEditGesture(POINT point) {
  if (tool_ == Tool::Select) {
    const PointF local = ToSelectionPoint(point);
    std::optional<size_t> hit = HitTestCommand(point);
    if (selectedCommand_ && NearRect(SelectedCommandBounds(), local, 10.0f)) hit = selectedCommand_;
    if (hit) {
      selectedCommand_ = *hit;
      commandAdjustment_ = HitTestCommandAdjustment(point);
      if (commandAdjustment_ == SelectionAdjustment::None) commandAdjustment_ = SelectionAdjustment::Move;
      if (const EditCommand* command = document_.At(*selectedCommand_)) {
        commandBeforeAdjust_ = *command;
        selectedCommandBeforeBounds_ = CommandBounds(*command);
        drawing_ = true;
        dragStart_ = point;
        currentPoint_ = point;
        SetCapture(hwnd_);
      }
      InvalidateRect(hwnd_, nullptr, FALSE);
      return;
    }
    selectedCommand_.reset();
    commandAdjustment_ = SelectionAdjustment::None;
    selectionAdjustment_ = HitTestSelectionAdjustment(point);
    if (selectionAdjustment_ != SelectionAdjustment::None) {
      drawing_ = true; dragStart_ = point; currentPoint_ = point;
      selectionBeforeAdjust_ = selection_; SetCapture(hwnd_);
    }
    return;
  }
  if (!Contains(selection_, point) || tool_ == Tool::Frame) return;
  if (tool_ == Tool::Text) {
    BeginTextInput(point);
    return;
  }
  drawing_ = true; dragStart_ = point; currentPoint_ = point; SetCapture(hwnd_);
  PointF local = ToSelectionPoint(point);
  if (tool_ == Tool::Pen) previewCommand_ = PenCommand{{local}, config_.pen};
  else if (tool_ == Tool::MosaicBrush) previewCommand_ = MosaicCommand{true, {local}, {}, config_.mosaicStyle,
      config_.mosaicBrushSize, config_.mosaicPixelSize, config_.mosaicBlurRadius};
  else if (tool_ == Tool::MosaicRectangle) previewCommand_ = MosaicCommand{false, {}, {local.x, local.y, local.x, local.y},
      config_.mosaicStyle, config_.mosaicBrushSize, config_.mosaicPixelSize, config_.mosaicBlurRadius};
  else {
    ShapeKind kind = tool_ == Tool::Rectangle ? ShapeKind::Rectangle : tool_ == Tool::Ellipse ? ShapeKind::Ellipse
                     : tool_ == Tool::Line ? ShapeKind::Line : ShapeKind::Arrow;
    ShapeSetting style = kind == ShapeKind::Rectangle ? config_.rectangle : kind == ShapeKind::Ellipse ? config_.ellipse
                         : ShapeSetting{kind == ShapeKind::Line ? config_.line : config_.arrow, {}, 0};
    previewCommand_ = ShapeCommand{kind, local, local, style};
  }
}

void CaptureOverlay::ContinueEditGesture(POINT point) {
  currentPoint_ = point;
  if (commandAdjustment_ != SelectionAdjustment::None) {
    ContinueCommandAdjustment(point);
    return;
  }
  if (selectionAdjustment_ != SelectionAdjustment::None) {
    ContinueSelectionAdjustment(point);
    return;
  }
  if (!previewCommand_) return;
  PointF local = ToSelectionPoint(point);
  local.x = std::clamp(local.x, 0.0f, static_cast<float>(selection_.right - selection_.left));
  local.y = std::clamp(local.y, 0.0f, static_cast<float>(selection_.bottom - selection_.top));
  if (auto* pen = std::get_if<PenCommand>(&*previewCommand_)) {
    if (pen->points.empty() || std::hypot(local.x - pen->points.back().x, local.y - pen->points.back().y) >= 1.5f) pen->points.push_back(local);
  } else if (auto* shape = std::get_if<ShapeCommand>(&*previewCommand_)) shape->end = local;
  else if (auto* mosaic = std::get_if<MosaicCommand>(&*previewCommand_)) {
    if (mosaic->brush) mosaic->points.push_back(local);
    else mosaic->bounds = NormalizeRect({static_cast<float>(dragStart_.x - selection_.left), static_cast<float>(dragStart_.y - selection_.top)}, local);
  }
}

void CaptureOverlay::EndEditGesture(POINT point) {
  ContinueEditGesture(point); drawing_ = false; ReleaseCapture();
  if (commandAdjustment_ != SelectionAdjustment::None) {
    commandAdjustment_ = SelectionAdjustment::None;
    commandBeforeAdjust_.reset();
    InvalidateRect(hwnd_, nullptr, FALSE);
    return;
  }
  if (selectionAdjustment_ != SelectionAdjustment::None) {
    selectionAdjustment_ = SelectionAdjustment::None;
    InvalidateRect(hwnd_, nullptr, FALSE);
    return;
  }
  if (previewCommand_) document_.Add(std::move(*previewCommand_));
  previewCommand_.reset(); InvalidateRect(hwnd_, nullptr, FALSE);
}

void CaptureOverlay::SelectTool(Tool tool) {
  if (textEdit_) CommitTextInput();
  tool_ = tool;
  if (tool_ != Tool::Select) selectedCommand_.reset();
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void CaptureOverlay::Complete(CaptureCompletion completion) {
  if (textEdit_) CommitTextInput();
  OverlayResult result;
  result.completion = completion; result.selection = selection_;
  OffsetRect(&result.selection, snapshot_.virtualBounds.left, snapshot_.virtualBounds.top);
  result.document = std::move(document_); result.windowSelection = windowSelection_;
  ShowWindow(hwnd_, SW_HIDE);
  SetCursor(LoadCursorW(nullptr, IDC_ARROW));
  if (completion_) completion_(std::move(result));
}

void CaptureOverlay::Cancel() {
  CancelTextInput();
  OverlayResult result; result.completion = CaptureCompletion::Cancel;
  ShowWindow(hwnd_, SW_HIDE);
  SetCursor(LoadCursorW(nullptr, IDC_ARROW));
  if (completion_) completion_(std::move(result));
}

void CaptureOverlay::DrawDocument() {
  std::vector<EditCommand> commands(document_.Commands().begin(), document_.Commands().end());
  if (previewCommand_) commands.push_back(*previewCommand_);
  renderTarget_->PushAxisAlignedClip(ToD2D(selection_), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
  const bool mosaicLayerDrawn = DrawMosaicLayer(commands);
  ComPtr<ID2D1StrokeStyle> roundStroke;
  if (d2dFactory_) {
    d2dFactory_->CreateStrokeStyle(
        D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                    D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND,
                                    10.0f, D2D1_DASH_STYLE_SOLID, 0.0f), nullptr, 0, &roundStroke);
  }
  const auto drawRoundPath = [&](const std::vector<PointF>& points, const D2D1_COLOR_F& color,
                                 float width) {
    if (points.empty()) return;
    ComPtr<ID2D1SolidColorBrush> brush;
    renderTarget_->CreateSolidColorBrush(color, &brush);
    if (points.size() == 1) {
      renderTarget_->FillEllipse({{selection_.left + points.front().x, selection_.top + points.front().y},
                                  width * 0.5f, width * 0.5f}, brush.Get());
      return;
    }
    ComPtr<ID2D1PathGeometry> geometry;
    if (!d2dFactory_ || FAILED(d2dFactory_->CreatePathGeometry(&geometry))) return;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(geometry->Open(&sink))) return;
    sink->BeginFigure({selection_.left + points.front().x, selection_.top + points.front().y},
                      D2D1_FIGURE_BEGIN_HOLLOW);
    for (size_t i = 1; i < points.size(); ++i) {
      sink->AddLine({selection_.left + points[i].x, selection_.top + points[i].y});
    }
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    if (FAILED(sink->Close())) return;
    renderTarget_->DrawGeometry(geometry.Get(), brush.Get(), width, roundStroke.Get());
  };
  for (const auto& command : commands) {
    if (const auto* pen = std::get_if<PenCommand>(&command)) {
      drawRoundPath(pen->points, ColorFromSetting(pen->style.color, pen->style.opacity), pen->style.width);
    } else if (const auto* shape = std::get_if<ShapeCommand>(&command)) {
      const RectF rect = NormalizeRect(shape->start, shape->end);
      const D2D1_RECT_F bounds = D2D1::RectF(selection_.left + rect.left, selection_.top + rect.top,
                                             selection_.left + rect.right, selection_.top + rect.bottom);
      ComPtr<ID2D1SolidColorBrush> stroke, fill;
      renderTarget_->CreateSolidColorBrush(ColorFromSetting(shape->style.stroke.color, shape->style.stroke.opacity), &stroke);
      if (shape->style.fillOpacity > 0) renderTarget_->CreateSolidColorBrush(ColorFromSetting(shape->style.fill, shape->style.fillOpacity), &fill);
      if (shape->kind == ShapeKind::Rectangle) {
        if (fill) renderTarget_->FillRectangle(bounds, fill.Get());
        renderTarget_->DrawRectangle(bounds, stroke.Get(), shape->style.stroke.width);
      } else if (shape->kind == ShapeKind::Ellipse) {
        D2D1_ELLIPSE ellipse{{(bounds.left + bounds.right) / 2, (bounds.top + bounds.bottom) / 2},
                             (bounds.right - bounds.left) / 2, (bounds.bottom - bounds.top) / 2};
        if (fill) renderTarget_->FillEllipse(ellipse, fill.Get());
        renderTarget_->DrawEllipse(ellipse, stroke.Get(), shape->style.stroke.width);
      } else {
        D2D1_POINT_2F start{selection_.left + shape->start.x, selection_.top + shape->start.y};
        D2D1_POINT_2F end{selection_.left + shape->end.x, selection_.top + shape->end.y};
        renderTarget_->DrawLine(start, end, stroke.Get(), shape->style.stroke.width);
        if (shape->kind == ShapeKind::Arrow) {
          const float angle = std::atan2(end.y - start.y, end.x - start.x);
          const float size = std::max(10.0f, shape->style.stroke.width * 4.0f);
          renderTarget_->DrawLine(end, {end.x - size * std::cos(angle - .55f), end.y - size * std::sin(angle - .55f)}, stroke.Get(), shape->style.stroke.width);
          renderTarget_->DrawLine(end, {end.x - size * std::cos(angle + .55f), end.y - size * std::sin(angle + .55f)}, stroke.Get(), shape->style.stroke.width);
        }
      }
    } else if (const auto* text = std::get_if<TextCommand>(&command)) {
      DrawTextCommand(*text);
    } else if (const auto* mosaic = std::get_if<MosaicCommand>(&command)) {
      if (!mosaicLayerDrawn) {
        if (!mosaic->brush) {
          ComPtr<ID2D1SolidColorBrush> brush;
          renderTarget_->CreateSolidColorBrush(D2D1::ColorF(.2f, .75f, 1.0f, .22f), &brush);
          renderTarget_->FillRectangle(D2D1::RectF(selection_.left + mosaic->bounds.left, selection_.top + mosaic->bounds.top,
                                                    selection_.left + mosaic->bounds.right, selection_.top + mosaic->bounds.bottom), brush.Get());
        } else {
          drawRoundPath(mosaic->points, D2D1::ColorF(.2f, .75f, 1.0f, .22f), mosaic->brushSize);
        }
      }
    }
  }
  renderTarget_->PopAxisAlignedClip();
  if (tool_ == Tool::Select) {
    if (selectedCommand_) DrawCommandHandles();
    else DrawSelectionHandles();
  }
}

bool CaptureOverlay::DrawMosaicLayer(std::span<const EditCommand> commands) {
  if (!HasArea(selection_)) return false;
  bool hasMosaic = false;
  for (const EditCommand& command : commands) {
    if (std::holds_alternative<MosaicCommand>(command)) {
      hasMosaic = true;
      break;
    }
  }
  if (!hasMosaic) return false;

  const LONG left = std::clamp(selection_.left, 0L, static_cast<LONG>(snapshot_.width));
  const LONG top = std::clamp(selection_.top, 0L, static_cast<LONG>(snapshot_.height));
  const LONG right = std::clamp(selection_.right, left, static_cast<LONG>(snapshot_.width));
  const LONG bottom = std::clamp(selection_.bottom, top, static_cast<LONG>(snapshot_.height));
  const int width = right - left;
  const int height = bottom - top;
  if (width <= 0 || height <= 0) return false;

  std::vector<uint8_t> pixels(static_cast<size_t>(width * height * 4));
  for (int y = 0; y < height; ++y) {
    memcpy(pixels.data() + static_cast<size_t>(y * width * 4),
           snapshot_.bgra.data() + static_cast<size_t>((top + y) * snapshot_.bgraStride + left * 4),
           static_cast<size_t>(width * 4));
  }
  ApplyMosaics(pixels, width, height, width * 4, commands);

  const auto bitmapProperties = D2D1::BitmapProperties(
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
  ComPtr<ID2D1Bitmap> bitmap;
  if (FAILED(renderTarget_->CreateBitmap(D2D1::SizeU(static_cast<UINT32>(width), static_cast<UINT32>(height)),
                                         pixels.data(), static_cast<UINT32>(width * 4),
                                         bitmapProperties, &bitmap))) return false;
  renderTarget_->DrawBitmap(bitmap.Get(), D2D1::RectF(static_cast<float>(left), static_cast<float>(top),
                                                      static_cast<float>(right), static_cast<float>(bottom)),
                             1.0f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
  return true;
}

void CaptureOverlay::DrawTextCommand(const TextCommand& command) {
  ComPtr<IDWriteTextFormat> format;
  if (FAILED(dwriteFactory_->CreateTextFormat(command.style.fontFamily.c_str(), nullptr,
      DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
      command.style.size, L"zh-CN", &format))) return;
  format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
  format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
  format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  ComPtr<ID2D1SolidColorBrush> brush;
  renderTarget_->CreateSolidColorBrush(ColorFromSetting(command.style.color, command.style.opacity), &brush);
  const float originX = selection_.left + command.origin.x;
  const float originY = selection_.top + command.origin.y;
  if (!command.style.vertical) {
    const D2D1_RECT_F bounds{originX, originY, originX + 4096.0f, originY + 4096.0f};
    renderTarget_->DrawTextW(command.text.data(), static_cast<UINT32>(command.text.size()),
                             format.Get(), bounds, brush.Get());
    return;
  }
  float x = originX, y = originY;
  const float advance = command.style.size * 1.16f;
  for (wchar_t character : command.text) {
    if (character == L'\r') continue;
    if (character == L'\n') { x += advance; y = originY; continue; }
    const D2D1_RECT_F cell{x, y, x + advance, y + advance};
    renderTarget_->DrawTextW(&character, 1, format.Get(), cell, brush.Get());
    y += advance;
  }
}

CaptureOverlay::SelectionAdjustment CaptureOverlay::HitTestSelectionAdjustment(POINT point) const {
  constexpr int radius = 9;
  const auto isNear = [&](int x, int y) {
    return std::abs(point.x - x) <= radius && std::abs(point.y - y) <= radius;
  };
  if (isNear(selection_.left, selection_.top)) return SelectionAdjustment::TopLeft;
  if (isNear(selection_.right, selection_.top)) return SelectionAdjustment::TopRight;
  if (isNear(selection_.left, selection_.bottom)) return SelectionAdjustment::BottomLeft;
  if (isNear(selection_.right, selection_.bottom)) return SelectionAdjustment::BottomRight;
  if (std::abs(point.x - selection_.left) <= radius && point.y >= selection_.top && point.y <= selection_.bottom)
    return SelectionAdjustment::Left;
  if (std::abs(point.x - selection_.right) <= radius && point.y >= selection_.top && point.y <= selection_.bottom)
    return SelectionAdjustment::Right;
  if (std::abs(point.y - selection_.top) <= radius && point.x >= selection_.left && point.x <= selection_.right)
    return SelectionAdjustment::Top;
  if (std::abs(point.y - selection_.bottom) <= radius && point.x >= selection_.left && point.x <= selection_.right)
    return SelectionAdjustment::Bottom;
  return Contains(selection_, point) ? SelectionAdjustment::Move : SelectionAdjustment::None;
}

std::optional<size_t> CaptureOverlay::HitTestCommand(POINT point) const {
  if (!Contains(selection_, point)) return std::nullopt;
  const PointF local = ToSelectionPoint(point);
  for (size_t index = document_.Size(); index > 0; --index) {
    const EditCommand* command = document_.At(index - 1);
    if (!command) continue;
    const RectF bounds = CommandBounds(*command);
    if (NearRect(bounds, local, 8.0f) && HitCommandGeometry(*command, local)) return index - 1;
  }
  return std::nullopt;
}

RectF CaptureOverlay::SelectedCommandBounds() const {
  if (!selectedCommand_) return {};
  const EditCommand* command = document_.At(*selectedCommand_);
  return command ? CommandBounds(*command) : RectF{};
}

CaptureOverlay::SelectionAdjustment CaptureOverlay::HitTestCommandAdjustment(POINT point) const {
  if (!selectedCommand_) return SelectionAdjustment::None;
  const RectF bounds = SelectedCommandBounds();
  if (bounds.right <= bounds.left || bounds.bottom <= bounds.top) return SelectionAdjustment::None;
  const PointF local = ToSelectionPoint(point);
  constexpr float radius = 9.0f;
  const auto isNearCommand = [&](float x, float y) {
    return std::abs(local.x - x) <= radius && std::abs(local.y - y) <= radius;
  };
  if (isNearCommand(bounds.left, bounds.top)) return SelectionAdjustment::TopLeft;
  if (isNearCommand(bounds.right, bounds.top)) return SelectionAdjustment::TopRight;
  if (isNearCommand(bounds.left, bounds.bottom)) return SelectionAdjustment::BottomLeft;
  if (isNearCommand(bounds.right, bounds.bottom)) return SelectionAdjustment::BottomRight;
  if (std::abs(local.x - bounds.left) <= radius && local.y >= bounds.top && local.y <= bounds.bottom)
    return SelectionAdjustment::Left;
  if (std::abs(local.x - bounds.right) <= radius && local.y >= bounds.top && local.y <= bounds.bottom)
    return SelectionAdjustment::Right;
  if (std::abs(local.y - bounds.top) <= radius && local.x >= bounds.left && local.x <= bounds.right)
    return SelectionAdjustment::Top;
  if (std::abs(local.y - bounds.bottom) <= radius && local.x >= bounds.left && local.x <= bounds.right)
    return SelectionAdjustment::Bottom;
  return Contains(bounds, local) ? SelectionAdjustment::Move : SelectionAdjustment::None;
}

void CaptureOverlay::ContinueCommandAdjustment(POINT point) {
  if (!selectedCommand_ || !commandBeforeAdjust_) return;
  const int dx = point.x - dragStart_.x;
  const int dy = point.y - dragStart_.y;
  RectF next = selectedCommandBeforeBounds_;
  const bool left = commandAdjustment_ == SelectionAdjustment::Left ||
                    commandAdjustment_ == SelectionAdjustment::TopLeft ||
                    commandAdjustment_ == SelectionAdjustment::BottomLeft;
  const bool right = commandAdjustment_ == SelectionAdjustment::Right ||
                     commandAdjustment_ == SelectionAdjustment::TopRight ||
                     commandAdjustment_ == SelectionAdjustment::BottomRight;
  const bool top = commandAdjustment_ == SelectionAdjustment::Top ||
                   commandAdjustment_ == SelectionAdjustment::TopLeft ||
                   commandAdjustment_ == SelectionAdjustment::TopRight;
  const bool bottom = commandAdjustment_ == SelectionAdjustment::Bottom ||
                      commandAdjustment_ == SelectionAdjustment::BottomLeft ||
                      commandAdjustment_ == SelectionAdjustment::BottomRight;
  if (commandAdjustment_ == SelectionAdjustment::Move) {
    const float width = next.right - next.left;
    const float height = next.bottom - next.top;
    next.left = std::clamp(selectedCommandBeforeBounds_.left + static_cast<float>(dx), 0.0f,
                           static_cast<float>(selection_.right - selection_.left) - width);
    next.top = std::clamp(selectedCommandBeforeBounds_.top + static_cast<float>(dy), 0.0f,
                          static_cast<float>(selection_.bottom - selection_.top) - height);
    next.right = next.left + width;
    next.bottom = next.top + height;
  } else {
    const float width = static_cast<float>(selection_.right - selection_.left);
    const float height = static_cast<float>(selection_.bottom - selection_.top);
    if (left) next.left = std::clamp(selectedCommandBeforeBounds_.left + static_cast<float>(dx), 0.0f,
                                     next.right - 4.0f);
    if (right) next.right = std::clamp(selectedCommandBeforeBounds_.right + static_cast<float>(dx),
                                       next.left + 4.0f, width);
    if (top) next.top = std::clamp(selectedCommandBeforeBounds_.top + static_cast<float>(dy), 0.0f,
                                   next.bottom - 4.0f);
    if (bottom) next.bottom = std::clamp(selectedCommandBeforeBounds_.bottom + static_cast<float>(dy),
                                         next.top + 4.0f, height);
  }
  EditCommand updated = *commandBeforeAdjust_;
  TransformCommand(updated, selectedCommandBeforeBounds_, next);
  document_.Replace(*selectedCommand_, std::move(updated));
}

void CaptureOverlay::ContinueSelectionAdjustment(POINT point) {
  const int dx = point.x - dragStart_.x, dy = point.y - dragStart_.y;
  RECT next = selectionBeforeAdjust_;
  const bool left = selectionAdjustment_ == SelectionAdjustment::Left ||
                    selectionAdjustment_ == SelectionAdjustment::TopLeft ||
                    selectionAdjustment_ == SelectionAdjustment::BottomLeft;
  const bool right = selectionAdjustment_ == SelectionAdjustment::Right ||
                     selectionAdjustment_ == SelectionAdjustment::TopRight ||
                     selectionAdjustment_ == SelectionAdjustment::BottomRight;
  const bool top = selectionAdjustment_ == SelectionAdjustment::Top ||
                   selectionAdjustment_ == SelectionAdjustment::TopLeft ||
                   selectionAdjustment_ == SelectionAdjustment::TopRight;
  const bool bottom = selectionAdjustment_ == SelectionAdjustment::Bottom ||
                      selectionAdjustment_ == SelectionAdjustment::BottomLeft ||
                      selectionAdjustment_ == SelectionAdjustment::BottomRight;
  if (selectionAdjustment_ == SelectionAdjustment::Move) {
    const int width = next.right - next.left, height = next.bottom - next.top;
    next.left = std::clamp(selectionBeforeAdjust_.left + dx, 0L,
                           static_cast<LONG>(snapshot_.width - width));
    next.top = std::clamp(selectionBeforeAdjust_.top + dy, 0L,
                          static_cast<LONG>(snapshot_.height - height));
    next.right = next.left + width; next.bottom = next.top + height;
  } else {
    if (left) next.left = std::clamp(selectionBeforeAdjust_.left + dx, 0L, next.right - 4);
    if (right) next.right = std::clamp(selectionBeforeAdjust_.right + dx, next.left + 4,
                                       static_cast<LONG>(snapshot_.width));
    if (top) next.top = std::clamp(selectionBeforeAdjust_.top + dy, 0L, next.bottom - 4);
    if (bottom) next.bottom = std::clamp(selectionBeforeAdjust_.bottom + dy, next.top + 4,
                                         static_cast<LONG>(snapshot_.height));
  }
  selection_ = next;
}

void CaptureOverlay::DrawSelectionHandles() {
  ComPtr<ID2D1SolidColorBrush> fill, border;
  renderTarget_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &fill);
  renderTarget_->CreateSolidColorBrush(D2D1::ColorF(.1f, .55f, 1.0f, 1), &border);
  const std::array<POINT, 8> points{{
      {selection_.left, selection_.top}, {(selection_.left + selection_.right) / 2, selection_.top},
      {selection_.right, selection_.top}, {selection_.right, (selection_.top + selection_.bottom) / 2},
      {selection_.right, selection_.bottom}, {(selection_.left + selection_.right) / 2, selection_.bottom},
      {selection_.left, selection_.bottom}, {selection_.left, (selection_.top + selection_.bottom) / 2}}};
  for (POINT point : points) {
    const D2D1_RECT_F handle = D2D1::RectF(static_cast<float>(point.x - 4), static_cast<float>(point.y - 4),
                                           static_cast<float>(point.x + 4), static_cast<float>(point.y + 4));
    renderTarget_->FillRectangle(handle, fill.Get()); renderTarget_->DrawRectangle(handle, border.Get(), 1.0f);
  }
}

void CaptureOverlay::DrawCommandHandles() {
  const RectF bounds = SelectedCommandBounds();
  if (bounds.right <= bounds.left || bounds.bottom <= bounds.top) return;
  const RECT screenBounds{
      static_cast<LONG>(std::lround(selection_.left + bounds.left)),
      static_cast<LONG>(std::lround(selection_.top + bounds.top)),
      static_cast<LONG>(std::lround(selection_.left + bounds.right)),
      static_cast<LONG>(std::lround(selection_.top + bounds.bottom))};
  ComPtr<ID2D1SolidColorBrush> border, fill;
  renderTarget_->CreateSolidColorBrush(D2D1::ColorF(.20f, .75f, 1.0f, .95f), &border);
  renderTarget_->CreateSolidColorBrush(D2D1::ColorF(.95f, .98f, 1.0f, 1.0f), &fill);
  ComPtr<ID2D1StrokeStyle> dashed;
  if (d2dFactory_) {
    d2dFactory_->CreateStrokeStyle(
        D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                                    D2D1_CAP_STYLE_FLAT, D2D1_LINE_JOIN_MITER,
                                    10.0f, D2D1_DASH_STYLE_DASH, 0.0f), nullptr, 0, &dashed);
  }
  renderTarget_->DrawRectangle(ToD2D(screenBounds), border.Get(), 1.5f, dashed.Get());
  const std::array<POINT, 8> points{{
      {screenBounds.left, screenBounds.top},
      {(screenBounds.left + screenBounds.right) / 2, screenBounds.top},
      {screenBounds.right, screenBounds.top},
      {screenBounds.right, (screenBounds.top + screenBounds.bottom) / 2},
      {screenBounds.right, screenBounds.bottom},
      {(screenBounds.left + screenBounds.right) / 2, screenBounds.bottom},
      {screenBounds.left, screenBounds.bottom},
      {screenBounds.left, (screenBounds.top + screenBounds.bottom) / 2}}};
  for (const POINT point : points) {
    const D2D1_RECT_F handle = D2D1::RectF(static_cast<float>(point.x - 4),
                                           static_cast<float>(point.y - 4),
                                           static_cast<float>(point.x + 4),
                                           static_cast<float>(point.y + 4));
    renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(handle, 2, 2), fill.Get());
    renderTarget_->DrawRoundedRectangle(D2D1::RoundedRect(handle, 2, 2), border.Get(), 1.0f);
  }
}

void CaptureOverlay::DrawToolIcon(Tool tool, const RECT& rect, bool active) {
  const D2D1_COLOR_F color = active ? D2D1::ColorF(.98f, .99f, 1.0f, 1.0f)
                                    : D2D1::ColorF(.72f, .78f, .88f, 1.0f);
  ComPtr<ID2D1SolidColorBrush> brush;
  renderTarget_->CreateSolidColorBrush(color, &brush);
  const float cx = (rect.left + rect.right) * 0.5f;
  const float cy = (rect.top + rect.bottom) * 0.5f;
  const float left = static_cast<float>(rect.left);
  const float top = static_cast<float>(rect.top);
  const float right = static_cast<float>(rect.right);
  const float bottom = static_cast<float>(rect.bottom);
  const auto line = [&](D2D1_POINT_2F a, D2D1_POINT_2F b, float width = 2.2f) {
    renderTarget_->DrawLine(a, b, brush.Get(), width);
  };
  switch (tool) {
    case Tool::Pen:
      line({left + 10, bottom - 11}, {right - 9, top + 10}, 3.0f);
      renderTarget_->FillEllipse({{left + 10, bottom - 11}, 2.4f, 2.4f}, brush.Get());
      break;
    case Tool::Rectangle:
      renderTarget_->DrawRoundedRectangle(D2D1::RoundedRect(
          D2D1::RectF(left + 9, top + 9, right - 9, bottom - 9), 2, 2), brush.Get(), 2.2f);
      break;
    case Tool::Ellipse:
      renderTarget_->DrawEllipse({{cx, cy}, 10.0f, 8.0f}, brush.Get(), 2.2f);
      break;
    case Tool::Line:
      line({left + 9, bottom - 10}, {right - 9, top + 10}, 2.4f);
      break;
    case Tool::Arrow:
      line({left + 9, bottom - 10}, {right - 10, top + 10}, 2.4f);
      line({right - 10, top + 10}, {right - 18, top + 11}, 2.4f);
      line({right - 10, top + 10}, {right - 11, top + 18}, 2.4f);
      break;
    case Tool::Text:
      DrawText(L"T", ToD2D(rect), 19, color);
      break;
    case Tool::MosaicBrush:
      for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
          if ((row + column) % 2 == 0) {
            renderTarget_->FillRectangle(D2D1::RectF(left + 9 + column * 7, top + 9 + row * 7,
                                                     left + 15 + column * 7, top + 15 + row * 7), brush.Get());
          }
        }
      }
      break;
    case Tool::MosaicRectangle:
      renderTarget_->DrawRectangle(D2D1::RectF(left + 8, top + 9, right - 8, bottom - 9), brush.Get(), 2.0f);
      for (int row = 0; row < 2; ++row) {
        for (int column = 0; column < 3; ++column) {
          if ((row + column) % 2 == 0) {
            renderTarget_->FillRectangle(D2D1::RectF(left + 11 + column * 6, top + 12 + row * 6,
                                                     left + 16 + column * 6, top + 17 + row * 6), brush.Get());
          }
        }
      }
      break;
    case Tool::Select:
      line({left + 11, top + 8}, {left + 18, bottom - 8}, 2.2f);
      line({left + 11, top + 8}, {right - 9, top + 15}, 2.2f);
      line({left + 18, bottom - 8}, {left + 21, bottom - 18}, 2.2f);
      line({left + 21, bottom - 18}, {right - 9, bottom - 10}, 2.2f);
      line({right - 9, bottom - 10}, {right - 18, bottom - 20}, 2.2f);
      break;
    case Tool::Frame:
      renderTarget_->DrawRectangle(D2D1::RectF(left + 8, top + 8, right - 8, bottom - 8), brush.Get(), 2.0f);
      break;
  }
}

void CaptureOverlay::DrawActionIcon(bool save, const RECT& rect) {
  ComPtr<ID2D1SolidColorBrush> brush;
  renderTarget_->CreateSolidColorBrush(D2D1::ColorF(.94f, .98f, 1.0f, 1.0f), &brush);
  const float left = static_cast<float>(rect.left);
  const float top = static_cast<float>(rect.top);
  const float right = static_cast<float>(rect.right);
  const float bottom = static_cast<float>(rect.bottom);
  if (!save) {
    renderTarget_->DrawRoundedRectangle(D2D1::RoundedRect(
        D2D1::RectF(left + 15, top + 13, right - 10, bottom - 10), 2, 2), brush.Get(), 2.0f);
    renderTarget_->DrawRoundedRectangle(D2D1::RoundedRect(
        D2D1::RectF(left + 10, top + 9, right - 15, bottom - 14), 2, 2), brush.Get(), 2.0f);
    return;
  }
  renderTarget_->DrawRoundedRectangle(D2D1::RoundedRect(
      D2D1::RectF(left + 11, top + 9, right - 11, bottom - 9), 2, 2), brush.Get(), 2.0f);
  renderTarget_->DrawLine({left + 16, top + 10}, {left + 16, top + 18}, brush.Get(), 2.0f);
  renderTarget_->DrawLine({right - 16, top + 10}, {right - 16, top + 18}, brush.Get(), 2.0f);
  renderTarget_->DrawLine({left + 16, top + 22}, {right - 16, top + 22}, brush.Get(), 2.0f);
}

void CaptureOverlay::DrawPropertyIcon(PropertyAction action, const RECT& rect) {
  ComPtr<ID2D1SolidColorBrush> brush;
  renderTarget_->CreateSolidColorBrush(D2D1::ColorF(.76f, .83f, .94f, 1.0f), &brush);
  const float left = static_cast<float>(rect.left);
  const float top = static_cast<float>(rect.top);
  const float right = static_cast<float>(rect.right);
  const float bottom = static_cast<float>(rect.bottom);
  const float cx = (left + right) * .5f;
  const float cy = (top + bottom) * .5f;
  const auto line = [&](D2D1_POINT_2F a, D2D1_POINT_2F b, float width = 2.0f) {
    renderTarget_->DrawLine(a, b, brush.Get(), width);
  };
  switch (action) {
    case PropertyAction::SizeDown:
      line({left + 12, cy}, {right - 12, cy});
      break;
    case PropertyAction::SizeUp:
      line({left + 12, cy}, {right - 12, cy});
      line({cx, top + 11}, {cx, bottom - 11});
      break;
    case PropertyAction::Color:
      renderTarget_->FillEllipse({{cx, cy}, 8, 8}, brush.Get());
      renderTarget_->DrawEllipse({{cx, cy}, 10, 10}, brush.Get(), 1.5f);
      break;
    case PropertyAction::Opacity:
      renderTarget_->DrawEllipse({{cx, cy}, 9, 9}, brush.Get(), 2.0f);
      line({left + 10, bottom - 11}, {right - 10, top + 11}, 2.0f);
      break;
    case PropertyAction::FillColor:
      renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(
          D2D1::RectF(left + 10, top + 10, right - 10, bottom - 10), 2, 2), brush.Get());
      break;
    case PropertyAction::FillOpacity:
      renderTarget_->DrawRoundedRectangle(D2D1::RoundedRect(
          D2D1::RectF(left + 10, top + 10, right - 10, bottom - 10), 2, 2), brush.Get(), 2.0f);
      line({left + 11, bottom - 11}, {right - 11, top + 11}, 1.7f);
      break;
    case PropertyAction::MosaicStyle:
      for (int i = 0; i < 3; ++i) {
        line({left + 10.0f + i * 6.0f, top + 10}, {left + 10.0f + i * 6.0f, bottom - 10}, 1.5f);
        line({left + 10, top + 10.0f + i * 6.0f}, {right - 10, top + 10.0f + i * 6.0f}, 1.5f);
      }
      break;
    case PropertyAction::MosaicStrengthDown:
      line({left + 11, cy}, {right - 11, cy});
      break;
    case PropertyAction::MosaicStrengthUp:
      line({left + 11, cy}, {right - 11, cy});
      line({cx, top + 11}, {cx, bottom - 11});
      break;
    case PropertyAction::FrameToggle:
      renderTarget_->DrawRectangle(D2D1::RectF(left + 9, top + 9, right - 9, bottom - 9), brush.Get(), 2.0f);
      break;
    case PropertyAction::TextOrientation:
      DrawText(L"T", ToD2D(rect), 15, D2D1::ColorF(.76f, .83f, .94f, 1.0f));
      break;
  }
}

RECT CaptureOverlay::ToolbarRect() const {
  const int width = kToolbarWidth;
  const int safeWidth = std::max(1, snapshot_.width);
  const int safeHeight = std::max(1, snapshot_.height);
  const int visibleWidth = std::min(width, std::max(1, safeWidth - 2 * kToolbarMargin));
  const int visibleHeight = std::min(kToolbarHeight, std::max(1, safeHeight - 2 * kToolbarMargin));
  const int maxLeft = std::max(kToolbarMargin, safeWidth - visibleWidth - kToolbarMargin);
  int left = std::clamp(selection_.left, static_cast<LONG>(kToolbarMargin), static_cast<LONG>(maxLeft));
  int top = selection_.bottom + kToolbarMargin;
  if (top + visibleHeight > safeHeight - kToolbarMargin) top = selection_.top - visibleHeight - kToolbarMargin;
  top = std::clamp(top, kToolbarMargin, std::max(kToolbarMargin, safeHeight - visibleHeight - kToolbarMargin));
  return {left, top, left + visibleWidth, top + visibleHeight};
}

void CaptureOverlay::DrawToolbar() {
  const RECT toolbar = ToolbarRect();
  ComPtr<ID2D1SolidColorBrush> shadow, background;
  renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, .28f), &shadow);
  renderTarget_->CreateSolidColorBrush(D2D1::ColorF(.055f, .07f, .10f, .98f), &background);
  RECT shadowRect = toolbar;
  OffsetRect(&shadowRect, 0, 4);
  renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(ToD2D(shadowRect), 12, 12), shadow.Get());
  renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(ToD2D(toolbar), 12, 12), background.Get());

  int x = toolbar.left + kToolbarMargin;
  for (const auto& button : toolButtons_) {
    const RECT item{x, toolbar.top + kToolbarMargin, x + kToolbarToolSize,
                    toolbar.top + kToolbarMargin + kToolbarToolSize};
    ComPtr<ID2D1SolidColorBrush> buttonBackground;
    renderTarget_->CreateSolidColorBrush(button.tool == tool_
                                             ? D2D1::ColorF(.12f, .55f, .95f, .95f)
                                             : D2D1::ColorF(.11f, .14f, .19f, .95f),
                                         &buttonBackground);
    renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(ToD2D(item), 9, 9), buttonBackground.Get());
    DrawToolIcon(button.tool, item, button.tool == tool_);
    x += kToolbarToolSize + kToolbarToolGap;
  }
  const RECT copy{x + kToolbarToolGap, toolbar.top + kToolbarMargin,
                  x + kToolbarToolGap + kToolbarActionSize, toolbar.top + kToolbarMargin + kToolbarToolSize};
  const RECT save{copy.right + kToolbarToolGap, copy.top,
                  copy.right + kToolbarToolGap + kToolbarActionSize, copy.bottom};
  ComPtr<ID2D1SolidColorBrush> copyBackground, saveBackground;
  renderTarget_->CreateSolidColorBrush(D2D1::ColorF(.10f, .33f, .25f, .98f), &copyBackground);
  renderTarget_->CreateSolidColorBrush(D2D1::ColorF(.10f, .28f, .48f, .98f), &saveBackground);
  renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(ToD2D(copy), 9, 9), copyBackground.Get());
  renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(ToD2D(save), 9, 9), saveBackground.Get());
  DrawActionIcon(false, copy);
  DrawActionIcon(true, save);

  if (tool_ != Tool::Select) {
    const RECT slider = SizeSliderRect();
    ComPtr<ID2D1SolidColorBrush> track, knob, sizeIcon;
    renderTarget_->CreateSolidColorBrush(D2D1::ColorF(.27f, .31f, .39f, 1), &track);
    renderTarget_->CreateSolidColorBrush(D2D1::ColorF(.22f, .67f, 1.0f, 1), &knob);
    renderTarget_->CreateSolidColorBrush(D2D1::ColorF(.70f, .76f, .85f, 1), &sizeIcon);
    const float minimum = tool_ == Tool::Text ? 8.0f :
                          (tool_ == Tool::MosaicBrush || tool_ == Tool::MosaicRectangle ? 4.0f : 1.0f);
    const float maximum = tool_ == Tool::MosaicBrush || tool_ == Tool::MosaicRectangle ? 256.0f :
                          (tool_ == Tool::Text ? 256.0f : 128.0f);
    const float ratio = std::clamp((ActiveSize() - minimum) / (maximum - minimum), 0.0f, 1.0f);
    const float centerY = (slider.top + slider.bottom) / 2.0f;
    renderTarget_->FillEllipse({{static_cast<float>(toolbar.left + 24), centerY}, 3.0f, 3.0f}, sizeIcon.Get());
    renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(
        D2D1::RectF(static_cast<float>(slider.left), centerY - 2,
                    static_cast<float>(slider.right), centerY + 2), 2, 2), track.Get());
    const float knobX = slider.left + ratio * (slider.right - slider.left);
    renderTarget_->FillEllipse({{knobX, centerY}, 7, 7}, knob.Get());
    DrawText(std::to_wstring(static_cast<int>(std::lround(ActiveSize()))),
             D2D1::RectF(static_cast<float>(slider.right + 5), static_cast<float>(slider.top - 5),
                         static_cast<float>(slider.right + 34), static_cast<float>(slider.bottom + 5)),
             10, D2D1::ColorF(.86f, .90f, .96f, 1));

    if (ActiveColor()) {
      for (size_t i = 0; i < kPresetColors.size(); ++i) {
        const int swatchX = toolbar.left + 190 + static_cast<int>(i) * 22;
        const RECT swatch{swatchX, toolbar.top + 54, swatchX + 18, toolbar.top + 72};
        ComPtr<ID2D1SolidColorBrush> colorBrush, outline;
        renderTarget_->CreateSolidColorBrush(ColorFromSetting(ColorSetting{kPresetColors[i]}), &colorBrush);
        renderTarget_->CreateSolidColorBrush(D2D1::ColorF(.78f, .84f, .94f, 1), &outline);
        renderTarget_->FillEllipse({{(swatch.left + swatch.right) / 2.0f,
                                     (swatch.top + swatch.bottom) / 2.0f}, 8, 8}, colorBrush.Get());
        if ((ActiveColor()->rgba & 0xFFFFFF00u) == (kPresetColors[i] & 0xFFFFFF00u))
          renderTarget_->DrawEllipse({{(swatch.left + swatch.right) / 2.0f,
                                       (swatch.top + swatch.bottom) / 2.0f}, 9, 9}, outline.Get(), 2);
      }
    }
  }

  ComPtr<ID2D1SolidColorBrush> propertyBackground;
  renderTarget_->CreateSolidColorBrush(D2D1::ColorF(.11f, .14f, .19f, .98f), &propertyBackground);
  for (const PropertyButton& button : PropertyButtons()) {
    renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(ToD2D(button.rect), 8, 8), propertyBackground.Get());
    DrawPropertyIcon(button.action, button.rect);
  }
}

void CaptureOverlay::DrawText(std::wstring_view text, const D2D1_RECT_F& rect, float size,
                              D2D1_COLOR_F color, DWRITE_TEXT_ALIGNMENT alignment) {
  ComPtr<IDWriteTextFormat> format;
  if (FAILED(dwriteFactory_->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"zh-CN", &format))) return;
  format->SetTextAlignment(alignment); format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
  ComPtr<ID2D1SolidColorBrush> brush; renderTarget_->CreateSolidColorBrush(color, &brush);
  renderTarget_->DrawTextW(text.data(), static_cast<UINT32>(text.size()), format.Get(), rect, brush.Get());
}

std::optional<Tool> CaptureOverlay::HitTestTool(POINT point) const {
  const RECT toolbar = ToolbarRect();
  int x = toolbar.left + kToolbarMargin;
  for (const auto& button : toolButtons_) {
    RECT item{x, toolbar.top + kToolbarMargin, x + kToolbarToolSize,
              toolbar.top + kToolbarMargin + kToolbarToolSize};
    if (Contains(item, point)) return button.tool;
    x += kToolbarToolSize + kToolbarToolGap;
  }
  return std::nullopt;
}

std::vector<CaptureOverlay::PropertyButton> CaptureOverlay::PropertyButtons() const {
  const RECT toolbar = ToolbarRect();
  int x = toolbar.left + 394;
  const int top = toolbar.top + 52;
  const int bottom = toolbar.bottom - 8;
  std::vector<PropertyButton> result;
  auto add = [&](PropertyAction action, int width, std::wstring label) {
    result.push_back({action, {x, top, x + width, bottom}, std::move(label)});
    x += width + 6;
  };
  if (tool_ == Tool::Select) return result;
  if (tool_ == Tool::MosaicBrush || tool_ == Tool::MosaicRectangle) {
    add(PropertyAction::MosaicStyle, 42,
        config_.mosaicStyle == MosaicStyle::Pixel ? L"样式：像素" : L"样式：模糊");
    add(PropertyAction::MosaicStrengthDown, 42, L"− 强度");
    add(PropertyAction::MosaicStrengthUp, 42, L"＋ 强度");
    return result;
  }
  add(PropertyAction::Color, 42, L"更多颜色");
  add(PropertyAction::Opacity, 42, L"透明度");
  if (tool_ == Tool::Text) {
    add(PropertyAction::TextOrientation, 42, config_.text.vertical ? L"竖排文字" : L"横排文字");
  } else if (tool_ == Tool::Rectangle || tool_ == Tool::Ellipse) {
    add(PropertyAction::FillColor, 42, L"填充颜色");
    add(PropertyAction::FillOpacity, 42, L"填充");
  }
  return result;
}

std::optional<CaptureOverlay::PropertyAction> CaptureOverlay::HitTestProperty(POINT point) const {
  for (const PropertyButton& button : PropertyButtons()) {
    if (Contains(button.rect, point)) return button.action;
  }
  return std::nullopt;
}

void CaptureOverlay::ActivateProperty(PropertyAction action) {
  switch (action) {
    case PropertyAction::SizeDown: AdjustActiveSize(-1.0f); break;
    case PropertyAction::SizeUp: AdjustActiveSize(1.0f); break;
    case PropertyAction::Color: ChooseActiveColor(); break;
    case PropertyAction::Opacity: CycleActiveOpacity(); break;
    case PropertyAction::FillColor: ChooseFillColor(); break;
    case PropertyAction::FillOpacity: CycleFillOpacity(); break;
    case PropertyAction::MosaicStyle:
      config_.mosaicStyle = config_.mosaicStyle == MosaicStyle::Pixel ? MosaicStyle::Blur : MosaicStyle::Pixel;
      if (configChanged_) configChanged_();
      break;
    case PropertyAction::MosaicStrengthDown:
      if (config_.mosaicStyle == MosaicStyle::Pixel)
        config_.mosaicPixelSize = std::clamp(config_.mosaicPixelSize - 2, 2, 128);
      else config_.mosaicBlurRadius = std::clamp(config_.mosaicBlurRadius - 1.0f, 1.0f, 64.0f);
      if (configChanged_) configChanged_();
      break;
    case PropertyAction::MosaicStrengthUp:
      if (config_.mosaicStyle == MosaicStyle::Pixel)
        config_.mosaicPixelSize = std::clamp(config_.mosaicPixelSize + 2, 2, 128);
      else config_.mosaicBlurRadius = std::clamp(config_.mosaicBlurRadius + 1.0f, 1.0f, 64.0f);
      if (configChanged_) configChanged_();
      break;
    case PropertyAction::FrameToggle:
      config_.frameEnabled = !config_.frameEnabled;
      if (configChanged_) configChanged_();
      break;
    case PropertyAction::TextOrientation:
      config_.text.vertical = !config_.text.vertical;
      if (configChanged_) configChanged_();
      break;
  }
  InvalidateRect(hwnd_, nullptr, FALSE);
}

bool CaptureOverlay::HitCopy(POINT point) const {
  const RECT toolbar = ToolbarRect();
  const int toolEnd = toolbar.left + kToolbarMargin + static_cast<int>(toolButtons_.size()) * kToolbarToolSize +
                      static_cast<int>(toolButtons_.size() - 1) * kToolbarToolGap;
  const RECT rect{toolEnd + kToolbarToolGap, toolbar.top + kToolbarMargin,
                  toolEnd + kToolbarToolGap + kToolbarActionSize,
                  toolbar.top + kToolbarMargin + kToolbarToolSize};
  return Contains(rect, point);
}

bool CaptureOverlay::HitSave(POINT point) const {
  const RECT toolbar = ToolbarRect();
  const int toolEnd = toolbar.left + kToolbarMargin + static_cast<int>(toolButtons_.size()) * kToolbarToolSize +
                      static_cast<int>(toolButtons_.size() - 1) * kToolbarToolGap;
  const int left = toolEnd + 2 * kToolbarToolGap + kToolbarActionSize;
  const RECT rect{left, toolbar.top + kToolbarMargin, left + kToolbarActionSize,
                  toolbar.top + kToolbarMargin + kToolbarToolSize};
  return Contains(rect, point);
}

PointF CaptureOverlay::ToSelectionPoint(POINT point) const { return {static_cast<float>(point.x - selection_.left), static_cast<float>(point.y - selection_.top)}; }

StrokeSetting* CaptureOverlay::ActiveStroke() {
  switch (tool_) {
    case Tool::Pen: return &config_.pen;
    case Tool::Rectangle: return &config_.rectangle.stroke;
    case Tool::Ellipse: return &config_.ellipse.stroke;
    case Tool::Line: return &config_.line;
    case Tool::Arrow: return &config_.arrow;
    case Tool::Frame: return &config_.frame;
    default: return nullptr;
  }
}

void CaptureOverlay::AdjustActiveSize(float delta) {
  const float step = tool_ == Tool::MosaicBrush || tool_ == Tool::MosaicRectangle ? 4.0f :
                     tool_ == Tool::Text ? 2.0f : 1.0f;
  SetActiveSize(ActiveSize() + delta * step);
}

void CaptureOverlay::CycleActiveOpacity() {
  if (tool_ == Tool::Text) {
    config_.text.opacity -= .25f;
    if (config_.text.opacity < .24f) config_.text.opacity = 1.0f;
  } else if (StrokeSetting* stroke = ActiveStroke()) {
    stroke->opacity -= .25f; if (stroke->opacity < .24f) stroke->opacity = 1.0f;
  } else return;
  if (configChanged_) configChanged_();
}

void CaptureOverlay::ChooseActiveColor() {
  ColorSetting* color = ActiveColor(); if (!color) return;
  if (ChooseColorFor(hwnd_, *color)) {
    if (configChanged_) configChanged_();
  }
}

void CaptureOverlay::ChooseFillColor() {
  ShapeSetting* shape = tool_ == Tool::Rectangle ? &config_.rectangle
                       : tool_ == Tool::Ellipse ? &config_.ellipse : nullptr;
  if (shape && ChooseColorFor(hwnd_, shape->fill) && configChanged_) configChanged_();
}

void CaptureOverlay::CycleFillOpacity() {
  ShapeSetting* shape = tool_ == Tool::Rectangle ? &config_.rectangle
                       : tool_ == Tool::Ellipse ? &config_.ellipse : nullptr;
  if (!shape) return;
  shape->fillOpacity += .25f;
  if (shape->fillOpacity > 1.0f) shape->fillOpacity = 0.0f;
  if (configChanged_) configChanged_();
}

float CaptureOverlay::ActiveSize() const {
  if (tool_ == Tool::Text) return config_.text.size;
  if (tool_ == Tool::MosaicBrush || tool_ == Tool::MosaicRectangle) return config_.mosaicBrushSize;
  if (const StrokeSetting* stroke = const_cast<CaptureOverlay*>(this)->ActiveStroke()) return stroke->width;
  return 1.0f;
}

void CaptureOverlay::SetActiveSize(float size) {
  if (tool_ == Tool::Text) config_.text.size = std::clamp(size, 8.0f, 256.0f);
  else if (tool_ == Tool::MosaicBrush || tool_ == Tool::MosaicRectangle)
    config_.mosaicBrushSize = std::clamp(size, 4.0f, 256.0f);
  else if (StrokeSetting* stroke = ActiveStroke()) stroke->width = std::clamp(size, 1.0f, 128.0f);
  else return;
  if (configChanged_) configChanged_();
  InvalidateRect(hwnd_, nullptr, FALSE);
}

ColorSetting* CaptureOverlay::ActiveColor() {
  if (tool_ == Tool::Text) return &config_.text.color;
  if (StrokeSetting* stroke = ActiveStroke()) return &stroke->color;
  return nullptr;
}

float CaptureOverlay::ActiveOpacity() const {
  if (tool_ == Tool::Text) return config_.text.opacity;
  if (const StrokeSetting* stroke = const_cast<CaptureOverlay*>(this)->ActiveStroke()) return stroke->opacity;
  return 1.0f;
}

RECT CaptureOverlay::SizeSliderRect() const {
  const RECT toolbar = ToolbarRect();
  return {toolbar.left + 52, toolbar.top + 54, toolbar.left + 178, toolbar.top + 76};
}

void CaptureOverlay::SetSizeFromSlider(POINT point) {
  const RECT slider = SizeSliderRect();
  const float ratio = std::clamp((point.x - slider.left) /
                                 static_cast<float>(slider.right - slider.left), 0.0f, 1.0f);
  const float minimum = tool_ == Tool::Text ? 8.0f :
                        (tool_ == Tool::MosaicBrush || tool_ == Tool::MosaicRectangle ? 4.0f : 1.0f);
  const float maximum = tool_ == Tool::MosaicBrush || tool_ == Tool::MosaicRectangle ? 256.0f :
                        (tool_ == Tool::Text ? 256.0f : 128.0f);
  SetActiveSize(std::round(minimum + ratio * (maximum - minimum)));
}

std::optional<size_t> CaptureOverlay::HitTestColorPreset(POINT point) const {
  if (!const_cast<CaptureOverlay*>(this)->ActiveColor()) return std::nullopt;
  const RECT toolbar = ToolbarRect();
  for (size_t i = 0; i < kPresetColors.size(); ++i) {
    const int x = toolbar.left + 190 + static_cast<int>(i) * 22;
    if (Contains(RECT{x, toolbar.top + 52, x + 20, toolbar.top + 74}, point)) return i;
  }
  return std::nullopt;
}

void CaptureOverlay::SetActivePresetColor(size_t index) {
  ColorSetting* color = ActiveColor();
  if (!color || index >= kPresetColors.size()) return;
  color->rgba = kPresetColors[index];
  if (configChanged_) configChanged_();
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void CaptureOverlay::ShowEditorContextMenu(POINT point) {
  HMENU menu = CreatePopupMenu();
  HMENU tools = CreatePopupMenu();
  HMENU colors = CreatePopupMenu();
  HMENU sizes = CreatePopupMenu();
  HMENU opacity = CreatePopupMenu();
  if (!menu || !tools || !colors || !sizes || !opacity) return;
  ScopeExit cleanup{[&] { DestroyMenu(menu); }};
  for (size_t i = 0; i < kContextTools.size(); ++i) {
    AppendMenuW(tools, MF_STRING | (tool_ == kContextTools[i] ? MF_CHECKED : 0),
                kContextToolBase + static_cast<UINT>(i), kContextToolNames[i]);
  }
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(tools), L"工具");
  if (ActiveColor()) {
    for (size_t i = 0; i < kPresetColors.size(); ++i)
      AppendMenuW(colors, MF_STRING, kContextColorBase + static_cast<UINT>(i), kPresetNames[i]);
    AppendMenuW(colors, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(colors, MF_STRING, kContextMoreColor, L"更多颜色…");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(colors), L"颜色");
  }
  for (size_t i = 0; i < kContextSizes.size(); ++i) {
    const std::wstring label = std::to_wstring(static_cast<int>(kContextSizes[i])) + L" px";
    AppendMenuW(sizes, MF_STRING, kContextSizeBase + static_cast<UINT>(i), label.c_str());
  }
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sizes), L"尺寸");
  for (size_t i = 0; i < kContextOpacities.size(); ++i) {
    const std::wstring label = std::to_wstring(static_cast<int>(kContextOpacities[i] * 100)) + L"%";
    AppendMenuW(opacity, MF_STRING, kContextOpacityBase + static_cast<UINT>(i), label.c_str());
  }
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(opacity), L"透明度");
  if (tool_ == Tool::Rectangle || tool_ == Tool::Ellipse) {
    HMENU fill = CreatePopupMenu();
    AppendMenuW(fill, MF_STRING, kContextFillOpacityBase, L"无填充");
    for (size_t i = 0; i < kContextOpacities.size(); ++i) {
      const std::wstring label = std::to_wstring(static_cast<int>(kContextOpacities[i] * 100)) + L"%";
      AppendMenuW(fill, MF_STRING, kContextFillOpacityBase + 1 + static_cast<UINT>(i), label.c_str());
    }
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(fill), L"填充透明度");
  }
  if (tool_ == Tool::MosaicBrush || tool_ == Tool::MosaicRectangle) {
    HMENU style = CreatePopupMenu();
    AppendMenuW(style, MF_STRING | (config_.mosaicStyle == MosaicStyle::Pixel ? MF_CHECKED : 0),
                kContextMosaicPixel, L"像素化");
    AppendMenuW(style, MF_STRING | (config_.mosaicStyle == MosaicStyle::Blur ? MF_CHECKED : 0),
                kContextMosaicBlur, L"高斯模糊");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(style), L"马赛克样式");
  }
  if (tool_ == Tool::Text) {
    HMENU orientation = CreatePopupMenu();
    AppendMenuW(orientation, MF_STRING | (!config_.text.vertical ? MF_CHECKED : 0),
                kContextTextHorizontal, L"横排");
    AppendMenuW(orientation, MF_STRING | (config_.text.vertical ? MF_CHECKED : 0),
                kContextTextVertical, L"竖排");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(orientation), L"文字方向");
  }
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING | (document_.CanUndo() ? 0 : MF_GRAYED), kContextUndo, L"撤销\tCtrl+Z");
  AppendMenuW(menu, MF_STRING | (document_.CanRedo() ? 0 : MF_GRAYED), kContextRedo, L"重做\tCtrl+Y");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, kContextCopy, L"复制\tCtrl+C");
  AppendMenuW(menu, MF_STRING, kContextSave, L"保存\tCtrl+S");
  AppendMenuW(menu, MF_STRING, kContextCancel, L"取消截图\tEsc");

  POINT screen = point;
  ClientToScreen(hwnd_, &screen);
  SetForegroundWindow(hwnd_);
  const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                      screen.x, screen.y, 0, hwnd_, nullptr);
  PostMessageW(hwnd_, WM_NULL, 0, 0);
  if (command >= kContextToolBase && command < kContextToolBase + kContextTools.size())
    SelectTool(kContextTools[command - kContextToolBase]);
  else if (command >= kContextColorBase && command < kContextColorBase + kPresetColors.size())
    SetActivePresetColor(command - kContextColorBase);
  else if (command == kContextMoreColor) ChooseActiveColor();
  else if (command >= kContextSizeBase && command < kContextSizeBase + kContextSizes.size())
    SetActiveSize(kContextSizes[command - kContextSizeBase]);
  else if (command >= kContextOpacityBase && command < kContextOpacityBase + kContextOpacities.size()) {
    const float value = kContextOpacities[command - kContextOpacityBase];
    if (tool_ == Tool::Text) config_.text.opacity = value;
    else if (StrokeSetting* stroke = ActiveStroke()) stroke->opacity = value;
    if (configChanged_) configChanged_();
  } else if (command >= kContextFillOpacityBase && command <= kContextFillOpacityBase + kContextOpacities.size()) {
    ShapeSetting& shape = tool_ == Tool::Ellipse ? config_.ellipse : config_.rectangle;
    shape.fillOpacity = command == kContextFillOpacityBase ? 0.0f :
        kContextOpacities[command - kContextFillOpacityBase - 1];
    if (configChanged_) configChanged_();
  } else if (command == kContextMosaicPixel || command == kContextMosaicBlur) {
    config_.mosaicStyle = command == kContextMosaicPixel ? MosaicStyle::Pixel : MosaicStyle::Blur;
    if (configChanged_) configChanged_();
  } else if (command == kContextTextHorizontal || command == kContextTextVertical) {
    config_.text.vertical = command == kContextTextVertical;
    if (configChanged_) configChanged_();
  } else if (command == kContextUndo) document_.Undo();
  else if (command == kContextRedo) document_.Redo();
  else if (command == kContextCopy) Complete(CaptureCompletion::Copy);
  else if (command == kContextSave) Complete(CaptureCompletion::Save);
  else if (command == kContextCancel) Cancel();
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void CaptureOverlay::BeginTextInput(POINT point, std::optional<size_t> existingCommand) {
  if (textEdit_) CommitTextInput();
  textEditingCommand_ = existingCommand;
  std::wstring initialValue;
  if (existingCommand) {
    const EditCommand* command = document_.At(*existingCommand);
    const auto* text = command ? std::get_if<TextCommand>(command) : nullptr;
    if (!text) {
      textEditingCommand_.reset();
    } else {
      textOrigin_ = text->origin;
      textInputStyle_ = text->style;
      initialValue = text->text;
      point = {selection_.left + static_cast<LONG>(std::lround(text->origin.x)),
               selection_.top + static_cast<LONG>(std::lround(text->origin.y))};
    }
  } else {
    textOrigin_ = ToSelectionPoint(point);
    textInputStyle_ = config_.text;
  }
  const int availableWidth = std::max(1, static_cast<int>(selection_.right - point.x));
  const int width = std::min(520, std::max(160, availableWidth));
  const int lineHeight = std::max(28, static_cast<int>(std::lround(textInputStyle_.size * 1.45f)));
  const int availableHeight = std::max(1, static_cast<int>(selection_.bottom - point.y));
  const int height = std::min(180, std::max(lineHeight + 8, availableHeight));
  const int selectionLeft = static_cast<int>(selection_.left);
  const int selectionTop = static_cast<int>(selection_.top);
  const int selectionRight = static_cast<int>(selection_.right);
  const int selectionBottom = static_cast<int>(selection_.bottom);
  const int x = std::clamp(static_cast<int>(point.x), selectionLeft, std::max(selectionLeft, selectionRight - width));
  const int y = std::clamp(static_cast<int>(point.y), selectionTop, std::max(selectionTop, selectionBottom - height));
  textEdit_ = CreateWindowExW(WS_EX_TRANSPARENT, L"EDIT", L"",
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
      x, y, width, height, hwnd_, nullptr, instance_, nullptr);
  if (!textEdit_) return;
  LOGFONTW font{};
  font.lfHeight = -std::max(8L, static_cast<LONG>(std::lround(textInputStyle_.size)));
  font.lfWeight = FW_NORMAL;
  font.lfQuality = CLEARTYPE_QUALITY;
  wcsncpy_s(font.lfFaceName, textInputStyle_.fontFamily.c_str(), _TRUNCATE);
  textEditFont_ = CreateFontIndirectW(&font);
  SendMessageW(textEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(textEditFont_), TRUE);
  if (!initialValue.empty()) SetWindowTextW(textEdit_, initialValue.c_str());
  SetWindowSubclass(textEdit_, TextEditProc, 1, reinterpret_cast<DWORD_PTR>(this));
  SetFocus(textEdit_);
  SendMessageW(textEdit_, EM_SETSEL, 0, -1);
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void CaptureOverlay::CommitTextInput() {
  if (!textEdit_) return;
  const int length = GetWindowTextLengthW(textEdit_);
  std::vector<wchar_t> buffer(static_cast<size_t>(length + 1));
  GetWindowTextW(textEdit_, buffer.data(), length + 1);
  const std::wstring value(buffer.data(), static_cast<size_t>(length));
  HWND edit = textEdit_;
  RemoveWindowSubclass(edit, TextEditProc, 1);
  textEdit_ = nullptr;
  DestroyWindow(edit);
  if (textEditingCommand_) {
    if (!value.empty()) document_.Replace(*textEditingCommand_, TextCommand{textOrigin_, value, textInputStyle_});
    else document_.Remove(*textEditingCommand_);
    if (value.empty()) selectedCommand_.reset();
    else selectedCommand_ = textEditingCommand_;
  } else if (!value.empty()) {
    document_.Add(TextCommand{textOrigin_, value, textInputStyle_});
  }
  textEditingCommand_.reset();
  if (textEditFont_) { DeleteObject(textEditFont_); textEditFont_ = nullptr; }
  if (hwnd_) { SetFocus(hwnd_); InvalidateRect(hwnd_, nullptr, FALSE); }
}

void CaptureOverlay::CancelTextInput() {
  if (!textEdit_) return;
  HWND edit = textEdit_;
  RemoveWindowSubclass(edit, TextEditProc, 1);
  textEdit_ = nullptr;
  DestroyWindow(edit);
  textEditingCommand_.reset();
  if (textEditFont_) { DeleteObject(textEditFont_); textEditFont_ = nullptr; }
  if (hwnd_) { SetFocus(hwnd_); InvalidateRect(hwnd_, nullptr, FALSE); }
}

LRESULT CALLBACK CaptureOverlay::TextEditProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
                                               UINT_PTR subclassId, DWORD_PTR referenceData) {
  (void)subclassId;
  auto* self = reinterpret_cast<CaptureOverlay*>(referenceData);
  if (message == WM_GETDLGCODE) return DLGC_WANTALLKEYS;
  if (message == WM_KEYDOWN && wParam == VK_RETURN &&
      (GetKeyState(VK_SHIFT) & 0x8000) == 0) { self->CommitTextInput(); return 0; }
  if (message == WM_KEYDOWN && wParam == VK_ESCAPE) { self->CancelTextInput(); return 0; }
  return DefSubclassProc(hwnd, message, wParam, lParam);
}

}  // namespace rc
