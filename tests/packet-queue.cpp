#include "packet_queue.hpp"
#include <cassert>
#include <cstdio>
#include <deque>
#include <vector>
#include <random>
struct Meta { uint64_t id; };
int main() {
    lr::PacketQueue<Meta, 4096, 7> queue;
    std::deque<std::vector<uint8_t>> expected;
    std::deque<uint64_t> ids;
    std::mt19937 rng(12345);
    size_t bytes = 0;
    for (uint64_t id = 0; id < 25000; ++id) {
        if ((rng() % 3) || expected.empty()) {
            std::vector<uint8_t> data(1 + rng() % 3000);
            for (auto& value : data) value = static_cast<uint8_t>(rng());
            const bool fits = expected.size() < 7 && bytes + data.size() <= 4096;
            assert(queue.push({id}, data.data(), data.size()) == fits);
            if (fits) { bytes += data.size(); expected.push_back(data); ids.push_back(id); }
        } else {
            decltype(queue)::View view;
            assert(queue.peek(view) && view.meta.id == ids.front());
            std::vector<uint8_t> result(view.first, view.first + view.firstSize);
            result.insert(result.end(), view.second, view.second + view.secondSize);
            assert(result == expected.front());
            queue.pop(); bytes -= expected.front().size(); expected.pop_front(); ids.pop_front();
        }
        assert(queue.snapshot().bytes == bytes && queue.snapshot().packets == expected.size());
        if (!expected.empty()) {
            decltype(queue)::View view; assert(queue.peek(view));
            assert(view.firstSize + view.secondSize == expected.front().size());
            assert(!std::memcmp(view.first, expected.front().data(), view.firstSize));
            assert(!std::memcmp(view.second, expected.front().data() + view.firstSize, view.secondSize));
        }
    }
    // 0.75 seconds without a writer: 23 video frames, 36 PCM chunks.
    // Old three-frame video / 24-chunk audio queues necessarily overflow.
    static lr::PacketQueue<Meta, 0x200000, 256> video;
    static lr::PacketQueue<Meta, 0x80000, 256> audio;
    std::vector<uint8_t> payload(0x80000, 0x65);
    for (unsigned i = 0; i < 23; ++i) assert(video.push({i}, payload.data(), i ? 12000 : 0x80000));
    for (unsigned i = 0; i < 36; ++i) assert(audio.push({i}, payload.data(), 4096));
    assert(video.snapshot().overflows == 0 && audio.snapshot().overflows == 0);
    while (audio.push({99}, payload.data(), 4096)) {}
    assert(audio.snapshot().bytes == 0x80000 && audio.snapshot().overflows == 1);
    decltype(audio)::View a; assert(audio.peek(a) && a.meta.id == 0);
    puts("Queue passed: 25000 randomized wrap/full/read operations, head stability, 0.75s writer stall, bounded overflow.");
}
