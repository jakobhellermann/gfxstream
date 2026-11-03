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
#include <chrono>
#include <deque>
#include <vector>

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

// Snapshot save readbacks of one device: like SnapshotUploadBatch, many objects are copied into one
// staging buffer with one submit, and one context's batch is written to the stream while the other's
// copies run.
struct SnapshotReadbackBatch {
    StateBlock* stateBlock = nullptr;
    std::array<SnapshotStagingContext, 2> contexts;
    size_t current = 0;
};

// Batched snapshot save (GFXSTREAM_SNAPSHOT_SAVE_BATCH_MB, default 32; 0 = the per-object
// saveImageContent / saveBufferContent). Everything the batched save functions write goes through
// here, in order: small fields wait in `header`, object bytes wait in their staging buffer until its
// submit finished. The stream gets exactly the bytes the per-object path writes.
struct SnapshotSaveWriter {
    gfxstream::Stream* stream = nullptr;
    VkDeviceSize batchBytes = 0;
    std::vector<uint8_t> header;
    size_t headerWritten = 0;
    struct Entry {
        // header bytes before this offset go to the stream before this entry's data
        size_t headerEnd;
        SnapshotReadbackBatch* batch;
        SnapshotStagingContext* staging;
        VkDeviceSize offset;
        VkDeviceSize size;
    };
    std::deque<Entry> entries;
    uint64_t items = 0;
    uint64_t bytes = 0;
    uint64_t submits = 0;
    double waitMs = 0;
    double writeMs = 0;
    std::chrono::steady_clock::time_point start;
};

VkDeviceSize snapshotSaveBatchBytes();
SnapshotSaveWriter createSnapshotSaveWriter(gfxstream::Stream* stream);
void snapshotSavePutBe32(SnapshotSaveWriter* writer, uint32_t value);
// Submits all open batches and writes everything still pending to the stream.
bool finishSnapshotSaveWriter(SnapshotSaveWriter* writer);
SnapshotReadbackBatch createSnapshotReadbackBatch(StateBlock* stateBlock);
void destroySnapshotReadbackBatch(SnapshotReadbackBatch* batch);
bool saveImageContentBatched(SnapshotSaveWriter* writer, SnapshotReadbackBatch* batch,
                             VkImage image, const ImageInfo* imageInfo);
bool saveBufferContentBatched(SnapshotSaveWriter* writer, SnapshotReadbackBatch* batch,
                              VkBuffer buffer, const BufferInfo* bufferInfo);

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
