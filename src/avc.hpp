// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "protocol.hpp"
#include <algorithm>
namespace lr {
inline size_t prefixSize(const uint8_t* p, size_t n) {
    if (n >= 4 && !p[0] && !p[1] && !p[2] && p[3] == 1) return 4;
    if (n >= 3 && !p[0] && !p[1] && p[2] == 1) return 3;
    return 0;
}
struct Bits {
    uint8_t data[4096]{};
    size_t size = 0, bit = 0;
    bool ok = true;
    Bits(const uint8_t* nal, size_t n) {
        if (n > sizeof(data)) { ok = false; return; }
        for (size_t i = 1; i < n; ++i) {
            if (i >= 3 && nal[i] == 3 && !nal[i-1] && !nal[i-2]) continue;
            data[size++] = nal[i];
        }
    }
    uint32_t get(unsigned n) {
        if (n > 32 || bit + n > size * 8) { ok = false; return 0; }
        uint32_t v = 0;
        while (n--) { v = (v << 1) | ((data[bit / 8] >> (7 - bit % 8)) & 1); ++bit; }
        return v;
    }
    uint32_t ue() {
        unsigned zeros = 0;
        while (ok && !get(1)) if (++zeros > 30) { ok = false; return 0; }
        return ((uint32_t(1) << zeros) - 1) + get(zeros);
    }
    int32_t se() { const auto v = ue(); return v & 1 ? int32_t((v + 1) / 2) : -int32_t(v / 2); }
};
struct AvcConfig {
    uint8_t sps[4096]{}, pps[4096]{};
    size_t spsSize = 0, ppsSize = 0;
    uint32_t width = 0, height = 0;
    bool ready = false;
    bool parse(const uint8_t* s, size_t sn, const uint8_t* p, size_t pn) {
        ready = false;
        const auto sh = prefixSize(s, sn), ph = prefixSize(p, pn);
        if (!sh || !ph || sn <= sh + 3 || pn <= ph || sn-sh > sizeof(sps) || pn-ph > sizeof(pps)) return false;
        s += sh; sn -= sh; p += ph; pn -= ph;
        if ((s[0] & 31) != 7 || (p[0] & 31) != 8) return false;
        Bits b(s, sn);
        const auto profile = b.get(8); b.get(16); b.ue();
        uint32_t chroma = 1, separate = 0;
        if (profile == 100 || profile == 110 || profile == 122 || profile == 244 ||
            profile == 44 || profile == 83 || profile == 86 || profile == 118 ||
            profile == 128 || profile == 138 || profile == 139 || profile == 134 || profile == 135) {
            chroma = b.ue(); if (chroma > 3) return false;
            if (chroma == 3) separate = b.get(1);
            if (b.ue() != 0 || b.ue() != 0) return false; // This writer accepts 8-bit AVC.
            b.get(1);
            if (b.get(1)) for (unsigned i = 0; i < (chroma == 3 ? 12u : 8u); ++i) {
                if (!b.get(1)) continue;
                int last = 8, next = 8;
                for (unsigned j = 0; j < (i < 6 ? 16u : 64u); ++j) {
                    if (next) next = (last + b.se()) & 255;
                    if (next) last = next;
                }
            }
        }
        b.ue();
        const auto order = b.ue();
        if (!order) b.ue();
        else if (order == 1) {
            b.get(1); b.se(); b.se(); const auto count = b.ue();
            if (count > 255) return false;
            for (unsigned i = 0; i < count; ++i) b.se();
        } else if (order != 2) return false;
        b.ue(); b.get(1);
        const auto w = b.ue() + 1, h = b.ue() + 1, frame = b.get(1);
        if (!frame) b.get(1);
        b.get(1);
        uint64_t left = 0, right = 0, top = 0, bottom = 0;
        if (b.get(1)) { left=b.ue(); right=b.ue(); top=b.ue(); bottom=b.ue(); }
        const auto array = separate ? 0 : chroma;
        const uint64_t cropX = array == 0 || array == 3 ? 1 : 2;
        const uint64_t cropY = (array == 1 ? 2 : 1) * (2-frame);
        const uint64_t rawW = uint64_t(w)*16, rawH = uint64_t(h)*16*(2-frame);
        if (!b.ok || rawW <= (left+right)*cropX || rawH <= (top+bottom)*cropY) return false;
        const auto ww = rawW-(left+right)*cropX, hh = rawH-(top+bottom)*cropY;
        if (ww != 1280 || hh != 720 || !frame) return false;
        width = uint32_t(ww); height = uint32_t(hh);
        memcpy(sps, s, sn); memcpy(pps, p, pn); spsSize=sn; ppsSize=pn; ready=true;
        return true;
    }
    bool same(const AvcConfig& other) const {
        return ready && other.ready && spsSize == other.spsSize && ppsSize == other.ppsSize &&
            !memcmp(sps, other.sps, spsSize) && !memcmp(pps, other.pps, ppsSize);
    }
};
}
