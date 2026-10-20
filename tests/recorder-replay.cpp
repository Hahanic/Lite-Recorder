// Read-only input fixture, new output directory. Uses EXACTLY the on-device core.
#include "recorder.hpp"
#include <cassert>
#include <fstream>
#include <sstream>
#include <vector>
#include <filesystem>
#include <iostream>
struct Packet { char stream; uint64_t ts,frames,offset; size_t size; };
static lr::Recorder recorder;
std::vector<uint8_t> read(const std::string& path) {
    std::ifstream f(path,std::ios::binary);assert(f);
    return {std::istreambuf_iterator<char>(f),std::istreambuf_iterator<char>()};
}
int main(int argc,char** argv) {
    assert(argc==4);
    const std::string input=argv[1],output=argv[2],mode=argv[3];
    assert(!std::filesystem::exists(output));std::filesystem::create_directories(output);
    const auto codec=read(input+"/codec.h264"),video=read(input+"/video.h264"),audio=read(input+"/audio.pcm");
    const uint8_t* sps=nullptr;const uint8_t* pps=nullptr;size_t sn=0,pn=0;
    lr::visitNals(codec.data(),codec.size(),[&](const uint8_t* p,size_t n,unsigned type) {
        if(type==7) { sps=p;sn=n; }if(type==8) { pps=p;pn=n; }
    });
    lr::AvcConfig config;assert(config.parse(sps,sn,pps,pn));
    lr::AvcConfig bad;assert(!bad.parse(sps,5,pps,pn));
    std::ifstream index(input+"/packets.csv");std::string line;std::getline(index,line);
    std::vector<Packet> packets;
    while(std::getline(index,line)) {
        std::replace(line.begin(),line.end(),',',' ');std::istringstream in(line);Packet p{};
        assert(in>>p.stream>>p.ts>>p.frames>>p.offset>>p.size);packets.push_back(p);
    }
    std::stable_sort(packets.begin(),packets.end(),[](auto& a,auto& b){return a.ts<b.ts;});
    assert(!packets.empty());
    if(mode=="size") recorder.limits(8*1024*1024,86400ULL*1000000);
    if(mode=="time") recorder.limits(1024ULL*1024*1024,10000000);
    if(mode=="missing") config.ready=false;
    assert(recorder.start(output.c_str()));
    // Parameter-only packets must not create a video frame or an empty clip.
    assert(recorder.video(packets.front().ts,codec.data(),codec.size(),config));
    assert(!recorder.writing()&&recorder.videoPackets==0);
    const auto base=packets.front().ts,span=packets.back().ts-base+33333;
    bool paused=false,resumed=false;
    uint64_t frozenDuration=0,frozenBytes=0;
    uint64_t lastSource=0;
    bool joinPending=false;
    std::vector<uint64_t> joins;
    for(unsigned repeat=0;repeat<(mode=="long"?3u:1u);++repeat) for(const auto& p:packets) {
        const auto ts=p.ts+repeat*span;
        if((mode=="pause"||mode=="quick-pause"||mode=="stop-paused")&&!paused&&ts-base>=20000000) {
            assert(recorder.pause());paused=true;frozenDuration=recorder.duration();frozenBytes=recorder.bytes();
            joinPending=true;
            assert(recorder.pause()); // repeated command is harmless
        }
        if((mode=="pause"||mode=="quick-pause")&&paused&&!resumed&&
            ts-base>=(mode=="quick-pause"?20100000u:30000000u)) {
            recorder.resume();recorder.resume();resumed=true;
            assert(!recorder.writing()); // still waiting for an independent frame
        }
        const auto& bytes=p.stream=='V'?video:audio;assert(p.offset+p.size<=bytes.size());
        if(p.stream=='V'&&!recorder.isPaused()) {
            if(lastSource&&ts-lastSource>=2000000) joinPending=true;
            lastSource=ts;
        }
        const auto beforeFrames=recorder.videoPackets,beforeDuration=recorder.duration();
        const bool ok=p.stream=='V'?recorder.video(ts,bytes.data()+p.offset,p.size,config):
            recorder.audio(ts,bytes.data()+p.offset,p.size);
        assert(ok);
        if(p.stream=='V'&&recorder.videoPackets!=beforeFrames&&joinPending) {
            assert(lr::nalMask(bytes.data()+p.offset,p.size)&(uint64_t(1)<<5));
            if(recorder.partCount()==1) {
                // A join appends immediately after the previous media endpoint.
                assert(recorder.duration()==beforeDuration+33333);
                joins.push_back(beforeDuration);
            }
            joinPending=false;
        }
        if(paused&&!resumed) assert(recorder.duration()==frozenDuration&&recorder.bytes()==frozenBytes);
    }
    if(mode=="timestamp") {
        const auto first=std::find_if(packets.begin(),packets.end(),[](auto p){return p.stream=='V';});
        assert(!recorder.video(1,video.data()+first->offset,first->size,config));
        assert(recorder.error==lr::Fault::Timestamp);
    }
    if(mode=="rename-failure") std::filesystem::create_directory(output+"/"+([](unsigned n) {
        char name[32];snprintf(name,sizeof(name),"%06u.mkv",n);return std::string(name);
    })(recorder.partCount()));
    const bool stopped=recorder.stop();
    if(mode=="rename-failure") {
        assert(!stopped&&recorder.error==lr::Fault::File);return 0;
    }
    assert(stopped);
    if(mode=="missing") assert(recorder.partCount()==0&&recorder.bytes()==0&&recorder.needCodec);
    else assert(recorder.completedCount()==recorder.partCount()&&recorder.completedCount()>0);
    if(mode=="long") assert(recorder.duration()>120000000);
    if(mode=="natural"||mode=="pause"||mode=="quick-pause"||mode=="stop-paused"||
        mode=="long"||mode=="timestamp") assert(recorder.partCount()==1);
    if(mode=="natural"||mode=="pause"||mode=="quick-pause"||mode=="long") assert(!joins.empty());
    uint64_t actualBytes=0;
    for(const auto& entry:std::filesystem::directory_iterator(output)) {
        assert(entry.path().extension()!=".part");
        if(entry.path().extension()==".mkv") actualBytes+=entry.file_size();
    }
    assert(actualBytes==recorder.bytes());
    std::ofstream report(output+"/replay.json");
    report<<"{\"mode\":\""<<mode<<"\",\"parts\":"<<recorder.partCount()
        <<",\"duration_us\":"<<recorder.duration()<<",\"bytes\":"<<recorder.bytes()
        <<",\"video_packets\":"<<recorder.videoPackets<<",\"audio_packets\":"<<recorder.audioPackets
        <<",\"skipped_video\":"<<recorder.skippedVideo<<",\"skipped_audio\":"<<recorder.skippedAudio
        <<",\"joins_us\":[";
    for(size_t i=0;i<joins.size();++i) { if(i) report<<',';report<<joins[i]; }
    report<<"]}\n";
    std::cout<<mode<<": "<<recorder.partCount()<<" parts, "<<recorder.duration()/1e6<<" seconds, "
        <<recorder.videoPackets<<" frames, "<<recorder.bytes()<<" bytes\n";
}
