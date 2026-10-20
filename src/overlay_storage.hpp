// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <cerrno>
#include <initializer_list>

namespace lr {
struct OverlayStorage {
    bool mounted = false, ready = false;
    uint32_t mountResult = 0;
    int directoryError = 0;
};

// libtesla initializes the FS service, but does NOT mount the stdio SD device.
// Keep this ordering testable: even mkdir must not run before a successful mount.
// makeDirectory returns 0 on success or an errno value on failure.
template<class Mount, class MakeDirectory>
OverlayStorage initializeOverlayStorage(Mount mount, MakeDirectory makeDirectory) {
    OverlayStorage state;
    state.mountResult = mount();
    if (state.mountResult != 0) return state;
    state.mounted = true;
    for (const char* path : {"sdmc:/config", "sdmc:/config/lite-recorder"}) {
        const int error = makeDirectory(path);
        if (error != 0 && error != EEXIST) {
            state.directoryError = error;
            return state;
        }
    }
    state.ready = true;
    return state;
}
}
