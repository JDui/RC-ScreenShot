#include "editor.hpp"

#include <cstring>

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

float PenWidthScaleForSpeed(float pixelsPerSecond) {
  // Slow movement deposits a wider stroke while quick movement produces a lighter line. The
  // square-root response keeps normal handwriting in the expressive part of the curve.
  const float normalized = std::clamp(std::max(0.0f, pixelsPerSecond) / 1400.0f, 0.0f, 1.0f);
  return std::lerp(1.35f, 0.55f, std::sqrt(normalized));
}

float PenPointWidth(const PenCommand& command, size_t index) {
  const float scale = index < command.widthScales.size()
                          ? std::clamp(command.widthScales[index], 0.35f, 1.6f)
                          : 1.0f;
  return std::clamp(command.style.width * scale, 0.5f, 192.0f);
}

float PenMaximumWidth(const PenCommand& command) {
  float width = std::max(0.5f, command.style.width);
  for (size_t index = 0; index < command.points.size(); ++index)
    width = std::max(width, PenPointWidth(command, index));
  return width;
}

namespace {

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

struct MosaicMask {
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;
  std::vector<uint8_t> pixels;

  int width() const { return right - left; }
  bool empty() const { return right <= left || bottom <= top || pixels.empty(); }
  bool At(int x, int y) const {
    if (x < left || x >= right || y < top || y >= bottom) return false;
    return pixels[static_cast<size_t>((y - top) * width() + (x - left))] != 0;
  }
  void Mark(int x, int y) {
    if (x < left || x >= right || y < top || y >= bottom) return;
    pixels[static_cast<size_t>((y - top) * width() + (x - left))] = 1;
  }
};

MosaicMask BuildMosaicMask(const MosaicCommand& command, int width, int height) {
  const RectF affect = MosaicAffectBounds(command, width, height);
  MosaicMask mask;
  mask.left = std::clamp(static_cast<int>(std::floor(affect.left)), 0, width);
  mask.top = std::clamp(static_cast<int>(std::floor(affect.top)), 0, height);
  mask.right = std::clamp(static_cast<int>(std::ceil(affect.right)), mask.left, width);
  mask.bottom = std::clamp(static_cast<int>(std::ceil(affect.bottom)), mask.top, height);
  // Check geometry before allocating; MosaicMask::empty() also looks at pixels.empty(), so
  // checking it here (before the buffer exists) would always bail out and no-op every mosaic.
  if (mask.right <= mask.left || mask.bottom <= mask.top) return mask;
  mask.pixels.resize(static_cast<size_t>(mask.width() * (mask.bottom - mask.top)));

  if (!command.brush) {
    std::fill(mask.pixels.begin(), mask.pixels.end(), static_cast<uint8_t>(1));
    return mask;
  }
  if (command.points.empty()) return mask;

  const float radius = std::max(command.brushSize * 0.5f, 0.5f);
  const auto markSegment = [&](PointF a, PointF b) {
    const int left = std::clamp(static_cast<int>(std::floor(std::min(a.x, b.x) - radius)), mask.left, mask.right - 1);
    const int right = std::clamp(static_cast<int>(std::ceil(std::max(a.x, b.x) + radius)), left + 1, mask.right);
    const int top = std::clamp(static_cast<int>(std::floor(std::min(a.y, b.y) - radius)), mask.top, mask.bottom - 1);
    const int bottom = std::clamp(static_cast<int>(std::ceil(std::max(a.y, b.y) + radius)), top + 1, mask.bottom);
    for (int y = top; y < bottom; ++y) {
      for (int x = left; x < right; ++x) {
        if (DistanceToSegment({x + 0.5f, y + 0.5f}, a, b) <= radius) mask.Mark(x, y);
      }
    }
  };
  if (command.points.size() == 1) markSegment(command.points.front(), command.points.front());
  else for (size_t i = 1; i < command.points.size(); ++i) markSegment(command.points[i - 1], command.points[i]);
  return mask;
}

void Pixelate(std::vector<uint8_t>& image, int width, int height, int stride,
              const MosaicCommand& command) {
  const MosaicMask mask = BuildMosaicMask(command, width, height);
  if (mask.empty()) return;
  const int block = std::max(command.pixelSize, 2);
  const int left = std::clamp((mask.left / block) * block, 0, width);
  const int top = std::clamp((mask.top / block) * block, 0, height);
  const int right = std::clamp(((mask.right + block - 1) / block) * block, 0, width);
  const int bottom = std::clamp(((mask.bottom + block - 1) / block) * block, 0, height);
  for (int by = top; by < bottom; by += block) {
    for (int bx = left; bx < right; bx += block) {
      const int blockRight = std::min(bx + block, width);
      const int blockBottom = std::min(by + block, height);
      uint64_t b = 0, g = 0, r = 0, a = 0, count = 0;
      bool touched = false;
      for (int y = by; y < blockBottom; ++y) {
        for (int x = bx; x < blockRight; ++x) {
          const uint8_t* pixel = image.data() + static_cast<size_t>(y * stride + x * 4);
          b += pixel[0]; g += pixel[1]; r += pixel[2]; a += pixel[3]; ++count;
          touched = touched || mask.At(x, y);
        }
      }
      if (!count || !touched) continue;
      const std::array<uint8_t, 4> average{
          static_cast<uint8_t>(b / count), static_cast<uint8_t>(g / count),
          static_cast<uint8_t>(r / count), static_cast<uint8_t>(a / count)};
      for (int y = by; y < blockBottom; ++y) {
        for (int x = bx; x < blockRight; ++x) {
          if (!mask.At(x, y)) continue;
          uint8_t* pixel = image.data() + static_cast<size_t>(y * stride + x * 4);
          std::copy(average.begin(), average.end(), pixel);
        }
      }
    }
  }
}

void MipmapBlur(std::vector<uint8_t>& image, int width, int height, int stride,
                const MosaicCommand& command) {
  const int radius = std::clamp(static_cast<int>(std::lround(command.blurRadius)), 1, 64);
  const MosaicMask mask = BuildMosaicMask(command, width, height);
  if (mask.empty()) return;

  const int sourceLeft = std::max(0, mask.left - radius);
  const int sourceTop = std::max(0, mask.top - radius);
  const int sourceRight = std::min(width, mask.right + radius);
  const int sourceBottom = std::min(height, mask.bottom + radius);
  const int sourceWidth = sourceRight - sourceLeft;
  const int sourceHeight = sourceBottom - sourceTop;
  if (sourceWidth <= 0 || sourceHeight <= 0) return;

  struct MipLevel {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels;
  };
  std::vector<MipLevel> levels;
  levels.push_back({sourceWidth, sourceHeight,
                    std::vector<uint8_t>(static_cast<size_t>(sourceWidth) * sourceHeight * 4)});
  for (int y = 0; y < sourceHeight; ++y) {
    const uint8_t* source = image.data() +
        static_cast<size_t>(sourceTop + y) * stride + sourceLeft * 4;
    std::memcpy(levels.front().pixels.data() + static_cast<size_t>(y) * sourceWidth * 4,
                source, static_cast<size_t>(sourceWidth) * 4);
  }

  const int levelCount = std::clamp(static_cast<int>(std::ceil(std::log2(radius + 1.0f))), 1, 6);
  for (int level = 0; level < levelCount; ++level) {
    const MipLevel& previous = levels.back();
    const int nextWidth = std::max(1, (previous.width + 1) / 2);
    const int nextHeight = std::max(1, (previous.height + 1) / 2);
    MipLevel next{nextWidth, nextHeight,
                  std::vector<uint8_t>(static_cast<size_t>(nextWidth) * nextHeight * 4)};
    for (int y = 0; y < nextHeight; ++y) {
      for (int x = 0; x < nextWidth; ++x) {
        uint32_t sums[4]{};
        int samples = 0;
        for (int sy = 0; sy < 2; ++sy) {
          const int sourceY = y * 2 + sy;
          if (sourceY >= previous.height) continue;
          for (int sx = 0; sx < 2; ++sx) {
            const int sourceX = x * 2 + sx;
            if (sourceX >= previous.width) continue;
            const uint8_t* pixel = previous.pixels.data() +
                (static_cast<size_t>(sourceY) * previous.width + sourceX) * 4;
            for (int channel = 0; channel < 4; ++channel) sums[channel] += pixel[channel];
            ++samples;
          }
        }
        uint8_t* output = next.pixels.data() +
            (static_cast<size_t>(y) * next.width + x) * 4;
        for (int channel = 0; channel < 4; ++channel)
          output[channel] = static_cast<uint8_t>(sums[channel] / std::max(samples, 1));
      }
    }
    levels.push_back(std::move(next));
  }

  MipLevel& mip = levels.back();
  // Smooth only the final low-resolution mip before upsampling.  A separable
  // [1,2,1] pass keeps the blur soft without paying a full-resolution
  // convolution cost, and clamped neighbors preserve crop boundaries.
  std::vector<uint8_t> horizontal(mip.pixels.size());
  for (int y = 0; y < mip.height; ++y) {
    for (int x = 0; x < mip.width; ++x) {
      uint8_t* output = horizontal.data() +
          (static_cast<size_t>(y) * mip.width + x) * 4;
      for (int channel = 0; channel < 4; ++channel) {
        const int left = std::max(0, x - 1);
        const int right = std::min(mip.width - 1, x + 1);
        const uint8_t* a = mip.pixels.data() +
            (static_cast<size_t>(y) * mip.width + left) * 4;
        const uint8_t* b = mip.pixels.data() +
            (static_cast<size_t>(y) * mip.width + x) * 4;
        const uint8_t* c = mip.pixels.data() +
            (static_cast<size_t>(y) * mip.width + right) * 4;
        output[channel] = static_cast<uint8_t>((a[channel] + 2u * b[channel] + c[channel]) / 4u);
      }
      output[3] = 255;
    }
  }
  for (int y = 0; y < mip.height; ++y) {
    for (int x = 0; x < mip.width; ++x) {
      uint8_t* output = mip.pixels.data() +
          (static_cast<size_t>(y) * mip.width + x) * 4;
      const int top = std::max(0, y - 1);
      const int bottom = std::min(mip.height - 1, y + 1);
      const uint8_t* a = horizontal.data() +
          (static_cast<size_t>(top) * mip.width + x) * 4;
      const uint8_t* b = horizontal.data() +
          (static_cast<size_t>(y) * mip.width + x) * 4;
      const uint8_t* c = horizontal.data() +
          (static_cast<size_t>(bottom) * mip.width + x) * 4;
      for (int channel = 0; channel < 4; ++channel)
        output[channel] = static_cast<uint8_t>((a[channel] + 2u * b[channel] + c[channel]) / 4u);
      output[3] = 255;
    }
  }
  // The crop can have a different aspect ratio from the final mip dimensions (for
  // example a thin brush stroke). Keep independent axes so absolute coordinates map
  // back to the correct source pixels without stretching one axis.
  const float scaleX = static_cast<float>(sourceWidth) / static_cast<float>(mip.width);
  const float scaleY = static_cast<float>(sourceHeight) / static_cast<float>(mip.height);
  for (int y = mask.top; y < mask.bottom; ++y) {
    for (int x = mask.left; x < mask.right; ++x) {
      if (!mask.At(x, y)) continue;
      const float sampleX = (x + 0.5f - sourceLeft) / scaleX - 0.5f;
      const float sampleY = (y + 0.5f - sourceTop) / scaleY - 0.5f;
      const float clampedX = std::clamp(sampleX, 0.0f, static_cast<float>(mip.width - 1));
      const float clampedY = std::clamp(sampleY, 0.0f, static_cast<float>(mip.height - 1));
      const int x0 = static_cast<int>(std::floor(clampedX));
      const int y0 = static_cast<int>(std::floor(clampedY));
      const int x1 = std::clamp(x0 + 1, 0, mip.width - 1);
      const int y1 = std::clamp(y0 + 1, 0, mip.height - 1);
      const float fx = clampedX - static_cast<float>(x0);
      const float fy = clampedY - static_cast<float>(y0);
      uint8_t* output = image.data() + static_cast<size_t>(y) * stride + x * 4;
      for (int channel = 0; channel < 4; ++channel) {
        const auto sample = [&](int sampleX, int sampleY) -> float {
          return static_cast<float>(mip.pixels[
              (static_cast<size_t>(sampleY) * mip.width + sampleX) * 4 + channel]);
        };
        const float top = sample(x0, y0) * (1.0f - fx) + sample(x1, y0) * fx;
        const float bottom = sample(x0, y1) * (1.0f - fx) + sample(x1, y1) * fx;
        output[channel] = static_cast<uint8_t>(std::clamp(
            std::lround(top * (1.0f - fy) + bottom * fy), 0l, 255l));
      }
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
    else MipmapBlur(bgra, width, height, stride, *mosaic);
  }
}

}  // namespace rc
