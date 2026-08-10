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
                 CompletionCallback completion, ConfigChangedCallback configChanged);
  ~CaptureOverlay();

  bool Show(std::wstring& error);
  HWND hwnd() const { return hwnd_; }
  const DesktopSnapshot& snapshot() const { return snapshot_; }

 private:
  static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
  LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
  bool CreateDeviceResources();
  void DiscardDeviceResources();
  void Paint();
  void BeginSettingPreview(POINT point);
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
  void DrawTextCommand(const TextCommand& command);
  void DrawText(std::wstring_view text, const D2D1_RECT_F& rect, float size,
                D2D1_COLOR_F color, DWRITE_TEXT_ALIGNMENT alignment = DWRITE_TEXT_ALIGNMENT_CENTER);
  std::optional<Tool> HitTestTool(POINT point) const;
  enum class PropertyAction {
    SizeDown, SizeUp, Color, Opacity, FillColor, FillOpacity,
    MosaicStyle, MosaicStrengthDown, MosaicStrengthUp, FrameToggle, TextOrientation
  };
  struct PropertyButton { PropertyAction action; RECT rect; std::wstring label; };
  void DrawPropertyIcon(PropertyAction action, const RECT& rect);
  std::vector<PropertyButton> PropertyButtons() const;
  std::optional<PropertyAction> HitTestProperty(POINT point) const;
  void ActivateProperty(PropertyAction action);
  bool HitCopy(POINT point) const;
  bool HitSave(POINT point) const;
  PointF ToSelectionPoint(POINT point) const;
  StrokeSetting* ActiveStroke();
  void AdjustActiveSize(float delta);
  void CycleActiveOpacity();
  void ChooseActiveColor();
  void ChooseFillColor();
  void CycleFillOpacity();
  float ActiveSize() const;
  void SetActiveSize(float size);
  ColorSetting* ActiveColor();
  float ActiveOpacity() const;
  RECT SizeSliderRect() const;
  void SetSizeFromSlider(POINT point);
  std::optional<size_t> HitTestColorPreset(POINT point) const;
  void SetActivePresetColor(size_t index);
  void ShowEditorContextMenu(POINT point);
  std::optional<size_t> HitTestCommand(POINT point) const;
  SelectionAdjustment HitTestCommandAdjustment(POINT point) const;
  RectF SelectedCommandBounds() const;
  void ContinueCommandAdjustment(POINT point);
  void BeginTextInput(POINT point, std::optional<size_t> existingCommand = std::nullopt);
  void CommitTextInput();
  void CancelTextInput();
  static LRESULT CALLBACK TextEditProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
                                       UINT_PTR subclassId, DWORD_PTR referenceData);
  RECT ToolbarRect() const;

  HINSTANCE instance_ = nullptr;
  HWND hwnd_ = nullptr;
  DesktopSnapshot snapshot_;
  AppConfig& config_;
  CompletionCallback completion_;
  ConfigChangedCallback configChanged_;

  ComPtr<ID2D1Factory> d2dFactory_;
  ComPtr<IDWriteFactory> dwriteFactory_;
  ComPtr<ID2D1HwndRenderTarget> renderTarget_;
  ComPtr<ID2D1Bitmap> desktopBitmap_;

  SelectionMode mode_ = SelectionMode::Normal;
  Tool tool_ = Tool::Pen;
  bool selecting_ = false;
  bool editing_ = false;
  bool drawing_ = false;
  bool sizeSliderDragging_ = false;
  bool settingPreview_ = false;
  bool windowSelection_ = false;
  SelectionAdjustment selectionAdjustment_ = SelectionAdjustment::None;
  RECT selectionBeforeAdjust_{};
  POINT dragStart_{};
  POINT currentPoint_{};
  POINT lastCanvasPoint_{};
  POINT settingPreviewPoint_{};
  RECT selection_{};  // overlay-local coordinates
  RECT hoverRect_{};
  EditorDocument document_;
  std::optional<EditCommand> previewCommand_;
  HWND textEdit_ = nullptr;
  PointF textOrigin_{};

  std::mutex unitMutex_;
  std::vector<UnitCandidate> unitCandidates_;
  std::atomic<bool> unitReady_{false};
  std::jthread unitThread_;
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
};

}  // namespace rc
