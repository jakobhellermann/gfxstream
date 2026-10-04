// TODO(ai-review): generated, not yet audited
#include "vk_recording_order.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <vector>

namespace gfxstream {
namespace host {
namespace vk {
namespace {

TEST(VkRecordingOrderTest, SecondaryPrecedesPrimaryThatArrivedFirst) {
    const std::map<uint64_t, std::vector<uint64_t>> dependencies = {
        {1, {3}}, {2, {}}, {3, {}}};
    EXPECT_EQ(orderCommandBufferRecordings({1, 2, 3}, dependencies),
              (std::vector<uint64_t>{3, 1, 2}));
}

TEST(VkRecordingOrderTest, IndependentBuffersKeepArrivalOrder) {
    EXPECT_EQ(orderCommandBufferRecordings({4, 5}, {}), (std::vector<uint64_t>{4, 5}));
}

}  // namespace
}  // namespace vk
}  // namespace host
}  // namespace gfxstream
