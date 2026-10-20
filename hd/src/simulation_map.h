#pragma once
#include "bwgame.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace simulation_map {
struct DisabledSprite { uint16_t id, x, y; uint8_t owner; };
// OpenBW's THG2 parser throws on disabled unit sprites. Load their normal unit first,
// then restore the disabled state; keep the actual unit and its collision footprint.
// All edits apply to a private simulation copy, never the editor's map.
inline std::vector<DisabledSprite> prepare(std::vector<uint8_t> &data) {
    std::vector<DisabledSprite> disabled;
    for (size_t off=0; off+8<=data.size();) {
        uint32_t size; std::memcpy(&size,data.data()+off+4,4);
        if (size>data.size()-off-8) break;
        if (!std::memcmp(data.data()+off,"THG2",4)) {
            for (size_t n=0; n+10<=size; n+=10) {
                uint8_t *p=data.data()+off+8+n;
                if ((p[9]&0x90)!=0x80) continue; // pure sprites have no disabled unit state
                DisabledSprite s{};
                std::memcpy(&s.id,p,2); std::memcpy(&s.x,p+2,2); std::memcpy(&s.y,p+4,2); s.owner=p[6];
                using namespace bwgame;
                if (s.id==unsigned(UnitTypes::Special_Upper_Level_Door) ||
                    s.id==unsigned(UnitTypes::Special_Right_Upper_Level_Door) ||
                    s.id==unsigned(UnitTypes::Special_Pit_Door) ||
                    s.id==unsigned(UnitTypes::Special_Right_Pit_Door)) s.owner=11;
                disabled.push_back(s); p[9]&=~0x80;
            }
        }
        off+=8+size;
    }
    return disabled;
}
template<class Loader,class Setup> void load(Loader &loader, const std::vector<uint8_t> &source, Setup setup) {
    auto data=source;
    auto disabled=prepare(data);
    loader.load_map_data(data.data(),data.size(),setup,disabled.empty());
    std::vector<size_t> used;
    for (const auto &s:disabled) for (auto *u:bwgame::ptr(loader.st.visible_units)) {
        if (unsigned(u->unit_type->id)!=s.id || u->position.x!=s.x || u->position.y!=s.y || u->owner!=s.owner ||
            std::find(used.begin(),used.end(),u->index)!=used.end()) continue;
        loader.u_set_status_flag(u,bwgame::unit_t::status_flag_disabled);
        loader.set_unit_disabled(u);
        loader.set_unit_order(u,loader.get_order_type(bwgame::Orders::Nothing));
        used.push_back(u->index);
        break;
    }
    if (!disabled.empty()) {
        loader.process_frame();
        loader.process_frame();
    }
}
} // namespace simulation_map
