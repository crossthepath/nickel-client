#include "nickel/module_core.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace nickel;
int main() {
    int checks = 0;
    auto check = [&](bool ok, const char* label) {
        ++checks;
        if (!ok) throw std::runtime_error(label);
    };
    auto close = [](float a, float b) { return std::fabs(a-b) < .0001f; };
    Player p; Input input;
    p.velocity = {2.f,-3.f,4.f}; glide_tick(p);
    check(close(p.velocity.y,-.1f) && p.velocity.x==2.f && p.velocity.z==4.f,"glide falling preserves horizontal motion");
    p.velocity.y = .4f; glide_tick(p);
    check(p.velocity.y==.4f,"glide leaves ascent intact");
    p.on_ground=true; p.velocity.y=-2.f; glide_tick(p);
    check(p.velocity.y==-2.f,"glide leaves grounded actor intact"); p.on_ground=false;
    input.jump=input.sneak=true; p.ability_flags=0x80;
    flight_tick(p,input,{});
    check(p.ability_flags==0x83 && close(p.velocity.y,.2f),"flight preserves other flags and prioritizes jump");
    flight_disable(p); check(p.ability_flags==0x80,"flight disable clears only historical bits");
    input={}; input.forward=true; input.right=true;
    FlightSettings settings{FlightMode::Motion,.2f,.4f};
    flight_tick(p,input,settings);
    check(close(std::hypot(p.velocity.x,p.velocity.z),.4f),"diagonal flight normalized");
    p.yaw_degrees=0; input.right=false; flight_tick(p,input,settings);
    check(close(p.velocity.x,0) && close(p.velocity.z,.4f),"forward flight at yaw zero");
    input.forward=false; input.right=true; flight_tick(p,input,settings);
    check(close(p.velocity.x,-.4f) && close(p.velocity.z,0),"right flight follows recovered coordinate convention");
    input={}; flight_tick(p,input,settings);
    check(p.velocity.x==0 && p.velocity.z==0,"motion flight stops without input");
    p.pitch_degrees=-90; p.on_ground=true; jetpack_tick(p);
    check(close(p.velocity.y,.5f) && close(p.velocity.z,0),"jetpack upward look also applies while grounded");
    p.velocity={1,2,3}; fast_stop_tick(p,input,false);
    check(p.velocity.x==1,"fast stop requires component flag");
    fast_stop_tick(p,input,true); check(p.velocity.x==0 && p.velocity.y==2 && p.velocity.z==0,"fast stop preserves vertical motion");
    int delay=10; no_jump_delay(delay); check(delay==0,"no jump delay clears supplied counter");
    bool enabled=false; float light=.8f; float intensity=.2f;
    full_bright_light(light,.4f); check(light==.8f,"full bright never darkens light route");
    full_bright_vision(enabled,intensity,.9f); check(enabled && intensity==.9f,"vision route overrides effect");
    float time=.7f; time_changer(time,.3f); check(time==.3f,"time event override");
    Timer timer; float rate=17.f; timer.enable(rate); timer.tick(rate,40.f); timer.enable(rate); timer.disable(rate);
    check(rate==17.f,"timer restores first activation value");
    timer.tick(rate,30.f); check(rate==17.f,"disabled timer leaves game rate intact");
    float once=approach(0,1,14,.1f), twice=approach(approach(0,1,14,.05f),1,14,.05f);
    check(close(once,twice),"exponential animation independent of frame subdivision");
    check(menu_opacity(0)==0 && menu_opacity(1)==1 && close(menu_opacity(.5f),.5f),"menu smoothstep endpoints");
    bool rejected=false; try { glide_tick(p,std::numeric_limits<float>::quiet_NaN()); } catch(const std::invalid_argument&) { rejected=true; }
    check(rejected,"reject malformed setting");
    std::cout << checks << " offline behavior checks passed; Minecraft integration is not tested.\n";
}
