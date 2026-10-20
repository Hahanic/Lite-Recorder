// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
namespace lr {
enum class StopReason : uint32_t { None, User, Duration, WaitTimeout, VideoStall, AudioStall, Failure, Clock };
constexpr uint64_t CaptureLimitUs = 120000000, WaitLimitUs = 120000000;
constexpr uint64_t StallLimitUs = 120000000;
constexpr uint64_t GapLimitUs = 2000000;
struct CaptureTiming {
    uint64_t requested = 0, firstVideo = 0, lastVideo = 0, lastAudio = 0;
    uint64_t lastVideoTimestamp = 0, activeVideoUs = 0;
    bool videoSeen = false, audioSeen = false;
    void video(uint64_t now, uint64_t timestamp) {
        if (!videoSeen) firstVideo = now;
        else if (timestamp >= lastVideoTimestamp && timestamp - lastVideoTimestamp < GapLimitUs)
            activeVideoUs += timestamp - lastVideoTimestamp;
        lastVideoTimestamp = timestamp; lastVideo = now; videoSeen = true;
    }
    void video(uint64_t now) { video(now, now); }
    void audio(uint64_t now) { lastAudio = now; audioSeen = true; }
    uint64_t elapsed(uint64_t) const { return activeVideoUs; }
    uint64_t wallElapsed(uint64_t now) const { return videoSeen && now >= firstVideo ? now - firstVideo : 0; }
    uint64_t videoAge(uint64_t now) const {
        const uint64_t ref = videoSeen ? lastVideo : requested;
        return now >= ref ? now - ref : 0;
    }
    uint64_t audioAge(uint64_t now) const {
        const uint64_t ref = audioSeen ? lastAudio : videoSeen ? firstVideo : requested;
        return now >= ref ? now - ref : 0;
    }
    StopReason poll(uint64_t now) const {
        if (now < requested || (videoSeen && now < lastVideo) || (audioSeen && now < lastAudio))
            return StopReason::Clock;
        if (!videoSeen) return now - requested >= WaitLimitUs ? StopReason::WaitTimeout : StopReason::None;
        // Never report a successful timed capture while either stream is stale.
        if (videoAge(now) >= StallLimitUs) return StopReason::VideoStall;
        if (audioAge(now) >= StallLimitUs) return StopReason::AudioStall;
        return activeVideoUs >= CaptureLimitUs && videoAge(now) < GapLimitUs && audioAge(now) < GapLimitUs
            ? StopReason::Duration : StopReason::None;
    }
};
inline const char* stopReasonName(StopReason reason) {
    switch (reason) {
    case StopReason::None: return "none";
    case StopReason::User: return "user_command";
    case StopReason::Duration: return "120_seconds_of_video_excluding_long_gaps";
    case StopReason::WaitTimeout: return "initial_video_wait_timeout";
    case StopReason::VideoStall: return "video_stalled_120_seconds";
    case StopReason::AudioStall: return "audio_stalled_120_seconds";
    case StopReason::Failure: return "error";
    case StopReason::Clock: return "monotonic_clock_regressed";
    }
    return "unknown";
}
}
