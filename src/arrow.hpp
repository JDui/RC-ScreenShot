#pragma once

#include "common.hpp"

#include <d2d1.h>

namespace rc {

// Draws a modern arrow from `start` to `end`: a round-capped shaft that tucks underneath a
// filled arrow head slightly wider than the shaft. The filled head reads crisply at every stroke
// width, unlike a stroked "V" which looks thin and jagged at small sizes.
inline void DrawArrow(ID2D1RenderTarget* target, ID2D1Factory* factory,
                      D2D1_POINT_2F start, D2D1_POINT_2F end,
                      const D2D1_COLOR_F& color, float width) {
  if (!target || !factory || width <= 0.0f) return;
  const float dx = end.x - start.x;
  const float dy = end.y - start.y;
  const float length = std::sqrt(dx * dx + dy * dy);
  if (length < 0.5f) return;
  const float ux = dx / length, uy = dy / length;
  const float px = -uy, py = ux;

  ComPtr<ID2D1SolidColorBrush> brush;
  if (FAILED(target->CreateSolidColorBrush(color, &brush))) return;

  const float headLength = std::min(std::max(14.0f, width * 3.5f), length * 0.85f);
  const float headHalfWidth = std::min(std::max(7.0f, width * 1.8f), length * 0.5f);
  const D2D1_POINT_2F base{end.x - ux * headLength, end.y - uy * headLength};

  // Shaft: round-capped line ending at the head base so the filled head covers the joint.
  ComPtr<ID2D1StrokeStyle> round;
  if (SUCCEEDED(factory->CreateStrokeStyle(
      D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                  D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND,
                                  10.0f, D2D1_DASH_STYLE_SOLID, 0.0f), nullptr, 0, &round))) {
    target->DrawLine(start, base, brush.Get(), width, round.Get());
  } else {
    target->DrawLine(start, base, brush.Get(), width);
  }

  // Filled head triangle, perpendicular to the shaft direction.
  ComPtr<ID2D1PathGeometry> geometry;
  if (FAILED(factory->CreatePathGeometry(&geometry))) return;
  ComPtr<ID2D1GeometrySink> sink;
  if (FAILED(geometry->Open(&sink))) return;
  sink->BeginFigure(end, D2D1_FIGURE_BEGIN_FILLED);
  sink->AddLine({base.x + px * headHalfWidth, base.y + py * headHalfWidth});
  sink->AddLine({base.x - px * headHalfWidth, base.y - py * headHalfWidth});
  sink->EndFigure(D2D1_FIGURE_END_CLOSED);
  if (FAILED(sink->Close())) return;
  target->FillGeometry(geometry.Get(), brush.Get());
}

}  // namespace rc
