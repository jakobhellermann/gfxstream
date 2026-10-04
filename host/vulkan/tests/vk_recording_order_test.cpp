// TODO(ai-review): generated, not yet audited
#include "../vk_recording_order.h"

#include <cassert>
#include <cstdint>
#include <map>
#include <vector>

int main() {
    using gfxstream::host::vk::orderCommandBufferRecordings;

    // A primary recording can arrive before the secondary it executes.
    const std::vector<uint64_t> arrival = {1, 2, 3};
    const std::map<uint64_t, std::vector<uint64_t>> dependencies = {
        {1, {3}}, {2, {}}, {3, {}}};
    const auto ordered = orderCommandBufferRecordings(arrival, dependencies);
    assert((ordered == std::vector<uint64_t>{3, 1, 2}));

    // Independent buffers retain their arrival order.
    assert((orderCommandBufferRecordings({4, 5}, {}) == std::vector<uint64_t>{4, 5}));
}
