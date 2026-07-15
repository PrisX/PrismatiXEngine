#include "Editor/Tools/UIDesigner/Canvas/SnapEngine.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace px::editor {
namespace {

struct Candidate { float position; SnapGuideKind kind; Rect owner; };

void AddRectCandidates(std::vector<Candidate>& x, std::vector<Candidate>& y, Rect rect,
                       SnapGuideKind kind) {
    x.push_back({rect.x, kind, rect});
    x.push_back({rect.x + rect.w * 0.5f, kind, rect});
    x.push_back({rect.x + rect.w, kind, rect});
    y.push_back({rect.y, kind, rect});
    y.push_back({rect.y + rect.h * 0.5f, kind, rect});
    y.push_back({rect.y + rect.h, kind, rect});
}

}  // namespace

SnapResult SnapEngine::Snap(const SnapRequest& request,
                            const DesignerDocumentView& view) const {
    SnapResult output{.rect = request.movingRect};
    const float threshold = 6.0f / std::max(0.01f, request.zoom);
    std::unordered_set<Uuid, UuidHash> ignored(request.ignoredNodes.begin(),
                                               request.ignoredNodes.end());
    std::vector<Candidate> xs, ys;
    if (request.alignmentEnabled) {
        AddRectCandidates(xs, ys, request.canvasRect, SnapGuideKind::Canvas);
        if (const auto parent = view.LayoutRect(request.parent))
            AddRectCandidates(xs, ys, *parent, SnapGuideKind::Parent);
        for (const Uuid& sibling : view.Children(request.parent)) {
            if (ignored.contains(sibling)) continue;
            if (const auto rect = view.LayoutRect(sibling))
                AddRectCandidates(xs, ys, *rect, SnapGuideKind::Sibling);
        }
        for (const auto& guide : request.userGuides) {
            if (guide.orientation == SnapGuideOrientation::Vertical)
                xs.push_back({guide.position, SnapGuideKind::User, {}});
            else ys.push_back({guide.position, SnapGuideKind::User, {}});
        }
    }

    const auto snapAxis = [&](std::span<const Candidate> candidates,
                              std::span<const float> edges, bool horizontal) {
        float best = threshold;
        float adjustment = 0.0f;
        const Candidate* winner = nullptr;
        for (const auto& candidate : candidates) {
            for (const float edge : edges) {
                const float distance = std::abs(candidate.position - edge);
                if (distance < best) {
                    best = distance;
                    adjustment = candidate.position - edge;
                    winner = &candidate;
                }
            }
        }
        if (!winner) return 0.0f;
        output.guides.push_back({horizontal ? SnapGuideOrientation::Vertical
                                            : SnapGuideOrientation::Horizontal,
                                 winner->kind, winner->position,
                                 horizontal ? request.canvasRect.y : request.canvasRect.x,
                                 horizontal ? request.canvasRect.y + request.canvasRect.h
                                            : request.canvasRect.x + request.canvasRect.w});
        return adjustment;
    };

    if (request.mode == SnapMode::Move) {
        const float xEdges[]{output.rect.x, output.rect.x + output.rect.w * 0.5f,
                             output.rect.x + output.rect.w};
        const float yEdges[]{output.rect.y, output.rect.y + output.rect.h * 0.5f,
                             output.rect.y + output.rect.h};
        output.rect.x += snapAxis(xs, xEdges, true);
        output.rect.y += snapAxis(ys, yEdges, false);
        if (request.gridEnabled && request.gridSize > 0.0f) {
            output.rect.x = std::round(output.rect.x / request.gridSize) * request.gridSize;
            output.rect.y = std::round(output.rect.y / request.gridSize) * request.gridSize;
        }
    } else {
        float left = output.rect.x, right = output.rect.x + output.rect.w;
        float top = output.rect.y, bottom = output.rect.y + output.rect.h;
        if (request.snapLeft) { const float edges[]{left}; left += snapAxis(xs, edges, true); }
        if (request.snapRight) { const float edges[]{right}; right += snapAxis(xs, edges, true); }
        if (request.snapTop) { const float edges[]{top}; top += snapAxis(ys, edges, false); }
        if (request.snapBottom) { const float edges[]{bottom}; bottom += snapAxis(ys, edges, false); }
        output.rect = {left, top, std::max(1.0f, right - left),
                       std::max(1.0f, bottom - top)};
        if (request.gridEnabled && request.gridSize > 0.0f) {
            const auto grid = [&](float value) {
                return std::round(value / request.gridSize) * request.gridSize;
            };
            if (request.snapLeft) left = grid(left);
            if (request.snapRight) right = grid(right);
            if (request.snapTop) top = grid(top);
            if (request.snapBottom) bottom = grid(bottom);
            output.rect = {left, top, std::max(1.0f, right - left),
                           std::max(1.0f, bottom - top)};
        }
    }

    // Equal-spacing feedback: when the moved rect sits between two siblings with
    // equal gaps, surface the distances and guide. Alignment remains the primary
    // snap, so this never introduces an unpredictable second adjustment.
    if (request.alignmentEnabled) {
        std::vector<Rect> siblings;
        for (const Uuid& id : view.Children(request.parent))
            if (!ignored.contains(id)) if (const auto rect = view.LayoutRect(id)) siblings.push_back(*rect);
        for (const auto& a : siblings) for (const auto& b : siblings) {
            if (&a == &b) continue;
            const float leftGap = output.rect.x - (a.x + a.w);
            const float rightGap = b.x - (output.rect.x + output.rect.w);
            if (leftGap >= 0.0f && rightGap >= 0.0f && std::abs(leftGap - rightGap) <= threshold) {
                output.distances.push_back({{a.x + a.w, output.rect.y - 8.0f}, leftGap, true});
                output.distances.push_back({{output.rect.x + output.rect.w, output.rect.y - 8.0f}, rightGap, true});
                break;
            }
        }
    }
    return output;
}

float SnapEngine::SnapNormalized(float value, float tolerance) const {
    for (const float target : {0.0f, 0.5f, 1.0f})
        if (std::abs(value - target) < tolerance) return target;
    return std::clamp(value, 0.0f, 1.0f);
}

}  // namespace px::editor
