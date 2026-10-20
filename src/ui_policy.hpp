// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "protocol.hpp"
namespace lr {
inline bool sessionActive(State s) {
    return s==State::Waiting || s==State::Recording || s==State::Stalled ||
        s==State::Paused || s==State::NeedCodec;
}
struct Controls { const char* label; Op op; bool showStop; };
inline Controls controls(State s) {
    if(s==State::Paused) return {"继续录制",Op::Resume,true};
    if(sessionActive(s)) return {"暂停录制",Op::Pause,true};
    return {"开始录制",Op::Start,false};
}
inline bool actionAllowed(State s,Op op) {
    if(op==Op::Start) return !sessionActive(s);
    if(op==Op::Resume) return s==State::Paused;
    if(op==Op::Pause) return sessionActive(s)&&s!=State::Paused;
    return op==Op::Stop&&sessionActive(s);
}
// Data age is a live reception indicator, not a timer since the last session.
inline void updateDataAges(Status& s,bool active,bool paused,uint64_t now,
    uint64_t started,uint64_t video,uint64_t audio) {
    s.videoAgeUs=s.audioAgeUs=0;
    if(!active||paused) return;
    const auto v=video?video:started,a=audio?audio:started;
    s.videoAgeUs=now>=v?now-v:0;s.audioAgeUs=now>=a?now-a:0;
}
inline const char* inactiveDataLabel(State s) {
    if(s==State::Paused) return "已暂停";
    if(s==State::Saved||s==State::Error) return "已结束";
    if(s==State::Idle) return "未录制";
    return nullptr;
}
}
