#include "overlay_storage.hpp"
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

int main() {
    // Reproduce the precondition of the reported crash: mount is unavailable.
    // No stdio directory operation may run in this case.
    int directoryCalls = 0;
    auto failed = lr::initializeOverlayStorage(
        []() { return uint32_t(0x402); },
        [&](const char*) { ++directoryCalls; return 0; });
    assert(!failed.mounted && !failed.ready && failed.mountResult == 0x402);
    assert(directoryCalls == 0);

    std::vector<std::string> events;
    auto mounted = lr::initializeOverlayStorage(
        [&]() { events.push_back("mount"); return uint32_t(0); },
        [&](const char* path) { events.push_back(path); return EEXIST; });
    assert(mounted.mounted && mounted.ready);
    assert((events == std::vector<std::string>{
        "mount", "sdmc:/config", "sdmc:/config/lite-recorder"}));

    directoryCalls = 0;
    auto readOnly = lr::initializeOverlayStorage(
        []() { return uint32_t(0); },
        [&](const char*) { ++directoryCalls; return EROFS; });
    assert(readOnly.mounted && !readOnly.ready && readOnly.directoryError == EROFS);
    assert(directoryCalls == 1);

    auto created = lr::initializeOverlayStorage(
        []() { return uint32_t(0); }, [](const char*) { return 0; });
    assert(created.mounted && created.ready && created.directoryError == 0);
    puts("Overlay storage regression passed: mount failure, mount-before-stdio, existing directories, directory failure.");
}
