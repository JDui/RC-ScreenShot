#pragma once

#include <cstdint>
#include <vector>

namespace rc {

// Tightly packed BGRA sub-rectangle cut out of a stitched long screenshot.
struct CroppedImage {
  int width = 0;
  int height = 0;
  int stride = 0;
  std::vector<uint8_t> bgra;
};

// ---------------------------------------------------------------------------
// Content-based scroll alignment.
//
// A scrolling long screenshot appends one captured viewport at a time, and the
// only thing standing between a clean panorama and a page full of ghosts is
// knowing exactly how far the content moved between two grabs.  The matcher
// below answers that from the pixels themselves -- it never trusts the wheel
// delta, the timer or any assumption about the application being captured.
//
// Everything is expressed as "shift": the number of rows (vertical captures)
// or columns (rightward captures) of *new* content the scroll revealed.  The
// complementary "overlap" is frameLines - shift, i.e. how much of the new
// frame repeats the edge of the stitched buffer.
// ---------------------------------------------------------------------------

struct ScrollMatch {
  // Rows/columns of new content revealed by the scroll.  Only meaningful when
  // `confident` is true.
  int shift = 0;
  // Robust mean per-channel absolute difference at `shift`, 0-255.  Low means
  // the overlapping pixels really are the same pixels.
  double residual = 255.0;
  // How much better `shift` is than the best alternative that is not its
  // immediate neighbour, 0-1, measured in per-channel difference units.  Zero
  // means another offset matches just as well (a repeating list, a flat
  // wallpaper) and the answer rests on the motion model alone; one means the
  // runner-up is decisively worse.
  double margin = 0.0;
  // True when the match is unambiguous enough to stitch.
  bool confident = false;
  // True when the viewport still shows exactly the pixels already at the edge
  // of the stitched buffer: the page did not move at all, so there is nothing
  // to append (bottom of the page, or the wheel never landed).
  bool identical = false;
};

// Optional prior knowledge handed to the matcher.
struct ScrollHint {
  // Shift accepted on the previous step, or 0 when unknown (first step, or the
  // previous step failed).  Scrolling is usually a steady rhythm, so this is
  // used as a soft tie-breaker -- the only reliable witness on pages whose
  // rows repeat (feeds, chat lists, product grids).
  int priorShift = 0;
};

// Downward capture: the frame's leading rows continue the stitched buffer's
// tail.  `width` is the shared image width in pixels.
ScrollMatch MatchScrollDown(const uint8_t* accumulated, int accumulatedHeight,
                            int accumulatedStride, const uint8_t* frame, int frameHeight,
                            int frameStride, int width, const ScrollHint& hint);

// Upward capture: the frame's trailing rows continue the stitched buffer's
// head, and its leading rows are the new content to prepend.
ScrollMatch MatchScrollUp(const uint8_t* accumulated, int accumulatedHeight,
                          int accumulatedStride, const uint8_t* frame, int frameHeight,
                          int frameStride, int width, const ScrollHint& hint);

// Rightward capture: the frame's leading columns continue the stitched
// buffer's trailing columns.  Both buffers share `frameHeight` rows.
ScrollMatch MatchScrollRight(const uint8_t* accumulated, int accumulatedWidth,
                             int accumulatedStride, const uint8_t* frame, int frameWidth,
                             int frameStride, int frameHeight, const ScrollHint& hint);

// Robust "did anything actually change between two grabs of the same region?"
// probe.  A frame taken while the page is still animating is a blend of two
// scroll positions and stitches into a ghost, so the capture waits for the
// region to hold still before aligning it.  Scoring runs per vertical segment
// and takes the median, so one animated element (a video, an ad, a blinking
// caret) cannot hold the capture hostage forever.
//
// Both buffers must have the same width and height.  Returns true when the
// median segment difference exceeds `threshold` (0-255 per channel).
bool FramesDiffer(const uint8_t* first, int firstStride, const uint8_t* second,
                  int secondStride, int width, int height, double threshold = 1.5);

// Copies the sub-rectangle [left, right) x [top, bottom) of a stitched BGRA
// image into a tightly packed buffer.  Clamps to the source bounds; returns
// an empty result when the clamped rect has no area.
CroppedImage CropStitchedPixels(const uint8_t* pixels, int width, int height, int stride,
                                int left, int top, int right, int bottom);

}  // namespace rc
