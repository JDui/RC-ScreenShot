#pragma once

#include "capture.hpp"
#include "editor.hpp"
#include "uia_detector.hpp"
#include "unit_detector.hpp"

#include <d2d1.h>
#include <dwrite.h>

namespace rc {

enum class CaptureCompletion { Cancel, Copy, Save };
enum class SelectionMode { Normal, Window, Unit };
// Direction of a scrolling long capture.  The wheel is sent in the matching
// direction and the stitched image grows at the matching edge.
enum class ScrollDirection { Up, Right, Down };

// Stitched pixels of a scrolling long screenshot.  When present in an
// OverlayResult, `selection` covers the whole image and the exporter renders
// against this buffer instead of the frozen desktop snapshot.
struct LongCaptureImage {
  int width = 0;
  int height = 0;
  int stride = 0;
  std::vector<uint8_t> bgra;
};

struct OverlayResult {
  CaptureCompletion completion = CaptureCompletion::Cancel;
  RECT selection{};  // virtual desktop coordinates
  EditorDocument document;
  bool windowSelection = false;
  std::shared_ptr<LongCaptureImage> longImage;
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
  RECT ToolbarLongEntryRectForTest() const { return ToolbarLongEntryRect(); }
  bool LongCaptureModeForTest() const { return longCaptureMode_; }
  bool SnapshotSwitcherExpandedForTest() const {
    return snapshotsExpanded_ || snapshotsAnimationProgress_ > 0.01f;
  }
  bool EditingForTest() const { return editing_; }

 private:
  // Preview float window stages and the drag gestures they support.  Declared
  // ahead of the method group so the stage-taking declarations can use them.
  enum class LongPreviewStage { View, Crop, Annotate };
  enum class LongPreviewDrag {
    None, CropMove, CropL, CropR, CropT, CropB, CropTL, CropTR, CropBL, CropBR,
    Annotate, CommandAdjust
  };
  static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
  LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
  bool CreateDeviceResources();
  void DiscardDeviceResources();
  void EnsureToolbarBackdrop();
  void Paint();
  void BeginSettingPreview(POINT point, bool timed = false);
  void EndSettingPreview();
  void DrawSettingPreview();
  void BeginWindowEnumeration();
  void FinishWindowEnumerationMessage();
  void BeginUnitDetection();
  void ScheduleUiaQuery(POINT point);
  void BeginUiaQuery();
  void FinishUiaQueryMessage();
  void StopUiaQuery();
  void SetHoverTarget(RECT target, bool immediate = false);
  void AdvanceHoverAnimation();
  void UpdateHover(POINT point);
  std::optional<size_t> HitUnitLevelBorder(POINT point) const;
  void DrawUnitLevelBoxes();
  void CycleMode();
  void HandleEscape();
  void ReturnToSelection();
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
  SelectionAdjustment HitTestSelectionHandle(POINT point) const;
  void ContinueSelectionAdjustment(POINT point);
  void DrawSelectionHandles();
  void DrawCommandHandles();
  void DrawToolIcon(Tool tool, const RECT& rect, bool active);
  void DrawActionIcon(bool save, const RECT& rect);
  void DrawToolbar();
  // Utility buttons (gray back / red close) that live in a dedicated topmost
  // strip of every bar except the capturing bar.
  RECT UtilityBackRect() const;
  RECT UtilityCloseRect() const;
  bool UtilityButtonsVisible() const;
  bool HitUtilityBack(POINT point) const;
  bool HitUtilityClose(POINT point) const;
  void UtilityBack();
  void DrawBarUtilityButtons(float alpha = 1.0f);
  // Long (scrolling) capture: rainbow toolbar entry, the three-direction bar,
  // the capturing bar and the live preview panel beside the selection.
  void EnterLongCaptureMode();
  void ExitLongCaptureMode();
  void DrawLongCaptureBar(const RECT& toolbar);
  void DrawCapturingBar(const RECT& toolbar);
  void DrawRainbowPanel(const D2D1_RECT_F& rect, float radius);
  // Primary-action panel for the long-capture controls: blue accent gradient
  // with the same finish as the rainbow panel, which stays exclusive to the
  // main-bar long-capture entry.
  void DrawAccentPanel(const D2D1_RECT_F& rect, float radius);
  // Neutral dark companion panel for the non-primary long-capture buttons.
  void DrawNeutralPanel(const D2D1_RECT_F& rect, float radius);
  void DrawLongCaptureGlyph(const D2D1_RECT_F& rect);
  void DrawScrollDirectionGlyph(ScrollDirection direction, const D2D1_RECT_F& rect);
  void DrawLongActionGlyph(bool finish, const D2D1_RECT_F& rect);
  void DrawScrollPreviewPanel();
  RECT ScrollPreviewPanelRect() const;
  RECT ToolbarLongEntryRect() const;
  RECT ToolbarLongUpRect() const;
  RECT ToolbarLongRightRect() const;
  RECT ToolbarLongDownRect() const;
  RECT ToolbarLongFinishRect() const;
  RECT ToolbarLongCancelRect() const;
  bool HitLongEntry(POINT point) const;
  bool IsBurstSession() const;  // burst (multi-frame) captures never offer long capture
  bool HitLongDirection(ScrollDirection direction, POINT point) const;
  bool HitLongFinish(POINT point) const;
  bool HitLongCancel(POINT point) const;
  void BeginScrollCapture(ScrollDirection direction);
  void ScrollTick();
  void ScrollCaptureStep();
  // True when the captured region is still changing: the page is mid-animation
  // and the frame is a blend of two scroll positions.
  bool ScrollPageStillMoving(const DesktopSnapshot& frame);
  void AppendScrollRows(const uint8_t* rows, int count, int sourceStride);
  void PrependScrollRows(const uint8_t* rows, int count, int sourceStride);
  void AppendScrollColumns(const uint8_t* frame, int frameStride, int fromColumn, int count);
  ComPtr<ID2D1Bitmap> EnsureScrollPreviewBitmap(int width, int height);
  void UpdateScrollPreviewTail(int appendedFrom);
  void UpdateScrollPreviewHead();
  void UpdateScrollPreviewFull();
  HWND FindScrollTarget(POINT point) const;
  void ApplyScrollCaptureRegion();
  void ResetScrollCaptureRegion();
  void SendScrollWheel();
  void FinishScrollCapture();
  void CancelScrollCapture();
  // Post-capture long-screenshot review: the standard toolbar stays in place
  // but its tool row swaps to long-capture editing (crop, pattern tools,
  // clipboard/save), with a zoom slider and wheel scrolling over the image.
  void EnterLongPreview();
  void ExitLongPreviewToBar();
  void DrawLongPreview();
  void AdvanceLongPreviewAnimation();
  void BeginLongPreviewStage(LongPreviewStage stage);
  void DrawLongPreviewGlyph(int kind, const D2D1_RECT_F& rect);
  void DrawLongToolbar();
  D2D1_RECT_F LongPreviewViewportRect() const;
  RECT ResolveLongPreviewMonitorRect() const;
  // Bounds of the monitor the cursor currently sits on, in snapshot-local
  // coordinates (clamped to the snapshot).
  RECT ActiveMonitorLocalRect() const;
  D2D1_POINT_2F LongPreviewImageOrigin() const;
  float LongPreviewScale() const;
  void ClampLongPreviewScroll();
  void EnsureLongPreviewTiles();
  void DrawLongPreviewCommands();
  // Shared annotation renderer for the 1:1 view and the minimap (image coords).
  void DrawLongCommandsTransformed(D2D1_POINT_2F origin, float scale);
  RECT LongToolbarButtonRect(int index) const;
  bool HitLongToolbarButton(int index, POINT point) const;
  void DrawLongPreviewThumb();
  RECT LongPreviewThumbRect() const;
  float LongThumbScale() const;
  PointF LongThumbToImage(POINT point) const;
  bool HitLongThumb(POINT point) const;
  void LongThumbBeginDrag(POINT point);
  void LongThumbDrag(POINT point);
  std::optional<size_t> HitLongCommand(PointF image, Tool toolFilter) const;
  SelectionAdjustment HitLongCommandAdjustment(PointF image) const;
  void ContinueLongCommandAdjustment(POINT point);
  void DrawLongCommandHandles();
  LongPreviewDrag HitLongCropAdjustment(PointF image, float tolerance) const;
  void DrawLongCropOverlay(float stage = 1.0f);
  void BeginLongCrop();
  void ApplyLongCrop();
  void LongPreviewGestureStart(POINT point);
  void LongPreviewGestureMove(POINT point);
  void LongPreviewGestureEnd(POINT point);
  PointF LongPreviewToImage(POINT point) const;
  void CommitLongMosaic(const MosaicCommand& command);
  void RebuildLongMosaicPixels();
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
  void LogFrameError(HRESULT hr) const;
  void LogFrameErrorAt(HRESULT hr, UINT64 stage, UINT64 tag1, UINT64 tag2) const;
  void LogFrameState(const char* event) const;
  void FlushCheck(UINT64 stage);
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
  void UpdateTextImePosition();
  static LRESULT CALLBACK TextEditProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
                                        UINT_PTR subclassId, DWORD_PTR referenceData);
  RECT ToolbarRect() const;
  int ToolbarNormalWidth() const;  // non long-capture width; shrinks for burst
  int ToolbarCurrentWidth() const;  // normal bar, long-capture bar, or long-review palette
  RECT ToolbarWorkArea() const;
  RECT ToolbarToolRect(size_t index) const;
  RECT ToolbarCopyRect() const;
  RECT ToolbarSaveRect() const;
  RECT LongBarActionRect(int column) const;  // long-review bar bottom row: 0 copy, 1 save
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
  // Frosted dark panel behind the text currently being edited.  Only valid
  // while textEdit_ is open; keyed by the panel rectangle.
  ComPtr<ID2D1Bitmap> textBackdropBitmap_;
  RECT textBackdropRect_{};
  bool textBackdropValid_ = false;
  void EnsureTextEditBackdrop(const RECT& panel);
  void DrawTextEditBackdrop(const TextCommand& command);
  void DrawTextCaret(const TextCommand& command);
  // Moves the EDIT caret to the glyph under point.  Returns false when the
  // click is outside the editing text panel (caller commits the text then).
  bool TryPositionTextCaret(POINT point, bool selectWord = false);
  ULONGLONG textCaretBlinkStart_ = 0;
  void ResetTextCaretBlink();

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
  RECT hoverTargetRect_{};
  RECT hoverAnimationFromRect_{};
  std::chrono::steady_clock::time_point hoverAnimationStart_{};
  bool hoverAnimating_ = false;
  EditorDocument document_;
  std::optional<EditCommand> previewCommand_;
  HWND textEdit_ = nullptr;
  PointF textOrigin_{};

  std::mutex unitMutex_;
  std::vector<WindowCandidate> windowCandidates_;
  std::vector<UnitCandidate> unitCandidates_;
  std::vector<UnitCandidate> uiaCandidates_;
  std::vector<UnitCandidate> pendingUiaCandidates_;
  std::atomic<bool> unitReady_{false};
  std::atomic<bool> unitDetectionRunning_{false};
  std::atomic<bool> unitDetectionFinished_{false};
  std::atomic<bool> windowReady_{false};
  std::atomic<bool> windowEnumerationFinished_{false};
  std::atomic<bool> uiaQueryRunning_{false};
  std::atomic<bool> uiaQueryFinished_{false};
  bool unitDetectionSuppressed_ = false;
  bool restartUnitDetectionPending_ = false;
  std::jthread unitThread_;
  std::jthread windowThread_;
  std::jthread uiaThread_;
  std::optional<POINT> requestedUiaPoint_;
  POINT uiaResultPoint_{};
  POINT pendingUiaPoint_{};
  uint64_t uiaRequestGeneration_ = 0;
  uint64_t pendingUiaGeneration_ = 0;
  bool uiaReady_ = false;
  bool uiaRestartPending_ = false;
  struct PendingSnapshotSwitch {
    size_t index = 0;
    bool collapse = false;
    bool refreshDetection = false;
  };
  std::optional<PendingSnapshotSwitch> pendingSnapshotSwitch_;
  std::optional<CaptureCompletion> pendingCompletion_;
  std::vector<RECT> hoverUnitRects_;
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

  // Long (scrolling) capture state.
  bool longCaptureMode_ = false;   // toolbar flipped to the long-capture bar
  bool scrollCapturing_ = false;   // auto-scroll running, live preview active
  bool scrollEnded_ = false;       // page bottom / size cap reached
  int scrollStuckCount_ = 0;
  ScrollDirection scrollDirection_ = ScrollDirection::Down;
  int scrollWidth_ = 0;            // stitched image width (grows rightward)
  int scrollHeight_ = 0;           // stitched image height (grows downward)
  int scrollStride_ = 0;
  int scrollMaxRows_ = 0;          // growth cap along the stitching axis
  RECT scrollRegion_{};            // virtual desktop coordinates
  std::vector<uint8_t> scrollPixels_;
  int scrollLastShift_ = 0;        // rows/columns added by the previous step
  int scrollSettleWaits_ = 0;      // steps skipped waiting for the page to rest
  bool scrollSettleDisabled_ = false;  // page never rests (video, ads): stop waiting
  ComPtr<ID2D1Bitmap> scrollPreviewBitmap_;
  int scrollPreviewWidth_ = 0;     // texture size in pixels
  int scrollPreviewHeight_ = 0;
  int scrollPreviewOffset_ = 0;    // first accumulated row/column held by the texture
  std::chrono::steady_clock::time_point scrollLastStep_{};
  std::shared_ptr<LongCaptureImage> longCaptureResult_;  // consumed by Complete()

  // Post-capture long-screenshot review state: fullscreen viewport with a
  // zoom/scroll preview plus the in-place editing bar.
  bool longPreview_ = false;
  LongPreviewStage longPreviewStage_ = LongPreviewStage::View;
  float longPreviewProgress_ = 0.0f;  // eased entrance 0→1
  bool longPreviewAnimating_ = false;
  std::chrono::steady_clock::time_point longPreviewAnimStart_{};
  float longPreviewStageProgress_ = 1.0f;  // eased stage transition 0→1
  std::chrono::steady_clock::time_point longPreviewStageAnimStart_{};
  float longPreviewScroll_ = 0.0f;    // viewport top in image rows
  bool longThumbDragging_ = false;    // minimap drag in progress
  float longThumbGrabOffset_ = 0.0f;  // image-row offset grabbed on the minimap
  bool longResumePending_ = false;    // stitched data parked for another pass
  bool longCropChanged_ = false;      // crop applied → continuing is unsafe
  RectF longCrop_{};                  // crop rectangle in image coordinates
  RectF longCropBefore_{};            // pre-drag snapshot for ESC revert
  // A non-empty longCropApplied_ means a crop has been committed: the
  // surviving pixels are still kept in scrollPixels_/longCaptureResult_ but
  // the regions outside the crop are grayed out instead of discarded, so the
  // user can re-enter Crop and pick a different rectangle to bring the lost
  // strips back.
  RectF longCropApplied_{};
  bool longCropAppliedValid_ = false;
  LongPreviewDrag longPreviewDrag_ = LongPreviewDrag::None;
  bool longCropOnThumb_ = false;        // crop drag started on the minimap
  PointF longPreviewDragStart_{};     // gesture anchor in image coordinates
  Tool longPreviewTool_ = Tool::Pen;
  std::vector<ComPtr<ID2D1Bitmap>> longPreviewTiles_;  // vertical chunks
  int longPreviewTileRows_ = 0;
  std::vector<uint8_t> longMosaicBase_;   // pristine pixels captured before mosaic bakes
  // Viewport cached at preview entry: the 1:1 review stays anchored to the
  // monitor the user was looking at when the capture finished, even if their
  // cursor later wanders to a different display.
  RECT longPreviewViewport_{};
};

}  // namespace rc
