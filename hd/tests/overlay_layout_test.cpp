#include "../src/overlay_layout.h"
#include <cassert>
#include <cstdio>
using namespace overlay_layout;
int main() {
    // Every direction keeps the panel on the far side, with its near edge at the
    // reflected mineral centroid. Its size and hitbox coordinate system stay intact.
    Rect panel{0,0,300,200}, view{0,0,1600,1000};
    auto right=oppositePanel(panel,800,500,500,500,view);
    assert(right.left==1100 && right.top==400 && right.right==1400 && right.bottom==600);
    auto left=oppositePanel(panel,800,500,1100,500,view);
    assert(left.right==500 && left.top==400);
    auto down=oppositePanel(panel,800,500,800,250,view);
    assert(down.top==750 && down.left==650);
    auto up=oppositePanel(panel,800,500,800,750,view);
    assert(up.bottom==250 && up.left==650);
    auto diagonal=oppositePanel(panel,800,500,500,650,view);
    assert(diagonal.left==1100 && diagonal.top==250);
    auto edge=oppositePanel(panel,800,100,500,200,view);
    assert(edge.left==1100 && edge.top==0); // fit vertically without crossing back over the depot
    auto offscreen=oppositePanel(panel,1500,500,1200,500,view);
    assert(offscreen.left==1800); // preserve the side when the view needs panning
    auto centered=oppositePanel(panel,800,500,800,500,view);
    assert(centered.left==panel.left && centered.top==panel.top);
    Rect screen{0, 0, 974, 822};
    // Reproduce the screenshot: wide income/results rows crowd the lower mineral fields.
    std::vector<Rect> used{{270,368,780,398}, {270,402,792,436}, {270,442,670,476},
        {270,482,940,516}, {270,522,960,556}, {270,562,870,596}};
    for (int y : {104,168,232,296,456,520,584,616,648}) {
        auto r = place({750,y-14,830,y+14},screen,used);
        assert(r);
        for (auto o : used) assert(!overlaps(*r,o));
        assert(r->left >= 0 && r->right <= 974 && r->top >= 0 && r->bottom <= 822);
        used.push_back(*r);
    }
    // More than the previous eight downward attempts; nearby horizontal space wins.
    used.clear();
    for (int y=0;y<300;y+=20) used.push_back({100,y,200,y+18});
    auto r=place({130,100,170,118}, {0,0,400,400},used);
    assert(r && (r->right <= 98 || r->left >= 202));
    used={{0,0,100,100}};
    assert(!place({10,10,40,30},{0,0,100,100},used));
    assert(!place({0,0,200,20},{0,0,100,100},{}));
    puts("Crowded mineral labels avoid controls/other labels and stay inside the viewport.");
}
