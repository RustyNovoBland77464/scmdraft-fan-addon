#pragma once
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <optional>
#include <queue>
#include <set>
#include <utility>
#include <vector>

namespace overlay_layout {
struct Rect { int left, top, right, bottom; };
inline bool overlaps(Rect a, Rect b, int gap = 2) {
    return a.left < b.right + gap && a.right + gap > b.left &&
           a.top < b.bottom + gap && a.bottom + gap > b.top;
}
// Reflect the mineral centroid through the depot. Put the panel's near edge at that
// point, rather than its center (a wide panel centered there would cover the depot again).
// Clamp only along the perpendicular axis, preserving the requested opposite side.
inline Rect oppositePanel(Rect panel, double depotX, double depotY, double mineralX, double mineralY, Rect viewport) {
    double dx=depotX-mineralX, dy=depotY-mineralY;
    if(std::abs(dx)<0.5 && std::abs(dy)<0.5) return panel;
    int w=panel.right-panel.left, h=panel.bottom-panel.top;
    int x=(int)std::lround(depotX+dx), y=(int)std::lround(depotY+dy);
    if(std::abs(dx)>=std::abs(dy)) {
        if(dx<0) x-=w;
        y-=h/2;
        if(h<=viewport.bottom-viewport.top) y=std::clamp(y,viewport.top,viewport.bottom-h);
    } else {
        if(dy<0) y-=h;
        x-=w/2;
        if(w<=viewport.right-viewport.left) x=std::clamp(x,viewport.left,viewport.right-w);
    }
    return {x,y,x+w,y+h};
}
// Visit the nearest available obstacle edges instead of stacking every collision downward.
// Positions come from a finite set of rectangle edges, with no fixed retry limit.
inline std::optional<Rect> place(Rect desired, Rect viewport, const std::vector<Rect> &occupied) {
    int w = desired.right - desired.left, h = desired.bottom - desired.top;
    if (w <= 0 || h <= 0 || w > viewport.right - viewport.left || h > viewport.bottom - viewport.top) return {};
    struct Candidate {
        int64_t distance;
        int x, y;
        bool operator<(const Candidate &o) const {
            if (distance != o.distance) return distance > o.distance;
            return y != o.y ? y > o.y : x > o.x;
        }
    };
    std::priority_queue<Candidate> queue;
    std::set<std::pair<int, int>> visited;
    auto add = [&](int x, int y) {
        x = std::clamp(x, viewport.left, viewport.right - w);
        y = std::clamp(y, viewport.top, viewport.bottom - h);
        if (!visited.insert({x, y}).second) return;
        int64_t dx = x - desired.left, dy = y - desired.top;
        queue.push({dx * dx + dy * dy, x, y});
    };
    add(desired.left, desired.top);
    while (!queue.empty()) {
        auto p = queue.top(); queue.pop();
        Rect r{p.x, p.y, p.x + w, p.y + h};
        const Rect *hit = nullptr;
        for (const auto &other : occupied) if (overlaps(r, other)) { hit = &other; break; }
        if (!hit) return r;
        add(hit->left - w - 2, p.y); add(hit->right + 2, p.y);
        add(p.x, hit->top - h - 2); add(p.x, hit->bottom + 2);
    }
    return {}; // No room: never paint illegible text over another label/button.
}
}
