// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <algorithm>

namespace lr {
// Externally synchronized, single producer/single consumer. A view remains
// valid until pop(); producer writes cannot overlap any unconsumed payload.
// Unlike fixed maximum-sized slots, small packets share the byte capacity.
template<class Meta, size_t Capacity, size_t Count> class PacketQueue {
    static_assert(Capacity && Count);
    struct Entry { Meta meta{}; size_t offset = 0, size = 0; };
    uint8_t bytes[Capacity]{};
    Entry entries[Count]{};
    size_t read = 0, write = 0, tail = 0;
public:
    struct Metrics {
        size_t bytes = 0, packets = 0, highBytes = 0, highPackets = 0;
        uint64_t overflows = 0;
    };
    struct View {
        Meta meta{};
        const uint8_t* first = nullptr;
        size_t firstSize = 0;
        const uint8_t* second = nullptr;
        size_t secondSize = 0;
    };
private:
    Metrics metrics{};
public:
    bool push(const Meta& meta, const void* data, size_t size) {
        if (!size || size > Capacity - metrics.bytes || metrics.packets == Count) {
            ++metrics.overflows; return false;
        }
        const size_t first = std::min(size, Capacity - tail);
        std::memcpy(bytes + tail, data, first);
        if (first < size) std::memcpy(bytes, static_cast<const uint8_t*>(data) + first, size - first);
        entries[write] = {meta, tail, size};
        write = (write + 1) % Count;
        tail = (tail + size) % Capacity;
        metrics.bytes += size; ++metrics.packets;
        metrics.highBytes = std::max(metrics.highBytes, metrics.bytes);
        metrics.highPackets = std::max(metrics.highPackets, metrics.packets);
        return true;
    }
    bool peek(View& view) const {
        if (!metrics.packets) return false;
        const auto& entry = entries[read];
        view.meta = entry.meta;
        view.first = bytes + entry.offset;
        view.firstSize = std::min(entry.size, Capacity - entry.offset);
        view.second = bytes;
        view.secondSize = entry.size - view.firstSize;
        return true;
    }
    void pop() {
        if (!metrics.packets) return;
        metrics.bytes -= entries[read].size; --metrics.packets;
        read = (read + 1) % Count;
    }
    Metrics snapshot() const { return metrics; }
};
}
