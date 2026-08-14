#pragma once

#include "unit_detector.hpp"

namespace rc {

class UiaDetector {
 public:
  // Walks only the accessibility branch below rootWindow that contains screenPoint.
  // Returned bounds use overlay-local coordinates relative to virtualBounds.
  std::vector<UnitCandidate> Detect(HWND rootWindow, POINT screenPoint, RECT virtualBounds,
                                    std::stop_token stopToken = {}) const;

  // Pure geometry stage kept public for deterministic unit coverage.
  static std::vector<UnitCandidate> NormalizeCandidates(std::span<const RECT> screenBounds,
                                                        POINT screenPoint, RECT virtualBounds);
};

}  // namespace rc
