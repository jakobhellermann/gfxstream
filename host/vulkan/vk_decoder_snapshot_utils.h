// Copyright 2024 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include "vk_decoder_internal_structs.h"

namespace gfxstream {
namespace host {
namespace vk {
struct StateBlock {
    VkPhysicalDevice physicalDevice;
    const PhysicalDeviceInfo* physicalDeviceInfo;
    VkDevice device;
    VulkanDispatch* deviceDispatch;
    VkQueue queue;
    VkCommandPool commandPool;
};

// Reusable per-device scratch resources for the snapshot save path: one
// readback staging allocation, one command buffer and one fence, amortized
// over all images and buffers saved from the same device.
struct SnapshotStagingContext {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceSize capacity = 0;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
};

SnapshotStagingContext createSnapshotStagingContext(StateBlock* stateBlock);
void destroySnapshotStagingContext(StateBlock* stateBlock,
                                   SnapshotStagingContext* staging);
bool ensureSnapshotStagingCapacity(StateBlock* stateBlock, SnapshotStagingContext* staging,
                                  VkDeviceSize needed);

bool saveImageContent(gfxstream::Stream* stream, StateBlock* stateBlock,
                      SnapshotStagingContext* staging, VkImage image,
                      const ImageInfo* imageInfo);
bool loadImageContent(gfxstream::Stream* stream, StateBlock* stateBlock, VkImage image,
                      const ImageInfo* imageInfo);
bool saveBufferContent(gfxstream::Stream* stream, StateBlock* stateBlock,
                       SnapshotStagingContext* staging, VkBuffer buffer,
                       const BufferInfo* bufferInfo);

void setEventInQueue(StateBlock* stateBlock, VkEvent event, uint64_t eventflags);

void signalSemaphore(StateBlock* stateBlock, VkSemaphore unboxed_semaphore);

bool loadBufferContent(gfxstream::Stream* stream, StateBlock* stateBlock, VkBuffer buffer,
                       const BufferInfo* bufferInfo);
}  // namespace vk
}  // namespace host
}  // namespace gfxstream
