#include "capture_timing.hpp"
#include <cassert>
#include <cstdio>
using lr::StopReason;
int main() {
    // Actual HOME test stopped receiving at ~158.6 s. Its old 10-second error
    // now stays waiting. This test does not claim the device itself resumes.
    lr::CaptureTiming t; t.requested = 132423999;
    t.video(146225130, 146210000); t.audio(146225130);
    for (uint64_t i = 1; i <= 372; ++i) {
        const auto ts = 146210000 + i * 100000 / 3;
        t.video(ts + 15130, ts); t.audio(ts + 15130);
    }
    assert(t.activeVideoUs == 12400000);
    assert(t.poll(168624515) == StopReason::None);
    assert(t.poll(t.lastVideo + lr::StallLimitUs) == StopReason::VideoStall);
    const auto before = t.activeVideoUs;
    t.video(188625130, 188610000); t.audio(188625130);
    assert(t.activeVideoUs == before);
    for (uint64_t i = 1; i <= 3230; ++i) {
        const auto ts = 188610000 + i * 100000 / 3;
        t.video(ts + 15130, ts); t.audio(ts + 15130);
        if (i < 3228) assert(t.poll(ts + 15130) == StopReason::None);
    }
    assert(t.poll(t.lastVideo) == StopReason::Duration);
    assert(t.wallElapsed(t.lastVideo) > t.activeVideoUs + 29000000);
    // Burst writes after a disk stall use source timestamps, not drain speed.
    lr::CaptureTiming burst; burst.requested = 1000000;
    burst.video(1100000, 500000); burst.audio(1100000);
    burst.video(1800000, 533333); burst.audio(1800000);
    burst.video(1800001, 566667);
    assert(burst.activeVideoUs == 66667);
    lr::CaptureTiming healthy; healthy.requested = 1000000;
    for (uint64_t i = 0; i <= 3600; ++i) {
        const auto now = 2000000 + i * 100000 / 3;
        healthy.video(now); healthy.audio(now);
        assert(healthy.poll(now) == (i == 3600 ? StopReason::Duration : StopReason::None));
    }
    lr::CaptureTiming missing; missing.requested = 1000000;
    assert(missing.poll(120999999) == StopReason::None);
    assert(missing.poll(121000000) == StopReason::WaitTimeout);
    assert(missing.poll(999999) == StopReason::Clock);
    missing.video(2000000); missing.video(122000000);
    assert(missing.poll(122000000) == StopReason::AudioStall);
    missing.audio(123000000);
    assert(missing.poll(122999999) == StopReason::Clock);
    puts("Timing passed: HOME trace, 30s resume, 120s active video, burst draining, wait timeout, missing audio, clock.");
}
