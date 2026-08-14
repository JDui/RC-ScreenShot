#include "uia_detector.hpp"

#include <UIAutomation.h>

namespace rc {
namespace {

constexpr size_t kMaximumVisitedElements = 512;
constexpr size_t kMaximumSiblingsPerBranch = 256;
constexpr int kMaximumDepth = 12;

bool SameRect(const RECT& a, const RECT& b) {
  return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

bool ReadBounds(IUIAutomationElement* element, RECT& bounds) {
  if (!element || FAILED(element->get_CurrentBoundingRectangle(&bounds))) return false;
  return !IsEmptyRect(bounds);
}

}  // namespace

std::vector<UnitCandidate> UiaDetector::Detect(HWND rootWindow, POINT screenPoint,
                                               RECT virtualBounds,
                                               std::stop_token stopToken) const {
  if (!rootWindow || !IsWindow(rootWindow) || stopToken.stop_requested()) return {};
  const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) return {};
  ScopeExit uninitialize{[&] { if (SUCCEEDED(initialized)) CoUninitialize(); }};
  const HRESULT cancellation = CoEnableCallCancellation(nullptr);
  ScopeExit disableCancellation{
      [&] { if (SUCCEEDED(cancellation)) CoDisableCallCancellation(nullptr); }};

  ComPtr<IUIAutomation> automation;
  if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&automation)))) return {};
  ComPtr<IUIAutomationElement> root;
  if (FAILED(automation->ElementFromHandle(rootWindow, &root)) || !root) return {};
  ComPtr<IUIAutomationTreeWalker> walker;
  if (FAILED(automation->get_ControlViewWalker(&walker)) || !walker) return {};

  struct PendingElement {
    ComPtr<IUIAutomationElement> element;
    int depth = 0;
  };
  std::vector<PendingElement> pending{{root, 0}};
  std::vector<RECT> bounds;
  size_t visited = 0;
  while (!pending.empty() && visited < kMaximumVisitedElements && !stopToken.stop_requested()) {
    PendingElement current = std::move(pending.back());
    pending.pop_back();
    ++visited;
    RECT currentBounds{};
    if (ReadBounds(current.element.Get(), currentBounds) && Contains(currentBounds, screenPoint))
      bounds.push_back(currentBounds);
    if (current.depth >= kMaximumDepth) continue;

    ComPtr<IUIAutomationElement> child;
    if (FAILED(walker->GetFirstChildElement(current.element.Get(), &child))) continue;
    size_t siblings = 0;
    while (child && siblings++ < kMaximumSiblingsPerBranch &&
           visited + pending.size() < kMaximumVisitedElements && !stopToken.stop_requested()) {
      RECT childBounds{};
      if (ReadBounds(child.Get(), childBounds) && Contains(childBounds, screenPoint))
        pending.push_back({child, current.depth + 1});
      ComPtr<IUIAutomationElement> next;
      if (FAILED(walker->GetNextSiblingElement(child.Get(), &next))) break;
      child = std::move(next);
    }
  }
  return stopToken.stop_requested()
      ? std::vector<UnitCandidate>{}
      : NormalizeCandidates(bounds, screenPoint, virtualBounds);
}

std::vector<UnitCandidate> UiaDetector::NormalizeCandidates(std::span<const RECT> screenBounds,
                                                             POINT screenPoint,
                                                             RECT virtualBounds) {
  std::vector<UnitCandidate> candidates;
  for (const RECT& bounds : screenBounds) {
    if (!Contains(bounds, screenPoint)) continue;
    RECT clipped{};
    if (!IntersectRect(&clipped, &bounds, &virtualBounds)) continue;
    if (clipped.right - clipped.left < 8 || clipped.bottom - clipped.top < 8) continue;
    OffsetRect(&clipped, -virtualBounds.left, -virtualBounds.top);
    if (std::any_of(candidates.begin(), candidates.end(),
                    [&](const UnitCandidate& value) { return SameRect(value.bounds, clipped); }))
      continue;
    candidates.push_back({clipped, 1.0f, -1});
  }
  const auto area = [](const RECT& rect) {
    return static_cast<int64_t>(rect.right - rect.left) * (rect.bottom - rect.top);
  };
  std::sort(candidates.begin(), candidates.end(),
            [&](const UnitCandidate& a, const UnitCandidate& b) {
              return area(a.bounds) < area(b.bounds);
            });
  for (size_t i = 0; i < candidates.size(); ++i) {
    for (size_t j = i + 1; j < candidates.size(); ++j) {
      const RECT& inner = candidates[i].bounds;
      const RECT& outer = candidates[j].bounds;
      if (inner.left >= outer.left && inner.top >= outer.top &&
          inner.right <= outer.right && inner.bottom <= outer.bottom) {
        candidates[i].parent = static_cast<int>(j);
        break;
      }
    }
  }
  return candidates;
}

}  // namespace rc
