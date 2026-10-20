// Local asset-backed integration check; build with the same OpenBW/CascLib inputs as minesim.
// Run: map-load-check <StarCraft CASC directory> <local map.scx>
#define main minesim_main
#include "../../minesim/minesim.cpp"
#undef main
#include <cassert>
int main(int argc,char **argv) {
    if(argc!=3) return 2;
    casc_loader assets(argv[1]);global_state gst;global_init(gst,assets);
    auto chk=load_chk(argv[2]);
    simulator sim{gst,chk,0,0,false};auto bases=sim.start_bases();assert(!bases.empty());
    xy pos=bases.front().pos;
    uint8_t records[]={'T','H','G','2',20,0,0,0,
        0,0,0,0,0,0,11,0,0,128,
        0,0,0,0,0,0,11,0,0,0};
    uint16_t x=pos.x,y=pos.y,otherX=x-64;
    memcpy(records+10,&x,2);memcpy(records+12,&y,2);
    memcpy(records+20,&otherX,2);memcpy(records+22,&y,2);
    chk.insert(chk.end(),records,records+sizeof(records));const auto original=chk;
    bool reproduced=false;
    {
        game_state game;state st;st.global=&gst;st.game=&game;game_load_functions f(st);
        try {f.load_map_data(chk.data(),chk.size(),[&]{
            for(int i=0;i<8;++i)st.players[i].controller=player_t::controller_neutral;
            f.setup_info.victory_condition=2;f.setup_info.starting_units=1;
        });} catch(const std::exception &e){reproduced=std::string(e.what()).find("disable thingy unit")!=std::string::npos;}
    }
    assert(reproduced);
    game_state game;state st;st.global=&gst;st.game=&game;game_load_functions f(st);
    simulation_map::load(f,chk,[&]{
        for(int i=0;i<8;++i)st.players[i].controller=player_t::controller_neutral;
        f.setup_info.victory_condition=2;f.setup_info.starting_units=1;
    });
    unit_t *disabled=nullptr,*enabled=nullptr;
    for(auto *u:ptr(st.visible_units)) if(u->unit_type->id==UnitTypes::Terran_Marine && u->owner==11) {
        if(u->position==pos)disabled=u;
        if(u->position==xy(otherX,y))enabled=u;
    }
    assert(disabled && enabled && f.u_disabled(disabled) && !f.u_disabled(enabled));
    assert(!f.u_status_flag(disabled,unit_t::status_flag_no_collide));
    for(int n=0;n<600;++n)f.next_frame();
    assert(f.u_disabled(disabled) && disabled->position==pos && !f.u_disabled(enabled));
    assert(chk==original);
    puts("PASS: original error reproduced; disabled unit retained with collision, enabled neighbor intact, 600 frames stable, source unchanged.");
}
