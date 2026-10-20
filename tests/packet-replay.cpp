// Optional, read-only replay against a real probe capture. Fixtures are not
// distributed. This models 0.75-second writer stalls, not console scheduling.
#include "packet_queue.hpp"
#include <cassert>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <iterator>
#include <iostream>
struct Packet { char stream; uint64_t timestamp, frames, offset; size_t size; };
static lr::PacketQueue<Packet, 0x200000, 256> videoQueue;
static lr::PacketQueue<Packet, 0x80000, 256> audioQueue;
std::vector<uint8_t> read(const std::string& name) {
    std::ifstream f(name, std::ios::binary); assert(f);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
template<class Queue> void drain(Queue& queue, const std::vector<uint8_t>& source, size_t& offset) {
    typename Queue::View view;
    while (queue.peek(view)) {
        assert(view.meta.offset == offset);
        assert(view.firstSize + view.secondSize == view.meta.size);
        assert(offset + view.meta.size <= source.size());
        assert(!std::memcmp(view.first, source.data() + offset, view.firstSize));
        assert(!std::memcmp(view.second, source.data() + offset + view.firstSize, view.secondSize));
        offset += view.meta.size;
        queue.pop();
    }
}
int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string root = argv[1];
    const auto video = read(root + "/video.h264"), audio = read(root + "/audio.pcm");
    std::ifstream index(root + "/packets.csv"); assert(index);
    std::string line; std::getline(index, line);
    std::vector<Packet> packets;
    while (std::getline(index, line)) {
        std::replace(line.begin(), line.end(), ',', ' ');
        std::istringstream in(line); Packet packet{};
        assert(in >> packet.stream >> packet.timestamp >> packet.frames >> packet.offset >> packet.size);
        packets.push_back(packet);
    }
    assert(!packets.empty());
    std::stable_sort(packets.begin(), packets.end(), [](const auto& a, const auto& b) {
        return a.timestamp < b.timestamp;
    });
    size_t videoOffset = 0, audioOffset = 0;
    const auto base = packets.front().timestamp;
    for (const auto& p : packets) {
        const auto& source = p.stream == 'V' ? video : audio;
        assert(p.offset + p.size <= source.size());
        assert(p.stream == 'V' ? videoQueue.push(p, source.data() + p.offset, p.size)
            : audioQueue.push(p, source.data() + p.offset, p.size));
        // Pause writer for first 750 ms of every 3-second interval.
        if ((p.timestamp - base) % 3000000 >= 750000) {
            drain(videoQueue, video, videoOffset); drain(audioQueue, audio, audioOffset);
        }
    }
    drain(videoQueue, video, videoOffset); drain(audioQueue, audio, audioOffset);
    assert(videoOffset == video.size() && audioOffset == audio.size());
    std::cout << "Replay passed: " << packets.size() << " packets, "
        << videoOffset + audioOffset << " bytes unchanged; peaks V="
        << videoQueue.snapshot().highPackets << " A=" << audioQueue.snapshot().highPackets << '\n';
}
