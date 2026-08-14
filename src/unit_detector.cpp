#include "unit_detector.hpp"

#include <numeric>

namespace rc {
namespace {

struct Downsampled {
  int width = 0;
  int height = 0;
  int scale = 1;
  std::vector<uint8_t> bgr;
};

Downsampled MakeColor(std::span<const uint8_t> bgra, int width, int height, int stride,
                      std::stop_token stopToken) {
  Downsampled out;
  // Preserve native pixels on ordinary 4K captures. Sparse point sampling at
  // scale 2-3 used to skip one-pixel borders and entire compact controls.
  out.scale = std::max(1, static_cast<int>(std::ceil(std::max(width / 5000.0, height / 2800.0))));
  out.width = (width + out.scale - 1) / out.scale;
  out.height = (height + out.scale - 1) / out.scale;
  out.bgr.resize(static_cast<size_t>(out.width * out.height * 3));
  for (int y = 0; y < out.height; ++y) {
    if (stopToken.stop_requested()) return {};
    for (int x = 0; x < out.width; ++x) {
      std::array<int, 3> sum{};
      int samples = 0;
      const int endX = std::min(width, (x + 1) * out.scale);
      const int endY = std::min(height, (y + 1) * out.scale);
      for (int sy = y * out.scale; sy < endY; ++sy) {
        for (int sx = x * out.scale; sx < endX; ++sx) {
          const uint8_t* pixel = bgra.data() + static_cast<size_t>(sy * stride + sx * 4);
          for (int channel = 0; channel < 3; ++channel) sum[channel] += pixel[channel];
          ++samples;
        }
      }
      const size_t target = static_cast<size_t>((y * out.width + x) * 3);
      for (int channel = 0; channel < 3; ++channel)
        out.bgr[target + channel] = static_cast<uint8_t>(sum[channel] / std::max(1, samples));
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

float SideSupport(const std::vector<uint8_t>& edges, int width, int height,
                  int start, int end, int fixed, bool horizontal) {
  if (start > end) return 0.0f;
  int supported = 0;
  int samples = 0;
  for (int value = start; value <= end; ++value) {
    bool hit = false;
    for (int offset = -2; offset <= 2; ++offset) {
      const int x = horizontal ? value : fixed + offset;
      const int y = horizontal ? fixed + offset : value;
      if (x >= 0 && x < width && y >= 0 && y < height &&
          edges[static_cast<size_t>(y * width + x)]) {
        hit = true;
        break;
      }
    }
    supported += hit ? 1 : 0;
    ++samples;
  }
  return samples ? static_cast<float>(supported) / static_cast<float>(samples) : 0.0f;
}

struct BorderScore {
  float minimum = 0.0f;
  float average = 0.0f;
};

BorderScore ScoreBorder(const std::vector<uint8_t>& vertical,
                        const std::vector<uint8_t>& horizontal, int width, int height,
                        int left, int top, int right, int bottom) {
  const float topScore = SideSupport(horizontal, width, height, left, right, top, true);
  const float bottomScore = SideSupport(horizontal, width, height, left, right, bottom, true);
  const float leftScore = SideSupport(vertical, width, height, top, bottom, left, false);
  const float rightScore = SideSupport(vertical, width, height, top, bottom, right, false);
  return {std::min(std::min(topScore, bottomScore), std::min(leftScore, rightScore)),
          (topScore + bottomScore + leftScore + rightScore) * 0.25f};
}

BorderScore ScoreRoundedBorder(const std::vector<uint8_t>& vertical,
                               const std::vector<uint8_t>& horizontal, int width, int height,
                               int left, int top, int right, int bottom) {
  BorderScore best{};
  const int shortSide = std::min(right - left, bottom - top);
  for (float fraction : {0.10f, 0.18f, 0.26f, 0.34f}) {
    const int inset = std::max(2, static_cast<int>(std::lround(shortSide * fraction)));
    if (left + inset >= right - inset || top + inset >= bottom - inset) continue;
    const float topScore = SideSupport(horizontal, width, height, left + inset, right - inset,
                                       top, true);
    const float bottomScore = SideSupport(horizontal, width, height, left + inset, right - inset,
                                          bottom, true);
    const float leftScore = SideSupport(vertical, width, height, top + inset, bottom - inset,
                                        left, false);
    const float rightScore = SideSupport(vertical, width, height, top + inset, bottom - inset,
                                         right, false);
    const BorderScore score{
        std::min(std::min(topScore, bottomScore), std::min(leftScore, rightScore)),
        (topScore + bottomScore + leftScore + rightScore) * 0.25f};
    if (score.minimum + score.average > best.minimum + best.average) best = score;
  }
  return best;
}

bool EdgeNear(const std::vector<uint8_t>& edges, int width, int height, int x, int y,
              int radius = 2) {
  for (int dy = -radius; dy <= radius; ++dy) {
    for (int dx = -radius; dx <= radius; ++dx) {
      const int px = x + dx, py = y + dy;
      if (px >= 0 && px < width && py >= 0 && py < height &&
          edges[static_cast<size_t>(py * width + px)]) return true;
    }
  }
  return false;
}

BorderScore ScoreEllipse(const std::vector<uint8_t>& edges, int width, int height,
                         int left, int top, int right, int bottom) {
  constexpr int samplesPerQuadrant = 24;
  constexpr float pi = 3.14159265358979323846f;
  const float centerX = (left + right) * 0.5f;
  const float centerY = (top + bottom) * 0.5f;
  const float radiusX = (right - left) * 0.5f;
  const float radiusY = (bottom - top) * 0.5f;
  if (radiusX < 6.0f || radiusY < 6.0f) return {};

  std::array<float, 4> quadrantSupport{};
  for (int quadrant = 0; quadrant < 4; ++quadrant) {
    int supported = 0;
    for (int sample = 0; sample < samplesPerQuadrant; ++sample) {
      const float angle = (quadrant + (sample + 0.5f) / samplesPerQuadrant) * pi * 0.5f;
      const int x = static_cast<int>(std::lround(centerX + std::cos(angle) * radiusX));
      const int y = static_cast<int>(std::lround(centerY + std::sin(angle) * radiusY));
      supported += EdgeNear(edges, width, height, x, y) ? 1 : 0;
    }
    quadrantSupport[static_cast<size_t>(quadrant)] =
        static_cast<float>(supported) / samplesPerQuadrant;
  }
  return {*std::min_element(quadrantSupport.begin(), quadrantSupport.end()),
          std::accumulate(quadrantSupport.begin(), quadrantSupport.end(), 0.0f) * 0.25f};
}

}  // namespace

std::vector<UnitCandidate> UnitDetector::Detect(std::span<const uint8_t> bgra, int width, int height,
                                                int stride, std::stop_token stopToken) const {
  if (width < 24 || height < 24 || stride < width * 4 ||
      bgra.size() < static_cast<size_t>(stride * height) || stopToken.stop_requested()) return {};
  const Downsampled image = MakeColor(bgra, width, height, stride, stopToken);
  if (stopToken.stop_requested() || image.width <= 0 || image.height <= 0) return {};
  const int w = image.width, h = image.height;
  std::vector<uint8_t> vertical(static_cast<size_t>(w * h), 0);
  std::vector<uint8_t> horizontal(static_cast<size_t>(w * h), 0);
  std::vector<uint8_t> edges(static_cast<size_t>(w * h), 0);
  for (int y = 1; y < h - 1; ++y) {
    if (stopToken.stop_requested()) return {};
    for (int x = 1; x < w - 1; ++x) {
      const auto at = [&](int px, int py, int channel) {
        return image.bgr[static_cast<size_t>((py * w + px) * 3 + channel)];
      };
      int gx = 0, gy = 0;
      for (int channel = 0; channel < 3; ++channel) {
        const int channelGx = -at(x - 1, y - 1, channel) - 2 * at(x - 1, y, channel) -
                              at(x - 1, y + 1, channel) + at(x + 1, y - 1, channel) +
                              2 * at(x + 1, y, channel) + at(x + 1, y + 1, channel);
        const int channelGy = -at(x - 1, y - 1, channel) - 2 * at(x, y - 1, channel) -
                              at(x + 1, y - 1, channel) + at(x - 1, y + 1, channel) +
                              2 * at(x, y + 1, channel) + at(x + 1, y + 1, channel);
        if (std::abs(channelGx) > std::abs(gx)) gx = channelGx;
        if (std::abs(channelGy) > std::abs(gy)) gy = channelGy;
      }
      // UI cards often differ from their background by only 8-16 luma levels.
      // Keep directional edges permissive, then reject noise by validating a
      // complete rectangle, rounded rectangle, or ellipse below.
      vertical[static_cast<size_t>(y * w + x)] = std::abs(gx) > 20 ? 1 : 0;
      horizontal[static_cast<size_t>(y * w + x)] = std::abs(gy) > 20 ? 1 : 0;
      edges[static_cast<size_t>(y * w + x)] =
          std::abs(gx) + std::abs(gy) > 20;
    }
  }

  std::vector<int> xEnergy(static_cast<size_t>(w)), yEnergy(static_cast<size_t>(h));
  for (int y = 0; y < h; ++y) {
    if (stopToken.stop_requested()) return {};
    for (int x = 0; x < w; ++x) {
      xEnergy[static_cast<size_t>(x)] += vertical[static_cast<size_t>(y * w + x)];
      yEnergy[static_cast<size_t>(y)] += horizontal[static_cast<size_t>(y * w + x)];
    }
  }
  std::vector<int> xs{0, w - 1}, ys{0, h - 1};
  // Use local rectangle-sized support, not a fraction of the whole screen.
  // Otherwise small cards disappear on large screenshots before border
  // validation even gets a chance to inspect them.
  const int xThreshold = 10, yThreshold = 10;
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
  const auto addCandidate = [&](int left, int top, int right, int bottom, float score) {
    if (left <= 0 || top <= 0 || right >= w - 1 || bottom >= h - 1) return;
    RECT rect{left * image.scale, top * image.scale,
              std::min(width, (right + 1) * image.scale), std::min(height, (bottom + 1) * image.scale)};
    if (rect.right - rect.left < 18 || rect.bottom - rect.top < 18) return;
    if ((rect.right - rect.left) * (rect.bottom - rect.top) < 20 * 20) return;
    for (const auto& existing : candidates) if (IoU(existing.bounds, rect) > 0.92f) return;
    candidates.push_back({rect, score, -1});
  };
  const auto addRectangle = [&](int left, int top, int right, int bottom, float score) {
    const BorderScore borderScore = ScoreBorder(vertical, horizontal, w, h, left, top, right, bottom);
    const BorderScore roundedScore =
        ScoreRoundedBorder(vertical, horizontal, w, h, left, top, right, bottom);
    // Full rectangles tolerate small interruptions. Rounded rectangles omit
    // their corners by design, so score only the central run of each side but
    // require those four runs to be convincing.
    const bool rectangle = borderScore.minimum >= 0.46f && borderScore.average >= 0.58f;
    const bool rounded = roundedScore.minimum >= 0.62f && roundedScore.average >= 0.72f;
    if (!rectangle && !rounded) return;
    const float shapeScore = std::max(borderScore.average, roundedScore.average);
    addCandidate(left, top, right, bottom, score * (0.5f + shapeScore));
  };

  // Horizontal and vertical spans are independent. A panel split into three
  // columns and two rows still has one valid outer rectangle; tying both axes
  // to the same span silently discarded it.
  constexpr size_t maxLineSpan = 6;
  for (size_t spanY = 1; spanY <= maxLineSpan; ++spanY) {
    if (stopToken.stop_requested()) return {};
    for (size_t spanX = 1; spanX <= maxLineSpan; ++spanX) {
      for (size_t yi = spanY; yi < ys.size(); ++yi) {
        if (stopToken.stop_requested()) return {};
        for (size_t xi = spanX; xi < xs.size(); ++xi) {
          const int left = xs[xi - spanX], right = xs[xi];
          const int top = ys[yi - spanY], bottom = ys[yi];
          if (right - left < 8 || bottom - top < 8) continue;
          const float border = static_cast<float>(xEnergy[left] + xEnergy[right] +
                                                  yEnergy[top] + yEnergy[bottom]);
          const float spanPenalty = static_cast<float>(std::max(spanX, spanY));
          addRectangle(left, top, right, bottom,
                       border / std::max(1, 2 * (right - left + bottom - top)) /
                           std::sqrt(spanPenalty));
        }
      }
    }
  }

  // Closed edge components expose shapes that global horizontal/vertical
  // projections cannot represent, especially circles and ellipses.
  std::vector<uint8_t> visited(edges.size(), 0);
  std::vector<int> queue;
  for (int startY = 1; startY < h - 1; ++startY) {
    if (stopToken.stop_requested()) return {};
    for (int startX = 1; startX < w - 1; ++startX) {
      const int start = startY * w + startX;
      if (!edges[static_cast<size_t>(start)] || visited[static_cast<size_t>(start)]) continue;
      queue.clear();
      queue.push_back(start);
      visited[static_cast<size_t>(start)] = 1;
      int left = startX, right = startX, top = startY, bottom = startY;
      for (size_t head = 0; head < queue.size(); ++head) {
        if ((head & 1023) == 0 && stopToken.stop_requested()) return {};
        const int current = queue[head];
        const int x = current % w, y = current / w;
        left = std::min(left, x); right = std::max(right, x);
        top = std::min(top, y); bottom = std::max(bottom, y);
        for (int dy = -1; dy <= 1; ++dy) {
          for (int dx = -1; dx <= 1; ++dx) {
            if ((!dx && !dy) || x + dx <= 0 || x + dx >= w - 1 ||
                y + dy <= 0 || y + dy >= h - 1) continue;
            const int next = (y + dy) * w + x + dx;
            if (!edges[static_cast<size_t>(next)] || visited[static_cast<size_t>(next)]) continue;
            visited[static_cast<size_t>(next)] = 1;
            queue.push_back(next);
          }
        }
      }
      if (right - left < 12 || bottom - top < 12 || queue.size() < 24) continue;
      const BorderScore rectangleScore =
          ScoreBorder(vertical, horizontal, w, h, left, top, right, bottom);
      const BorderScore roundedScore =
          ScoreRoundedBorder(vertical, horizontal, w, h, left, top, right, bottom);
      if (rectangleScore.minimum >= 0.46f && rectangleScore.average >= 0.58f)
        addCandidate(left, top, right, bottom, 0.9f * rectangleScore.average);
      else if (roundedScore.minimum >= 0.62f && roundedScore.average >= 0.72f)
        addCandidate(left, top, right, bottom, 0.85f * roundedScore.average);
      const BorderScore ellipseScore = ScoreEllipse(edges, w, h, left, top, right, bottom);
      if (ellipseScore.minimum >= 0.58f && ellipseScore.average >= 0.68f)
        addCandidate(left, top, right, bottom, 0.8f * ellipseScore.average);
    }
  }
  // Keep the full capture as a safe fallback. It is not a detected unit and
  // therefore must not compete with real bordered rectangles.
  candidates.push_back({{0, 0, width, height}, 0.01f, -1});

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
