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

#include <array>

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

// Staging buffer, command buffer and fence shared by the snapshot transfers of one device.
struct SnapshotStagingContext {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceSize capacity = 0;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    // a submitted transfer may still use the buffer and command buffer
    bool pending = false;
    // snapshot load: the command buffer is begun and collects uploads, `used` bytes of the
    // staging buffer are taken
    bool recording = false;
    VkDeviceSize used = 0;
};

// Snapshot load uploads of one device. Many images and buffers share one staging buffer, command
// buffer and submit (GFXSTREAM_SNAPSHOT_UPLOAD_BATCH_MB, default 32; 0 = one submit per object).
// While one context's batch uploads, the next batch is read into the other context.
struct SnapshotUploadBatch {
    std::array<SnapshotStagingContext, 2> contexts;
    size_t current = 0;
    VkDeviceSize batchBytes = 0;
    uint64_t items = 0;
    uint64_t bytes = 0;
    uint64_t submits = 0;
};

SnapshotUploadBatch createSnapshotUploadBatch(StateBlock* stateBlock);
// Submits the open batch, waits for all uploads and frees the staging.
void destroySnapshotUploadBatch(StateBlock* stateBlock, SnapshotUploadBatch* batch);

SnapshotStagingContext createSnapshotStagingContext(StateBlock* stateBlock);
void destroySnapshotStagingContext(StateBlock* stateBlock,
                                   SnapshotStagingContext* staging);
bool ensureSnapshotStagingCapacity(StateBlock* stateBlock, SnapshotStagingContext* staging,
                                  VkDeviceSize needed);
// Waits for the context's submitted transfer, if any.
bool finishSnapshotTransfer(StateBlock* stateBlock, SnapshotStagingContext* staging);

bool saveImageContent(gfxstream::Stream* stream, StateBlock* stateBlock,
                      SnapshotStagingContext* staging, VkImage image,
                      const ImageInfo* imageInfo);
bool loadImageContent(gfxstream::Stream* stream, StateBlock* stateBlock,
                      SnapshotUploadBatch* batch, VkImage image,
                      const ImageInfo* imageInfo);
bool saveBufferContent(gfxstream::Stream* stream, StateBlock* stateBlock,
                       SnapshotStagingContext* staging, VkBuffer buffer,
                       const BufferInfo* bufferInfo);

void setEventInQueue(StateBlock* stateBlock, VkEvent event, uint64_t eventflags);

void signalSemaphore(StateBlock* stateBlock, VkSemaphore unboxed_semaphore);

bool loadBufferContent(gfxstream::Stream* stream, StateBlock* stateBlock,
                      SnapshotUploadBatch* batch, VkBuffer buffer,
                      const BufferInfo* bufferInfo);
}  // namespace vk
}  // namespace host
}  // namespace gfxstream
