// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "mkv.hpp"
namespace lr {
// Shared by the console backend and read-only host replay tests.
// Pause/recovery joins at an IDR in the same MKV with a shared A/V time mapping.
class Recorder {
    MkvWriter writer;
    AvcConfig activeConfig;
    char root[160]{}, partial[224]{}, finalPath[224]{};
    uint64_t base=0,outputBase=0,lastVideoSource=0,closedBytes=0,closedDuration=0;
    bool seenVideo=false,started=false,paused=false,waitingKey=true;
    uint64_t segmentBytes=1024ULL*1024*1024, segmentUs=1800ULL*1000000;
    uint32_t parts=0,completed=0;
    bool endPart(const char* reason) {
        if(!writer.isOpen()) return true;
        bool ok=writer.close();
        const auto duration=writer.duration();
        closedBytes+=writer.bytes();closedDuration+=duration;
        if(!ok || rename(partial,finalPath)) { error=Fault::File;return false; }
        ++completed;
        char path[224];snprintf(path,sizeof(path),"%s/segments.csv",root);
        FILE* f=fopen(path,"a");
        if(!f) { error=Fault::File;return false; }
        ok=fprintf(f,"%06u.mkv,%llu,%llu,%s\n",parts,(unsigned long long)duration,
            (unsigned long long)writer.bytes(),reason)>=0;
        if(fclose(f)) ok=false;
        snprintf(path,sizeof(path),"%s/playlist.m3u",root);f=fopen(path,"a");
        if(!f) ok=false;
        else {
            if(fprintf(f,"#EXTINF:%.6f,Part %u\n%06u.mkv\n",duration/1e6,parts,parts)<0) ok=false;
            if(fclose(f)) ok=false;
        }
        if(!ok) error=Fault::File;
        return ok;
    }
public:
    Fault error=Fault::None;
    uint64_t videoPackets=0,audioPackets=0,videoBytes=0,audioBytes=0,skippedVideo=0,skippedAudio=0;
    bool needCodec=false;
    bool active() const { return started; }
    bool isPaused() const { return paused; }
    bool writing() const { return writer.isOpen()&&!waitingKey; }
    uint32_t partCount() const { return parts; }
    uint32_t completedCount() const { return completed; }
    uint64_t bytes() const { return closedBytes+(writer.isOpen()?writer.bytes():0); }
    uint64_t duration() const { return closedDuration+(writer.isOpen()?writer.duration():0); }
    // Small rollover settings permit exercising the real path without GB fixtures.
    void limits(uint64_t bytes,uint64_t us) { segmentBytes=bytes;segmentUs=us; }
    bool start(const char* directory) {
        if(started || writer.isOpen() || strlen(directory)>=sizeof(root)) return false;
        snprintf(root,sizeof(root),"%s",directory);
        closedBytes=closedDuration=base=outputBase=lastVideoSource=0;parts=completed=0;
        videoPackets=audioPackets=videoBytes=audioBytes=skippedVideo=skippedAudio=0;
        seenVideo=paused=needCodec=false;waitingKey=true;error=Fault::None;started=true;
        char path[224];snprintf(path,sizeof(path),"%s/segments.csv",root);
        FILE* f=fopen(path,"w");
        if(!f) { error=Fault::File;return false; }
        bool ok=fputs("file,duration_us,bytes,reason\n",f)>=0;if(fclose(f)) ok=false;
        snprintf(path,sizeof(path),"%s/playlist.m3u",root);f=fopen(path,"w");
        if(!f) ok=false;
        else { if(fputs("#EXTM3U\n",f)<0) ok=false;if(fclose(f)) ok=false; }
        if(!ok) error=Fault::File;
        return ok;
    }
    bool video(uint64_t ts,const uint8_t* data,size_t size,const AvcConfig& config) {
        if(!started || paused) return true;
        if(error!=Fault::None) return false;
        const auto mask=nalMask(data,size);
        if(!(mask&((uint64_t(1)<<1)|(uint64_t(1)<<5)))) { ++skippedVideo;return true; }
        if(seenVideo && ts<lastVideoSource) { error=Fault::Timestamp;return false; }
        const bool gap=seenVideo && ts-lastVideoSource>=2000000;
        const bool key=(mask&(uint64_t(1)<<5))!=0;
        if(gap) waitingKey=true;
        if(writer.isOpen()) {
            const bool roll=key&&(writer.bytes()>=segmentBytes||writer.duration()>=segmentUs);
            const bool changed=config.ready&&!activeConfig.same(config);
            if(roll||changed) {
                if(!endPart(changed?"codec_change":"size_or_time")) return false;
                waitingKey=true;
            }
        }
        lastVideoSource=ts;seenVideo=true;needCodec=!config.ready;
        if(waitingKey&&(!key||!config.ready)) { ++skippedVideo;return true; }
        if(!writer.isOpen()) {
            if(!key || !config.ready) { ++skippedVideo;return true; }
            if(parts==999999) { error=Fault::File;return false; }
            ++parts;activeConfig=config;
            snprintf(partial,sizeof(partial),"%s/%06u.mkv.part",root,parts);
            snprintf(finalPath,sizeof(finalPath),"%s/%06u.mkv",root,parts);
            if(!writer.open(partial,config)) { error=Fault::File;return false; }
        }
        if(waitingKey) {
            // Continue after BOTH tracks' previous endpoints, avoiding overlapping
            // audio at a join. Use two anchors, not unsigned source-time subtraction.
            base=ts;outputBase=writer.duration();waitingKey=false;
        }
        if(writer.bytes()>1536ULL*1024*1024) { error=Fault::Disk;return false; }
        if(!writer.video(outputBase+(ts-base),data,size)) { error=Fault::File;return false; }
        ++videoPackets;videoBytes+=size;return true;
    }
    bool audio(uint64_t ts,const uint8_t* data,size_t size) {
        if(!started || paused) return true;
        if(error!=Fault::None) return false;
        if(size%4) { error=Fault::Capture;return false; }
        if(!writer.isOpen() || waitingKey || ts>lastVideoSource+2000000) { ++skippedAudio;return true; }
        if(ts<base) {
            const uint64_t delta=base-ts;
            if(delta>1000000) { ++skippedAudio;return true; }
            const size_t cut=std::min<size_t>(size/4,size_t((delta*48000+999999)/1000000));
            data+=cut*4;size-=cut*4;ts+=(uint64_t(cut)*1000000+47999)/48000;
            if(!size || ts<base) { ++skippedAudio;return true; }
        }
        if(!writer.audio(outputBase+(ts-base),data,size)) { error=Fault::File;return false; }
        ++audioPackets;audioBytes+=size;return true;
    }
    bool pause() {
        if(!started || paused) return true;
        paused=true;waitingKey=true;return flush();
    }
    void resume() { if(started&&paused) { paused=false;seenVideo=false;waitingKey=true; } }
    bool flush() { if(!writer.flush()) { error=Fault::File;return false; }return true; }
    bool stop(const char* reason="stop") {
        const bool ok=endPart(reason);started=false;paused=false;return ok;
    }
};
}
