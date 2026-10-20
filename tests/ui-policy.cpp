#include "ui_policy.hpp"
#include <cassert>
#include <cstring>
#include <initializer_list>
int main() {
    using namespace lr;
    for(auto s:{State::Idle,State::Saved,State::Error}) {
        const auto c=controls(s);
        assert(c.op==Op::Start&&!c.showStop);
        assert(actionAllowed(s,Op::Start)&&!actionAllowed(s,Op::Stop));
        assert(!actionAllowed(s,Op::Pause)&&!actionAllowed(s,Op::Resume));
        assert(inactiveDataLabel(s));
    }
    for(auto s:{State::Waiting,State::NeedCodec,State::Recording,State::Stalled}) {
        assert(controls(s).op==Op::Pause&&controls(s).showStop);
        assert(actionAllowed(s,Op::Pause)&&actionAllowed(s,Op::Stop));
        assert(!actionAllowed(s,Op::Start)&&!actionAllowed(s,Op::Resume));
        assert(!inactiveDataLabel(s));
    }
    assert(controls(State::Paused).op==Op::Resume&&controls(State::Paused).showStop);
    assert(actionAllowed(State::Paused,Op::Resume)&&actionAllowed(State::Paused,Op::Stop));
    assert(!actionAllowed(State::Paused,Op::Pause)&&!actionAllowed(State::Paused,Op::Start));
    Status s;
    updateDataAges(s,true,false,10000000,1000000,8000000,9000000);
    assert(s.videoAgeUs==2000000&&s.audioAgeUs==1000000);
    for(auto now:{11000000ULL,900000000ULL}) {
        updateDataAges(s,false,false,now,1000000,8000000,9000000);
        assert(!s.videoAgeUs&&!s.audioAgeUs); // saved/error do not keep counting
        updateDataAges(s,true,true,now,1000000,8000000,9000000);
        assert(!s.videoAgeUs&&!s.audioAgeUs); // a deliberate pause is not a stall
    }
    updateDataAges(s,true,false,20000000,19000000,0,0);
    assert(s.videoAgeUs==1000000&&s.audioAgeUs==1000000);
    puts("UI policy passed: state actions, stale-action rejection, idle/paused/stopped data ages, new-session ages.");
}
