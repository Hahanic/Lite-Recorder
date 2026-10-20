#include "protocol.hpp"
#include <cassert>
#include <vector>
int main() {
    lr::Command c; c.boot = 42; c.id = 7; c.op = lr::Op::Start;
    assert(lr::valid(c, 42, 6));
    assert(!lr::valid(c, 41, 6)); // old boot cannot start a new recording
    assert(!lr::valid(c, 42, 7)); // duplicate commands are not replayed
    c.op=lr::Op::Pause;assert(lr::valid(c,42,6));
    c.op=lr::Op::Resume;assert(lr::valid(c,42,6));
    c.version=3;assert(!lr::valid(c,42,6));c.version=lr::Version;
    c.magic = 0; assert(!lr::valid(c, 42, 6)); c.magic = lr::Magic;
    c.op = static_cast<lr::Op>(99); assert(!lr::valid(c, 42, 6));
    const uint8_t stream[] = {0,0,0,1,0x67,0x55,0,0,1,0x68,0x66,0,0,0,1,0x65,0x88};
    assert(lr::nalMask(stream, sizeof(stream)) == ((1ULL<<7)|(1ULL<<8)|(1ULL<<5)));
    std::vector<unsigned> types; std::vector<size_t> lengths;
    lr::visitNals(stream, sizeof(stream), [&](const uint8_t*, size_t n, unsigned type) {
        types.push_back(type); lengths.push_back(n);
    });
    assert((types == std::vector<unsigned>{7,8,5}));
    assert((lengths == std::vector<size_t>{6,5,6}));
    for (size_t n = 0; n < 5; ++n) assert(lr::nalMask(stream, n) == 0);
    lr::Status status; status.boot = 42;
    assert(lr::replaceFile("protocol-test.bin", status));
    lr::Status read; assert(lr::readFile("protocol-test.bin", read) && read.boot == 42);
    FILE* f = fopen("protocol-test.bin", "ab"); fputc(0, f); fclose(f);
    assert(!lr::readFile("protocol-test.bin", read));
    f = fopen("protocol-test.bin", "wb"); fwrite(&status, 1, 30, f); fclose(f);
    assert(!lr::readFile("protocol-test.bin", read));
    remove("protocol-test.bin");
    puts("Protocol tests passed: boot isolation, replay, formats, Annex-B, truncated files.");
}
