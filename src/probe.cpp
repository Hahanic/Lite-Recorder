// Lite Recorder Probe: original implementation using the public libnx GRC IPC API.
// No screenshot calls, JPEG encoder, network transport or firmware patches.
#include <switch.h>
#include <atomic>
#include <cerrno>
#include <initializer_list>
#include <sys/stat.h>
#include <time.h>
#include "protocol.hpp"
#include "packet_queue.hpp"
#include "recorder.hpp"

extern "C" {
u32 __nx_applet_type = AppletType_None;
u32 __nx_fs_num_sessions = 2;
static char heap[0x60000];
void __libnx_initheap() {
    extern char* fake_heap_start;
    extern char* fake_heap_end;
    fake_heap_start = heap;
    fake_heap_end = heap + sizeof(heap);
}
void __appInit() {
    Result rc = smInitialize(); if (R_FAILED(rc)) diagAbortWithResult(rc);
    rc = setsysInitialize(); if (R_FAILED(rc)) diagAbortWithResult(rc);
    SetSysFirmwareVersion fw{};
    rc = setsysGetFirmwareVersion(&fw);
    if (R_SUCCEEDED(rc)) hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
    setsysExit();
    if (R_FAILED(rc)) diagAbortWithResult(rc);
    rc = fsInitialize(); if (R_FAILED(rc)) diagAbortWithResult(rc);
    rc = fsdevMountSdmc(); if (R_FAILED(rc)) diagAbortWithResult(rc);
    timeInitialize();
}
void __appExit() { timeExit(); fsdevUnmountAll(); fsExit(); smExit(); }
}

namespace {
uint64_t clockUs() { return armTicksToNs(armGetSystemTick()) / 1000; }
constexpr uint64_t ReserveBytes = 256ULL * 1024 * 1024;
constexpr size_t VideoCapacity = 0x80000, AudioCapacity = 0x1000;
struct Meta { uint64_t timestamp, generation, nalMask, received, codecVersion; uint32_t frames, size; };
template<size_t Capacity, size_t Count> struct Ring {
    lr::PacketQueue<Meta, Capacity, Count> queue;
    using View = typename lr::PacketQueue<Meta, Capacity, Count>::View;
    Mutex mutex{};
    bool push(const Meta& m, const void* p) {
        mutexLock(&mutex);
        bool ok = queue.push(m, p, m.size);
        mutexUnlock(&mutex);
        return ok;
    }
    bool peek(View& p) { mutexLock(&mutex); bool ok = queue.peek(p); mutexUnlock(&mutex); return ok; }
    void pop() { mutexLock(&mutex); queue.pop(); mutexUnlock(&mutex); }
    auto snapshot() { mutexLock(&mutex); auto s = queue.snapshot(); mutexUnlock(&mutex); return s; }
};
// Capacity is actual payload bytes, not maximum packet size times three.
Ring<0x200000, 256> videoQueue;
Ring<0x80000, 256> audioQueue;
alignas(0x1000) uint8_t videoBuffer[VideoCapacity];
alignas(0x1000) uint8_t audioBuffer[AudioCapacity];
alignas(0x1000) uint8_t videoStack[0x8000], audioStack[0x8000];
Thread videoThread{}, audioThread{};
std::atomic<uint64_t> generation{0};
std::atomic<uint32_t> captureResult{0};
std::atomic<lr::Fault> captureFault{lr::Fault::None};
struct ReaderHealth {
    std::atomic<uint64_t> entered{0}, returned{0}, packets{0}, empty{0};
    std::atomic<uint32_t> result{0};
    std::atomic<bool> inFlight{false};
};
ReaderHealth videoReader, audioReader;
Mutex initMutex{};
bool beginAttempted = false;
Result beginResult = 0;
lr::Status status;
lr::Recorder recorder;
lr::AvcConfig config;
FILE* healthFile = nullptr;
uint64_t nextGeneration = 0, acceptedGeneration = 0, startedUs = 0, lastVideoUs = 0, lastAudioUs = 0;
uint64_t maxPacketWriteUs = 0, maxFlushUs = 0;
uint64_t loadedCodecVersion = 0;
bool codecPresent = false;
std::atomic<uint64_t> codecVersion{0};
char healthStdio[4096];
uint8_t writerPacket[VideoCapacity];
// Capture parameter sets even while idle: the encoder may emit them only when
// the game starts. Keep them separate from the unmodified packet dump.
Mutex codecMutex{};
struct ParameterSet { uint8_t data[4096]; size_t size = 0; uint64_t timestamp = 0; };
ParameterSet sps, pps;
void rememberCodec(const uint8_t* data, const Meta& m) {
    lr::visitNals(data, m.size, [&](const uint8_t* nal, size_t n, unsigned type) {
        if (type != 7 && type != 8) return;
        mutexLock(&codecMutex);
        auto& set = type == 7 ? sps : pps;
        const bool changed = set.size != n || n > sizeof(set.data) || memcmp(set.data,nal,n);
        set.size = n <= sizeof(set.data) ? n : 0;
        if (set.size) memcpy(set.data, nal, n);
        set.timestamp = m.timestamp;
        if (changed) codecVersion.fetch_add(1);
        mutexUnlock(&codecMutex);
    });
}

// GRC commands 1/2 are documented by libnx. Each blocking reader owns its
// own service session; the controller never joins a blocked capture thread.
Result transfer(Service& service, uint32_t stream, void* buffer, size_t capacity, Meta& meta) {
    struct Out { uint32_t frames, size; uint64_t timestamp; } out{};
    Result rc = serviceDispatchInOut(&service, 2, stream, out,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
        .buffers = {{buffer, capacity}});
    if (R_SUCCEEDED(rc)) { meta.timestamp = out.timestamp; meta.frames = out.frames; meta.size = out.size; }
    return rc;
}
void fault(lr::Fault f, Result rc) {
    captureResult.store(rc);
    captureFault.store(f);
}
void capture(void* arg) {
    const uint32_t stream = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));
    Service service{};
    Result rc = smGetService(&service, "grc:d");
    if (R_FAILED(rc)) { fault(lr::Fault::Capture, rc); return; }
    void* buffer = stream ? static_cast<void*>(audioBuffer) : static_cast<void*>(videoBuffer);
    const size_t capacity = stream ? AudioCapacity : VideoCapacity;
    ReaderHealth& reader = stream ? audioReader : videoReader;
    for (;;) {
        Meta m{};
        m.generation = generation.load();
        reader.entered.store(clockUs()); reader.inFlight.store(true);
        rc = transfer(service, stream, buffer, capacity, m);
        reader.result.store(rc); reader.returned.store(clockUs()); reader.inFlight.store(false);
        if (R_SUCCEEDED(rc)) {
            if (m.size) reader.packets.fetch_add(1);
            else reader.empty.fetch_add(1);
        }
        // Calling Begin twice can assert the OS recorder. Only try after its
        // documented "not initialized" response; never call Begin on every start.
        if (rc == 0x3E8D4) {
            mutexLock(&initMutex);
            if (!beginAttempted) { beginAttempted = true; beginResult = serviceDispatch(&service, 1); }
            rc = beginResult;
            mutexUnlock(&initMutex);
            if (R_SUCCEEDED(rc)) continue;
        }
        if (R_FAILED(rc)) {
            fault(lr::Fault::Capture, rc);
            svcSleepThread(250000000);
            continue;
        }
        if (m.size > capacity) { fault(lr::Fault::Capture, 0); continue; }
        if (!stream && m.size) {
            rememberCodec(static_cast<const uint8_t*>(buffer), m);
            m.nalMask = lr::nalMask(static_cast<const uint8_t*>(buffer), m.size);
            m.codecVersion = codecVersion.load();
        }
        m.received = clockUs();
        if (!m.size || !m.generation || m.generation != generation.load()) continue;
        bool ok = stream ? audioQueue.push(m, buffer) : videoQueue.push(m, buffer);
        if (!ok && m.generation == generation.load()) fault(lr::Fault::QueueFull, 0);
    }
}

bool checkSpace() {
    s64 space=0;
    Result rc=fsFsGetFreeSpace(fsdevGetDeviceFileSystem("sdmc"),"/",&space);
    if(R_FAILED(rc)) { status.result=rc;return false; }
    status.freeBytes=space>0?uint64_t(space):0;
    return status.freeBytes>ReserveBytes;
}
void refreshCodec() {
    if(loadedCodecVersion==codecVersion.load()) return;
    mutexLock(&codecMutex);
    codecPresent=sps.size&&pps.size;
    config.parse(sps.data,sps.size,pps.data,pps.size);
    loadedCodecVersion=codecVersion.load();
    mutexUnlock(&codecMutex);
}
void updateStatus(uint64_t now) {
    if(!status.directory[0]&&!recorder.active()) return;
    status.videoBytes=recorder.videoBytes;status.audioBytes=recorder.audioBytes;
    status.videoPackets=recorder.videoPackets;status.audioPackets=recorder.audioPackets;
    status.recordedBytes=recorder.bytes();status.durationUs=recorder.duration();
    status.parts=recorder.partCount();status.completedParts=recorder.completedCount();
    if(recorder.active()) status.elapsedUs=now>=startedUs?now-startedUs:0;
    const auto v=lastVideoUs?lastVideoUs:startedUs,a=lastAudioUs?lastAudioUs:startedUs;
    status.videoAgeUs=now>=v?now-v:0;status.audioAgeUs=now>=a?now-a:0;
    if(!recorder.active()) return;
    if(recorder.isPaused()) status.state=lr::State::Paused;
    else if(!config.ready) status.state=lr::State::NeedCodec;
    else if(!recorder.writing()) status.state=lr::State::Waiting;
    else if(status.videoAgeUs>=2000000 || status.audioAgeUs>=2000000) status.state=lr::State::Stalled;
    else status.state=lr::State::Recording;
}
bool health(const char* event,uint64_t now) {
    if(!healthFile) return true;
    const auto v=videoQueue.snapshot();const auto a=audioQueue.snapshot();
    return fprintf(healthFile,"%s,%llu,%u,%llu,%llu,%llu,%llu,%llu,%u,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
        event,(unsigned long long)now,(unsigned)status.state,(unsigned long long)status.commandId,
        (unsigned long long)status.durationUs,(unsigned long long)status.recordedBytes,
        (unsigned long long)status.videoAgeUs,(unsigned long long)status.audioAgeUs,status.parts,
        (unsigned long long)v.highBytes,(unsigned long long)v.highPackets,(unsigned long long)v.overflows,
        (unsigned long long)a.highBytes,(unsigned long long)a.highPackets,(unsigned long long)a.overflows,
        (unsigned long long)maxPacketWriteUs,(unsigned long long)maxFlushUs)>=0;
}
bool consume(const Meta& m,const uint8_t* first,size_t firstSize,const uint8_t* second,size_t secondSize,bool audio) {
    if(!acceptedGeneration || m.generation!=acceptedGeneration) return true;
    if(firstSize+secondSize>sizeof(writerPacket) || firstSize+secondSize!=m.size) {
        recorder.error=lr::Fault::Capture;return false;
    }
    const auto began=clockUs();
    memcpy(writerPacket,first,firstSize);if(secondSize) memcpy(writerPacket+firstSize,second,secondSize);
    bool ok;
    if(audio) { lastAudioUs=m.received;ok=recorder.audio(m.timestamp,writerPacket,m.size); }
    else {
        lastVideoUs=m.received;refreshCodec();
        // Do not describe a queued old frame using newer codec parameters.
        if(m.codecVersion!=loadedCodecVersion) return true;
        if(!config.ready && codecPresent) { recorder.error=lr::Fault::Codec;return false; }
        ok=recorder.video(m.timestamp,writerPacket,m.size,config);
    }
    maxPacketWriteUs=std::max(maxPacketWriteUs,clockUs()-began);
    return ok;
}
bool drain(unsigned limit) {
    for(unsigned i=0;i<limit;++i) {
        decltype(videoQueue)::View v;decltype(audioQueue)::View a;
        const bool hasV=videoQueue.peek(v),hasA=audioQueue.peek(a);
        if(!hasV&&!hasA) return true;
        bool ok;
        if(hasV&&(!hasA || v.meta.timestamp<=a.meta.timestamp)) {
            ok=consume(v.meta,v.first,v.firstSize,v.second,v.secondSize,false);videoQueue.pop();
        } else {
            ok=consume(a.meta,a.first,a.firstSize,a.second,a.secondSize,true);audioQueue.pop();
        }
        if(!ok) return false;
    }
    return true;
}
bool saveSummary() {
    if(!status.directory[0]) return true;
    char path[224];snprintf(path,sizeof(path),"%s/summary.json",status.directory);
    FILE* f=fopen(path,"w");if(!f) return false;
    bool ok=fprintf(f,"{\n  \"format\":\"lite-recorder-mkv-1\",\n  \"version\":\"0.2.1\",\n"
        "  \"state\":%u,\n  \"fault\":%u,\n  \"result\":%u,\n  \"stop_reason\":\"%s\",\n"
        "  \"duration_us\":%llu,\n  \"mkv_bytes\":%llu,\n  \"parts\":%u,\n  \"completed_parts\":%u,\n"
        "  \"video_packets\":%llu,\n  \"audio_packets\":%llu,\n"
        "  \"skipped_video_before_keyframe_or_codec\":%llu,\n  \"skipped_audio_outside_video\":%llu\n}\n",
        (unsigned)status.state,(unsigned)status.fault,status.result,lr::stopReasonName(status.stopReason),
        (unsigned long long)status.durationUs,(unsigned long long)status.recordedBytes,status.parts,status.completedParts,
        (unsigned long long)status.videoPackets,(unsigned long long)status.audioPackets,
        (unsigned long long)recorder.skippedVideo,(unsigned long long)recorder.skippedAudio)>=0;
    if(fclose(f)) ok=false;
    return ok;
}
void finish(lr::Fault why=lr::Fault::None,lr::StopReason reason=lr::StopReason::Failure) {
    generation.store(0);
    if(why==lr::Fault::None && !drain(1024)) why=recorder.error;
    acceptedGeneration=0;
    if(!recorder.stop(why==lr::Fault::None?"stop":"error")) why=lr::Fault::File;
    updateStatus(clockUs());
    if(why==lr::Fault::None && !status.completedParts) why=lr::Fault::NoVideo;
    status.fault=why;status.stopReason=reason;
    status.state=why==lr::Fault::None?lr::State::Saved:lr::State::Error;
    bool ok=health("stop",clockUs());
    if(healthFile) { if(fclose(healthFile)) ok=false;healthFile=nullptr; }
    if(!ok) { status.fault=lr::Fault::File;status.state=lr::State::Error; }
    if(!saveSummary()) { status.fault=lr::Fault::File;status.state=lr::State::Error; }
}
void start() {
    if(recorder.active()) return;
    const auto boot=status.boot,cmd=status.commandId;const auto fw=status.firmware;
    status=lr::Status{};status.boot=boot;status.commandId=cmd;status.firmware=fw;
    if(captureFault.load()==lr::Fault::QueueFull) { captureFault.store(lr::Fault::None);captureResult.store(0); }
    if(captureFault.load()!=lr::Fault::None) {
        status.fault=captureFault.load();status.result=captureResult.load();status.state=lr::State::Error;return;
    }
    if(!checkSpace()) { status.fault=lr::Fault::Disk;status.state=lr::State::Error;return; }
    uint64_t epoch=0;timeGetCurrentTime(TimeType_LocalSystemClock,&epoch);
    bool made=false;
    for(unsigned i=0;i<100;++i) {
        snprintf(status.directory,sizeof(status.directory),"/recordings/lite-recorder/%llu-%llu-%u",
            (unsigned long long)epoch,(unsigned long long)clockUs(),i);
        if(!mkdir(status.directory,0777)) { made=true;break; }
    }
    if(!made) { status.directory[0]=0;status.state=lr::State::Error;status.fault=lr::Fault::File;return; }
    startedUs=clockUs();lastVideoUs=lastAudioUs=maxPacketWriteUs=maxFlushUs=0;
    if(!recorder.start(status.directory)) { finish(lr::Fault::File);return; }
    char path[224];snprintf(path,sizeof(path),"%s/health.csv",status.directory);healthFile=fopen(path,"w");
    if(!healthFile || setvbuf(healthFile,healthStdio,_IOFBF,sizeof(healthStdio)) ||
        fprintf(healthFile,"event,monotonic_us,state,command_id,duration_us,mkv_bytes,video_age_us,audio_age_us,parts,"
            "v_peak_bytes,v_peak_packets,v_overflows,a_peak_bytes,a_peak_packets,a_overflows,max_packet_write_us,max_flush_us\n")<0) {
        finish(lr::Fault::File);return;
    }
    refreshCodec();updateStatus(clockUs());
    if(!health("start",clockUs())) { finish(lr::Fault::File);return; }
    acceptedGeneration=++nextGeneration;generation.store(acceptedGeneration);
}
void pause() {
    if(!recorder.active() || recorder.isPaused()) return;
    generation.store(0);
    if(!drain(1024)) { finish(recorder.error);return; }
    acceptedGeneration=0;
    if(!recorder.pause()) { finish(recorder.error);return; }
    updateStatus(clockUs());if(!health("pause",clockUs())) finish(lr::Fault::File);
}
void resume() {
    if(!recorder.active() || !recorder.isPaused()) return;
    recorder.resume();lastVideoUs=lastAudioUs=clockUs();
    acceptedGeneration=++nextGeneration;generation.store(acceptedGeneration);
    updateStatus(clockUs());if(!health("resume",clockUs())) finish(lr::Fault::File);
}
}
int main() {
    mkdir("/config",0777);mkdir(lr::Root,0777);
    mkdir("/recordings",0777);mkdir("/recordings/lite-recorder",0777);
    status.boot=clockUs();status.firmware=hosversionGet();
    Result pm=pmdmntInitialize();
    if(R_SUCCEEDED(pm)) {
        for(uint64_t tid:{0x0000000000918649ULL,0x00FF0000A53BB665ULL,0x00FF0000A53BB666ULL}) {
            uint64_t pid=0;
            if(R_SUCCEEDED(pmdmntGetProcessId(&pid,tid))&&pid) captureFault.store(lr::Fault::Conflict);
        }
        pmdmntExit();
    } else fault(lr::Fault::Capture,pm);
    Result rc=0;
    if(captureFault.load()==lr::Fault::None) {
        rc=threadCreate(&videoThread,capture,nullptr,videoStack,sizeof(videoStack),0x2C,3);
        if(R_SUCCEEDED(rc)) rc=threadStart(&videoThread);
        if(R_SUCCEEDED(rc)) rc=threadCreate(&audioThread,capture,(void*)1,audioStack,sizeof(audioStack),0x2C,3);
        if(R_SUCCEEDED(rc)) rc=threadStart(&audioThread);
        if(R_FAILED(rc)) fault(lr::Fault::Thread,rc);
    }
    uint64_t lastPublish=0,lastSpace=0;
    for(;;) {
        lr::Command cmd;
        if(lr::readFile(lr::CommandPath,cmd)&&lr::valid(cmd,status.boot,status.commandId)) {
            status.commandId=cmd.id;
            switch(cmd.op) {
            case lr::Op::Start:start();break;
            case lr::Op::Stop:if(recorder.active()) finish(lr::Fault::None,lr::StopReason::User);break;
            case lr::Op::Pause:pause();break;
            case lr::Op::Resume:resume();break;
            }
        }
        if(captureFault.load()!=lr::Fault::None) {
            status.result=captureResult.load();
            if(recorder.active()) finish(captureFault.load());
            else { status.state=lr::State::Error;status.fault=captureFault.load(); }
        }
        if(!drain(64)&&recorder.active()) finish(recorder.error);
        auto now=clockUs();updateStatus(now);
        if(now-lastSpace>=1000000) {
            lastSpace=now;
            if(!checkSpace()&&recorder.active()) finish(lr::Fault::Disk);
            if(recorder.active()) {
                const auto began=clockUs();
                const bool ok=recorder.flush()&&health("tick",now)&&!fflush(healthFile);
                maxFlushUs=std::max(maxFlushUs,clockUs()-began);
                if(!ok) finish(lr::Fault::File);
            }
        }
        if(now-lastPublish>=1000000) {
            status.heartbeatUs=clockUs();lr::replaceFile(lr::StatusPath,status);lastPublish=now;
        }
        svcSleepThread(10000000);
    }
}
