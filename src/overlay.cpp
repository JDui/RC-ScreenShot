#include "overlay.hpp"

#include "arrow.hpp"

#include <commdlg.h>
#include <commctrl.h>
#include <imm.h>
#include <windowsx.h>

#include <cwctype>
#include <limits>

namespace rc {
namespace {

constexpr wchar_t kOverlayClass[] = L"RC-ScreenShot.Overlay";
constexpr UINT_PTR kUnitTimer = 1;
constexpr UINT_PTR kSettingPreviewTimer = 2;
constexpr int kToolbarMargin = 8;
constexpr int kToolbarSelectHeight = 54;
constexpr int kToolbarEditHeight = 138;
constexpr int kToolbarWidth = 620;
constexpr int kToolbarToolSize = 36;
constexpr int kToolbarToolGap = 5;
constexpr int kToolbarActionSize = 40;
constexpr int kToolbarSecondaryTop = 52;
constexpr int kToolbarSecondaryHeight = 36;
constexpr int kToolbarPropertyTop = 94;
constexpr int kToolbarPresetStart = 210;
constexpr int kToolbarPresetStep = 22;
constexpr int kToolbarPresetSize = 20;
constexpr int kToolbarPropertyGap = 5;
constexpr int kToolbarPropertySize = 36;
constexpr int kToolbarPillWidth = 54;

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
  kContextTextShadow,
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
    return Inflate(bounds, std::max(4.0f, PenMaximumWidth(*pen) * 0.5f + 2.0f));
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
    const RectF bounds{text->origin.x, text->origin.y,
                      text->origin.x + std::max(advance, static_cast<float>(lines) * advance),
                      text->origin.y + std::max(advance, static_cast<float>(maxColumnLength) * advance)};
    return text->style.shadow ? Inflate(bounds, std::max(2.0f, text->style.size * 0.08f)) : bounds;
  }
  const RectF bounds{text->origin.x, text->origin.y, text->origin.x + std::max(advance, maxLineWidth),
                     text->origin.y + std::max(advance, static_cast<float>(lines) * text->style.size * 1.25f)};
  return text->style.shadow ? Inflate(bounds, std::max(2.0f, text->style.size * 0.08f)) : bounds;
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
    const float radius = std::max(5.0f, PenMaximumWidth(*pen) * 0.5f + 4.0f);
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
    const float scale = std::sqrt(std::abs((after.right - after.left) /
                                           std::max(1.0f, before.right - before.left)) *
                                  std::abs((after.bottom - after.top) /
                                           std::max(1.0f, before.bottom - before.top)));
    pen->style.width = std::clamp(pen->style.width * scale, 1.0f, 128.0f);
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

bool CommandMatchesTool(const EditCommand& command, Tool tool) {
  if (tool == Tool::Select) return true;
  if (tool == Tool::Pen) return std::holds_alternative<PenCommand>(command);
  if (tool == Tool::Text) return std::holds_alternative<TextCommand>(command);
  if (tool == Tool::MosaicBrush || tool == Tool::MosaicRectangle) {
    const auto* mosaic = std::get_if<MosaicCommand>(&command);
    return mosaic && mosaic->brush == (tool == Tool::MosaicBrush);
  }
  const auto* shape = std::get_if<ShapeCommand>(&command);
  if (!shape) return false;
  switch (tool) {
    case Tool::Rectangle: return shape->kind == ShapeKind::Rectangle;
    case Tool::Ellipse: return shape->kind == ShapeKind::Ellipse;
    case Tool::Line: return shape->kind == ShapeKind::Line;
    case Tool::Arrow: return shape->kind == ShapeKind::Arrow;
    default: return false;
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

uint64_t MosaicSignature(std::span<const EditCommand> commands, const RECT& selection) {
  uint64_t hash = 1469598103934665603ull;
  const auto mix = [&](uint64_t value) {
    hash ^= value;
    hash *= 1099511628211ull;
  };
  const auto mixFloat = [&](float value) {
    mix(static_cast<uint64_t>(std::llround(value * 1000.0f)));
  };
  mix(static_cast<uint64_t>(selection.left));
  mix(static_cast<uint64_t>(selection.top));
  mix(static_cast<uint64_t>(selection.right));
  mix(static_cast<uint64_t>(selection.bottom));
  size_t index = 0;
  for (const EditCommand& command : commands) {
    const auto* mosaic = std::get_if<MosaicCommand>(&command);
    if (!mosaic) continue;
    mix(0x4D4F53414943ull + static_cast<uint64_t>(index++));
    mix(mosaic->brush ? 1ull : 0ull);
    mix(static_cast<uint64_t>(mosaic->style));
    mixFloat(mosaic->brushSize);
    mix(static_cast<uint64_t>(mosaic->pixelSize));
    mixFloat(mosaic->blurRadius);
    mixFloat(mosaic->bounds.left); mixFloat(mosaic->bounds.top);
    mixFloat(mosaic->bounds.right); mixFloat(mosaic->bounds.bottom);
    mix(static_cast<uint64_t>(mosaic->points.size()));
    for (const PointF point : mosaic->points) { mixFloat(point.x); mixFloat(point.y); }
  }
  return hash;
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
  if (textEditBrush_) { DeleteObject(textEditBrush_); textEditBrush_ = nullptr; }
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
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, RGB(16, 22, 32));
        SetBkMode(dc, TRANSPARENT);
        return reinterpret_cast<LRESULT>(GetStockObject(NULL_BRUSH));
      }
      break;
    case WM_PAINT: Paint(); return 0;
    case WM_SIZE:
      if (renderTarget_ && renderTarget_->Resize(D2D1::SizeU(LOWORD(lParam), HIWORD(lParam))) == D2DERR_RECREATE_TARGET)
        DiscardDeviceResources();
      InvalidateRect(hwnd_, nullptr, FALSE); return 0;
    case WM_COMMAND:
      if (textEdit_ && reinterpret_cast<HWND>(lParam) == textEdit_ &&
          HIWORD(wParam) == EN_CHANGE) {
        RECT dirty = selection_;
        InflateRect(&dirty, 8, 8);
        InvalidateRect(hwnd_, &dirty, FALSE);
        return 0;
      }
      return 0;
    case WM_DISPLAYCHANGE: Cancel(); return 0;
    case WM_TIMER:
      if (wParam == kUnitTimer && unitReady_) { KillTimer(hwnd_, kUnitTimer); InvalidateRect(hwnd_, nullptr, FALSE); }
      else if (wParam == kSettingPreviewTimer) EndSettingPreview();
      return 0;
    case WM_MOUSEMOVE: {
      POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
      currentPoint_ = point;
      if (settingPreview_ && !selectedCommand_ && Contains(selection_, point)) {
        settingPreviewPoint_ = point;
      }
      if (toolbarDragging_) {
        toolbarPosition_.x = toolbarPositionStart_.x + point.x - toolbarDragStart_.x;
        toolbarPosition_.y = toolbarPositionStart_.y + point.y - toolbarDragStart_.y;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
      }
      if (Contains(selection_, point)) {
        lastCanvasPoint_ = point;
      }
      if (sizeSliderDragging_) SetSizeFromSlider(point);
      else if (editing_ && propertySliderDragging_) {
        for (const PropertyButton& button : PropertyButtons()) {
          if (button.action != *propertySliderDragging_ || !button.slider) continue;
          if (button.action == PropertyAction::Opacity) SetOpacityFromSlider(point, button.rect);
          else if (button.action == PropertyAction::FillOpacity) SetFillOpacityFromSlider(point, button.rect);
          else if (button.action == PropertyAction::MosaicStrength) SetMosaicStrengthFromSlider(point, button.rect);
          break;
        }
      } else if (selecting_) ContinueSelection(point);
      else if (drawing_) ContinueEditGesture(point);
      else UpdateHover(point);
      if (editing_) UpdateTooltip(point);
      InvalidateRect(hwnd_, nullptr, FALSE); return 0;
    }
    case WM_LBUTTONDOWN: {
        SetFocus(hwnd_);
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
      if (editing_) {
        if (textEdit_) CommitTextInput();
        if (HasSizeControl() && Contains(SizeSliderRect(), point)) {
          sizeSliderDragging_ = true;
          BeginSettingPreview(point);
          SetCapture(hwnd_);
          SetSizeFromSlider(point);
          return 0;
        }
        if (HitTestToolbarMoreColor(point)) {
          ChooseActiveColor();
          BeginSettingPreview(lastCanvasPoint_, true);
          return 0;
        }
        if (auto fillPreset = HitTestFillColorPreset(point)) {
          SetActiveFillPresetColor(*fillPreset);
          BeginSettingPreview(lastCanvasPoint_, true);
          return 0;
        }
        if (HitTestToolbarFillMoreColor(point)) {
          ChooseFillColor();
          BeginSettingPreview(lastCanvasPoint_, true);
          return 0;
        }
        for (const PropertyButton& button : PropertyButtons()) {
          if (!button.slider || !Contains(button.rect, point)) continue;
          propertySliderDragging_ = button.action;
          BeginSettingPreview(point);
          SetCapture(hwnd_);
          if (button.action == PropertyAction::Opacity) SetOpacityFromSlider(point, button.rect);
          else if (button.action == PropertyAction::FillOpacity) SetFillOpacityFromSlider(point, button.rect);
          else if (button.action == PropertyAction::MosaicStrength) SetMosaicStrengthFromSlider(point, button.rect);
          return 0;
        }
        if (auto preset = HitTestColorPreset(point)) {
          SetActivePresetColor(*preset);
          BeginSettingPreview(lastCanvasPoint_, true);
          return 0;
        }
        if (HitCopy(point)) { Complete(CaptureCompletion::Copy); return 0; }
        if (HitSave(point)) { Complete(CaptureCompletion::Save); return 0; }
        if (auto hit = HitTestTool(point)) { SelectTool(*hit); return 0; }
        if (auto property = HitTestProperty(point)) {
          ActivateProperty(*property);
          BeginSettingPreview(lastCanvasPoint_, true);
          return 0;
        }
        const RECT toolbar = ToolbarRect();
        if (Contains(toolbar, point)) {
          toolbarDragging_ = true;
          toolbarPositionSet_ = true;
          toolbarDragStart_ = point;
          toolbarPositionStart_ = {toolbar.left, toolbar.top};
          toolbarPosition_ = toolbarPositionStart_;
          SetCapture(hwnd_);
          return 0;
        }
        BeginEditGesture(point);
      } else BeginSelection(point);
      return 0;
    }
    case WM_LBUTTONUP: {
      POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
      if (toolbarDragging_) {
        toolbarDragging_ = false;
        toolbarPosition_.x = toolbarPositionStart_.x + point.x - toolbarDragStart_.x;
        toolbarPosition_.y = toolbarPositionStart_.y + point.y - toolbarDragStart_.y;
        if (GetCapture() == hwnd_) ReleaseCapture();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
      }
      if (sizeSliderDragging_) {
        SetSizeFromSlider(point);
        sizeSliderDragging_ = false;
        if (GetCapture() == hwnd_) ReleaseCapture();
        EndSettingPreview();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
      }
      if (propertySliderDragging_) {
        propertySliderDragging_.reset();
        if (GetCapture() == hwnd_) ReleaseCapture();
        EndSettingPreview();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
      }
      if (selecting_) EndSelection(point);
      else if (drawing_) EndEditGesture(point);
      return 0;
    }
    case WM_CAPTURECHANGED:
      if (toolbarDragging_) toolbarDragging_ = false;
      if (sizeSliderDragging_) { sizeSliderDragging_ = false; EndSettingPreview(); }
      if (propertySliderDragging_) { propertySliderDragging_.reset(); EndSettingPreview(); }
      InvalidateRect(hwnd_, nullptr, FALSE); return 0;
    case WM_LBUTTONDBLCLK: {
      if (editing_ && (tool_ == Tool::Select || tool_ == Tool::Text)) {
        const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const auto hit = HitTestCommand(point, tool_ == Tool::Text ? std::optional<Tool>(Tool::Text)
                                                                    : std::nullopt);
        if (hit) {
          selectedCommand_ = *hit;
          drawing_ = false;
          commandAdjustment_ = SelectionAdjustment::None;
          commandBeforeAdjust_.reset();
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

void CaptureOverlay::DiscardDeviceResources() {
  mosaicPreviewBitmap_.Reset();
  mosaicPreviewSignature_ = 0;
  toolbarBackdropBitmap_.Reset();
  toolbarBackdropPixels_.clear();
  toolbarBackdropValid_ = false;
  desktopBitmap_.Reset();
  renderTarget_.Reset();
}

void CaptureOverlay::EnsureToolbarBackdrop() {
  if (!renderTarget_ || !snapshot_.IsValid()) return;
  const RECT toolbar = ToolbarRect();
  if (toolbarBackdropValid_ && EqualRect(&toolbar, &toolbarBackdropRect_) &&
      toolbarBackdropBitmap_) return;
  toolbarBackdropBitmap_.Reset();
  toolbarBackdropValid_ = false;
  const int width = toolbar.right - toolbar.left;
  const int height = toolbar.bottom - toolbar.top;
  if (width <= 0 || height <= 0) return;
  constexpr int edgeExpansion = 3;
  const int cropLeft = std::max(0, static_cast<int>(toolbar.left) - edgeExpansion);
  const int cropTop = std::max(0, static_cast<int>(toolbar.top) - edgeExpansion);
  const int cropRight = std::min(snapshot_.width, static_cast<int>(toolbar.right) + edgeExpansion);
  const int cropBottom = std::min(snapshot_.height, static_cast<int>(toolbar.bottom) + edgeExpansion);
  int mipWidth = std::max(1, cropRight - cropLeft);
  int mipHeight = std::max(1, cropBottom - cropTop);
  std::vector<uint8_t> mip(static_cast<size_t>(mipWidth) * mipHeight * 4);
  for (int y = 0; y < mipHeight; ++y) {
    const uint8_t* source = snapshot_.bgra.data() +
        static_cast<size_t>(cropTop + y) * snapshot_.bgraStride + cropLeft * 4;
    std::memcpy(mip.data() + static_cast<size_t>(y) * mipWidth * 4,
                source, static_cast<size_t>(mipWidth) * 4);
  }
  // Build a short mip chain with 2x2 box reductions. This removes high-frequency
  // detail at a fraction of the cost of a per-pixel convolution while retaining
  // enough context for the toolbar's translucent backdrop.
  for (int level = 0; level < 4 && (mipWidth > 1 || mipHeight > 1); ++level) {
    const int nextWidth = std::max(1, (mipWidth + 1) / 2);
    const int nextHeight = std::max(1, (mipHeight + 1) / 2);
    std::vector<uint8_t> next(static_cast<size_t>(nextWidth) * nextHeight * 4);
    for (int y = 0; y < nextHeight; ++y) {
      for (int x = 0; x < nextWidth; ++x) {
        uint32_t sums[4]{};
        int samples = 0;
        for (int sy = 0; sy < 2; ++sy) {
          const int sourceY = y * 2 + sy;
          if (sourceY >= mipHeight) continue;
          for (int sx = 0; sx < 2; ++sx) {
            const int sourceX = x * 2 + sx;
            if (sourceX >= mipWidth) continue;
            const uint8_t* pixel = mip.data() +
                (static_cast<size_t>(sourceY) * mipWidth + sourceX) * 4;
            for (int channel = 0; channel < 4; ++channel) sums[channel] += pixel[channel];
            ++samples;
          }
        }
        uint8_t* output = next.data() +
            (static_cast<size_t>(y) * nextWidth + x) * 4;
        for (int channel = 0; channel < 4; ++channel)
          output[channel] = static_cast<uint8_t>(sums[channel] / std::max(1, samples));
      }
    }
    mip.swap(next);
    mipWidth = nextWidth;
    mipHeight = nextHeight;
  }
  toolbarBackdropPixels_.assign(static_cast<size_t>(width) * height * 4, 0);
  for (int y = 0; y < height; ++y) {
    const float sourceY = (y + 0.5f) * mipHeight / static_cast<float>(height) - 0.5f;
    const int y0 = std::clamp(static_cast<int>(std::floor(sourceY)), 0, mipHeight - 1);
    const int y1 = std::clamp(y0 + 1, 0, mipHeight - 1);
    const float fy = std::clamp(sourceY - std::floor(sourceY), 0.0f, 1.0f);
    for (int x = 0; x < width; ++x) {
      const float sourceX = (x + 0.5f) * mipWidth / static_cast<float>(width) - 0.5f;
      const int x0 = std::clamp(static_cast<int>(std::floor(sourceX)), 0, mipWidth - 1);
      const int x1 = std::clamp(x0 + 1, 0, mipWidth - 1);
      const float fx = std::clamp(sourceX - std::floor(sourceX), 0.0f, 1.0f);
      uint8_t* output = toolbarBackdropPixels_.data() +
          (static_cast<size_t>(y) * width + x) * 4;
      for (int channel = 0; channel < 4; ++channel) {
        const auto sample = [&](int sampleX, int sampleY) -> float {
          return static_cast<float>(mip[(static_cast<size_t>(sampleY) * mipWidth + sampleX) * 4 + channel]);
        };
        const float top = sample(x0, y0) * (1.0f - fx) + sample(x1, y0) * fx;
        const float bottom = sample(x0, y1) * (1.0f - fx) + sample(x1, y1) * fx;
        output[channel] = static_cast<uint8_t>(std::clamp(std::lround(top * (1.0f - fy) + bottom * fy), 0l, 255l));
      }
      output[3] = 0xFF;
    }
  }
  const auto properties = D2D1::BitmapProperties(
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
  if (FAILED(renderTarget_->CreateBitmap(D2D1::SizeU(static_cast<UINT32>(width), static_cast<UINT32>(height)),
                                          toolbarBackdropPixels_.data(), static_cast<UINT32>(width * 4),
                                          properties, &toolbarBackdropBitmap_))) {
    toolbarBackdropPixels_.clear();
    return;
  }
  toolbarBackdropRect_ = toolbar;
  toolbarBackdropValid_ = true;
}

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
  if (editing_) {
    DrawDocument(); DrawSettingPreview(); DrawToolbar(); DrawTooltip();
  }
  const wchar_t* modeName = mode_ == SelectionMode::Normal ? L"普通模式" : mode_ == SelectionMode::Window ? L"窗口模式" : L"单元模式";
  std::wstring status = std::wstring(L"空格切换  ·  ") + modeName;
  if (mode_ == SelectionMode::Unit && !unitReady_) status += L"（正在分析区域…）";
  DrawText(status, D2D1::RectF(16, 14, 330, 48), 15, D2D1::ColorF(D2D1::ColorF::White), DWRITE_TEXT_ALIGNMENT_LEADING);
  HRESULT hr = renderTarget_->EndDraw();
  if (hr == D2DERR_RECREATE_TARGET) DiscardDeviceResources();
}

void CaptureOverlay::BeginSettingPreview(POINT point, bool timed) {
  if (selectedCommand_ || drawing_ || textEdit_ || tool_ == Tool::Select || !HasArea(selection_)) return;
  settingPreviewPoint_ = point;
  if (!Contains(selection_, point)) settingPreviewPoint_ = lastCanvasPoint_;
  settingPreviewPoint_.x = std::clamp(settingPreviewPoint_.x, selection_.left, selection_.right - 1);
  settingPreviewPoint_.y = std::clamp(settingPreviewPoint_.y, selection_.top, selection_.bottom - 1);
  settingPreview_ = true;
  if (timed) SetTimer(hwnd_, kSettingPreviewTimer, 800, nullptr);
  else KillTimer(hwnd_, kSettingPreviewTimer);
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void CaptureOverlay::EndSettingPreview() {
  settingPreview_ = false;
  KillTimer(hwnd_, kSettingPreviewTimer);
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void CaptureOverlay::DrawSettingPreview() {
  if (!settingPreview_ || selectedCommand_ || drawing_ || textEdit_ || tool_ == Tool::Select ||
      !HasArea(selection_)) return;
  POINT point = settingPreviewPoint_;
  point.x = std::clamp(point.x, selection_.left, selection_.right - 1);
  point.y = std::clamp(point.y, selection_.top, selection_.bottom - 1);
  const float x = static_cast<float>(point.x);
  const float y = static_cast<float>(point.y);
  const float size = std::max(1.0f, ActiveSize());
  ComPtr<ID2D1SolidColorBrush> brush;
  ComPtr<ID2D1SolidColorBrush> outline;
  if (tool_ == Tool::Pen) {
    renderTarget_->CreateSolidColorBrush(ColorFromSetting(config_.pen.color, config_.pen.opacity * .20f), &brush);
    renderTarget_->CreateSolidColorBrush(ColorFromSetting(config_.pen.color,
                                                          std::min(1.0f, config_.pen.opacity + .2f)), &outline);
    const float radius = std::max(3.0f, size * .5f);
    renderTarget_->FillEllipse({{x, y}, radius, radius}, brush.Get());
    renderTarget_->DrawEllipse({{x, y}, radius, radius}, outline.Get(), 1.5f);
    return;
  }
  if (tool_ == Tool::MosaicBrush) {
    renderTarget_->CreateSolidColorBrush(D2D1::ColorF(.20f, .75f, 1.0f, .16f), &brush);
    renderTarget_->CreateSolidColorBrush(D2D1::ColorF(.48f, .88f, 1.0f, .92f), &outline);
    const float radius = std::max(5.0f, size * .5f);
    renderTarget_->FillEllipse({{x, y}, radius, radius}, brush.Get());
    renderTarget_->DrawEllipse({{x, y}, radius, radius}, outline.Get(), 1.5f);
    return;
  }
  if (tool_ == Tool::MosaicRectangle) {
    renderTarget_->CreateSolidColorBrush(D2D1::ColorF(.20f, .75f, 1.0f, .16f), &brush);
    renderTarget_->CreateSolidColorBrush(D2D1::ColorF(.48f, .88f, 1.0f, .92f), &outline);
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
  const ShapeSetting shapeStyle = tool_ == Tool::Rectangle ? config_.rectangle
                               : tool_ == Tool::Ellipse ? config_.ellipse
                               : ShapeSetting{tool_ == Tool::Line ? config_.line : config_.arrow, {}, 0.0f};
  renderTarget_->CreateSolidColorBrush(ColorFromSetting(shapeStyle.stroke.color, shapeStyle.stroke.opacity), &brush);
  if (shapeStyle.fillOpacity > 0.0f)
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
    // Window candidates are only consumed in Window mode, so enumerate them off the capture
    // path. GetWindowTextW crosses process boundaries and can block on hung windows; running
    // it here keeps the hotkey-to-overlay path free of that variable latency.
    { std::scoped_lock lock(unitMutex_); EnumerateWindows(snapshot_); }
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
    std::scoped_lock lock(unitMutex_);
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
  const PointF local = ToSelectionPoint(point);
  std::optional<size_t> hit = HitTestCommand(point, tool_);
  if (selectedCommand_) {
    const EditCommand* selected = document_.At(*selectedCommand_);
    if (selected && CommandMatchesTool(*selected, tool_) &&
        NearRect(SelectedCommandBounds(), local, 10.0f)) hit = selectedCommand_;
  }
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
  commandBeforeAdjust_.reset();
  if (tool_ == Tool::Text) {
    BeginTextInput(point);
    return;
  }
  drawing_ = true; dragStart_ = point; currentPoint_ = point; SetCapture(hwnd_);
  if (tool_ == Tool::Pen) {
    penLastSampleTime_ = std::chrono::steady_clock::now();
    penWidthScale_ = 1.0f;
    previewCommand_ = PenCommand{{local}, config_.pen, {penWidthScale_}};
  }
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
    const float distance = pen->points.empty()
                               ? 0.0f
                               : std::hypot(local.x - pen->points.back().x, local.y - pen->points.back().y);
    if (pen->points.empty() || distance >= 1.25f) {
      const auto now = std::chrono::steady_clock::now();
      const float elapsed = std::clamp(std::chrono::duration<float>(now - penLastSampleTime_).count(),
                                       0.004f, 0.12f);
      const float targetScale = PenWidthScaleForSpeed(distance / elapsed);
      const float smoothing = std::clamp(elapsed * 18.0f, 0.16f, 0.55f);
      penWidthScale_ = std::lerp(penWidthScale_, targetScale, smoothing);
      pen->points.push_back(local);
      pen->widthScales.push_back(penWidthScale_);
      penLastSampleTime_ = now;
    }
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
  EndSettingPreview();
  tool_ = tool;
  if (selectedCommand_) {
    const EditCommand* command = document_.At(*selectedCommand_);
    if (!command || !CommandMatchesTool(*command, tool_)) selectedCommand_.reset();
  }
  commandAdjustment_ = SelectionAdjustment::None;
  commandBeforeAdjust_.reset();
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
  std::wstring liveText;
  if (textEdit_) {
    const int length = GetWindowTextLengthW(textEdit_);
    if (length > 0) {
      std::vector<wchar_t> buffer(static_cast<size_t>(length) + 1);
      GetWindowTextW(textEdit_, buffer.data(), length + 1);
      liveText.assign(buffer.data(), static_cast<size_t>(length));
    }
    if (textImeComposing_) {
      if (HIMC context = ImmGetContext(textEdit_)) {
        const LONG bytes = ImmGetCompositionStringW(context, GCS_COMPSTR, nullptr, 0);
        if (bytes > 0) {
          std::wstring composition(static_cast<size_t>(bytes) / sizeof(wchar_t), L'\0');
          ImmGetCompositionStringW(context, GCS_COMPSTR, composition.data(), bytes);
          if (liveText.size() < composition.size() ||
              liveText.compare(liveText.size() - composition.size(), composition.size(), composition) != 0) {
            liveText += composition;
          }
        }
        ImmReleaseContext(textEdit_, context);
      }
    }
  }
  std::vector<EditCommand> commands;
  commands.reserve(document_.Size() + (liveText.empty() ? 0u : 1u));
  for (size_t index = 0; index < document_.Size(); ++index) {
    const EditCommand* command = document_.At(index);
    if (!command) continue;
    if (textEditingCommand_ && *textEditingCommand_ == index) {
      if (!liveText.empty()) commands.emplace_back(TextCommand{textOrigin_, liveText, textInputStyle_});
      continue;
    }
    commands.push_back(*command);
  }
  if (textEdit_ && !textEditingCommand_ && !liveText.empty())
    commands.emplace_back(TextCommand{textOrigin_, liveText, textInputStyle_});
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
  const auto drawPressurePath = [&](const PenCommand& pen, const D2D1_COLOR_F& color) {
    if (pen.points.empty()) return;
    if (pen.widthScales.empty()) {
      drawRoundPath(pen.points, color, pen.style.width);
      return;
    }
    ComPtr<ID2D1SolidColorBrush> brush;
    renderTarget_->CreateSolidColorBrush(color, &brush);
    const auto center = [&](size_t index) {
      return D2D1::Point2F(selection_.left + pen.points[index].x,
                          selection_.top + pen.points[index].y);
    };
    const auto dot = [&](size_t index) {
      const float radius = PenPointWidth(pen, index) * 0.5f;
      renderTarget_->FillEllipse({center(index), radius, radius}, brush.Get());
    };
    dot(0);
    for (size_t index = 1; index < pen.points.size(); ++index) {
      const float width = (PenPointWidth(pen, index - 1) + PenPointWidth(pen, index)) * 0.5f;
      renderTarget_->DrawLine(center(index - 1), center(index), brush.Get(), width, roundStroke.Get());
      dot(index);
    }
  };
  for (const auto& command : commands) {
    if (const auto* pen = std::get_if<PenCommand>(&command)) {
      drawPressurePath(*pen, ColorFromSetting(pen->style.color, pen->style.opacity));
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
      } else if (shape->kind == ShapeKind::Line) {
        D2D1_POINT_2F start{selection_.left + shape->start.x, selection_.top + shape->start.y};
        D2D1_POINT_2F end{selection_.left + shape->end.x, selection_.top + shape->end.y};
        renderTarget_->DrawLine(start, end, stroke.Get(), shape->style.stroke.width);
      } else {
        DrawArrow(renderTarget_.Get(), d2dFactory_.Get(),
                  {selection_.left + shape->start.x, selection_.top + shape->start.y},
                  {selection_.left + shape->end.x, selection_.top + shape->end.y},
                  ColorFromSetting(shape->style.stroke.color, shape->style.stroke.opacity),
                  shape->style.stroke.width);
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
  if (selectedCommand_) DrawCommandHandles();
  else if (tool_ == Tool::Select) DrawSelectionHandles();
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
  if (!hasMosaic) {
    mosaicPreviewBitmap_.Reset();
    mosaicPreviewSignature_ = 0;
    return false;
  }

  const uint64_t signature = MosaicSignature(commands, selection_);

  const LONG left = std::clamp(selection_.left, 0L, static_cast<LONG>(snapshot_.width));
  const LONG top = std::clamp(selection_.top, 0L, static_cast<LONG>(snapshot_.height));
  const LONG right = std::clamp(selection_.right, left, static_cast<LONG>(snapshot_.width));
  const LONG bottom = std::clamp(selection_.bottom, top, static_cast<LONG>(snapshot_.height));
  const int width = right - left;
  const int height = bottom - top;
  if (width <= 0 || height <= 0) return false;

  const auto drawCached = [&] {
    renderTarget_->DrawBitmap(mosaicPreviewBitmap_.Get(),
                              D2D1::RectF(static_cast<float>(left), static_cast<float>(top),
                                          static_cast<float>(right), static_cast<float>(bottom)),
                              1.0f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
  };
  if (mosaicPreviewBitmap_ && mosaicPreviewSignature_ == signature) {
    drawCached();
    return true;
  }

  std::vector<uint8_t> pixels(static_cast<size_t>(width * height * 4));
  for (int y = 0; y < height; ++y) {
    memcpy(pixels.data() + static_cast<size_t>(y * width * 4),
           snapshot_.bgra.data() + static_cast<size_t>((top + y) * snapshot_.bgraStride + left * 4),
           static_cast<size_t>(width * 4));
  }
  ApplyMosaics(pixels, width, height, width * 4, commands);

  const auto bitmapProperties = D2D1::BitmapProperties(
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
  if (FAILED(renderTarget_->CreateBitmap(D2D1::SizeU(static_cast<UINT32>(width), static_cast<UINT32>(height)),
                                          pixels.data(), static_cast<UINT32>(width * 4),
                                          bitmapProperties, &mosaicPreviewBitmap_))) {
    mosaicPreviewBitmap_.Reset();
    mosaicPreviewSignature_ = 0;
    return false;
  }
  mosaicPreviewSignature_ = signature;
  drawCached();
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
  const float originX = selection_.left + command.origin.x;
  const float originY = selection_.top + command.origin.y;
  const auto draw = [&](float offsetX, float offsetY, D2D1_COLOR_F color) {
    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(renderTarget_->CreateSolidColorBrush(color, &brush))) return;
    if (!command.style.vertical) {
      const D2D1_RECT_F bounds{originX + offsetX, originY + offsetY,
                               originX + offsetX + 4096.0f, originY + offsetY + 4096.0f};
      renderTarget_->DrawTextW(command.text.data(), static_cast<UINT32>(command.text.size()),
                               format.Get(), bounds, brush.Get());
      return;
    }
    float x = originX + offsetX, y = originY + offsetY;
    const float advance = command.style.size * 1.16f;
    for (wchar_t character : command.text) {
      if (character == L'\r') continue;
      if (character == L'\n') { x += advance; y = originY + offsetY; continue; }
      const D2D1_RECT_F cell{x, y, x + advance, y + advance};
      renderTarget_->DrawTextW(&character, 1, format.Get(), cell, brush.Get());
      y += advance;
    }
  };
  if (command.style.shadow) {
    const float offset = std::clamp(command.style.size * 0.08f, 2.0f, 5.0f);
    const float shadowAlpha = command.style.opacity * 0.22f;
    draw(-offset, -offset * .35f, D2D1::ColorF(0.0f, 0.0f, 0.0f, shadowAlpha * .20f));
    draw(offset, -offset * .15f, D2D1::ColorF(0.0f, 0.0f, 0.0f, shadowAlpha * .28f));
    draw(-offset * .25f, offset, D2D1::ColorF(0.0f, 0.0f, 0.0f, shadowAlpha * .28f));
    draw(offset * .25f, offset, D2D1::ColorF(0.0f, 0.0f, 0.0f, shadowAlpha * .24f));
    draw(offset, offset, D2D1::ColorF(0.0f, 0.0f, 0.0f, shadowAlpha * .30f));
  }
  draw(0.0f, 0.0f, ColorFromSetting(command.style.color, command.style.opacity));
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

std::optional<size_t> CaptureOverlay::HitTestCommand(POINT point,
                                                     std::optional<Tool> toolFilter) const {
  if (!Contains(selection_, point)) return std::nullopt;
  const PointF local = ToSelectionPoint(point);
  for (size_t index = document_.Size(); index > 0; --index) {
    const EditCommand* command = document_.At(index - 1);
    if (!command) continue;
    if (toolFilter && !CommandMatchesTool(*command, *toolFilter)) continue;
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
      line({left + 9, bottom - 9}, {left + 12, bottom - 17}, 2.0f);
      line({left + 12, bottom - 17}, {right - 13, top + 9}, 2.0f);
      line({right - 13, top + 9}, {right - 8, top + 14}, 2.0f);
      line({right - 8, top + 14}, {left + 17, bottom - 12}, 2.0f);
      line({left + 17, bottom - 12}, {left + 9, bottom - 9}, 2.0f);
      line({left + 13, bottom - 16}, {left + 18, bottom - 11}, 1.6f);
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
      line({left + 8, bottom - 9}, {right - 9, top + 9}, 2.4f);
      line({right - 9, top + 9}, {right - 19, top + 11}, 2.4f);
      line({right - 9, top + 9}, {right - 11, top + 19}, 2.4f);
      break;
    case Tool::Text:
      DrawText(L"Aa", ToD2D(rect), 14, color);
      break;
    case Tool::MosaicBrush:
      renderTarget_->FillRectangle(D2D1::RectF(left + 8, top + 8, left + 13, top + 13), brush.Get());
      renderTarget_->FillRectangle(D2D1::RectF(left + 14, top + 14, left + 19, top + 19), brush.Get());
      renderTarget_->FillRectangle(D2D1::RectF(left + 20, top + 8, left + 25, top + 13), brush.Get());
      line({left + 12, bottom - 9}, {right - 9, top + 15}, 3.5f);
      renderTarget_->FillEllipse({{left + 11, bottom - 10}, 3.0f, 3.0f}, brush.Get());
      break;
    case Tool::MosaicRectangle:
      renderTarget_->DrawRoundedRectangle(D2D1::RoundedRect(
          D2D1::RectF(left + 7, top + 9, right - 7, bottom - 9), 2, 2), brush.Get(), 1.8f);
      renderTarget_->FillRectangle(D2D1::RectF(left + 11, top + 13, left + 16, top + 18), brush.Get());
      renderTarget_->FillRectangle(D2D1::RectF(left + 17, top + 19, left + 22, top + 24), brush.Get());
      renderTarget_->FillRectangle(D2D1::RectF(left + 23, top + 13, left + 28, top + 18), brush.Get());
      break;
    case Tool::Select:
      line({left + 10, top + 7}, {left + 17, bottom - 7}, 2.1f);
      line({left + 10, top + 7}, {right - 8, top + 15}, 2.1f);
      line({right - 8, top + 15}, {right - 18, top + 18}, 2.1f);
      line({right - 18, top + 18}, {right - 11, bottom - 9}, 2.1f);
      line({right - 11, bottom - 9}, {right - 16, bottom - 7}, 2.1f);
      line({right - 18, top + 18}, {left + 17, bottom - 7}, 2.1f);
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
        D2D1::RectF(left + 15, top + 14, right - 9, bottom - 9), 2, 2), brush.Get(), 2.0f);
    renderTarget_->DrawRoundedRectangle(D2D1::RoundedRect(
        D2D1::RectF(left + 9, top + 9, right - 15, bottom - 14), 2, 2), brush.Get(), 2.0f);
    return;
  }
  const float cx = (left + right) * .5f;
  renderTarget_->DrawLine({cx, top + 8}, {cx, bottom - 14}, brush.Get(), 2.4f);
  renderTarget_->DrawLine({cx, bottom - 14}, {cx - 6, bottom - 20}, brush.Get(), 2.4f);
  renderTarget_->DrawLine({cx, bottom - 14}, {cx + 6, bottom - 20}, brush.Get(), 2.4f);
  renderTarget_->DrawLine({left + 9, bottom - 10}, {left + 9, bottom - 6}, brush.Get(), 2.0f);
  renderTarget_->DrawLine({left + 9, bottom - 6}, {right - 9, bottom - 6}, brush.Get(), 2.0f);
  renderTarget_->DrawLine({right - 9, bottom - 6}, {right - 9, bottom - 10}, brush.Get(), 2.0f);
}

void CaptureOverlay::DrawPropertyIcon(PropertyAction action, const RECT& rect) {
  ComPtr<ID2D1SolidColorBrush> brush;
  const D2D1_COLOR_F iconColor =
      action == PropertyAction::TextShadow && ActiveTextStyle()->shadow
          ? D2D1::ColorF(.36f, .67f, 1.0f, 1.0f)
          : D2D1::ColorF(.76f, .83f, .94f, 1.0f);
  renderTarget_->CreateSolidColorBrush(iconColor, &brush);
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
    case PropertyAction::FillToggle:
      renderTarget_->DrawRoundedRectangle(D2D1::RoundedRect(
          D2D1::RectF(left + 8, top + 10, right - 8, bottom - 10), 6, 6), brush.Get(), 1.8f);
      break;
    case PropertyAction::MosaicStyle:
      for (int i = 0; i < 3; ++i) {
        line({left + 10.0f + i * 6.0f, top + 10}, {left + 10.0f + i * 6.0f, bottom - 10}, 1.5f);
        line({left + 10, top + 10.0f + i * 6.0f}, {right - 10, top + 10.0f + i * 6.0f}, 1.5f);
      }
      break;
    case PropertyAction::MosaicStrength:
      line({left + 11, cy}, {right - 11, cy});
      break;
    case PropertyAction::FrameToggle:
      renderTarget_->DrawRectangle(D2D1::RectF(left + 9, top + 9, right - 9, bottom - 9), brush.Get(), 2.0f);
      break;
    case PropertyAction::TextOrientation:
      DrawText(L"T", ToD2D(rect), 15, iconColor);
      break;
    case PropertyAction::TextShadow:
      DrawText(L"T", D2D1::RectF(left + 4, top + 4, right + 4, bottom + 4), 13,
               D2D1::ColorF(.05f, .08f, .12f, .85f));
      DrawText(L"T", ToD2D(rect), 13, iconColor);
      break;
  }
}

RECT CaptureOverlay::ToolbarRect() const {
  const int requestedWidth = kToolbarWidth;
  const int requestedHeight = tool_ == Tool::Select ? kToolbarSelectHeight : kToolbarEditHeight;
  RECT work = ToolbarWorkArea();
  const int workWidth = std::max(1, static_cast<int>(work.right - work.left));
  const int workHeight = std::max(1, static_cast<int>(work.bottom - work.top));
  const int width = std::min(requestedWidth, workWidth);
  const int height = std::min(requestedHeight, workHeight);
  const RECT anchor = HasArea(selection_) ? selection_
                                          : RECT{snapshot_.width / 2, snapshot_.height / 2,
                                                 snapshot_.width / 2, snapshot_.height / 2};
  const int gap = 12;
  const std::array<RECT, 4> candidates{{
      {anchor.left, anchor.bottom + gap, anchor.left + width, anchor.bottom + gap + height},
      {anchor.left, anchor.top - gap - height, anchor.left + width, anchor.top - gap},
      {anchor.right + gap, anchor.top, anchor.right + gap + width, anchor.top + height},
      {anchor.left - gap - width, anchor.top, anchor.left - gap, anchor.top + height},
  }};
  auto fits = [&](const RECT& rect) {
    return rect.left >= work.left && rect.top >= work.top &&
           rect.right <= work.right && rect.bottom <= work.bottom;
  };
  if (!toolbarPositionSet_) {
    for (const RECT& candidate : candidates) {
      if (fits(candidate)) return candidate;
    }
    RECT clamped = candidates.front();
    clamped.left = std::clamp(clamped.left, work.left, work.right - width);
    clamped.top = std::clamp(clamped.top, work.top, work.bottom - height);
    clamped.right = clamped.left + width;
    clamped.bottom = clamped.top + height;
    return clamped;
  }
  RECT positioned{toolbarPosition_.x, toolbarPosition_.y,
                  toolbarPosition_.x + width, toolbarPosition_.y + height};
  positioned.left = std::clamp(positioned.left, work.left, work.right - width);
  positioned.top = std::clamp(positioned.top, work.top, work.bottom - height);
  positioned.right = positioned.left + width;
  positioned.bottom = positioned.top + height;
  return positioned;
}

RECT CaptureOverlay::ToolbarWorkArea() const {
  POINT anchor{};
  if (toolbarPositionSet_) {
    anchor = {toolbarPosition_.x + kToolbarWidth / 2, toolbarPosition_.y + kToolbarSelectHeight / 2};
  } else if (HasArea(selection_)) {
    anchor = {(selection_.left + selection_.right) / 2, (selection_.top + selection_.bottom) / 2};
  } else {
    anchor = {snapshot_.width / 2, snapshot_.height / 2};
  }
  POINT screen{anchor.x + snapshot_.virtualBounds.left, anchor.y + snapshot_.virtualBounds.top};
  HMONITOR monitor = MonitorFromPoint(screen, MONITOR_DEFAULTTONEAREST);
  MONITORINFO info{sizeof(info)};
  RECT work{0, 0, snapshot_.width, snapshot_.height};
  if (monitor && GetMonitorInfoW(monitor, &info)) {
    work = info.rcWork;
    OffsetRect(&work, -snapshot_.virtualBounds.left, -snapshot_.virtualBounds.top);
    work.left = std::clamp(work.left, 0L, static_cast<LONG>(snapshot_.width));
    work.top = std::clamp(work.top, 0L, static_cast<LONG>(snapshot_.height));
    work.right = std::clamp(work.right, work.left + 1L, static_cast<LONG>(snapshot_.width));
    work.bottom = std::clamp(work.bottom, work.top + 1L, static_cast<LONG>(snapshot_.height));
  }
  return work;
}

RECT CaptureOverlay::ToolbarToolRect(size_t index) const {
  const RECT toolbar = ToolbarRect();
  const int x = toolbar.left + kToolbarMargin +
                static_cast<int>(index) * (kToolbarToolSize + kToolbarToolGap);
  return {x, toolbar.top + kToolbarMargin, x + kToolbarToolSize,
          toolbar.top + kToolbarMargin + kToolbarToolSize};
}

RECT CaptureOverlay::ToolbarCopyRect() const {
  const RECT toolbar = ToolbarRect();
  const int x = toolbar.right - kToolbarMargin - kToolbarActionSize * 2 - kToolbarToolGap;
  return {x, toolbar.top + kToolbarMargin, x + kToolbarActionSize,
          toolbar.top + kToolbarMargin + kToolbarToolSize};
}

RECT CaptureOverlay::ToolbarSaveRect() const {
  const RECT copy = ToolbarCopyRect();
  return {copy.right + kToolbarToolGap, copy.top,
          copy.right + kToolbarToolGap + kToolbarActionSize, copy.bottom};
}

RECT CaptureOverlay::ToolbarMoreColorRect() const {
  const RECT toolbar = ToolbarRect();
  return {toolbar.left + 184, toolbar.top + kToolbarSecondaryTop + 5,
          toolbar.left + 204, toolbar.top + kToolbarSecondaryTop + 5 + kToolbarPresetSize};
}

RECT CaptureOverlay::ToolbarPresetRect(size_t index) const {
  const RECT toolbar = ToolbarRect();
  const int x = toolbar.left + kToolbarPresetStart +
                static_cast<int>(index) * kToolbarPresetStep;
  return {x, toolbar.top + kToolbarSecondaryTop + 5, x + kToolbarPresetSize,
          toolbar.top + kToolbarSecondaryTop + 5 + kToolbarPresetSize};
}

RECT CaptureOverlay::ToolbarFillPresetRect(size_t index) const {
  const RECT toolbar = ToolbarRect();
  int start = toolbar.left + 12;
  for (const PropertyButton& button : PropertyButtons()) {
    if (button.action != PropertyAction::FillColor) continue;
    start = button.rect.left + 42;
    break;
  }
  const int x = start + static_cast<int>(index) * 18;
  const int y = toolbar.top + kToolbarPropertyTop + 10;
  return {x, y, x + 14, y + 14};
}

RECT CaptureOverlay::ToolbarFillMoreColorRect() const {
  for (const PropertyButton& button : PropertyButtons()) {
    if (button.action == PropertyAction::FillColor) {
      const int y = ToolbarRect().top + kToolbarPropertyTop + 10;
      return {button.rect.left + 24, y, button.rect.left + 38, y + 14};
    }
  }
  return {};
}

bool CaptureOverlay::HitTestToolbarMoreColor(POINT point) const {
  return const_cast<CaptureOverlay*>(this)->ActiveColor() && HasSizeControl() &&
         Contains(ToolbarMoreColorRect(), point);
}

bool CaptureOverlay::HitTestToolbarFillMoreColor(POINT point) const {
  const RECT rect = ToolbarFillMoreColorRect();
  return HasArea(rect, 1) && Contains(rect, point);
}

void CaptureOverlay::DrawToolbar() {
  const RECT toolbar = ToolbarRect();
  EnsureToolbarBackdrop();
  ComPtr<ID2D1SolidColorBrush> shadow, background;
  renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, .28f), &shadow);
  renderTarget_->CreateSolidColorBrush(D2D1::ColorF(.055f, .07f, .10f, .60f), &background);
  RECT shadowRect = toolbar;
  OffsetRect(&shadowRect, 0, 4);
  renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(ToD2D(shadowRect), 12, 12), shadow.Get());
  ComPtr<ID2D1RoundedRectangleGeometry> toolbarMask;
  ComPtr<ID2D1Layer> toolbarLayer;
  const bool useToolbarMask =
      SUCCEEDED(d2dFactory_->CreateRoundedRectangleGeometry(
          D2D1::RoundedRect(ToD2D(toolbar), 12, 12), &toolbarMask)) &&
      SUCCEEDED(renderTarget_->CreateLayer(nullptr, &toolbarLayer));
  if (useToolbarMask) {
    renderTarget_->PushLayer(D2D1::LayerParameters(ToD2D(toolbar), toolbarMask.Get()), toolbarLayer.Get());
  }
  if (toolbarBackdropBitmap_) {
    renderTarget_->DrawBitmap(toolbarBackdropBitmap_.Get(), ToD2D(toolbar), 1.0f,
                              D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
  }
  renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(ToD2D(toolbar), 12, 12), background.Get());
  if (useToolbarMask) renderTarget_->PopLayer();
  const auto drawPalette = [&](const RECT& rect) {
    if (!HasArea(rect, 2)) return;
    const std::array<D2D1_COLOR_F, 6> colors{{
        D2D1::ColorF(.96f, .20f, .25f, 1), D2D1::ColorF(1.0f, .65f, .16f, 1),
        D2D1::ColorF(.98f, .88f, .20f, 1), D2D1::ColorF(.20f, .78f, .40f, 1),
        D2D1::ColorF(.18f, .55f, .95f, 1), D2D1::ColorF(.63f, .28f, .88f, 1)}};
    const int width = std::max(1, static_cast<int>(rect.right - rect.left));
    for (size_t index = 0; index < colors.size(); ++index) {
      ComPtr<ID2D1SolidColorBrush> brush;
      renderTarget_->CreateSolidColorBrush(colors[index], &brush);
      const int left = rect.left + static_cast<int>(index) * width / static_cast<int>(colors.size());
      const int right = rect.left + static_cast<int>(index + 1) * width / static_cast<int>(colors.size());
      renderTarget_->FillRectangle(D2D1::RectF(static_cast<float>(left), static_cast<float>(rect.top),
                                                static_cast<float>(right), static_cast<float>(rect.bottom)),
                                   brush.Get());
    }
    ComPtr<ID2D1SolidColorBrush> outline;
    renderTarget_->CreateSolidColorBrush(D2D1::ColorF(.86f, .92f, 1.0f, 1), &outline);
    renderTarget_->DrawRoundedRectangle(D2D1::RoundedRect(ToD2D(rect), 4, 4), outline.Get(), 1.0f);
  };

  for (size_t index = 0; index < toolButtons_.size(); ++index) {
    const auto& button = toolButtons_[index];
    const RECT item = ToolbarToolRect(index);
    ComPtr<ID2D1SolidColorBrush> buttonBackground;
    renderTarget_->CreateSolidColorBrush(button.tool == tool_
                                             ? D2D1::ColorF(.12f, .55f, .95f, .95f)
                                             : D2D1::ColorF(.11f, .14f, .19f, .95f),
                                         &buttonBackground);
    renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(ToD2D(item), 9, 9), buttonBackground.Get());
    DrawToolIcon(button.tool, item, button.tool == tool_);
  }
  const RECT copy = ToolbarCopyRect();
  const RECT save = ToolbarSaveRect();
  ComPtr<ID2D1SolidColorBrush> separator;
  renderTarget_->CreateSolidColorBrush(D2D1::ColorF(.27f, .34f, .45f, .85f), &separator);
  renderTarget_->DrawLine({static_cast<float>(copy.left - 10), static_cast<float>(toolbar.top + 10)},
                          {static_cast<float>(copy.left - 10), static_cast<float>(toolbar.top + 44)},
                          separator.Get(), 1.0f);
  ComPtr<ID2D1SolidColorBrush> copyBackground, saveBackground;
  renderTarget_->CreateSolidColorBrush(D2D1::ColorF(.10f, .33f, .25f, .98f), &copyBackground);
  renderTarget_->CreateSolidColorBrush(D2D1::ColorF(.10f, .28f, .48f, .98f), &saveBackground);
  renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(ToD2D(copy), 9, 9), copyBackground.Get());
  renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(ToD2D(save), 9, 9), saveBackground.Get());
  DrawActionIcon(false, copy);
  DrawActionIcon(true, save);

  if (HasSizeControl()) {
    const RECT slider = SizeSliderRect();
    ComPtr<ID2D1SolidColorBrush> track, knob, sizeIcon;
    renderTarget_->CreateSolidColorBrush(D2D1::ColorF(.27f, .31f, .39f, 1), &track);
    renderTarget_->CreateSolidColorBrush(D2D1::ColorF(.22f, .67f, 1.0f, 1), &knob);
    renderTarget_->CreateSolidColorBrush(D2D1::ColorF(.70f, .76f, .85f, 1), &sizeIcon);
    const float minimum = tool_ == Tool::Text ? 8.0f :
                          (tool_ == Tool::MosaicBrush ? 4.0f : 1.0f);
    const float maximum = tool_ == Tool::MosaicBrush ? 256.0f :
                          (tool_ == Tool::Text ? 256.0f : 128.0f);
    const float ratio = std::clamp((ActiveSize() - minimum) / (maximum - minimum), 0.0f, 1.0f);
    const float centerY = (slider.top + slider.bottom) / 2.0f;
    renderTarget_->FillEllipse({{static_cast<float>(toolbar.left + 28), centerY}, 3.0f, 3.0f}, sizeIcon.Get());
    renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(
        D2D1::RectF(static_cast<float>(slider.left), centerY - 2,
                    static_cast<float>(slider.right), centerY + 2), 2, 2), track.Get());
    const float knobX = slider.left + ratio * (slider.right - slider.left);
    renderTarget_->FillEllipse({{knobX, centerY}, 7, 7}, knob.Get());
    if (ActiveColor()) {
      drawPalette(ToolbarMoreColorRect());
      for (size_t i = 0; i < kPresetColors.size(); ++i) {
        const RECT swatch = ToolbarPresetRect(i);
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

  for (const PropertyButton& button : PropertyButtons()) {
    if (button.action == PropertyAction::FillColor && button.rect.right - button.rect.left > kToolbarPropertySize) {
      drawPalette(ToolbarFillMoreColorRect());
      const ShapeSetting* shape = ActiveShape();
      for (size_t i = 0; i < kPresetColors.size(); ++i) {
        const RECT swatch = ToolbarFillPresetRect(i);
        ComPtr<ID2D1SolidColorBrush> colorBrush, outline;
        renderTarget_->CreateSolidColorBrush(ColorFromSetting(ColorSetting{kPresetColors[i]}), &colorBrush);
        renderTarget_->CreateSolidColorBrush(D2D1::ColorF(.78f, .84f, .94f, 1), &outline);
        renderTarget_->FillEllipse({{(swatch.left + swatch.right) / 2.0f,
                                     (swatch.top + swatch.bottom) / 2.0f}, 6, 6}, colorBrush.Get());
        if (shape && (shape->fill.rgba & 0xFFFFFF00u) == (kPresetColors[i] & 0xFFFFFF00u)) {
          renderTarget_->DrawEllipse({{(swatch.left + swatch.right) / 2.0f,
                                       (swatch.top + swatch.bottom) / 2.0f}, 8, 8}, outline.Get(), 2);
        }
      }
      continue;
    }
    if (button.slider) {
      const float value = button.action == PropertyAction::Opacity ? ActiveOpacity()
                         : button.action == PropertyAction::FillOpacity ? ActiveFillOpacity()
                         : ActiveMosaicStrength();
      const MosaicStyle activeMosaicStyle = ActiveMosaic() ? ActiveMosaic()->style : config_.mosaicStyle;
      const float minimum = button.action == PropertyAction::MosaicStrength
                                ? (activeMosaicStyle == MosaicStyle::Blur ? 1.0f : 2.0f)
                                : 0.0f;
      const float maximum = button.action == PropertyAction::MosaicStrength
                                ? (activeMosaicStyle == MosaicStyle::Blur ? 64.0f : 128.0f)
                                : 1.0f;
      const float ratio = std::clamp((value - minimum) / std::max(1.0f, maximum - minimum), 0.0f, 1.0f);
      const float centerY = (button.rect.top + button.rect.bottom) / 2.0f;
      ComPtr<ID2D1SolidColorBrush> sliderTrack, sliderFill, sliderKnob;
      renderTarget_->CreateSolidColorBrush(D2D1::ColorF(.22f, .28f, .37f, 1), &sliderTrack);
      renderTarget_->CreateSolidColorBrush(D2D1::ColorF(.24f, .67f, 1.0f, 1), &sliderFill);
      renderTarget_->CreateSolidColorBrush(D2D1::ColorF(.82f, .91f, 1.0f, 1), &sliderKnob);
      renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(
          D2D1::RectF(static_cast<float>(button.rect.left + 5), centerY - 2,
                      static_cast<float>(button.rect.right - 5), centerY + 2), 2, 2), sliderTrack.Get());
      const float knobX = static_cast<float>(button.rect.left + 5) + ratio * (button.rect.right - button.rect.left - 10);
      renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(
          D2D1::RectF(static_cast<float>(button.rect.left + 5), centerY - 2, knobX, centerY + 2), 2, 2), sliderFill.Get());
      renderTarget_->FillEllipse({{knobX, centerY}, 6, 6}, sliderKnob.Get());
      continue;
    }
    bool enabled = false;
    if (button.action == PropertyAction::MosaicStyle)
      enabled = (ActiveMosaic() ? ActiveMosaic()->style : config_.mosaicStyle) == MosaicStyle::Blur;
    else if (button.action == PropertyAction::TextOrientation)
      enabled = ActiveTextStyle()->vertical;
    else if (button.action == PropertyAction::TextShadow)
      enabled = ActiveTextStyle()->shadow;
    else if (button.action == PropertyAction::FrameToggle)
      enabled = config_.frameEnabled;
    else if (button.action == PropertyAction::FillToggle) {
      const ShapeSetting* shape = ActiveShape(); enabled = shape && shape->fillOpacity > 0.0f;
    }

    ComPtr<ID2D1SolidColorBrush> propertyBackground, propertyOutline;
    renderTarget_->CreateSolidColorBrush(
        button.pill && enabled ? D2D1::ColorF(.12f, .55f, .95f, .98f)
                               : D2D1::ColorF(.11f, .14f, .19f, .98f),
        &propertyBackground);
    renderTarget_->CreateSolidColorBrush(
        button.pill ? D2D1::ColorF(.29f, .35f, .44f, 1.0f)
                    : D2D1::ColorF(.11f, .14f, .19f, .98f),
        &propertyOutline);
    const float radius = button.pill ? 18.0f : 8.0f;
    const D2D1_ROUNDED_RECT rounded = D2D1::RoundedRect(ToD2D(button.rect), radius, radius);
    renderTarget_->FillRoundedRectangle(rounded, propertyBackground.Get());
    if (button.pill) {
      if (!enabled) renderTarget_->DrawRoundedRectangle(rounded, propertyOutline.Get(), 1.0f);
      DrawText(button.label, ToD2D(button.rect), 10,
               enabled ? D2D1::ColorF(.98f, .99f, 1.0f, 1.0f)
                       : D2D1::ColorF(.77f, .83f, .92f, 1.0f));
    } else {
      DrawPropertyIcon(button.action, button.rect);
    }
  }
}

void CaptureOverlay::UpdateTooltip(POINT point) {
  tooltipVisible_ = false;
  tooltipText_.clear();
  if (!editing_) return;
  static constexpr std::array<const wchar_t*, 9> toolTips{{
      L"画笔", L"矩形", L"圆形", L"直线", L"箭头", L"文字", L"马赛克画笔", L"马赛克矩形", L"选择/调整"}};
  for (size_t index = 0; index < toolButtons_.size(); ++index) {
    if (Contains(ToolbarToolRect(index), point)) {
      tooltipText_ = toolTips[index]; tooltipVisible_ = true; return;
    }
  }
  if (HitCopy(point)) { tooltipText_ = L"复制到剪贴板"; tooltipVisible_ = true; return; }
  if (HitSave(point)) { tooltipText_ = L"保存为图片"; tooltipVisible_ = true; return; }
  if (HasSizeControl() && Contains(SizeSliderRect(), point)) {
    tooltipText_ = tool_ == Tool::MosaicBrush ? L"画笔尺寸" : tool_ == Tool::Text ? L"文字大小" : L"描边尺寸";
    tooltipVisible_ = true; return;
  }
  if (HitTestColorPreset(point)) return;  // palette dots intentionally have no tooltip
  if (HitTestToolbarMoreColor(point)) { tooltipText_ = L"更多颜色"; tooltipVisible_ = true; return; }
  if (HitTestFillColorPreset(point)) return;  // fill palette dots intentionally have no tooltip
  if (HitTestToolbarFillMoreColor(point)) { tooltipText_ = L"更多颜色"; tooltipVisible_ = true; return; }
  if (const auto property = HitTestProperty(point)) {
    switch (*property) {
      case PropertyAction::Color: tooltipText_ = L"更多颜色"; break;
      case PropertyAction::Opacity: tooltipText_ = L"透明度"; break;
      case PropertyAction::FillToggle: tooltipText_ = L"填充开关"; break;
      case PropertyAction::FillColor: tooltipText_ = L"填充颜色"; break;
      case PropertyAction::FillOpacity: tooltipText_ = L"填充透明度"; break;
      case PropertyAction::MosaicStyle: tooltipText_ = L"马赛克模式"; break;
      case PropertyAction::MosaicStrength: tooltipText_ = L"马赛克强度"; break;
      case PropertyAction::TextOrientation: tooltipText_ = L"文字方向"; break;
      case PropertyAction::TextShadow: tooltipText_ = L"文字阴影开关"; break;
      case PropertyAction::FrameToggle: tooltipText_ = L"截图外框开关"; break;
      default: break;
    }
    tooltipVisible_ = !tooltipText_.empty();
    return;
  }
  if (Contains(ToolbarRect(), point)) {
    tooltipText_ = L"拖动工具栏";
    tooltipVisible_ = true;
  }
}

void CaptureOverlay::DrawTooltip() {
  if (!tooltipVisible_ || tooltipText_.empty()) return;
  const RECT toolbar = ToolbarRect();
  const int width = std::clamp(static_cast<int>(tooltipText_.size()) * 14 + 24, 76, 280);
  const int height = 28;
  int left = toolbar.left + (toolbar.right - toolbar.left - width) / 2;
  left = std::clamp(left, 4, std::max(4, static_cast<int>(snapshot_.width) - width - 4));
  int top = toolbar.bottom + 5;
  if (top + height > static_cast<int>(snapshot_.height) - 4)
    top = std::max(4, static_cast<int>(toolbar.top) - height - 5);
  RECT rect{left, top, left + width, top + height};
  ComPtr<ID2D1SolidColorBrush> background, border;
  renderTarget_->CreateSolidColorBrush(D2D1::ColorF(.03f, .05f, .08f, .96f), &background);
  renderTarget_->CreateSolidColorBrush(D2D1::ColorF(.28f, .54f, .82f, .98f), &border);
  renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(ToD2D(rect), 7, 7), background.Get());
  renderTarget_->DrawRoundedRectangle(D2D1::RoundedRect(ToD2D(rect), 7, 7), border.Get(), 1.0f);
  DrawText(tooltipText_, ToD2D(rect), 12, D2D1::ColorF(.92f, .96f, 1.0f, 1.0f));
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
  for (size_t index = 0; index < toolButtons_.size(); ++index) {
    if (Contains(ToolbarToolRect(index), point)) return toolButtons_[index].tool;
  }
  return std::nullopt;
}

std::vector<CaptureOverlay::PropertyButton> CaptureOverlay::PropertyButtons() const {
  const RECT toolbar = ToolbarRect();
  int x = toolbar.left + 12;
  const int top = toolbar.top + kToolbarPropertyTop;
  const int bottom = top + kToolbarSecondaryHeight;
  std::vector<PropertyButton> result;
  auto add = [&](PropertyAction action, std::wstring label, bool pill = false, bool slider = false,
                 int customWidth = 0) {
    const int width = customWidth > 0 ? customWidth : pill ? kToolbarPillWidth : kToolbarPropertySize;
    result.push_back({action, {x, top, x + width, bottom}, std::move(label), pill, slider});
    x += width + kToolbarPropertyGap;
  };
  if (tool_ == Tool::Select) return result;
  if (tool_ == Tool::MosaicBrush || tool_ == Tool::MosaicRectangle) {
    const MosaicStyle style = ActiveMosaic() ? ActiveMosaic()->style : config_.mosaicStyle;
    add(PropertyAction::MosaicStyle, style == MosaicStyle::Pixel ? L"像素" : L"模糊", true);
    add(PropertyAction::MosaicStrength, L"", false, true, 180);
    return result;
  }
  add(PropertyAction::Opacity, L"", false, true, 140);
  if (tool_ == Tool::Text) {
    add(PropertyAction::TextOrientation, ActiveTextStyle()->vertical ? L"竖排" : L"横排", true);
    add(PropertyAction::TextShadow, ActiveTextStyle()->shadow ? L"阴影开" : L"阴影关", true);
  } else if (tool_ == Tool::Rectangle || tool_ == Tool::Ellipse) {
    const ShapeSetting* shape = ActiveShape();
    add(PropertyAction::FillToggle, shape && shape->fillOpacity > 0.0f ? L"填充开" : L"填充关", true);
    if (shape && shape->fillOpacity > 0.0f) {
      add(PropertyAction::FillColor, L"填充颜色");
      add(PropertyAction::FillOpacity, L"", false, true, 140);
    }
  }
  if ((tool_ == Tool::Rectangle || tool_ == Tool::Ellipse) && ActiveFillOpacity() > 0.0f) {
    constexpr int kFillPresetWidth = 9 * 18;
    bool afterFillColor = false;
    for (PropertyButton& button : result) {
      if (button.action == PropertyAction::FillColor) {
        button.rect.right += kFillPresetWidth;
        afterFillColor = true;
      } else if (afterFillColor) {
        OffsetRect(&button.rect, kFillPresetWidth, 0);
      }
    }
  }
  return result;
}

std::optional<CaptureOverlay::PropertyAction> CaptureOverlay::HitTestProperty(POINT point) const {
  for (const PropertyButton& button : PropertyButtons()) {
    if (button.action == PropertyAction::FillColor) continue;
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
    case PropertyAction::FillToggle: {
      if (ShapeSetting* shape = ActiveShape()) {
        shape->fillOpacity = shape->fillOpacity > 0.0f ? 0.0f : 0.5f;
        if (shape->fillOpacity > 0.0f && (shape->fill.rgba & 0xFFu) == 0) shape->fill.rgba |= 0xFFu;
        if (configChanged_) configChanged_();
      }
      break;
    }
    case PropertyAction::MosaicStyle:
      if (MosaicCommand* mosaic = ActiveMosaic())
        mosaic->style = mosaic->style == MosaicStyle::Pixel ? MosaicStyle::Blur : MosaicStyle::Pixel;
      else config_.mosaicStyle = config_.mosaicStyle == MosaicStyle::Pixel ? MosaicStyle::Blur : MosaicStyle::Pixel;
      if (configChanged_) configChanged_();
      break;
    case PropertyAction::MosaicStrength:
      break;
    case PropertyAction::FrameToggle:
      config_.frameEnabled = !config_.frameEnabled;
      if (configChanged_) configChanged_();
      break;
    case PropertyAction::TextOrientation:
      ActiveTextStyle()->vertical = !ActiveTextStyle()->vertical;
      if (configChanged_) configChanged_();
      break;
    case PropertyAction::TextShadow:
      ActiveTextStyle()->shadow = !ActiveTextStyle()->shadow;
      if (configChanged_) configChanged_();
      break;
  }
  InvalidateRect(hwnd_, nullptr, FALSE);
}

bool CaptureOverlay::HitCopy(POINT point) const {
  return Contains(ToolbarCopyRect(), point);
}

bool CaptureOverlay::HitSave(POINT point) const {
  return Contains(ToolbarSaveRect(), point);
}

PointF CaptureOverlay::ToSelectionPoint(POINT point) const { return {static_cast<float>(point.x - selection_.left), static_cast<float>(point.y - selection_.top)}; }

TextSetting* CaptureOverlay::ActiveTextStyle() {
  if (selectedCommand_ && tool_ == Tool::Text) {
    if (EditCommand* command = document_.At(*selectedCommand_)) {
      if (auto* text = std::get_if<TextCommand>(command)) return &text->style;
    }
  }
  return &config_.text;
}

const TextSetting* CaptureOverlay::ActiveTextStyle() const {
  if (selectedCommand_ && tool_ == Tool::Text) {
    if (const EditCommand* command = document_.At(*selectedCommand_)) {
      if (const auto* text = std::get_if<TextCommand>(command)) return &text->style;
    }
  }
  return &config_.text;
}

ShapeSetting* CaptureOverlay::ActiveShape() {
  if (selectedCommand_ && (tool_ == Tool::Rectangle || tool_ == Tool::Ellipse)) {
    if (EditCommand* command = document_.At(*selectedCommand_)) {
      if (auto* shape = std::get_if<ShapeCommand>(command); shape && CommandMatchesTool(*command, tool_))
        return &shape->style;
    }
  }
  if (tool_ == Tool::Rectangle) return &config_.rectangle;
  if (tool_ == Tool::Ellipse) return &config_.ellipse;
  return nullptr;
}

const ShapeSetting* CaptureOverlay::ActiveShape() const {
  if (selectedCommand_ && (tool_ == Tool::Rectangle || tool_ == Tool::Ellipse)) {
    if (const EditCommand* command = document_.At(*selectedCommand_)) {
      if (const auto* shape = std::get_if<ShapeCommand>(command); shape && CommandMatchesTool(*command, tool_))
        return &shape->style;
    }
  }
  if (tool_ == Tool::Rectangle) return &config_.rectangle;
  if (tool_ == Tool::Ellipse) return &config_.ellipse;
  return nullptr;
}

MosaicCommand* CaptureOverlay::ActiveMosaic() {
  if (selectedCommand_ && (tool_ == Tool::MosaicBrush || tool_ == Tool::MosaicRectangle)) {
    if (EditCommand* command = document_.At(*selectedCommand_)) {
      if (auto* mosaic = std::get_if<MosaicCommand>(command); mosaic && CommandMatchesTool(*command, tool_))
        return mosaic;
    }
  }
  return nullptr;
}

const MosaicCommand* CaptureOverlay::ActiveMosaic() const {
  if (selectedCommand_ && (tool_ == Tool::MosaicBrush || tool_ == Tool::MosaicRectangle)) {
    if (const EditCommand* command = document_.At(*selectedCommand_)) {
      if (const auto* mosaic = std::get_if<MosaicCommand>(command); mosaic && CommandMatchesTool(*command, tool_))
        return mosaic;
    }
  }
  return nullptr;
}

StrokeSetting* CaptureOverlay::ActiveStroke() {
  if (selectedCommand_) {
    if (EditCommand* command = document_.At(*selectedCommand_)) {
      if (tool_ == Tool::Pen) {
        if (auto* pen = std::get_if<PenCommand>(command)) return &pen->style;
      } else if (auto* shape = std::get_if<ShapeCommand>(command); shape && CommandMatchesTool(*command, tool_)) {
        return &shape->style.stroke;
      }
    }
  }
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
  if (!HasSizeControl()) return;
  const float step = tool_ == Tool::MosaicBrush ? 4.0f :
                     tool_ == Tool::Text ? 2.0f : 1.0f;
  SetActiveSize(ActiveSize() + delta * step);
}

void CaptureOverlay::CycleActiveOpacity() {
  if (tool_ == Tool::Text) {
    TextSetting* style = ActiveTextStyle();
    style->opacity -= .25f;
    if (style->opacity < .24f) style->opacity = 1.0f;
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
  ShapeSetting* shape = ActiveShape();
  if (shape && ChooseColorFor(hwnd_, shape->fill) && configChanged_) configChanged_();
}

void CaptureOverlay::CycleFillOpacity() {
  ShapeSetting* shape = ActiveShape();
  if (!shape) return;
  shape->fillOpacity += .25f;
  if (shape->fillOpacity > 1.0f) shape->fillOpacity = 0.0f;
  if (configChanged_) configChanged_();
}

float CaptureOverlay::ActiveSize() const {
  if (tool_ == Tool::Text) return ActiveTextStyle()->size;
  if (tool_ == Tool::MosaicBrush) {
    if (const MosaicCommand* mosaic = ActiveMosaic()) return mosaic->brushSize;
    return config_.mosaicBrushSize;
  }
  if (const StrokeSetting* stroke = const_cast<CaptureOverlay*>(this)->ActiveStroke()) return stroke->width;
  return 1.0f;
}

void CaptureOverlay::SetActiveSize(float size) {
  if (!HasSizeControl()) return;
  if (tool_ == Tool::Text) ActiveTextStyle()->size = std::clamp(size, 8.0f, 256.0f);
  else if (tool_ == Tool::MosaicBrush && ActiveMosaic())
    ActiveMosaic()->brushSize = std::clamp(size, 4.0f, 256.0f);
  else if (tool_ == Tool::MosaicBrush)
    config_.mosaicBrushSize = std::clamp(size, 4.0f, 256.0f);
  else if (StrokeSetting* stroke = ActiveStroke()) stroke->width = std::clamp(size, 1.0f, 128.0f);
  else return;
  if (configChanged_) configChanged_();
  InvalidateRect(hwnd_, nullptr, FALSE);
}

ColorSetting* CaptureOverlay::ActiveColor() {
  if (tool_ == Tool::Text) return &ActiveTextStyle()->color;
  if (StrokeSetting* stroke = ActiveStroke()) return &stroke->color;
  return nullptr;
}

float CaptureOverlay::ActiveOpacity() const {
  if (tool_ == Tool::Text) return ActiveTextStyle()->opacity;
  if (const StrokeSetting* stroke = const_cast<CaptureOverlay*>(this)->ActiveStroke()) return stroke->opacity;
  return 1.0f;
}

void CaptureOverlay::SetActiveOpacity(float opacity) {
  opacity = std::clamp(opacity, 0.0f, 1.0f);
  if (tool_ == Tool::Text) ActiveTextStyle()->opacity = opacity;
  else if (StrokeSetting* stroke = ActiveStroke()) stroke->opacity = opacity;
  else return;
  if (configChanged_) configChanged_();
  InvalidateRect(hwnd_, nullptr, FALSE);
}

float CaptureOverlay::ActiveFillOpacity() const {
  const ShapeSetting* shape = ActiveShape();
  return shape ? shape->fillOpacity : 0.0f;
}

void CaptureOverlay::SetActiveFillOpacity(float opacity) {
  ShapeSetting* shape = ActiveShape();
  if (!shape) return;
  shape->fillOpacity = std::clamp(opacity, 0.0f, 1.0f);
  if (configChanged_) configChanged_();
  InvalidateRect(hwnd_, nullptr, FALSE);
}

float CaptureOverlay::ActiveMosaicStrength() const {
  const MosaicStyle style = ActiveMosaic() ? ActiveMosaic()->style : config_.mosaicStyle;
  if (const MosaicCommand* mosaic = ActiveMosaic()) return style == MosaicStyle::Pixel
                                                         ? static_cast<float>(mosaic->pixelSize)
                                                         : mosaic->blurRadius;
  return style == MosaicStyle::Pixel ? static_cast<float>(config_.mosaicPixelSize) : config_.mosaicBlurRadius;
}

void CaptureOverlay::SetActiveMosaicStrength(float value) {
  const MosaicStyle style = ActiveMosaic() ? ActiveMosaic()->style : config_.mosaicStyle;
  if (MosaicCommand* mosaic = ActiveMosaic()) {
    if (style == MosaicStyle::Pixel) mosaic->pixelSize = std::clamp(static_cast<int>(std::lround(value)), 2, 128);
    else mosaic->blurRadius = std::clamp(value, 1.0f, 64.0f);
  } else if (style == MosaicStyle::Pixel) config_.mosaicPixelSize = std::clamp(static_cast<int>(std::lround(value)), 2, 128);
  else config_.mosaicBlurRadius = std::clamp(value, 1.0f, 64.0f);
  if (configChanged_) configChanged_();
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void CaptureOverlay::SetOpacityFromSlider(POINT point, const RECT& slider) {
  const float ratio = std::clamp((point.x - slider.left - 5.0f) /
                                 std::max(1.0f, static_cast<float>(slider.right - slider.left - 10)), 0.0f, 1.0f);
  SetActiveOpacity(ratio);
}

void CaptureOverlay::SetFillOpacityFromSlider(POINT point, const RECT& slider) {
  const float ratio = std::clamp((point.x - slider.left - 5.0f) /
                                 std::max(1.0f, static_cast<float>(slider.right - slider.left - 10)), 0.0f, 1.0f);
  SetActiveFillOpacity(ratio);
}

void CaptureOverlay::SetMosaicStrengthFromSlider(POINT point, const RECT& slider) {
  const bool blur = (ActiveMosaic() ? ActiveMosaic()->style : config_.mosaicStyle) == MosaicStyle::Blur;
  const float minimum = blur ? 1.0f : 2.0f;
  const float maximum = blur ? 64.0f : 128.0f;
  const float ratio = std::clamp((point.x - slider.left - 5.0f) /
                                 std::max(1.0f, static_cast<float>(slider.right - slider.left - 10)), 0.0f, 1.0f);
  SetActiveMosaicStrength(std::round(minimum + ratio * (maximum - minimum)));
}

bool CaptureOverlay::HasSizeControl() const {
  return tool_ != Tool::Select && tool_ != Tool::MosaicRectangle;
}

RECT CaptureOverlay::SizeSliderRect() const {
  const RECT toolbar = ToolbarRect();
  return {toolbar.left + 44, toolbar.top + 57, toolbar.left + 170, toolbar.top + 79};
}

void CaptureOverlay::SetSizeFromSlider(POINT point) {
  if (!HasSizeControl()) return;
  const RECT slider = SizeSliderRect();
  const float ratio = std::clamp((point.x - slider.left) /
                                 static_cast<float>(slider.right - slider.left), 0.0f, 1.0f);
  const float minimum = tool_ == Tool::Text ? 8.0f :
                        (tool_ == Tool::MosaicBrush ? 4.0f : 1.0f);
  const float maximum = tool_ == Tool::MosaicBrush ? 256.0f :
                        (tool_ == Tool::Text ? 256.0f : 128.0f);
  SetActiveSize(std::round(minimum + ratio * (maximum - minimum)));
}

std::optional<size_t> CaptureOverlay::HitTestColorPreset(POINT point) const {
  if (!const_cast<CaptureOverlay*>(this)->ActiveColor()) return std::nullopt;
  for (size_t i = 0; i < kPresetColors.size(); ++i) {
    if (Contains(ToolbarPresetRect(i), point)) return i;
  }
  return std::nullopt;
}

std::optional<size_t> CaptureOverlay::HitTestFillColorPreset(POINT point) const {
  if ((tool_ != Tool::Rectangle && tool_ != Tool::Ellipse) || ActiveFillOpacity() <= 0.0f) return std::nullopt;
  for (size_t i = 0; i < kPresetColors.size(); ++i) {
    if (Contains(ToolbarFillPresetRect(i), point)) return i;
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

void CaptureOverlay::SetActiveFillPresetColor(size_t index) {
  ShapeSetting* shape = ActiveShape();
  if (!shape || index >= kPresetColors.size()) return;
  shape->fill.rgba = kPresetColors[index];
  if (configChanged_) configChanged_();
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void CaptureOverlay::ShowEditorContextMenu(POINT point) {
  HMENU menu = CreatePopupMenu();
  HMENU tools = CreatePopupMenu();
  HMENU colors = CreatePopupMenu();
  HMENU sizes = HasSizeControl() ? CreatePopupMenu() : nullptr;
  HMENU opacity = CreatePopupMenu();
  if (!menu || !tools || !colors || (HasSizeControl() && !sizes) || !opacity) return;
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
  if (HasSizeControl()) {
    for (size_t i = 0; i < kContextSizes.size(); ++i) {
      const std::wstring label = std::to_wstring(static_cast<int>(kContextSizes[i])) + L" px";
      AppendMenuW(sizes, MF_STRING, kContextSizeBase + static_cast<UINT>(i), label.c_str());
    }
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sizes), L"尺寸");
  }
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
    const MosaicStyle activeStyle = ActiveMosaic() ? ActiveMosaic()->style : config_.mosaicStyle;
    AppendMenuW(style, MF_STRING | (activeStyle == MosaicStyle::Pixel ? MF_CHECKED : 0),
                kContextMosaicPixel, L"像素化");
    AppendMenuW(style, MF_STRING | (activeStyle == MosaicStyle::Blur ? MF_CHECKED : 0),
                kContextMosaicBlur, L"高斯模糊");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(style), L"马赛克样式");
  }
  if (tool_ == Tool::Text) {
    HMENU orientation = CreatePopupMenu();
    const bool vertical = ActiveTextStyle()->vertical;
    AppendMenuW(orientation, MF_STRING | (!vertical ? MF_CHECKED : 0),
                kContextTextHorizontal, L"横排");
    AppendMenuW(orientation, MF_STRING | (vertical ? MF_CHECKED : 0),
                kContextTextVertical, L"竖排");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(orientation), L"文字方向");
    AppendMenuW(menu, MF_STRING | (ActiveTextStyle()->shadow ? MF_CHECKED : 0),
                kContextTextShadow, L"文字阴影");
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
    if (tool_ == Tool::Text) ActiveTextStyle()->opacity = value;
    else if (StrokeSetting* stroke = ActiveStroke()) stroke->opacity = value;
    if (configChanged_) configChanged_();
  } else if (command >= kContextFillOpacityBase && command <= kContextFillOpacityBase + kContextOpacities.size()) {
    ShapeSetting* shape = ActiveShape();
    if (!shape) return;
    shape->fillOpacity = command == kContextFillOpacityBase ? 0.0f :
        kContextOpacities[command - kContextFillOpacityBase - 1];
    if (configChanged_) configChanged_();
  } else if (command == kContextMosaicPixel || command == kContextMosaicBlur) {
    if (MosaicCommand* mosaic = ActiveMosaic())
      mosaic->style = command == kContextMosaicPixel ? MosaicStyle::Pixel : MosaicStyle::Blur;
    else config_.mosaicStyle = command == kContextMosaicPixel ? MosaicStyle::Pixel : MosaicStyle::Blur;
    if (configChanged_) configChanged_();
  } else if (command == kContextTextHorizontal || command == kContextTextVertical) {
    ActiveTextStyle()->vertical = command == kContextTextVertical;
    if (configChanged_) configChanged_();
  } else if (command == kContextTextShadow) {
    ActiveTextStyle()->shadow = !ActiveTextStyle()->shadow;
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
  const int lineHeight = std::max(28, static_cast<int>(std::lround(textInputStyle_.size * 1.45f)));
  const int selectionLeft = static_cast<int>(selection_.left);
  const int selectionTop = static_cast<int>(selection_.top);
  const int x = std::clamp(static_cast<int>(point.x), selectionLeft,
                           std::max(selectionLeft, static_cast<int>(selection_.right) - 2));
  const int y = std::clamp(static_cast<int>(point.y), selectionTop,
                           std::max(selectionTop, static_cast<int>(selection_.bottom) - 2));
  // The EDIT is an invisible keyboard/IME host.  DrawDocument renders the
  // current value through the same DWrite path used by committed text.
  textEdit_ = CreateWindowExW(WS_EX_TRANSPARENT, L"EDIT", L"",
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN |
          ES_NOHIDESEL,
      x, y, 2, 2, hwnd_, nullptr, instance_, nullptr);
  if (!textEdit_) return;
  LOGFONTW font{};
  font.lfHeight = -std::max(8L, static_cast<LONG>(std::lround(textInputStyle_.size)));
  font.lfWeight = FW_NORMAL;
  font.lfQuality = CLEARTYPE_QUALITY;
  wcsncpy_s(font.lfFaceName, textInputStyle_.fontFamily.c_str(), _TRUNCATE);
  textEditFont_ = CreateFontIndirectW(&font);
  SendMessageW(textEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(textEditFont_), TRUE);
  SendMessageW(textEdit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELONG(8, 8));
  if (!initialValue.empty()) SetWindowTextW(textEdit_, initialValue.c_str());
  SetWindowSubclass(textEdit_, TextEditProc, 1, reinterpret_cast<DWORD_PTR>(this));
  SetFocus(textEdit_);
  textImeComposing_ = false;
  SendMessageW(textEdit_, EM_SETSEL, 0, -1);
  if (HIMC context = ImmGetContext(textEdit_)) {
    COMPOSITIONFORM composition{};
    composition.dwStyle = CFS_POINT;
    composition.ptCurrentPos = {0, lineHeight};
    ImmSetCompositionWindow(context, &composition);
    CANDIDATEFORM candidate{};
    candidate.dwIndex = 0;
    candidate.dwStyle = CFS_CANDIDATEPOS;
    candidate.ptCurrentPos = {0, lineHeight};
    ImmSetCandidateWindow(context, &candidate);
    ImmReleaseContext(textEdit_, context);
  }
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
    selectedCommand_ = document_.Size() - 1;
  }
  textEditingCommand_.reset();
  textImeComposing_ = false;
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
  textImeComposing_ = false;
  if (textEditFont_) { DeleteObject(textEditFont_); textEditFont_ = nullptr; }
  if (hwnd_) { SetFocus(hwnd_); InvalidateRect(hwnd_, nullptr, FALSE); }
}

LRESULT CALLBACK CaptureOverlay::TextEditProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
                                               UINT_PTR subclassId, DWORD_PTR referenceData) {
  (void)subclassId;
  auto* self = reinterpret_cast<CaptureOverlay*>(referenceData);
  const auto invalidateText = [self]() {
    if (!self || !self->hwnd_) return;
    RECT dirty = self->selection_;
    if (HasArea(dirty)) {
      InflateRect(&dirty, 8, 8);
      InvalidateRect(self->hwnd_, &dirty, FALSE);
    } else {
      InvalidateRect(self->hwnd_, nullptr, FALSE);
    }
  };
  if (message == WM_GETDLGCODE) return DLGC_WANTALLKEYS;
  if (message == WM_IME_STARTCOMPOSITION) {
    self->textImeComposing_ = true;
    const LRESULT result = DefSubclassProc(hwnd, message, wParam, lParam);
    invalidateText();
    return result;
  }
  if (message == WM_IME_ENDCOMPOSITION) {
    const LRESULT result = DefSubclassProc(hwnd, message, wParam, lParam);
    self->textImeComposing_ = false;
    invalidateText();
    return result;
  }
  if (message == WM_IME_COMPOSITION) {
    const LRESULT result = DefSubclassProc(hwnd, message, wParam, lParam);
    if (lParam & GCS_RESULTSTR) self->textImeComposing_ = false;
    else if (lParam & GCS_COMPSTR) self->textImeComposing_ = true;
    invalidateText();
    return result;
  }
  if (message == WM_KEYDOWN && wParam == VK_RETURN &&
      (GetKeyState(VK_SHIFT) & 0x8000) == 0 && !self->textImeComposing_) {
    HIMC context = ImmGetContext(hwnd);
    const LONG composingLength = context ? ImmGetCompositionStringW(context, GCS_COMPSTR, nullptr, 0) : 0;
    if (context) ImmReleaseContext(hwnd, context);
    if (composingLength <= 0) { self->CommitTextInput(); return 0; }
  }
  if (message == WM_KEYDOWN && wParam == VK_ESCAPE) { self->CancelTextInput(); return 0; }
  return DefSubclassProc(hwnd, message, wParam, lParam);
}

}  // namespace rc
