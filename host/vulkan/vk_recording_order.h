// TODO(ai-review): generated, not yet audited
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <vector>

namespace gfxstream::host::vk {

inline std::vector<uint64_t> orderCommandBufferRecordings(
    const std::vector<uint64_t>& arrivalOrder,
    const std::map<uint64_t, std::vector<uint64_t>>& dependencies) {
    std::map<uint64_t, uint8_t> state;
    std::vector<uint64_t> ordered;
    std::function<void(uint64_t)> visit = [&](uint64_t buffer) {
        if (state[buffer]) return;
        state[buffer] = 1;
        if (const auto it = dependencies.find(buffer); it != dependencies.end()) {
            for (uint64_t secondary : it->second) {
                // A referenced buffer with no saved recording cannot be replayed.
                if (dependencies.count(secondary)) visit(secondary);
            }
        }
        state[buffer] = 2;
        ordered.push_back(buffer);
    };
    for (uint64_t buffer : arrivalOrder) visit(buffer);
    return ordered;
}

}  // namespace gfxstream::host::vk
