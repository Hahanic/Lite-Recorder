#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "capture_timing.hpp"

namespace lr {
constexpr uint64_t ModuleId = 0x00FF4C5250524231ULL;
constexpr uint32_t Magic = 0x3150524c; // LRP1
constexpr uint32_t Version = 4;
constexpr const char* Root = "/config/lite-recorder";
constexpr const char* StatusPath = "/config/lite-recorder/status.bin";
constexpr const char* CommandPath = "/config/lite-recorder/command.bin";
enum class State : uint32_t { Idle, Waiting, Recording, Saved, Error, Stalled, Paused, NeedCodec };
enum class Op : uint32_t { Start = 1, Stop = 2, Pause = 3, Resume = 4 };
enum class Fault : uint32_t { None, Capture, QueueFull, Disk, File, NoVideo, Thread, Conflict, Timestamp, Stalled, Clock, Codec };
struct Command {
    uint32_t magic = Magic, version = Version;
    uint64_t boot = 0, id = 0;
    Op op = Op::Stop;
    uint32_t reserved = 0;
};
struct Status {
    uint32_t magic = Magic, version = Version;
    uint64_t boot = 0, heartbeatUs = 0, commandId = 0;
    State state = State::Idle;
    Fault fault = Fault::None;
    uint32_t result = 0, firmware = 0;
    uint64_t videoBytes = 0, audioBytes = 0, indexBytes = 0;
    uint64_t videoPackets = 0, audioPackets = 0;
    uint64_t durationUs = 0, freeBytes = 0;
    uint64_t nalMask = 0;
    char directory[160] = {};
    uint64_t elapsedUs = 0, videoAgeUs = 0, audioAgeUs = 0;
    StopReason stopReason = StopReason::None;
    uint32_t reserved = 0;
    uint64_t recordedBytes = 0;
    uint32_t parts = 0, completedParts = 0;
};
static_assert(sizeof(Command) == 32);
static_assert(sizeof(Status) == 320);
inline bool valid(const Command& c, uint64_t boot, uint64_t last) {
    return c.magic == Magic && c.version == Version && c.boot == boot && c.id > last &&
        (c.op == Op::Start || c.op == Op::Stop || c.op == Op::Pause || c.op == Op::Resume);
}
inline bool valid(const Status& s) { return s.magic == Magic && s.version == Version; }
template<class T> bool readFile(const char* path, T& out) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    T temp{};
    const bool ok = fread(&temp, 1, sizeof(temp), f) == sizeof(temp) && fgetc(f) == EOF;
    fclose(f);
    if (ok) out = temp;
    return ok;
}
template<class T> bool replaceFile(const char* path, const T& data) {
    char temp[224];
    snprintf(temp, sizeof(temp), "%s.tmp", path);
    FILE* f = fopen(temp, "wb");
    if (!f) return false;
    bool ok = fwrite(&data, 1, sizeof(data), f) == sizeof(data);
    if (fclose(f) != 0) ok = false;
    if (ok && rename(temp, path) == 0) return true;
    // libnx's FAT rename may not replace an existing destination. Readers tolerate
    // the brief absent-file interval; partial structs are never published.
    if (ok) { remove(path); if (rename(temp, path) == 0) return true; }
    remove(temp);
    return false;
}
inline uint64_t nalMask(const uint8_t* p, size_t n) {
    uint64_t mask = 0;
    for (size_t i = 0; i + 3 < n; ++i) {
        if (p[i] || p[i+1]) continue;
        size_t h = i + 2;
        if (p[h] == 0) ++h;
        if (h + 1 < n && p[h] == 1) mask |= uint64_t(1) << (p[h+1] & 31);
    }
    return mask;
}
// Visit complete Annex-B NAL units in a GRC packet. Never invent codec headers.
template<class Fn> void visitNals(const uint8_t* p, size_t n, Fn fn) {
    size_t start = n, header = n;
    for (size_t i = 0; i + 3 < n; ++i) {
        if (p[i] || p[i + 1]) continue;
        size_t h = i + 2;
        if (p[h] == 0) ++h;
        if (h + 1 >= n || p[h] != 1) continue;
        if (start != n) fn(p + start, i - start, p[header] & 31);
        start = i; header = h + 1; i = h;
    }
    if (start != n) fn(p + start, n - start, p[header] & 31);
}
}
