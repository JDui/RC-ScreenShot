#include "editor.hpp"

namespace rc {

void EditorDocument::Clear() {
  commands_.clear();
  cursor_ = 0;
}

void EditorDocument::Add(EditCommand command) {
  if (cursor_ < commands_.size())
    commands_.erase(commands_.begin() + static_cast<ptrdiff_t>(cursor_), commands_.end());
  commands_.push_back(std::move(command));
  cursor_ = commands_.size();
}

bool EditorDocument::Replace(size_t index, EditCommand command) {
  if (index >= cursor_) return false;
  commands_[index] = std::move(command);
  return true;
}

bool EditorDocument::Remove(size_t index) {
  if (index >= cursor_) return false;
  if (cursor_ < commands_.size())
    commands_.erase(commands_.begin() + static_cast<ptrdiff_t>(cursor_), commands_.end());
  commands_.erase(commands_.begin() + static_cast<ptrdiff_t>(index));
  cursor_ = commands_.size();
  return true;
}

bool EditorDocument::Undo() {
  if (!cursor_) return false;
  --cursor_;
  return true;
}

bool EditorDocument::Redo() {
  if (cursor_ >= commands_.size()) return false;
  ++cursor_;
  return true;
}

EditCommand* EditorDocument::At(size_t index) {
  return index < cursor_ ? &commands_[index] : nullptr;
}

const EditCommand* EditorDocument::At(size_t index) const {
  return index < cursor_ ? &commands_[index] : nullptr;
}

RectF NormalizeRect(PointF a, PointF b) {
  return {std::min(a.x, b.x), std::min(a.y, b.y), std::max(a.x, b.x), std::max(a.y, b.y)};
}

bool Contains(const RectF& rect, PointF point) {
  return point.x >= rect.left && point.x < rect.right && point.y >= rect.top && point.y < rect.bottom;
}

float DistanceToSegment(PointF point, PointF a, PointF b) {
  const float dx = b.x - a.x;
  const float dy = b.y - a.y;
  const float lengthSquared = dx * dx + dy * dy;
  if (lengthSquared < 0.0001f) return std::hypot(point.x - a.x, point.y - a.y);
  const float t = std::clamp(((point.x - a.x) * dx + (point.y - a.y) * dy) / lengthSquared, 0.0f, 1.0f);
  return std::hypot(point.x - (a.x + t * dx), point.y - (a.y + t * dy));
}

namespace {

bool InMosaic(const MosaicCommand& command, float x, float y) {
  if (!command.brush) return Contains(command.bounds, {x, y});
  if (command.points.empty()) return false;
  const float radius = command.brushSize * 0.5f;
  if (command.points.size() == 1) return std::hypot(x - command.points[0].x, y - command.points[0].y) <= radius;
  for (size_t i = 1; i < command.points.size(); ++i) {
    if (DistanceToSegment({x, y}, command.points[i - 1], command.points[i]) <= radius) return true;
  }
  return false;
}

RectF MosaicAffectBounds(const MosaicCommand& command, int width, int height) {
  if (!command.brush) return command.bounds;
  if (command.points.empty()) return {};
  RectF bounds{command.points.front().x, command.points.front().y,
               command.points.front().x, command.points.front().y};
  for (const PointF point : command.points) {
    bounds.left = std::min(bounds.left, point.x);
    bounds.top = std::min(bounds.top, point.y);
    bounds.right = std::max(bounds.right, point.x);
    bounds.bottom = std::max(bounds.bottom, point.y);
  }
  const float radius = command.brushSize * 0.5f;
  return {std::clamp(bounds.left - radius, 0.0f, static_cast<float>(width)),
          std::clamp(bounds.top - radius, 0.0f, static_cast<float>(height)),
          std::clamp(bounds.right + radius, 0.0f, static_cast<float>(width)),
          std::clamp(bounds.bottom + radius, 0.0f, static_cast<float>(height))};
}

void Pixelate(std::vector<uint8_t>& image, int width, int height, int stride,
              const MosaicCommand& command) {
  const int block = std::max(command.pixelSize, 2);
  const RectF bounds = MosaicAffectBounds(command, width, height);
  const int left = std::clamp(static_cast<int>(std::floor(bounds.left / block)) * block, 0, width);
  const int top = std::clamp(static_cast<int>(std::floor(bounds.top / block)) * block, 0, height);
  const int right = std::clamp(static_cast<int>(std::ceil(bounds.right / block)) * block, 0, width);
  const int bottom = std::clamp(static_cast<int>(std::ceil(bounds.bottom / block)) * block, 0, height);
  for (int by = top; by < bottom; by += block) {
    for (int bx = left; bx < right; bx += block) {
      uint64_t b = 0, g = 0, r = 0, a = 0, count = 0;
      for (int y = by; y < std::min(by + block, bottom); ++y) {
        for (int x = bx; x < std::min(bx + block, right); ++x) {
          const uint8_t* pixel = image.data() + static_cast<size_t>(y * stride + x * 4);
          b += pixel[0]; g += pixel[1]; r += pixel[2]; a += pixel[3]; ++count;
        }
      }
      if (!count) continue;
      bool touched = false;
      for (int y = by; y < std::min(by + block, bottom) && !touched; ++y) {
        for (int x = bx; x < std::min(bx + block, right); ++x) {
          if (InMosaic(command, x + 0.5f, y + 0.5f)) { touched = true; break; }
        }
      }
      if (!touched) continue;
      const std::array<uint8_t, 4> average{
          static_cast<uint8_t>(b / count), static_cast<uint8_t>(g / count),
          static_cast<uint8_t>(r / count), static_cast<uint8_t>(a / count)};
      for (int y = by; y < std::min(by + block, bottom); ++y) {
        for (int x = bx; x < std::min(bx + block, right); ++x) {
          if (!InMosaic(command, x + 0.5f, y + 0.5f)) continue;
          uint8_t* pixel = image.data() + static_cast<size_t>(y * stride + x * 4);
          std::copy(average.begin(), average.end(), pixel);
        }
      }
    }
  }
}

void BoxBlur(std::vector<uint8_t>& image, int width, int height, int stride,
             const MosaicCommand& command) {
  const int radius = std::clamp(static_cast<int>(std::lround(command.blurRadius)), 1, 64);
  std::vector<uint8_t> source = image;
  const RectF bounds = MosaicAffectBounds(command, width, height);
  const int left = std::clamp(static_cast<int>(std::floor(bounds.left)), 0, width);
  const int top = std::clamp(static_cast<int>(std::floor(bounds.top)), 0, height);
  const int right = std::clamp(static_cast<int>(std::ceil(bounds.right)), 0, width);
  const int bottom = std::clamp(static_cast<int>(std::ceil(bounds.bottom)), 0, height);
  for (int y = top; y < bottom; ++y) {
    for (int x = left; x < right; ++x) {
      if (!InMosaic(command, x + 0.5f, y + 0.5f)) continue;
      uint64_t b = 0, g = 0, r = 0, a = 0, count = 0;
      for (int sy = std::max(0, y - radius); sy <= std::min(height - 1, y + radius); sy += 2) {
        for (int sx = std::max(0, x - radius); sx <= std::min(width - 1, x + radius); sx += 2) {
          const uint8_t* pixel = source.data() + static_cast<size_t>(sy * stride + sx * 4);
          b += pixel[0]; g += pixel[1]; r += pixel[2]; a += pixel[3]; ++count;
        }
      }
      uint8_t* pixel = image.data() + static_cast<size_t>(y * stride + x * 4);
      pixel[0] = static_cast<uint8_t>(b / count); pixel[1] = static_cast<uint8_t>(g / count);
      pixel[2] = static_cast<uint8_t>(r / count); pixel[3] = static_cast<uint8_t>(a / count);
    }
  }
}

}  // namespace

void ApplyMosaics(std::vector<uint8_t>& bgra, int width, int height, int stride,
                  std::span<const EditCommand> commands) {
  if (width <= 0 || height <= 0 || stride < width * 4 || bgra.size() < static_cast<size_t>(stride * height)) return;
  for (const auto& command : commands) {
    const auto* mosaic = std::get_if<MosaicCommand>(&command);
    if (!mosaic) continue;
    if (mosaic->style == MosaicStyle::Pixel) Pixelate(bgra, width, height, stride, *mosaic);
    else BoxBlur(bgra, width, height, stride, *mosaic);
  }
}

}  // namespace rc
