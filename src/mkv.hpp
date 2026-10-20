// SPDX-License-Identifier: GPL-3.0-or-later
// Matroska V_MPEG4/ISO/AVC + A_PCM/INT/LIT; bounded memory, no encoding.
#pragma once
#include "avc.hpp"
#include <climits>
namespace lr {
template<size_t N> struct Ebml {
    uint8_t data[N]{};
    size_t size = 0;
    bool ok = true;
    void raw(const void* p, size_t n) {
        if (n > N-size) { ok=false; return; }
        memcpy(data+size,p,n); size+=n;
    }
    void be(uint64_t v, unsigned n) {
        uint8_t b[8]; for (unsigned i=0;i<n;++i) b[n-1-i]=uint8_t(v>>(i*8));
        raw(b,n);
    }
    void id(uint32_t id) { unsigned n=1; for(auto v=id;v>>=8;) ++n; be(id,n); }
    void length(uint64_t v) {
        for(unsigned n=1;n<=8;++n) if(v<(uint64_t(1)<<(7*n))-1) { be(v|(uint64_t(1)<<(7*n)),n);return; }
        ok=false;
    }
    void header(uint32_t tag,uint64_t n) { id(tag);length(n); }
    void number(uint32_t tag,uint64_t v,unsigned n=0) {
        if(!n) { n=1;for(auto t=v;t>>=8;) ++n; } header(tag,n);be(v,n);
    }
    void text(uint32_t tag,const char* s) { auto n=strlen(s);header(tag,n);raw(s,n); }
    void real(uint32_t tag,double d) { uint64_t u;memcpy(&u,&d,8);header(tag,8);be(u,8); }
    template<size_t M> void master(uint32_t tag,const Ebml<M>& b) {
        if(!b.ok) { ok=false;return; } header(tag,b.size);raw(b.data,b.size);
    }
    void padding(size_t n) {
        if(n<2 || n>128) { ok=false;return; } header(0xec,n-2);
        for(size_t i=2;i<n;++i) be(0,1);
    }
};
class MkvWriter {
    FILE* file = nullptr;
    uint64_t offset=0, extent=0, segmentStart=0, seekPos=0, durationPos=0;
    bool good=true;
    struct Cue { uint64_t tick,position; };
    Cue cues[1024]{};
    size_t cueCount=0;
    uint64_t lastCue=0, endUs=0, lastVideo=0, lastAudio=0;
    bool hasVideo=false,hasAudio=false;
    char buffering[65536]{};
    bool put(const void* p,size_t n) {
        if(!file || !good) return false;
        const auto wrote=fwrite(p,1,n,file);offset+=wrote;extent=std::max(extent,offset);
        good=wrote==n;return good;
    }
    template<size_t N> bool put(const Ebml<N>& b) { if(!b.ok) good=false;return good&&put(b.data,b.size); }
    bool seek(uint64_t p) {
        if(p>LONG_MAX || !file || fseek(file,long(p),SEEK_SET)) { good=false;return false; }
        offset=p;return true;
    }
    Ebml<128> cue(const Cue& c) {
        Ebml<64> track;track.number(0xf7,1);track.number(0xf1,c.position);
        Ebml<96> body;body.number(0xb3,c.tick);body.master(0xb7,track);
        Ebml<128> entry;entry.master(0xbb,body);return entry;
    }
    bool block(uint8_t track,uint64_t us,bool key,size_t payload) {
        const uint64_t tick=(us+50)/100; // TimecodeScale = 0.1 ms.
        const uint64_t pos=offset-segmentStart;
        Ebml<32> time;time.number(0xe7,tick);
        Ebml<32> sample;sample.header(0xa3,payload+4);
        sample.be(0x80|track,1);sample.be(0,2);sample.be(key?0x80:0,1);
        Ebml<16> cluster;cluster.header(0x1f43b675,time.size+sample.size+payload);
        if(track==1 && key && cueCount<1024 && (!cueCount || us-lastCue>=2000000)) {
            cues[cueCount++]={tick,pos};lastCue=us;
        }
        return put(cluster)&&put(time)&&put(sample);
    }
public:
    bool isOpen() const { return file!=nullptr; }
    uint64_t bytes() const { return extent; }
    uint64_t duration() const { return endUs; }
    bool open(const char* path,const AvcConfig& config) {
        if(file || !config.ready) return false;
        file=fopen(path,"wb+");
        if(!file) return false;
        good=true;offset=extent=0;cueCount=0;lastCue=endUs=lastVideo=lastAudio=0;hasVideo=hasAudio=false;
        if(setvbuf(file,buffering,_IOFBF,sizeof(buffering))) { fclose(file);file=nullptr;return false; }
        Ebml<128> ebml;ebml.number(0x4286,1);ebml.number(0x42f7,1);
        ebml.number(0x42f2,4);ebml.number(0x42f3,8);ebml.text(0x4282,"matroska");
        ebml.number(0x4287,4);ebml.number(0x4285,2);
        Ebml<160> head;head.master(0x1a45dfa3,ebml);put(head);
        const uint8_t segment[]={0x18,0x53,0x80,0x67,0x01,0xff,0xff,0xff,0xff,0xff,0xff,0xff};
        put(segment,sizeof(segment));segmentStart=offset;seekPos=offset;
        Ebml<64> padding;padding.padding(64);put(padding);
        Ebml<160> info;info.number(0x2ad7b1,100000);info.text(0x4d80,"Lite Recorder");
        info.text(0x5741,"Lite Recorder 0.2.3");info.real(0x4489,0);
        Ebml<192> infoMaster;infoMaster.master(0x1549a966,info);put(infoMaster);durationPos=offset-8;
        Ebml<9000> avcc;avcc.be(1,1);avcc.raw(config.sps+1,3);avcc.be(0xff,1);avcc.be(0xe1,1);
        avcc.be(config.spsSize,2);avcc.raw(config.sps,config.spsSize);avcc.be(1,1);
        avcc.be(config.ppsSize,2);avcc.raw(config.pps,config.ppsSize);
        Ebml<64> dimensions;dimensions.number(0xb0,config.width);dimensions.number(0xba,config.height);
        Ebml<10000> video;video.number(0xd7,1);video.number(0x73c5,1);video.number(0x83,1);
        video.number(0x9c,0);video.number(0x23e383,33333333);video.text(0x86,"V_MPEG4/ISO/AVC");
        video.master(0x63a2,avcc);video.master(0xe0,dimensions);
        Ebml<64> audioSpec;audioSpec.real(0xb5,48000);audioSpec.number(0x9f,2);audioSpec.number(0x6264,16);
        Ebml<192> audio;audio.number(0xd7,2);audio.number(0x73c5,2);audio.number(0x83,2);
        audio.number(0x9c,0);audio.text(0x86,"A_PCM/INT/LIT");audio.master(0xe1,audioSpec);
        Ebml<11000> tracks;tracks.master(0xae,video);tracks.master(0xae,audio);
        Ebml<16> tracksHead;tracksHead.header(0x1654ae6b,tracks.size);put(tracksHead);put(tracks);
        return good;
    }
    bool video(uint64_t us,const uint8_t* data,size_t size) {
        if(hasVideo && us<lastVideo) return false;
        size_t payload=0;bool key=false,valid=true;
        visitNals(data,size,[&](const uint8_t* p,size_t n,unsigned type) {
            auto prefix=prefixSize(p,n);if(!prefix || n<=prefix) { valid=false;return; }
            payload+=4+n-prefix;if(type==5) key=true;
        });
        if(!valid || !payload || (!hasVideo && !key)) return false;
        if(!block(1,us,key,payload)) return false;
        visitNals(data,size,[&](const uint8_t* p,size_t n,unsigned) {
            const auto prefix=prefixSize(p,n);Ebml<4> len;len.be(n-prefix,4);put(len);put(p+prefix,n-prefix);
        });
        if(!good) return false;
        lastVideo=us;hasVideo=true;endUs=std::max(endUs,us+33333);return true;
    }
    bool audio(uint64_t us,const uint8_t* data,size_t size) {
        if(!size || size%4 || (hasAudio&&us<lastAudio)) return false;
        if(!block(2,us,true,size)||!put(data,size)) return false;
        lastAudio=us;hasAudio=true;endUs=std::max(endUs,us+uint64_t(size/4)*1000000/48000);return true;
    }
    bool flush() { if(!file) return true;if(fflush(file)) good=false;return good; }
    bool close() {
        if(!file) return true;
        const auto cuesPosition=offset-segmentStart;
        uint64_t cueBytes=0;
        for(size_t i=0;i<cueCount;++i) cueBytes+=cue(cues[i]).size;
        Ebml<16> header;header.header(0x1c53bb6b,cueBytes);put(header);
        for(size_t i=0;i<cueCount;++i) put(cue(cues[i]));
        const auto end=offset;
        if(good && seek(durationPos)) {
            Ebml<8> duration;double value=double(endUs)/100;uint64_t bits;memcpy(&bits,&value,8);
            duration.be(bits,8);put(duration);
        }
        if(good && seek(seekPos)) {
            Ebml<40> body;body.header(0x53ab,4);body.be(0x1c53bb6b,4);body.number(0x53ac,cuesPosition,8);
            Ebml<48> entry;entry.master(0x4dbb,body);
            Ebml<64> seekHead;seekHead.master(0x114d9b74,entry);seekHead.padding(64-seekHead.size);put(seekHead);
        }
        if(good) seek(end);
        bool ok=flush();if(fclose(file)) ok=false;file=nullptr;return ok;
    }
};
}
