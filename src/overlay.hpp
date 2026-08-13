#pragma once

#include "capture.hpp"
#include "editor.hpp"
#include "unit_detector.hpp"

#include <d2d1.h>
#include <dwrite.h>

namespace rc {

enum class CaptureCompletion { Cancel, Copy, Save };
enum class SelectionMode { Normal, Window, Unit };

struct OverlayResult {
  CaptureCompletion completion = CaptureCompletion::Cancel;
  RECT selection{};  // virtual desktop coordinates
  EditorDocument document;
  bool windowSelection = false;
};

class CaptureOverlay {
 public:
  using CompletionCallback = std::function<void(OverlayResult)>;
  using ConfigChangedCallback = std::function<void()>;

  CaptureOverlay(HINSTANCE instance, DesktopSnapshot snapshot, AppConfig& config,
                 CompletionCallback completion, ConfigChangedCallback configChanged,
                 std::optional<RECT> targetWorkArea = std::nullopt);
  CaptureOverlay(HINSTANCE instance, std::vector<DesktopSnapshot> snapshots, AppConfig& config,
                 CompletionCallback completion, ConfigChangedCallback configChanged,
                 std::optional<RECT> targetWorkArea = std::nullopt);
  ~CaptureOverlay();

  bool Show(std::wstring& error);
  HWND hwnd() const { return hwnd_; }
  const DesktopSnapshot& snapshot() const { return snapshot_; }
  // Lightweight geometry/state accessors used by the native smoke test.  They
  // intentionally expose no mutable overlay state and keep hit-testing on the
  // same dynamic icon rect used by production input handling.
  RECT SnapshotIconRectForTest() const { return SnapshotIconRect(); }
  RECT SnapshotPanelRectForTest() const { return SnapshotPanelRect(); }
  RECT SnapshotThumbnailRectForTest(size_t index) const { return SnapshotThumbnailRect(index); }
  RECT ToolbarRectForTest() const { return ToolbarRect(); }
  bool SnapshotSwitcherExpandedForTest() const {
    return snapshotsExpanded_ || snapshotsAnimationProgress_ > 0.01f;
  }

 private:
  static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
  LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
  bool CreateDeviceResources();
  void DiscardDeviceResources();
  void EnsureToolbarBackdrop();
  void Paint();
  void BeginSettingPreview(POINT point, bool timed = false);
  void EndSettingPreview();
  void DrawSettingPreview();
  void BeginUnitDetection();
  void UpdateHover(POINT point);
  void CycleMode();
  void BeginSelection(POINT point);
  void ContinueSelection(POINT point);
  void EndSelection(POINT point);
  void BeginEditGesture(POINT point);
  void ContinueEditGesture(POINT point);
  void EndEditGesture(POINT point);
  void SelectTool(Tool tool);
  void Complete(CaptureCompletion completion);
  void Cancel();
  void DrawDocument();
  bool DrawMosaicLayer(std::span<const EditCommand> commands);
  enum class SelectionAdjustment {
    None, Move, Left, Right, Top, Bottom, TopLeft, TopRight, BottomLeft, BottomRight
  };
  SelectionAdjustment HitTestSelectionAdjustment(POINT point) const;
  void ContinueSelectionAdjustment(POINT point);
  void DrawSelectionHandles();
  void DrawCommandHandles();
  void DrawToolIcon(Tool tool, const RECT& rect, bool active);
  void DrawActionIcon(bool save, const RECT& rect);
  void DrawToolbar();
  void DrawSnapshotSwitcher();
  void EnsureSnapshotThumbnails();
  void DiscardSnapshotThumbnails();
  void SetActiveSnapshot(size_t index, bool collapse = false, bool refreshDetection = true);
  void RestoreHoverSnapshot(bool refreshDetection = false);
  bool HitSnapshotIcon(POINT point) const;
  bool HitSnapshotPanel(POINT point) const;
  std::optional<size_t> HitSnapshotThumbnail(POINT point) const;
  RECT SnapshotIconRect() const;
  RECT SnapshotDockTargetRect() const;
  void BeginSnapshotDockAnimation();
  RECT SnapshotPanelRect() const;
  RECT SnapshotThumbnailRect(size_t index) const;
  RECT SnapshotTargetRect() const;
  struct SnapshotLayout {
    RECT panel{};
    int columns = 1;
    int rows = 1;
    int thumbWidth = 1;
    int thumbHeight = 1;
    int gap = 1;
    bool opensDownward = false;
  };
  SnapshotLayout SnapshotLayoutFor() const;
  const DesktopSnapshot& SnapshotAt(size_t index) const;
  DesktopSnapshot& SnapshotAt(size_t index);
  void StopUnitDetection();
  void ResetUnitDetection();
  void SuppressUnitDetection();
  void FinishUnitDetectionMessage();
  const DesktopSnapshot& ActiveSnapshot() const;
  DesktopSnapshot& ActiveSnapshot();
  void DrawTooltip();
  void UpdateTooltip(POINT point);
  void DrawTextCommand(const TextCommand& command);
  void DrawText(std::wstring_view text, const D2D1_RECT_F& rect, float size,
                D2D1_COLOR_F color, DWRITE_TEXT_ALIGNMENT alignment = DWRITE_TEXT_ALIGNMENT_CENTER,
                DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL);
  std::optional<Tool> HitTestTool(POINT point) const;
  enum class PropertyAction {
    SizeDown, SizeUp, Color, Opacity, FillColor, FillOpacity, FillToggle,
    MosaicStyle, MosaicStrength, FrameToggle, TextOrientation, TextShadow
  };
  struct PropertyButton {
    PropertyAction action;
    RECT rect;
    std::wstring label;
    bool pill = false;
    bool slider = false;
  };
  void DrawPropertyIcon(PropertyAction action, const RECT& rect);
  std::vector<PropertyButton> PropertyButtons() const;
  std::optional<PropertyAction> HitTestProperty(POINT point) const;
  void ActivateProperty(PropertyAction action);
  void SetOpacityFromSlider(POINT point, const RECT& slider);
  void SetFillOpacityFromSlider(POINT point, const RECT& slider);
  void SetMosaicStrengthFromSlider(POINT point, const RECT& slider);
  bool HitCopy(POINT point) const;
  bool HitSave(POINT point) const;
  PointF ToSelectionPoint(POINT point) const;
  StrokeSetting* ActiveStroke();
  void AdjustActiveSize(float delta);
  void CycleActiveOpacity();
  void ChooseActiveColor();
  void ChooseFillColor();
  void CycleFillOpacity();
  TextSetting* ActiveTextStyle();
  const TextSetting* ActiveTextStyle() const;
  ShapeSetting* ActiveShape();
  const ShapeSetting* ActiveShape() const;
  MosaicCommand* ActiveMosaic();
  const MosaicCommand* ActiveMosaic() const;
  float ActiveSize() const;
  void SetActiveSize(float size);
  ColorSetting* ActiveColor();
  float ActiveOpacity() const;
  float ActiveFillOpacity() const;
  void SetActiveOpacity(float opacity);
  void SetActiveFillOpacity(float opacity);
  float ActiveMosaicStrength() const;
  void SetActiveMosaicStrength(float value);
  bool HasSizeControl() const;
  RECT SizeSliderRect() const;
  void SetSizeFromSlider(POINT point);
  std::optional<size_t> HitTestColorPreset(POINT point) const;
  void SetActivePresetColor(size_t index);
  void ShowEditorContextMenu(POINT point);
  std::optional<size_t> HitTestCommand(POINT point,
                                       std::optional<Tool> toolFilter = std::nullopt) const;
  SelectionAdjustment HitTestCommandAdjustment(POINT point) const;
  RectF SelectedCommandBounds() const;
  void ContinueCommandAdjustment(POINT point);
  void BeginTextInput(POINT point, std::optional<size_t> existingCommand = std::nullopt);
  void CommitTextInput();
  void CancelTextInput();
  static LRESULT CALLBACK TextEditProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
                                        UINT_PTR subclassId, DWORD_PTR referenceData);
  RECT ToolbarRect() const;
  RECT ToolbarWorkArea() const;
  RECT ToolbarToolRect(size_t index) const;
  RECT ToolbarCopyRect() const;
  RECT ToolbarSaveRect() const;
  RECT ToolbarMoreColorRect() const;
  RECT ToolbarPresetRect(size_t index) const;
  RECT ToolbarFillPresetRect(size_t index) const;
  RECT ToolbarFillMoreColorRect() const;
  bool HitTestToolbarMoreColor(POINT point) const;
  bool HitTestToolbarFillMoreColor(POINT point) const;
  std::optional<size_t> HitTestFillColorPreset(POINT point) const;
  void SetActiveFillPresetColor(size_t index);

  HINSTANCE instance_ = nullptr;
  HWND hwnd_ = nullptr;
  DesktopSnapshot snapshot_;
  RECT targetWorkArea_{};  // overlay-local work area used to anchor the switcher
  std::vector<DesktopSnapshot> snapshots_;
  size_t activeSnapshot_ = 0;
  std::optional<size_t> hoverSnapshot_;
  std::optional<size_t> hoverSnapshotPrevious_;
  bool snapshotRestorePending_ = false;
  bool snapshotsExpanded_ = false;
  bool snapshotsAnimating_ = false;
  float snapshotsAnimationProgress_ = 0.0f;
  float snapshotsAnimationFromProgress_ = 0.0f;
  std::chrono::steady_clock::time_point snapshotsAnimationStart_{};
  bool snapshotDocked_ = false;
  bool snapshotDockAnimating_ = false;
  float snapshotDockProgress_ = 0.0f;
  RECT snapshotDockStartRect_{};
  std::chrono::steady_clock::time_point snapshotDockAnimationStart_{};
  struct SnapshotThumbnail {
    ComPtr<ID2D1Bitmap> bitmap;
    int width = 0;
    int height = 0;
  };
  std::vector<SnapshotThumbnail> snapshotThumbnails_;
  AppConfig& config_;
  CompletionCallback completion_;
  ConfigChangedCallback configChanged_;

  ComPtr<ID2D1Factory> d2dFactory_;
  ComPtr<IDWriteFactory> dwriteFactory_;
  ComPtr<ID2D1HwndRenderTarget> renderTarget_;
  ComPtr<ID2D1Bitmap> desktopBitmap_;
  ComPtr<ID2D1Bitmap> mosaicPreviewBitmap_;
  ComPtr<ID2D1Bitmap> toolbarBackdropBitmap_;
  uint64_t mosaicPreviewSignature_ = 0;
  RECT toolbarBackdropRect_{};
  bool toolbarBackdropValid_ = false;
  std::vector<uint8_t> toolbarBackdropPixels_;
  int toolbarBackdropStride_ = 0;

  SelectionMode mode_ = SelectionMode::Normal;
  Tool tool_ = Tool::Pen;
  bool selecting_ = false;
  bool editing_ = false;
  bool drawing_ = false;
  bool sizeSliderDragging_ = false;
  std::optional<PropertyAction> propertySliderDragging_;
  bool toolbarDragging_ = false;
  bool toolbarPositionSet_ = false;
  bool settingPreview_ = false;
  POINT settingPreviewPoint_{};
  bool windowSelection_ = false;
  SelectionAdjustment selectionAdjustment_ = SelectionAdjustment::None;
  RECT selectionBeforeAdjust_{};
  POINT dragStart_{};
  POINT toolbarDragStart_{};
  POINT toolbarPositionStart_{};
  POINT toolbarPosition_{};
  POINT currentPoint_{};
  POINT lastCanvasPoint_{};
  std::chrono::steady_clock::time_point penLastSampleTime_{};
  float penWidthScale_ = 1.0f;
  RECT selection_{};  // overlay-local coordinates
  RECT hoverRect_{};
  EditorDocument document_;
  std::optional<EditCommand> previewCommand_;
  HWND textEdit_ = nullptr;
  PointF textOrigin_{};

  std::mutex unitMutex_;
  std::vector<WindowCandidate> windowCandidates_;
  std::vector<UnitCandidate> unitCandidates_;
  std::atomic<bool> unitReady_{false};
  std::atomic<bool> unitDetectionRunning_{false};
  std::atomic<bool> unitDetectionFinished_{false};
  bool unitDetectionSuppressed_ = false;
  std::jthread unitThread_;
  struct PendingSnapshotSwitch {
    size_t index = 0;
    bool collapse = false;
    bool refreshDetection = false;
  };
  std::optional<PendingSnapshotSwitch> pendingSnapshotSwitch_;
  std::optional<CaptureCompletion> pendingCompletion_;
  std::vector<size_t> hoverUnitChain_;
  size_t hoverUnitIndex_ = 0;

  struct ToolButton { Tool tool; };
  const std::array<ToolButton, 9> toolButtons_{{
      {Tool::Pen}, {Tool::Rectangle}, {Tool::Ellipse}, {Tool::Line}, {Tool::Arrow},
      {Tool::Text}, {Tool::MosaicBrush}, {Tool::MosaicRectangle}, {Tool::Select}}};

  std::optional<size_t> selectedCommand_;
  SelectionAdjustment commandAdjustment_ = SelectionAdjustment::None;
  RectF selectedCommandBeforeBounds_{};
  std::optional<EditCommand> commandBeforeAdjust_;
  std::optional<size_t> textEditingCommand_;
  TextSetting textInputStyle_{};
  HFONT textEditFont_ = nullptr;
  HBRUSH textEditBrush_ = nullptr;
  bool textImeComposing_ = false;
  std::wstring tooltipText_;
  bool tooltipVisible_ = false;
};

}  // namespace rc
