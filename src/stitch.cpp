#include "stitch.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>

namespace rc {

namespace {

// ---------------------------------------------------------------------------
// A "line" is the unit of stitching: a pixel row for vertical captures, a
// pixel column for rightward ones.  Both strides are signed, which lets a
// single matcher core serve all three scroll directions:
//
//   Down   frame and accumulated both walk forward; the seam joins the
//          accumulated tail to the frame head.
//   Up     both walk backward, which is exactly the mirrored Down case: the
//          seam joins the accumulated head to the frame tail.
//   Right  lines are columns (element stride = row stride), everything else
//          identical to Down.
// ---------------------------------------------------------------------------
struct LineView {
  const uint8_t* base = nullptr;
  std::ptrdiff_t lineStride = 0;  // bytes from one line to the next
  std::ptrdiff_t elemStride = 0;  // bytes from one element to the next
  int lineCount = 0;
  int elemCount = 0;

  bool valid() const { return base != nullptr && lineCount > 0 && elemCount > 0; }
};

inline const uint8_t* At(const LineView& view, int line, int elem) {
  return view.base + static_cast<std::ptrdiff_t>(line) * view.lineStride +
         static_cast<std::ptrdiff_t>(elem) * view.elemStride;
}

inline int Luma(const uint8_t* pixel) {
  return (77 * static_cast<int>(pixel[2]) + 150 * static_cast<int>(pixel[1]) +
          29 * static_cast<int>(pixel[0])) >> 8;
}

// Per-line brightness and horizontal-detail signatures: cheap 1D projections
// that survive a pure translation, used to screen every candidate offset
// before any expensive 2D comparison runs.
struct LineProfile {
  std::vector<float> mean;
  std::vector<float> edge;
};

void BuildProfile(const LineView& view, int firstLine, int lineCount, LineProfile& out) {
  out.mean.assign(static_cast<size_t>(lineCount), 0.0f);
  out.edge.assign(static_cast<size_t>(lineCount), 0.0f);
  constexpr int kStep = 2;  // every other element is plenty for a projection
  for (int index = 0; index < lineCount; ++index) {
    const int line = firstLine + index;
    long long sum = 0;
    long long gradient = 0;
    int samples = 0;
    int previous = Luma(At(view, line, 0));
    for (int elem = kStep; elem < view.elemCount; elem += kStep) {
      const int value = Luma(At(view, line, elem));
      sum += value;
      gradient += std::abs(value - previous);
      previous = value;
      ++samples;
    }
    if (samples > 0) {
      out.mean[static_cast<size_t>(index)] = static_cast<float>(sum) / samples;
      out.edge[static_cast<size_t>(index)] = static_cast<float>(gradient) / samples;
    }
  }
}

// Tuning constants.  Residuals are mean per-channel absolute differences on a
// 0-255 scale, so everything below ~2 means "pixel identical".
constexpr int kMinOverlap = 2;          // never align on fewer than this many lines
constexpr int kMinConfidentOverlap = 16;  // below this the match must be perfect
constexpr int kShortlist = 10;          // candidates surviving the 1D screening
constexpr int kRefineBand = 96;         // seam-adjacent lines re-scored at 1:1
constexpr int kSegmentTarget = 96;      // pixels per voting segment
constexpr int kMaxSegments = 8;
constexpr double kRowDiffCap = 40.0;    // per-line cap: one animated line cannot drown the rest
constexpr double kResidualAccept = 12.0;
constexpr double kResidualPerfect = 1.5;
constexpr double kMinMargin = 0.10;
constexpr double kIdenticalResidual = 2.0;
constexpr double kPriorPenalty = 1.5;   // diff units, worst case over half a frame
// Difference units the winner must lead the runner-up by to count as decisive.
// Measured on an absolute scale rather than as a ratio: when two offsets both
// match perfectly (a page whose rows repeat) the ratio says "ambiguous" only
// by accident of floating point, while the absolute lead is plainly zero.
constexpr double kMarginScale = 3.0;

// Robust 2D score for one candidate offset.
//
// The overlap is compared line by line, but the buffer is split into segments
// across the *other* axis and each segment votes with a trimmed mean of its
// per-line differences.  The score is the median across segments, so sticky
// headers, fixed sidebars, video panels and other regions that refuse to
// scroll are outvoted instead of dragging the whole match off by a few pixels
// -- which is precisely what used to smear a ghost band across the seam.
//
// `from`/`to` bound the compared lines inside the overlap so the caller can
// either score the whole overlap (coarse pass) or only the seam-adjacent band
// (1:1 refinement).
double RobustScore(const LineView& frame, const LineView& accumulated, int shift, int from,
                   int to, int lineStep, int elemStep) {
  const int overlap = frame.lineCount - shift;
  if (overlap <= 0) return 255.0;
  from = std::max(0, from);
  to = std::min(overlap, to);
  if (to <= from) return 255.0;
  const int segments = std::clamp(frame.elemCount / kSegmentTarget, 1, kMaxSegments);
  std::vector<double> sums(static_cast<size_t>(segments), 0.0);
  int lines = 0;
  for (int index = from; index < to; index += lineStep) {
    const int accLine = accumulated.lineCount - overlap + index;
    if (accLine < 0 || accLine >= accumulated.lineCount) continue;
    const uint8_t* f = At(frame, index, 0);
    const uint8_t* a = At(accumulated, accLine, 0);
    for (int segment = 0; segment < segments; ++segment) {
      const int first = segment * frame.elemCount / segments;
      const int last = (segment + 1) * frame.elemCount / segments;
      long long total = 0;
      int samples = 0;
      for (int elem = first; elem < last; elem += elemStep) {
        const uint8_t* pf = f + static_cast<std::ptrdiff_t>(elem) * frame.elemStride;
        const uint8_t* pa = a + static_cast<std::ptrdiff_t>(elem) * accumulated.elemStride;
        total += std::abs(static_cast<int>(pf[0]) - static_cast<int>(pa[0])) +
                 std::abs(static_cast<int>(pf[1]) - static_cast<int>(pa[1])) +
                 std::abs(static_cast<int>(pf[2]) - static_cast<int>(pa[2]));
        samples += 3;
      }
      if (samples > 0)
        sums[static_cast<size_t>(segment)] +=
            std::min(kRowDiffCap, static_cast<double>(total) / samples);
    }
    ++lines;
  }
  if (lines <= 0) return 255.0;
  std::vector<double> scores(sums.size());
  for (size_t index = 0; index < sums.size(); ++index) scores[index] = sums[index] / lines;
  const size_t middle = scores.size() / 2;
  std::nth_element(scores.begin(), scores.begin() + middle, scores.end());
  return scores[middle];
}

// Coarse-to-fine matcher over a pair of line views.  See MatchScroll* for the
// direction-specific wiring.
class LineMatcher {
 public:
  LineMatcher(LineView frame, LineView accumulated, int priorShift)
      : frame_(frame), accumulated_(accumulated), prior_(priorShift) {}

  ScrollMatch Run() {
    ScrollMatch result;
    if (!frame_.valid() || !accumulated_.valid()) return result;
    const int lines = frame_.lineCount;
    const int profileLines = std::min(lines, accumulated_.lineCount);
    if (profileLines <= 0) return result;
    BuildProfile(frame_, 0, lines, frameProfile_);
    BuildProfile(accumulated_, accumulated_.lineCount - profileLines, profileLines,
                 accumulatedProfile_);

    // Pass 0 -- did the page move at all?  A static viewport means the wheel
    // never landed or the page hit its end; reporting that explicitly keeps a
    // confident-looking false match from appending duplicate content.
    if (profileLines >= lines) {
      const double still = RobustScore(frame_, accumulated_, 0, 0, lines, 2, 2);
      if (still <= kIdenticalResidual) {
        result.identical = true;
        result.residual = still;
        return result;
      }
    }

    // Pass 1 -- screen every plausible offset with the 1D projections.  This
    // is O(lines^2) on tiny arrays, cheap enough to run exhaustively, and it
    // keeps the expensive 2D pass down to a handful of candidates.
    const int minShift = std::max(1, lines - profileLines);
    const int maxShift = lines - kMinOverlap;
    if (minShift > maxShift) return result;
    std::vector<double> profileScore(static_cast<size_t>(maxShift + 1), 255.0);
    std::vector<int> candidates;
    candidates.reserve(static_cast<size_t>(maxShift - minShift + 1));
    for (int shift = minShift; shift <= maxShift; ++shift) {
      profileScore[static_cast<size_t>(shift)] = ProfileScore(shift);
      candidates.push_back(shift);
    }
    std::sort(candidates.begin(), candidates.end(), [&](int a, int b) {
      return profileScore[static_cast<size_t>(a)] < profileScore[static_cast<size_t>(b)];
    });
    std::vector<int> shortlist;
    for (int shift : candidates) {
      if (static_cast<int>(shortlist.size()) >= kShortlist) break;
      shortlist.push_back(shift);
    }
    // The motion model always gets a seat: on periodic pages the projections
    // cannot tell two offsets apart and the rhythm of previous steps is the
    // only reliable witness.
    if (prior_ >= minShift && prior_ <= maxShift &&
        std::find(shortlist.begin(), shortlist.end(), prior_) == shortlist.end()) {
      shortlist.push_back(prior_);
    }

    // Pass 2 -- full 2D scoring of the survivors, with the motion model as a
    // mild penalty so a plateau breaks toward the expected step size.
    std::vector<double> raw(shortlist.size(), 255.0);
    std::vector<double> penalized(shortlist.size(), 255.0);
    size_t bestIndex = 0;
    for (size_t index = 0; index < shortlist.size(); ++index) {
      const int shift = shortlist[index];
      raw[index] = RobustScore(frame_, accumulated_, shift, 0, lines - shift, 2, 2);
      penalized[index] = raw[index] + PriorPenalty(shift, lines);
      if (penalized[index] < penalized[bestIndex]) bestIndex = index;
    }
    int best = shortlist[bestIndex];

    // Pass 3 -- re-score the seam-adjacent band at 1:1 for the winner and its
    // immediate neighbours.  A one-line slip is most visible right at the
    // boundary, and this is the last chance to catch it.
    const int band = std::min(kRefineBand, lines - best);
    if (band >= 8) {
      double bestBand = RobustScore(frame_, accumulated_, best, lines - best - band,
                                    lines - best, 1, 1);
      for (int delta = -1; delta <= 1; delta += 2) {
        const int shift = best + delta;
        if (shift < minShift || shift > maxShift) continue;
        const int candidateBand = std::min(kRefineBand, lines - shift);
        const double score = RobustScore(frame_, accumulated_, shift,
                                        lines - shift - candidateBand, lines - shift, 1, 1);
        if (score < bestBand - 0.02) {
          bestBand = score;
          best = shift;
        }
      }
    }

    // Confidence: the winner has to be both a genuinely good match and clearly
    // better than any *other* offset.  Skipping near neighbours keeps a wide
    // minimum (sub-pixel text rendering) from being mistaken for ambiguity.
    // The margin is measured on the raw scores: the motion model may break a
    // tie, but it must not fake certainty the pixels do not support.
    double runnerUp = 255.0;
    for (size_t index = 0; index < shortlist.size(); ++index) {
      if (std::abs(shortlist[index] - best) <= 2) continue;
      runnerUp = std::min(runnerUp, raw[index]);
    }
    result.shift = best;
    result.residual = RobustScore(frame_, accumulated_, best, 0, lines - best, 2, 2);
    result.margin = std::clamp((runnerUp - result.residual) / kMarginScale, 0.0, 1.0);
    const bool sharp = result.residual <= kResidualPerfect;
    const bool overlapUsable = (lines - best) >= kMinConfidentOverlap || sharp;
    result.confident = result.residual <= kResidualAccept && overlapUsable &&
                       (sharp || result.margin >= kMinMargin);

    // Fallback -- the content genuinely cannot decide (a list of identical
    // rows).  Trust the rhythm instead of duplicating a chunk: pick the
    // previous step size when it scores just as well.
    if (!result.confident && prior_ >= minShift && prior_ <= maxShift) {
      const double priorScore =
          RobustScore(frame_, accumulated_, prior_, 0, lines - prior_, 2, 2);
      const bool priorOverlapUsable =
          (lines - prior_) >= kMinConfidentOverlap || priorScore <= kResidualPerfect;
      if (priorScore <= kResidualAccept && priorScore <= result.residual + 1.0 &&
          priorOverlapUsable) {
        result.shift = prior_;
        result.residual = priorScore;
        result.confident = true;
      }
    }
    return result;
  }

 private:
  double ProfileScore(int shift) const {
    const int overlap = frame_.lineCount - shift;
    const int profileLines = static_cast<int>(accumulatedProfile_.mean.size());
    if (overlap <= 0 || overlap > profileLines) return 255.0;
    const int base = profileLines - overlap;
    double weighted = 0.0;
    double weight = 0.0;
    for (int index = 0; index < overlap; ++index) {
      const double edgeA = accumulatedProfile_.edge[static_cast<size_t>(base + index)];
      const double edgeF = frameProfile_.edge[static_cast<size_t>(index)];
      // Detailed lines carry the alignment signal, flat ones carry none.
      const double w = 1.0 + (edgeA + edgeF) * 0.05;
      weighted += w * (std::abs(edgeA - edgeF) +
                       0.5 * std::abs(accumulatedProfile_.mean[static_cast<size_t>(base + index)] -
                                      frameProfile_.mean[static_cast<size_t>(index)]));
      weight += w;
    }
    return weight > 0.0 ? weighted / weight : 255.0;
  }

  double PriorPenalty(int shift, int lines) const {
    if (prior_ <= 0) return 0.0;
    const double span = std::max(1.0, lines * 0.5);
    return kPriorPenalty * std::min(1.0, std::abs(shift - prior_) / span);
  }

  LineView frame_;
  LineView accumulated_;
  int prior_ = 0;
  LineProfile frameProfile_;
  LineProfile accumulatedProfile_;
};

}  // namespace

ScrollMatch MatchScrollDown(const uint8_t* accumulated, int accumulatedHeight,
                            int accumulatedStride, const uint8_t* frame, int frameHeight,
                            int frameStride, int width, const ScrollHint& hint) {
  ScrollMatch empty;
  if (!accumulated || !frame || width <= 0 || frameHeight <= 0 || accumulatedHeight <= 0 ||
      accumulatedStride <= 0 || frameStride <= 0) {
    return empty;
  }
  const LineView frameView{frame, frameStride, 4, frameHeight, width};
  const LineView accumulatedView{accumulated, accumulatedStride, 4, accumulatedHeight, width};
  return LineMatcher(frameView, accumulatedView, hint.priorShift).Run();
}

ScrollMatch MatchScrollUp(const uint8_t* accumulated, int accumulatedHeight,
                          int accumulatedStride, const uint8_t* frame, int frameHeight,
                          int frameStride, int width, const ScrollHint& hint) {
  ScrollMatch empty;
  if (!accumulated || !frame || width <= 0 || frameHeight <= 0 || accumulatedHeight <= 0 ||
      accumulatedStride <= 0 || frameStride <= 0) {
    return empty;
  }
  // Both buffers walk backward: the seam joins the accumulated head to the
  // frame tail, which is the mirrored form of the downward case.
  const LineView frameView{
      frame + static_cast<std::ptrdiff_t>(frameHeight - 1) * frameStride,
      -static_cast<std::ptrdiff_t>(frameStride), 4, frameHeight, width};
  const LineView accumulatedView{
      accumulated + static_cast<std::ptrdiff_t>(accumulatedHeight - 1) * accumulatedStride,
      -static_cast<std::ptrdiff_t>(accumulatedStride), 4, accumulatedHeight, width};
  return LineMatcher(frameView, accumulatedView, hint.priorShift).Run();
}

ScrollMatch MatchScrollRight(const uint8_t* accumulated, int accumulatedWidth,
                             int accumulatedStride, const uint8_t* frame, int frameWidth,
                             int frameStride, int frameHeight, const ScrollHint& hint) {
  ScrollMatch empty;
  if (!accumulated || !frame || accumulatedWidth <= 0 || frameWidth <= 0 || frameHeight <= 0 ||
      accumulatedStride <= 0 || frameStride <= 0) {
    return empty;
  }
  // Lines are columns: the element stride is the row stride.
  const LineView frameView{frame, 4, frameStride, frameWidth, frameHeight};
  const LineView accumulatedView{accumulated, 4, accumulatedStride, accumulatedWidth,
                                 frameHeight};
  return LineMatcher(frameView, accumulatedView, hint.priorShift).Run();
}

bool FramesDiffer(const uint8_t* first, int firstStride, const uint8_t* second,
                  int secondStride, int width, int height, double threshold) {
  if (!first || !second || width <= 0 || height <= 0 || firstStride <= 0 || secondStride <= 0) {
    return false;
  }
  const LineView a{first, firstStride, 4, height, width};
  const LineView b{second, secondStride, 4, height, width};
  // shift 0 compares line to line; sampling every fourth pixel keeps the
  // settle probe off the capture's critical path.
  return RobustScore(a, b, 0, 0, height, 4, 4) > threshold;
}

CroppedImage CropStitchedPixels(const uint8_t* pixels, int width, int height, int stride,
                                int left, int top, int right, int bottom) {
  CroppedImage result;
  if (!pixels || width <= 0 || height <= 0 || stride <= 0) return result;
  left = std::max(0, left);
  top = std::max(0, top);
  right = std::min(width, right);
  bottom = std::min(height, bottom);
  if (right - left <= 0 || bottom - top <= 0) return result;
  result.width = right - left;
  result.height = bottom - top;
  result.stride = result.width * 4;
  result.bgra.resize(static_cast<size_t>(result.stride) * result.height);
  for (int row = 0; row < result.height; ++row) {
    memcpy(result.bgra.data() + static_cast<size_t>(result.stride) * row,
           pixels + static_cast<size_t>(stride) * (top + row) + static_cast<size_t>(left) * 4,
           static_cast<size_t>(result.stride));
  }
  return result;
}

}  // namespace rc
