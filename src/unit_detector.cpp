#include "unit_detector.hpp"

#include <numeric>

namespace rc {
namespace {

struct Downsampled {
  int width = 0;
  int height = 0;
  int scale = 1;
  std::vector<uint8_t> luma;
};

Downsampled MakeLuma(std::span<const uint8_t> bgra, int width, int height, int stride) {
  Downsampled out;
  out.scale = std::max(1, static_cast<int>(std::ceil(std::max(width / 1600.0, height / 1000.0))));
  out.width = (width + out.scale - 1) / out.scale;
  out.height = (height + out.scale - 1) / out.scale;
  out.luma.resize(static_cast<size_t>(out.width * out.height));
  for (int y = 0; y < out.height; ++y) {
    for (int x = 0; x < out.width; ++x) {
      const int sx = std::min(x * out.scale, width - 1);
      const int sy = std::min(y * out.scale, height - 1);
      const uint8_t* pixel = bgra.data() + static_cast<size_t>(sy * stride + sx * 4);
      out.luma[static_cast<size_t>(y * out.width + x)] =
          static_cast<uint8_t>((29 * pixel[0] + 150 * pixel[1] + 77 * pixel[2]) >> 8);
    }
  }
  return out;
}

float IoU(const RECT& a, const RECT& b) {
  const int left = std::max(a.left, b.left), top = std::max(a.top, b.top);
  const int right = std::min(a.right, b.right), bottom = std::min(a.bottom, b.bottom);
  const float intersection = static_cast<float>(std::max(0, right - left) * std::max(0, bottom - top));
  const float areaA = static_cast<float>((a.right - a.left) * (a.bottom - a.top));
  const float areaB = static_cast<float>((b.right - b.left) * (b.bottom - b.top));
  return intersection / std::max(1.0f, areaA + areaB - intersection);
}

}  // namespace

std::vector<UnitCandidate> UnitDetector::Detect(std::span<const uint8_t> bgra, int width, int height,
                                                int stride) const {
  if (width < 24 || height < 24 || stride < width * 4 ||
      bgra.size() < static_cast<size_t>(stride * height)) return {};
  const Downsampled image = MakeLuma(bgra, width, height, stride);
  const int w = image.width, h = image.height;
  std::vector<uint8_t> vertical(static_cast<size_t>(w * h), 0);
  std::vector<uint8_t> horizontal(static_cast<size_t>(w * h), 0);
  for (int y = 1; y < h - 1; ++y) {
    for (int x = 1; x < w - 1; ++x) {
      const auto at = [&](int px, int py) { return image.luma[static_cast<size_t>(py * w + px)]; };
      const int gx = -at(x - 1, y - 1) - 2 * at(x - 1, y) - at(x - 1, y + 1)
                     + at(x + 1, y - 1) + 2 * at(x + 1, y) + at(x + 1, y + 1);
      const int gy = -at(x - 1, y - 1) - 2 * at(x, y - 1) - at(x + 1, y - 1)
                     + at(x - 1, y + 1) + 2 * at(x, y + 1) + at(x + 1, y + 1);
      vertical[static_cast<size_t>(y * w + x)] = std::abs(gx) > 96 ? 1 : 0;
      horizontal[static_cast<size_t>(y * w + x)] = std::abs(gy) > 96 ? 1 : 0;
    }
  }

  std::vector<int> xEnergy(static_cast<size_t>(w)), yEnergy(static_cast<size_t>(h));
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      xEnergy[static_cast<size_t>(x)] += vertical[static_cast<size_t>(y * w + x)];
      yEnergy[static_cast<size_t>(y)] += horizontal[static_cast<size_t>(y * w + x)];
    }
  }
  std::vector<int> xs{0, w - 1}, ys{0, h - 1};
  const int xThreshold = std::max(8, h / 7), yThreshold = std::max(8, w / 7);
  for (int x = 1; x < w - 1; ++x) if (xEnergy[static_cast<size_t>(x)] >= xThreshold) xs.push_back(x);
  for (int y = 1; y < h - 1; ++y) if (yEnergy[static_cast<size_t>(y)] >= yThreshold) ys.push_back(y);
  auto collapse = [](std::vector<int>& lines) {
    std::sort(lines.begin(), lines.end());
    std::vector<int> output;
    for (int line : lines) {
      if (output.empty() || line - output.back() > 3) output.push_back(line);
      else output.back() = (output.back() + line) / 2;
    }
    lines = std::move(output);
  };
  collapse(xs); collapse(ys);

  std::vector<UnitCandidate> candidates;
  const auto add = [&](int left, int top, int right, int bottom, float score) {
    RECT rect{left * image.scale, top * image.scale,
              std::min(width, (right + 1) * image.scale), std::min(height, (bottom + 1) * image.scale)};
    if (rect.right - rect.left < 24 || rect.bottom - rect.top < 24) return;
    if ((rect.right - rect.left) * (rect.bottom - rect.top) < 32 * 32) return;
    for (const auto& existing : candidates) if (IoU(existing.bounds, rect) > 0.92f) return;
    candidates.push_back({rect, score, -1});
  };

  // Adjacent line cells capture panels and cards; wider spans capture nested containers.
  for (size_t yi = 1; yi < ys.size(); ++yi) {
    for (size_t xi = 1; xi < xs.size(); ++xi) {
      const int l = xs[xi - 1], r = xs[xi], t = ys[yi - 1], b = ys[yi];
      if (r - l < 8 || b - t < 8) continue;
      const float border = static_cast<float>(xEnergy[l] + xEnergy[r] + yEnergy[t] + yEnergy[b]);
      add(l, t, r, b, border / std::max(1, 2 * (r - l + b - t)));
    }
  }
  for (size_t span = 2; span <= 4; ++span) {
    for (size_t yi = span; yi < ys.size(); ++yi) {
      for (size_t xi = span; xi < xs.size(); ++xi) {
        add(xs[xi - span], ys[yi - span], xs[xi], ys[yi], 0.25f / static_cast<float>(span));
      }
    }
  }
  add(0, 0, w - 1, h - 1, 0.01f);

  std::sort(candidates.begin(), candidates.end(), [](const UnitCandidate& a, const UnitCandidate& b) {
    const int areaA = (a.bounds.right - a.bounds.left) * (a.bounds.bottom - a.bounds.top);
    const int areaB = (b.bounds.right - b.bounds.left) * (b.bounds.bottom - b.bounds.top);
    return areaA < areaB;
  });
  for (size_t i = 0; i < candidates.size(); ++i) {
    for (size_t j = i + 1; j < candidates.size(); ++j) {
      const RECT& inner = candidates[i].bounds;
      const RECT& outer = candidates[j].bounds;
      if (inner.left >= outer.left && inner.top >= outer.top && inner.right <= outer.right && inner.bottom <= outer.bottom) {
        candidates[i].parent = static_cast<int>(j);
        break;
      }
    }
  }
  return candidates;
}

std::vector<size_t> UnitDetector::CandidatesAt(std::span<const UnitCandidate> candidates, POINT point) const {
  std::vector<size_t> result;
  for (size_t i = 0; i < candidates.size(); ++i) if (Contains(candidates[i].bounds, point)) result.push_back(i);
  std::sort(result.begin(), result.end(), [&](size_t a, size_t b) {
    const RECT& ra = candidates[a].bounds; const RECT& rb = candidates[b].bounds;
    return (ra.right - ra.left) * (ra.bottom - ra.top) < (rb.right - rb.left) * (rb.bottom - rb.top);
  });
  return result;
}

}  // namespace rc
