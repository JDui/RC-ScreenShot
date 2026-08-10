#pragma once

#include "common.hpp"

namespace rc {

struct UnitCandidate {
  RECT bounds{};
  float score = 0;
  int parent = -1;
};

class UnitDetector {
 public:
  // BGRA8 input. Coordinates in the returned candidates are in source-image pixels.
  std::vector<UnitCandidate> Detect(std::span<const uint8_t> bgra, int width, int height,
                                    int stride) const;
  std::vector<size_t> CandidatesAt(std::span<const UnitCandidate> candidates, POINT point) const;
};

}  // namespace rc
