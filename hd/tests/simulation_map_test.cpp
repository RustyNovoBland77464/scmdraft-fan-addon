#include "../src/simulation_map.h"
#include <cassert>
#include <cstdio>
int main() {
    // One disabled unit, one enabled unit, and a pure sprite with the same high flag.
    std::vector<uint8_t> source={'T','H','G','2',30,0,0,0,
        0,0,128,0,160,0,11,0,0,128,
        0,0,192,0,160,0,11,0,0,0,
        1,0,128,0,160,0,11,0,0,144};
    auto copy=source;
    auto disabled=simulation_map::prepare(copy);
    assert(disabled.size()==1 && disabled[0].id==0 && disabled[0].x==128 && disabled[0].y==160 && disabled[0].owner==11);
    assert(source[17]==128 && copy[17]==0 && copy[27]==0 && copy[37]==144);
    // The unit is retained, along with every field except the flag being restored after load.
    copy[17]=128;assert(copy==source);
    for(size_t n=0;n<source.size();++n) {
        auto truncated=std::vector<uint8_t>(source.begin(),source.begin()+n);
        assert(simulation_map::prepare(truncated).empty());
    }
    std::puts("Disabled unit sprites survive simulation preprocessing; pure sprites and source map stay intact.");
}
