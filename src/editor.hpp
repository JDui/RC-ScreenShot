#pragma once

#include "config.hpp"

namespace rc {

struct PointF {
  float x = 0;
  float y = 0;
};

struct RectF {
  float left = 0;
  float top = 0;
  float right = 0;
  float bottom = 0;
};

enum class Tool {
  Select,
  Pen,
  Rectangle,
  Ellipse,
  Line,
  Arrow,
  Text,
  MosaicBrush,
  MosaicRectangle,
  Frame
};

enum class ShapeKind { Rectangle, Ellipse, Line, Arrow };

struct PenCommand {
  std::vector<PointF> points;
  StrokeSetting style;
};

struct ShapeCommand {
  ShapeKind kind = ShapeKind::Rectangle;
  PointF start{};
  PointF end{};
  ShapeSetting style{};
};

struct MosaicCommand {
  bool brush = true;
  std::vector<PointF> points;
  RectF bounds{};
  MosaicStyle style = MosaicStyle::Pixel;
  float brushSize = 32.0f;
  int pixelSize = 16;
  float blurRadius = 12.0f;
};

struct TextCommand {
  PointF origin{};
  std::wstring text;
  TextSetting style{};
};

using EditCommand = std::variant<PenCommand, ShapeCommand, MosaicCommand, TextCommand>;

class EditorDocument {
 public:
  void Clear();
  void Add(EditCommand command);
  bool Replace(size_t index, EditCommand command);
  bool Remove(size_t index);
  bool Undo();
  bool Redo();
  bool CanUndo() const { return cursor_ > 0; }
  bool CanRedo() const { return cursor_ < commands_.size(); }
  size_t Size() const { return cursor_; }
  std::span<const EditCommand> Commands() const { return {commands_.data(), cursor_}; }
  EditCommand* At(size_t index);
  const EditCommand* At(size_t index) const;

 private:
  std::vector<EditCommand> commands_;
  size_t cursor_ = 0;
};

RectF NormalizeRect(PointF a, PointF b);
bool Contains(const RectF& rect, PointF point);
float DistanceToSegment(PointF point, PointF a, PointF b);

// Applies only the destructive pixel/blur operations. Vector commands are rendered by Direct2D.
void ApplyMosaics(std::vector<uint8_t>& bgra, int width, int height, int stride,
                  std::span<const EditCommand> commands);

}  // namespace rc
