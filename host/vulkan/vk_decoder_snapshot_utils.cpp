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

#include "vk_decoder_snapshot_utils.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "gfxstream/common/logging.h"
#include "vk_common_operations.h"
#include "vk_decoder_global_state.h"
#include "vk_utils.h"
#include "vulkan_boxed_handles.h"

namespace gfxstream {
namespace host {
namespace vk {

namespace {

#define VK_RETURN_FALSE_ON_ERROR(x)                  \
    do {                                             \
        VkResult err = x;                            \
        if (err != VK_SUCCESS) {                     \
            GFXSTREAM_ERROR("Vulkan error %d", err); \
            return false;                            \
        }                                            \
    } while (0)

uint32_t GetMemoryType(const PhysicalDeviceInfo& physicalDevice,
                       const VkMemoryRequirements& memoryRequirements,
                       VkMemoryPropertyFlags memoryProperties) {
    const auto& props = physicalDevice.memoryPropertiesHelper->getHostMemoryProperties();
    for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
        if (!(memoryRequirements.memoryTypeBits & (1 << i))) {
            continue;
        }
        if ((props.memoryTypes[i].propertyFlags & memoryProperties) != memoryProperties) {
            continue;
        }
        return i;
    }
    GFXSTREAM_FATAL("Cannot find memory type for snapshot save.");
    return -1;
}

// Snapshot save reads the staged content back on the CPU, which is slow from the write-combined
// mappings drivers commonly use for uncached host visible memory.
uint32_t GetReadbackMemoryType(const PhysicalDeviceInfo& physicalDevice,
                               const VkMemoryRequirements& memoryRequirements) {
    const VkMemoryPropertyFlags cached = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                         VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    const auto& props = physicalDevice.memoryPropertiesHelper->getHostMemoryProperties();
    for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
        if ((memoryRequirements.memoryTypeBits & (1 << i)) &&
            (props.memoryTypes[i].propertyFlags & cached) == cached) {
            return i;
        }
    }
    return GetMemoryType(physicalDevice, memoryRequirements,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
}

VkResult waitForSnapshotTransfer(StateBlock* stateBlock, VkFence fence) {
    VulkanDispatch* dispatch = stateBlock->deviceDispatch;
    VkResult result = dispatch->vkWaitForFences(stateBlock->device, 1, &fence, VK_TRUE, 500000000L);
    if (result == VK_TIMEOUT) {
        VkDecoderGlobalState::get()->logSnapshotStallLocked();
        result = dispatch->vkWaitForFences(stateBlock->device, 1, &fence, VK_TRUE, 2500000000L);
    }
    return result;
}

VkExtent3D getMipmapExtent(VkExtent3D baseExtent, uint32_t mipLevel) {
    return VkExtent3D{
        .width = std::max<uint32_t>(baseExtent.width >> mipLevel, 1u),
        .height = std::max<uint32_t>(baseExtent.height >> mipLevel, 1u),
        .depth = std::max<uint32_t>(baseExtent.depth >> mipLevel, 1u),
    };
}

constexpr uint32_t kBadImageSnapshot = 0xbaadbeef;
constexpr uint32_t kGoodImageSnapshot = 0x900df00d;

// Covers every access. Setting all bits instead includes reserved ones, which drivers may reject.
constexpr VkAccessFlags kAllMemoryAccess = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
}  // namespace
SnapshotStagingContext createSnapshotStagingContext(StateBlock* stateBlock) {
    VulkanDispatch* dispatch = stateBlock->deviceDispatch;
    SnapshotStagingContext staging;
    VkCommandBufferAllocateInfo allocInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = stateBlock->commandPool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VK_CHECK(dispatch->vkAllocateCommandBuffers(stateBlock->device, &allocInfo,
                                                &staging.commandBuffer));
    VkFenceCreateInfo fenceCreateInfo{
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
    };
    VK_CHECK(
        dispatch->vkCreateFence(stateBlock->device, &fenceCreateInfo, nullptr, &staging.fence));
    return staging;
}

bool finishSnapshotTransfer(StateBlock* stateBlock, SnapshotStagingContext* staging) {
    if (!staging->pending) {
        return true;
    }
    VK_RETURN_FALSE_ON_ERROR(waitForSnapshotTransfer(stateBlock, staging->fence));
    VK_CHECK(stateBlock->deviceDispatch->vkResetFences(stateBlock->device, 1, &staging->fence));
    staging->pending = false;
    return true;
}

void destroySnapshotStagingContext(StateBlock* stateBlock, SnapshotStagingContext* staging) {
    VulkanDispatch* dispatch = stateBlock->deviceDispatch;
    if (!finishSnapshotTransfer(stateBlock, staging)) {
        GFXSTREAM_FATAL("Snapshot transfer did not finish before destroying its staging");
    }
    if (staging->mapped != nullptr) {
        dispatch->vkUnmapMemory(stateBlock->device, staging->memory);
        staging->mapped = nullptr;
    }
    if (staging->memory != VK_NULL_HANDLE) {
        dispatch->vkFreeMemory(stateBlock->device, staging->memory, nullptr);
        staging->memory = VK_NULL_HANDLE;
    }
    if (staging->buffer != VK_NULL_HANDLE) {
        dispatch->vkDestroyBuffer(stateBlock->device, staging->buffer, nullptr);
        staging->buffer = VK_NULL_HANDLE;
    }
    if (staging->fence != VK_NULL_HANDLE) {
        dispatch->vkDestroyFence(stateBlock->device, staging->fence, nullptr);
        staging->fence = VK_NULL_HANDLE;
    }
    if (staging->commandBuffer != VK_NULL_HANDLE) {
        dispatch->vkFreeCommandBuffers(stateBlock->device, stateBlock->commandPool, 1,
                                       &staging->commandBuffer);
        staging->commandBuffer = VK_NULL_HANDLE;
    }
    staging->capacity = 0;
}

bool ensureSnapshotStagingCapacity(StateBlock* stateBlock, SnapshotStagingContext* staging,
                                   VkDeviceSize needed) {
    if (staging->capacity >= needed) {
        return true;
    }
    VulkanDispatch* dispatch = stateBlock->deviceDispatch;
    if (staging->mapped != nullptr) {
        dispatch->vkUnmapMemory(stateBlock->device, staging->memory);
        staging->mapped = nullptr;
    }
    if (staging->memory != VK_NULL_HANDLE) {
        dispatch->vkFreeMemory(stateBlock->device, staging->memory, nullptr);
        staging->memory = VK_NULL_HANDLE;
    }
    if (staging->buffer != VK_NULL_HANDLE) {
        dispatch->vkDestroyBuffer(stateBlock->device, staging->buffer, nullptr);
        staging->buffer = VK_NULL_HANDLE;
    }
    staging->capacity = 0;

    VkBufferCreateInfo bufferCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = needed,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VK_CHECK(
        dispatch->vkCreateBuffer(stateBlock->device, &bufferCreateInfo, nullptr, &staging->buffer));

    VkMemoryRequirements stagingBufferMemoryRequirements{};
    dispatch->vkGetBufferMemoryRequirements(stateBlock->device, staging->buffer,
                                            &stagingBufferMemoryRequirements);

    const auto stagingBufferMemoryType =
        GetReadbackMemoryType(*stateBlock->physicalDeviceInfo, stagingBufferMemoryRequirements);
    VkMemoryAllocateInfo stagingBufferMemoryAllocateInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = stagingBufferMemoryRequirements.size,
        .memoryTypeIndex = stagingBufferMemoryType,
    };
    VK_CHECK(dispatch->vkAllocateMemory(stateBlock->device, &stagingBufferMemoryAllocateInfo,
                                       nullptr, &staging->memory));
    VK_CHECK(dispatch->vkBindBufferMemory(stateBlock->device, staging->buffer, staging->memory, 0));

    if (dispatch->vkMapMemory(stateBlock->device, staging->memory, 0, VK_WHOLE_SIZE,
                              VkMemoryMapFlags{}, &staging->mapped) != VK_SUCCESS ||
        staging->mapped == nullptr) {
        GFXSTREAM_ERROR("Failed to map readback staging memory for snapshot save");
        return false;
    }
    staging->capacity = needed;
    return true;
}

namespace {

// Base alignment of each object's data in a batch: a multiple of every texel size (1, 2, 3, 4,
// 6, 8, 12, 16 bytes) and of the copy offset alignments drivers ask for.
constexpr VkDeviceSize kUploadAlignment = 768;

VkDeviceSize snapshotUploadBatchBytes() {
    static const VkDeviceSize bytes = [] {
        const char* env = getenv("GFXSTREAM_SNAPSHOT_UPLOAD_BATCH_MB");
        long mb = env && *env ? strtol(env, nullptr, 10) : 32;
        return static_cast<VkDeviceSize>(mb > 0 ? mb : 0) << 20;
    }();
    return bytes;
}

bool submitSnapshotUpload(StateBlock* stateBlock, SnapshotUploadBatch* batch,
                          SnapshotStagingContext* staging) {
    if (!staging->recording) {
        return true;
    }
    VulkanDispatch* dispatch = stateBlock->deviceDispatch;
    staging->recording = false;
    VK_RETURN_FALSE_ON_ERROR(dispatch->vkEndCommandBuffer(staging->commandBuffer));
    VkSubmitInfo submitInfo = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &staging->commandBuffer,
    };
    VK_RETURN_FALSE_ON_ERROR(
        dispatch->vkQueueSubmit(stateBlock->queue, 1, &submitInfo, staging->fence));
    // waited for when the context is used next, so the next batch's read overlaps this upload
    staging->pending = true;
    batch->submits++;
    return true;
}

// Returns the context to record an upload into, with its command buffer begun, and the offset
// of `size` free bytes in its staging buffer.
bool reserveSnapshotUpload(StateBlock* stateBlock, SnapshotUploadBatch* batch, VkDeviceSize size,
                           SnapshotStagingContext** outStaging, VkDeviceSize* outOffset) {
    SnapshotStagingContext* staging = &batch->contexts[batch->current];
    if (staging->recording) {
        VkDeviceSize offset = (staging->used + kUploadAlignment - 1) & ~(kUploadAlignment - 1);
        if (batch->batchBytes > 0 && offset + size <= staging->capacity) {
            staging->used = offset + size;
            batch->items++;
            batch->bytes += size;
            *outStaging = staging;
            *outOffset = offset;
            return true;
        }
        if (!submitSnapshotUpload(stateBlock, batch, staging)) {
            return false;
        }
        batch->current ^= 1;
        staging = &batch->contexts[batch->current];
    }
    if (!finishSnapshotTransfer(stateBlock, staging)) {
        return false;
    }
    if (!ensureSnapshotStagingCapacity(stateBlock, staging, std::max(size, batch->batchBytes))) {
        return false;
    }
    VkCommandBufferBeginInfo beginInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    if (stateBlock->deviceDispatch->vkBeginCommandBuffer(staging->commandBuffer, &beginInfo) !=
        VK_SUCCESS) {
        GFXSTREAM_ERROR("Failed to start command buffer on snapshot load");
        return false;
    }
    staging->recording = true;
    staging->used = size;
    batch->items++;
    batch->bytes += size;
    *outStaging = staging;
    *outOffset = 0;
    return true;
}

}  // namespace

SnapshotUploadBatch createSnapshotUploadBatch(StateBlock* stateBlock) {
    SnapshotUploadBatch batch;
    batch.contexts[0] = createSnapshotStagingContext(stateBlock);
    batch.contexts[1] = createSnapshotStagingContext(stateBlock);
    batch.batchBytes = snapshotUploadBatchBytes();
    return batch;
}

void destroySnapshotUploadBatch(StateBlock* stateBlock, SnapshotUploadBatch* batch) {
    for (auto& staging : batch->contexts) {
        if (!submitSnapshotUpload(stateBlock, batch, &staging)) {
            GFXSTREAM_FATAL("Failed to submit the last snapshot load uploads");
        }
    }
    for (auto& staging : batch->contexts) {
        destroySnapshotStagingContext(stateBlock, &staging);
    }
    GFXSTREAM_DEBUG("snapshot load: uploaded %llu objects, %.1f MB in %llu submits (batch %llu MB)",
                    static_cast<unsigned long long>(batch->items), batch->bytes / 1048576.0,
                    static_cast<unsigned long long>(batch->submits),
                    static_cast<unsigned long long>(batch->batchBytes >> 20));
}

bool saveImageContent(gfxstream::Stream* stream, StateBlock* stateBlock,
                      SnapshotStagingContext* staging, VkImage image,
                      const ImageInfo* imageInfo) {
    if (imageInfo->layout == VK_IMAGE_LAYOUT_UNDEFINED) {
        stream->putBe32(kBadImageSnapshot);
        return true;
    }
    // TODO(b/333936705): snapshot multi-sample images
    if (imageInfo->imageCreateInfoShallow.samples != VK_SAMPLE_COUNT_1_BIT) {
        stream->putBe32(kBadImageSnapshot);
        return true;
    }

    VulkanDispatch* dispatch = stateBlock->deviceDispatch;
    const VkImageCreateInfo& imageCreateInfo = imageInfo->imageCreateInfoShallow;
    const uint32_t mipLevels = imageInfo->imageCreateInfoShallow.mipLevels;
    const uint32_t arrayLayers = imageInfo->imageCreateInfoShallow.arrayLayers;

    TransferInfo transferInfo;
    if (!getFormatTransferInfo(imageCreateInfo.format, imageCreateInfo.extent, &transferInfo) ||
        !transferInfo.stagingBufferCopySize) {
        stream->putBe32(kBadImageSnapshot);
        return true;
    }

    // Region bases are 16-byte aligned, enough for any format's buffer offset alignment; only
    // the base shifts, so plane-relative offsets inside a region stay valid.
    constexpr VkDeviceSize kRegionAlignment = 16;
    std::vector<VkDeviceSize> regionSizes;
    std::vector<TransferInfo> regionTransferInfos;
    VkDeviceSize stagingNeeded = 0;
    for (uint32_t mipLevel = 0; mipLevel < mipLevels; mipLevel++) {
        VkExtent3D mipmapExtent = getMipmapExtent(imageCreateInfo.extent, mipLevel);
        TransferInfo mipmapTransferInfo;
        if (!getFormatTransferInfo(imageCreateInfo.format, mipmapExtent, &mipmapTransferInfo)) {
            GFXSTREAM_ERROR("Failed to get transfer info for snapshot save");
            return false;
        }
        // A zero-sized region still gets an empty stream entry.
        for (uint32_t arrayLayer = 0; arrayLayer < arrayLayers; arrayLayer++) {
            regionSizes.push_back(mipmapTransferInfo.stagingBufferCopySize);
            regionTransferInfos.push_back(mipmapTransferInfo);
            stagingNeeded = (stagingNeeded + mipmapTransferInfo.stagingBufferCopySize +
                             kRegionAlignment - 1) &
                            ~(kRegionAlignment - 1);
        }
    }

    stream->putBe32(kGoodImageSnapshot);
    if (!ensureSnapshotStagingCapacity(stateBlock, staging, stagingNeeded)) {
        return false;
    }

    VkCommandBuffer commandBuffer = staging->commandBuffer;
    VkCommandBufferBeginInfo beginInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    };
    if (dispatch->vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
        GFXSTREAM_ERROR("Failed to start command buffer on snapshot save");
        return false;
    }

    VkDeviceSize regionBase = 0;
    for (uint32_t mipLevel = 0; mipLevel < mipLevels; mipLevel++) {
        for (uint32_t arrayLayer = 0; arrayLayer < arrayLayers; arrayLayer++) {
            TransferInfo& mipmapTransferInfo =
                regionTransferInfos[mipLevel * arrayLayers + arrayLayer];
            const VkDeviceSize mipmapStagingBufferSize =
                regionSizes[mipLevel * arrayLayers + arrayLayer];
            std::vector<VkBufferImageCopy>& bufferImageCopies =
                mipmapTransferInfo.bufferImageCopies;
            VkImageAspectFlags aspects = 0;
            for (const auto& copy : bufferImageCopies) {
                aspects |= copy.imageSubresource.aspectMask;
            }
            VkImageLayout layoutBeforeSave = imageInfo->layout;
            VkImageMemoryBarrier imgMemoryBarrier = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = kAllMemoryAccess,
                .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                .oldLayout = layoutBeforeSave,
                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = image,
                .subresourceRange = VkImageSubresourceRange{.aspectMask = aspects,
                                                            .baseMipLevel = mipLevel,
                                                            .levelCount = 1,
                                                            .baseArrayLayer = arrayLayer,
                                                            .layerCount = 1}};

            dispatch->vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                           VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                                           nullptr, 1, &imgMemoryBarrier);

            for (auto& region : bufferImageCopies) {
                region.imageSubresource.mipLevel = mipLevel;
                region.imageSubresource.baseArrayLayer = arrayLayer;
                region.bufferOffset += regionBase;
                dispatch->vkCmdCopyImageToBuffer(commandBuffer, image,
                                                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                                 staging->buffer, 1, &region);
            }

            // Cannot really translate it back to VK_IMAGE_LAYOUT_PREINITIALIZED
            if (layoutBeforeSave != VK_IMAGE_LAYOUT_PREINITIALIZED) {
                imgMemoryBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                imgMemoryBarrier.newLayout = layoutBeforeSave;
                imgMemoryBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                imgMemoryBarrier.dstAccessMask = kAllMemoryAccess;
                dispatch->vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                                               nullptr, 1, &imgMemoryBarrier);
            }
            regionBase =
                (regionBase + mipmapStagingBufferSize + kRegionAlignment - 1) &
                ~(kRegionAlignment - 1);
        }
    }
    VK_CHECK(dispatch->vkEndCommandBuffer(commandBuffer));

    VkSubmitInfo submitInfo = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &commandBuffer,
    };
    VK_CHECK(dispatch->vkQueueSubmit(stateBlock->queue, 1, &submitInfo, staging->fence));
    VK_RETURN_FALSE_ON_ERROR(waitForSnapshotTransfer(stateBlock, staging->fence));
    VK_CHECK(dispatch->vkResetFences(stateBlock->device, 1, &staging->fence));

    regionBase = 0;
    for (uint32_t mipLevel = 0; mipLevel < mipLevels; mipLevel++) {
        for (uint32_t arrayLayer = 0; arrayLayer < arrayLayers; arrayLayer++) {
            auto bytes = regionSizes[mipLevel * arrayLayers + arrayLayer];
            stream->putBe64(bytes);
            stream->write(static_cast<const char*>(staging->mapped) + regionBase, bytes);
            regionBase = (regionBase + bytes + kRegionAlignment - 1) & ~(kRegionAlignment - 1);
        }
    }
    return true;
}

bool loadImageContent(gfxstream::Stream* stream, StateBlock* stateBlock,
                      SnapshotUploadBatch* batch, VkImage image,
                      const ImageInfo* imageInfo) {
    const bool validImage = (stream->getBe32() == kGoodImageSnapshot);
    if (!validImage) {
        return true;
    }

    VulkanDispatch* dispatch = stateBlock->deviceDispatch;
    const VkImageCreateInfo& imageCreateInfo = imageInfo->imageCreateInfoShallow;

    TransferInfo transferInfo;
    if (!getFormatTransferInfo(imageCreateInfo.format, imageCreateInfo.extent, &transferInfo)) {
        return true;
    }

    if (imageInfo->imageCreateInfoShallow.samples != VK_SAMPLE_COUNT_1_BIT) {
        SnapshotStagingContext* staging = nullptr;
        VkDeviceSize unusedOffset = 0;
        if (!reserveSnapshotUpload(stateBlock, batch, 0, &staging, &unusedOffset)) {
            return false;
        }
        VkCommandBuffer commandBuffer = staging->commandBuffer;
        // Set the layout and quit
        // TODO: resolve and save image content
        getFormatTransferInfo(imageCreateInfo.format, imageCreateInfo.extent, &transferInfo);
        VkImageAspectFlags aspects = 0;
        for (const auto& copy : transferInfo.bufferImageCopies) {
            aspects |= copy.imageSubresource.aspectMask;
        }
        VkImageMemoryBarrier imgMemoryBarrier = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = kAllMemoryAccess,
            .dstAccessMask = kAllMemoryAccess,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = imageInfo->layout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = VkImageSubresourceRange{.aspectMask = aspects,
                                                        .baseMipLevel = 0,
                                                        .levelCount = VK_REMAINING_MIP_LEVELS,
                                                        .baseArrayLayer = 0,
                                                        .layerCount = VK_REMAINING_ARRAY_LAYERS}};
        dispatch->vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                       VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                                       nullptr, 1, &imgMemoryBarrier);
        return true;
    }
    // Region bases match the save path's.
    constexpr VkDeviceSize kRegionAlignment = 16;
    const uint32_t mipLevels = imageInfo->imageCreateInfoShallow.mipLevels;
    const uint32_t arrayLayers = imageInfo->imageCreateInfoShallow.arrayLayers;
    std::vector<VkDeviceSize> regionSizes;
    std::vector<TransferInfo> regionTransferInfos;
    VkDeviceSize stagingNeeded = 0;
    for (uint32_t mipLevel = 0; mipLevel < mipLevels; mipLevel++) {
        VkExtent3D mipmapExtent = getMipmapExtent(imageCreateInfo.extent, mipLevel);
        TransferInfo mipmapTransferInfo;
        if (!getFormatTransferInfo(imageCreateInfo.format, mipmapExtent, &mipmapTransferInfo)) {
            GFXSTREAM_ERROR("Failed to get transfer info for snapshot load");
            return false;
        }
        for (uint32_t arrayLayer = 0; arrayLayer < arrayLayers; arrayLayer++) {
            regionSizes.push_back(mipmapTransferInfo.stagingBufferCopySize);
            regionTransferInfos.push_back(mipmapTransferInfo);
            stagingNeeded = (stagingNeeded + mipmapTransferInfo.stagingBufferCopySize +
                             kRegionAlignment - 1) &
                            ~(kRegionAlignment - 1);
        }
    }
    SnapshotStagingContext* staging = nullptr;
    VkDeviceSize batchOffset = 0;
    if (!reserveSnapshotUpload(stateBlock, batch, stagingNeeded, &staging, &batchOffset)) {
        return false;
    }
    VkCommandBuffer commandBuffer = staging->commandBuffer;

    VkDeviceSize regionBase = batchOffset;
    for (uint32_t mipLevel = 0; mipLevel < mipLevels; mipLevel++) {
        for (uint32_t arrayLayer = 0; arrayLayer < arrayLayers; arrayLayer++) {
            // Require the serialized size to match the expected per-mip transfer size.
            size_t bytes = stream->getBe64();
            if (bytes != regionSizes[mipLevel * arrayLayers + arrayLayer]) {
                GFXSTREAM_FATAL("Unexpected image content size on snapshot load");
            }
            stream->read(static_cast<char*>(staging->mapped) + regionBase, bytes);
            regionBase = (regionBase + bytes + kRegionAlignment - 1) & ~(kRegionAlignment - 1);
        }
    }

    regionBase = batchOffset;
    for (uint32_t mipLevel = 0; mipLevel < mipLevels; mipLevel++) {
        for (uint32_t arrayLayer = 0; arrayLayer < arrayLayers; arrayLayer++) {
            TransferInfo& mipmapTransferInfo =
                regionTransferInfos[mipLevel * arrayLayers + arrayLayer];
            std::vector<VkBufferImageCopy>& bufferImageCopies =
                mipmapTransferInfo.bufferImageCopies;
            VkImageAspectFlags aspects = 0;
            for (const auto& copy : bufferImageCopies) {
                aspects |= copy.imageSubresource.aspectMask;
            }
            VkImageMemoryBarrier imgMemoryBarrier = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = kAllMemoryAccess,
                .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = image,
                .subresourceRange = VkImageSubresourceRange{.aspectMask = aspects,
                                                            .baseMipLevel = mipLevel,
                                                            .levelCount = 1,
                                                            .baseArrayLayer = arrayLayer,
                                                            .layerCount = 1}};

            dispatch->vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                           VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                                           nullptr, 1, &imgMemoryBarrier);

            for (auto& region : bufferImageCopies) {
                region.imageSubresource.mipLevel = mipLevel;
                region.imageSubresource.baseArrayLayer = arrayLayer;
                region.bufferOffset += regionBase;
                dispatch->vkCmdCopyBufferToImage(commandBuffer, staging->buffer, image,
                                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            }

            // Cannot really translate it back to VK_IMAGE_LAYOUT_PREINITIALIZED
            if (imageInfo->layout != VK_IMAGE_LAYOUT_PREINITIALIZED) {
                imgMemoryBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                imgMemoryBarrier.newLayout = imageInfo->layout;
                imgMemoryBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                imgMemoryBarrier.dstAccessMask = kAllMemoryAccess;
                dispatch->vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                                               nullptr, 1, &imgMemoryBarrier);
            }
            regionBase =
                (regionBase + regionSizes[mipLevel * arrayLayers + arrayLayer] +
                 kRegionAlignment - 1) &
                ~(kRegionAlignment - 1);
        }
    }
    return true;
}

bool saveBufferContent(gfxstream::Stream* stream, StateBlock* stateBlock,
                       SnapshotStagingContext* staging, VkBuffer buffer,
                       const BufferInfo* bufferInfo) {
    VkBufferUsageFlags requiredUsages =
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if ((bufferInfo->usage & requiredUsages) != requiredUsages) {
        return true;
    }
    VulkanDispatch* dispatch = stateBlock->deviceDispatch;

    if (!ensureSnapshotStagingCapacity(stateBlock, staging,
                                       static_cast<VkDeviceSize>(bufferInfo->size))) {
        return false;
    }

    VkCommandBuffer commandBuffer = staging->commandBuffer;

    VkBufferCopy bufferCopy = {
        .srcOffset = 0,
        .dstOffset = 0,
        .size = bufferInfo->size,
    };

    VkCommandBufferBeginInfo beginInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    };
    if (dispatch->vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
        GFXSTREAM_ERROR("Failed to start command buffer on snapshot save");
        return false;
    }
    dispatch->vkCmdCopyBuffer(commandBuffer, buffer, staging->buffer, 1, &bufferCopy);
    VkBufferMemoryBarrier barrier{.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                                  .pNext = nullptr,
                                  .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                  .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
                                  .srcQueueFamilyIndex = 0xFFFFFFFF,
                                  .dstQueueFamilyIndex = 0xFFFFFFFF,
                                  .buffer = staging->buffer,
                                  .offset = 0,
                                  .size = bufferInfo->size};
    dispatch->vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &barrier, 0,
                                   nullptr);

    // Execute the command to copy buffer
    VkSubmitInfo submitInfo = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &commandBuffer,
    };
    VK_CHECK(dispatch->vkEndCommandBuffer(commandBuffer));
    VK_CHECK(dispatch->vkQueueSubmit(stateBlock->queue, 1, &submitInfo, staging->fence));
    VK_RETURN_FALSE_ON_ERROR(waitForSnapshotTransfer(stateBlock, staging->fence));
    VK_CHECK(dispatch->vkResetFences(stateBlock->device, 1, &staging->fence));
    stream->putBe64(bufferInfo->size);
    stream->write(staging->mapped, bufferInfo->size);
    return true;
}

namespace {

double msSince(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

void snapshotSavePutBe64(SnapshotSaveWriter* writer, uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        writer->header.push_back(static_cast<uint8_t>(value >> shift));
    }
}

void writeSnapshotSaveHeader(SnapshotSaveWriter* writer, size_t end) {
    if (end > writer->headerWritten) {
        writer->stream->write(writer->header.data() + writer->headerWritten,
                              end - writer->headerWritten);
        writer->headerWritten = end;
    }
}

bool submitSnapshotReadback(SnapshotSaveWriter* writer, SnapshotReadbackBatch* batch,
                            SnapshotStagingContext* staging) {
    if (!staging->recording) {
        return true;
    }
    StateBlock* stateBlock = batch->stateBlock;
    VulkanDispatch* dispatch = stateBlock->deviceDispatch;
    staging->recording = false;
    // the copies' writes to the staging buffer, for the CPU
    VkMemoryBarrier toHost{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
    };
    dispatch->vkCmdPipelineBarrier(staging->commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                   VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &toHost, 0, nullptr, 0,
                                   nullptr);
    VK_RETURN_FALSE_ON_ERROR(dispatch->vkEndCommandBuffer(staging->commandBuffer));
    VkSubmitInfo submitInfo = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &staging->commandBuffer,
    };
    VK_RETURN_FALSE_ON_ERROR(
        dispatch->vkQueueSubmit(stateBlock->queue, 1, &submitInfo, staging->fence));
    staging->pending = true;
    writer->submits++;
    return true;
}

// Writes pending entries to the stream in order: all of them (untilFree == nullptr), or up to the
// last one whose bytes are in `untilFree`, so that context can take a new batch.
bool flushSnapshotSaveWriter(SnapshotSaveWriter* writer, SnapshotStagingContext* untilFree) {
    size_t count = writer->entries.size();
    if (untilFree != nullptr) {
        count = 0;
        for (size_t i = writer->entries.size(); i > 0; i--) {
            if (writer->entries[i - 1].staging == untilFree) {
                count = i;
                break;
            }
        }
    }
    for (size_t i = 0; i < count; i++) {
        SnapshotSaveWriter::Entry entry = writer->entries.front();
        writer->entries.pop_front();
        if (entry.staging->recording &&
            !submitSnapshotReadback(writer, entry.batch, entry.staging)) {
            return false;
        }
        if (entry.staging->pending) {
            auto waitStart = std::chrono::steady_clock::now();
            if (!finishSnapshotTransfer(entry.batch->stateBlock, entry.staging)) {
                return false;
            }
            writer->waitMs += msSince(waitStart);
        }
        auto writeStart = std::chrono::steady_clock::now();
        writeSnapshotSaveHeader(writer, entry.headerEnd);
        if (entry.size > 0) {
            writer->stream->write(static_cast<const char*>(entry.staging->mapped) + entry.offset,
                                  entry.size);
        }
        writer->writeMs += msSince(writeStart);
    }
    if (untilFree == nullptr) {
        writeSnapshotSaveHeader(writer, writer->header.size());
    }
    if (writer->entries.empty()) {
        writer->header.erase(writer->header.begin(),
                             writer->header.begin() + writer->headerWritten);
        writer->headerWritten = 0;
    }
    return true;
}

// Like reserveSnapshotUpload, for readbacks: the context to record copies into, with its command
// buffer begun, and the offset of `size` free bytes in its staging buffer.
bool reserveSnapshotReadback(SnapshotSaveWriter* writer, SnapshotReadbackBatch* batch,
                             VkDeviceSize size, SnapshotStagingContext** outStaging,
                             VkDeviceSize* outOffset) {
    StateBlock* stateBlock = batch->stateBlock;
    VulkanDispatch* dispatch = stateBlock->deviceDispatch;
    SnapshotStagingContext* staging = &batch->contexts[batch->current];
    if (staging->recording) {
        VkDeviceSize offset = (staging->used + kUploadAlignment - 1) & ~(kUploadAlignment - 1);
        if (offset + size <= staging->capacity) {
            staging->used = offset + size;
            *outStaging = staging;
            *outOffset = offset;
            return true;
        }
        if (!submitSnapshotReadback(writer, batch, staging)) {
            return false;
        }
        batch->current ^= 1;
        staging = &batch->contexts[batch->current];
    }
    // the other context's batch goes to the stream while this one's copies run
    if (!flushSnapshotSaveWriter(writer, staging) || !finishSnapshotTransfer(stateBlock, staging)) {
        return false;
    }
    if (!ensureSnapshotStagingCapacity(stateBlock, staging, std::max(size, writer->batchBytes))) {
        return false;
    }
    VkCommandBufferBeginInfo beginInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    if (dispatch->vkBeginCommandBuffer(staging->commandBuffer, &beginInfo) != VK_SUCCESS) {
        GFXSTREAM_ERROR("Failed to start command buffer on snapshot save");
        return false;
    }
    // the game's earlier writes, for the copies
    VkMemoryBarrier fromGame{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = kAllMemoryAccess,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
    };
    dispatch->vkCmdPipelineBarrier(staging->commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                   VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &fromGame, 0, nullptr, 0,
                                   nullptr);
    staging->recording = true;
    staging->used = size;
    *outStaging = staging;
    *outOffset = 0;
    return true;
}

void queueSnapshotSaveBytes(SnapshotSaveWriter* writer, SnapshotReadbackBatch* batch,
                            SnapshotStagingContext* staging, VkDeviceSize offset,
                            VkDeviceSize size) {
    writer->entries.push_back(SnapshotSaveWriter::Entry{
        .headerEnd = writer->header.size(),
        .batch = batch,
        .staging = staging,
        .offset = offset,
        .size = size,
    });
}

}  // namespace

VkDeviceSize snapshotSaveBatchBytes() {
    static const VkDeviceSize bytes = [] {
        const char* env = getenv("GFXSTREAM_SNAPSHOT_SAVE_BATCH_MB");
        long mb = env && *env ? strtol(env, nullptr, 10) : 32;
        return static_cast<VkDeviceSize>(mb > 0 ? mb : 0) << 20;
    }();
    return bytes;
}

SnapshotSaveWriter createSnapshotSaveWriter(gfxstream::Stream* stream) {
    SnapshotSaveWriter writer;
    writer.stream = stream;
    writer.batchBytes = snapshotSaveBatchBytes();
    writer.start = std::chrono::steady_clock::now();
    return writer;
}

void snapshotSavePutBe32(SnapshotSaveWriter* writer, uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        writer->header.push_back(static_cast<uint8_t>(value >> shift));
    }
}

bool finishSnapshotSaveWriter(SnapshotSaveWriter* writer) {
    // submit every open batch first, so all of them run while the stream is written
    for (const auto& entry : writer->entries) {
        if (entry.staging->recording && !submitSnapshotReadback(writer, entry.batch, entry.staging)) {
            return false;
        }
    }
    if (!flushSnapshotSaveWriter(writer, nullptr)) {
        return false;
    }
    GFXSTREAM_DEBUG(
        "snapshot save: read back %llu objects, %.1f MB in %llu submits (batch %llu MB), %.1f ms: "
        "waits %.1f ms, stream writes %.1f ms",
        static_cast<unsigned long long>(writer->items), writer->bytes / 1048576.0,
        static_cast<unsigned long long>(writer->submits),
        static_cast<unsigned long long>(writer->batchBytes >> 20), msSince(writer->start),
        writer->waitMs, writer->writeMs);
    return true;
}

SnapshotReadbackBatch createSnapshotReadbackBatch(StateBlock* stateBlock) {
    SnapshotReadbackBatch batch;
    batch.stateBlock = stateBlock;
    batch.contexts[0] = createSnapshotStagingContext(stateBlock);
    batch.contexts[1] = createSnapshotStagingContext(stateBlock);
    return batch;
}

void destroySnapshotReadbackBatch(SnapshotReadbackBatch* batch) {
    for (auto& staging : batch->contexts) {
        // a batch left open by a failed save: its command buffer is freed while recording
        staging.recording = false;
        destroySnapshotStagingContext(batch->stateBlock, &staging);
    }
}

bool saveImageContentBatched(SnapshotSaveWriter* writer, SnapshotReadbackBatch* batch,
                             VkImage image, const ImageInfo* imageInfo) {
    if (imageInfo->layout == VK_IMAGE_LAYOUT_UNDEFINED) {
        snapshotSavePutBe32(writer, kBadImageSnapshot);
        return true;
    }
    // TODO(b/333936705): snapshot multi-sample images
    if (imageInfo->imageCreateInfoShallow.samples != VK_SAMPLE_COUNT_1_BIT) {
        snapshotSavePutBe32(writer, kBadImageSnapshot);
        return true;
    }

    VulkanDispatch* dispatch = batch->stateBlock->deviceDispatch;
    const VkImageCreateInfo& imageCreateInfo = imageInfo->imageCreateInfoShallow;
    const uint32_t mipLevels = imageInfo->imageCreateInfoShallow.mipLevels;
    const uint32_t arrayLayers = imageInfo->imageCreateInfoShallow.arrayLayers;

    TransferInfo transferInfo;
    if (!getFormatTransferInfo(imageCreateInfo.format, imageCreateInfo.extent, &transferInfo) ||
        !transferInfo.stagingBufferCopySize) {
        snapshotSavePutBe32(writer, kBadImageSnapshot);
        return true;
    }

    // Same region layout as saveImageContent, from the object's base in the batch.
    constexpr VkDeviceSize kRegionAlignment = 16;
    std::vector<VkDeviceSize> regionSizes;
    std::vector<TransferInfo> regionTransferInfos;
    VkDeviceSize stagingNeeded = 0;
    for (uint32_t mipLevel = 0; mipLevel < mipLevels; mipLevel++) {
        VkExtent3D mipmapExtent = getMipmapExtent(imageCreateInfo.extent, mipLevel);
        TransferInfo mipmapTransferInfo;
        if (!getFormatTransferInfo(imageCreateInfo.format, mipmapExtent, &mipmapTransferInfo)) {
            GFXSTREAM_ERROR("Failed to get transfer info for snapshot save");
            return false;
        }
        for (uint32_t arrayLayer = 0; arrayLayer < arrayLayers; arrayLayer++) {
            regionSizes.push_back(mipmapTransferInfo.stagingBufferCopySize);
            regionTransferInfos.push_back(mipmapTransferInfo);
            stagingNeeded = (stagingNeeded + mipmapTransferInfo.stagingBufferCopySize +
                             kRegionAlignment - 1) &
                            ~(kRegionAlignment - 1);
        }
    }

    snapshotSavePutBe32(writer, kGoodImageSnapshot);
    SnapshotStagingContext* staging = nullptr;
    VkDeviceSize batchOffset = 0;
    if (!reserveSnapshotReadback(writer, batch, stagingNeeded, &staging, &batchOffset)) {
        return false;
    }
    VkCommandBuffer commandBuffer = staging->commandBuffer;

    VkDeviceSize regionBase = batchOffset;
    for (uint32_t mipLevel = 0; mipLevel < mipLevels; mipLevel++) {
        for (uint32_t arrayLayer = 0; arrayLayer < arrayLayers; arrayLayer++) {
            TransferInfo& mipmapTransferInfo =
                regionTransferInfos[mipLevel * arrayLayers + arrayLayer];
            const VkDeviceSize mipmapStagingBufferSize =
                regionSizes[mipLevel * arrayLayers + arrayLayer];
            std::vector<VkBufferImageCopy>& bufferImageCopies =
                mipmapTransferInfo.bufferImageCopies;
            VkImageAspectFlags aspects = 0;
            for (const auto& copy : bufferImageCopies) {
                aspects |= copy.imageSubresource.aspectMask;
            }
            VkImageLayout layoutBeforeSave = imageInfo->layout;
            VkImageMemoryBarrier imgMemoryBarrier = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = kAllMemoryAccess,
                .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                .oldLayout = layoutBeforeSave,
                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = image,
                .subresourceRange = VkImageSubresourceRange{.aspectMask = aspects,
                                                            .baseMipLevel = mipLevel,
                                                            .levelCount = 1,
                                                            .baseArrayLayer = arrayLayer,
                                                            .layerCount = 1}};

            dispatch->vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                           VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                                           nullptr, 1, &imgMemoryBarrier);

            for (auto& region : bufferImageCopies) {
                region.imageSubresource.mipLevel = mipLevel;
                region.imageSubresource.baseArrayLayer = arrayLayer;
                region.bufferOffset += regionBase;
                dispatch->vkCmdCopyImageToBuffer(commandBuffer, image,
                                                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                                 staging->buffer, 1, &region);
            }

            // Cannot really translate it back to VK_IMAGE_LAYOUT_PREINITIALIZED
            if (layoutBeforeSave != VK_IMAGE_LAYOUT_PREINITIALIZED) {
                imgMemoryBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                imgMemoryBarrier.newLayout = layoutBeforeSave;
                imgMemoryBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                imgMemoryBarrier.dstAccessMask = kAllMemoryAccess;
                dispatch->vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                                               nullptr, 1, &imgMemoryBarrier);
            }
            snapshotSavePutBe64(writer, mipmapStagingBufferSize);
            queueSnapshotSaveBytes(writer, batch, staging, regionBase, mipmapStagingBufferSize);
            regionBase =
                (regionBase + mipmapStagingBufferSize + kRegionAlignment - 1) &
                ~(kRegionAlignment - 1);
        }
    }
    writer->items++;
    writer->bytes += stagingNeeded;
    return true;
}

bool saveBufferContentBatched(SnapshotSaveWriter* writer, SnapshotReadbackBatch* batch,
                              VkBuffer buffer, const BufferInfo* bufferInfo) {
    VkBufferUsageFlags requiredUsages =
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if ((bufferInfo->usage & requiredUsages) != requiredUsages) {
        return true;
    }
    const VkDeviceSize size = static_cast<VkDeviceSize>(bufferInfo->size);
    SnapshotStagingContext* staging = nullptr;
    VkDeviceSize batchOffset = 0;
    if (!reserveSnapshotReadback(writer, batch, size, &staging, &batchOffset)) {
        return false;
    }
    VkBufferCopy bufferCopy = {
        .srcOffset = 0,
        .dstOffset = batchOffset,
        .size = size,
    };
    batch->stateBlock->deviceDispatch->vkCmdCopyBuffer(staging->commandBuffer, buffer,
                                                       staging->buffer, 1, &bufferCopy);
    snapshotSavePutBe64(writer, size);
    queueSnapshotSaveBytes(writer, batch, staging, batchOffset, size);
    writer->items++;
    writer->bytes += size;
    return true;
}

void setEventInQueue(StateBlock* stateBlock, VkEvent event, uint64_t eventflags) {
    VulkanDispatch* dispatch = stateBlock->deviceDispatch;
    VkCommandBufferAllocateInfo allocInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = stateBlock->commandPool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer commandBuffer;
    VK_CHECK(dispatch->vkAllocateCommandBuffers(stateBlock->device, &allocInfo, &commandBuffer));
    VkFenceCreateInfo fenceCreateInfo{
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
    };
    VkFence fence;
    VK_CHECK(dispatch->vkCreateFence(stateBlock->device, &fenceCreateInfo, nullptr, &fence));
    VkCommandBufferBeginInfo beginInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    };
    if (dispatch->vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
        GFXSTREAM_FATAL("Failed to start command buffer on snapshot load");
    }
    dispatch->vkCmdSetEvent(commandBuffer, event, eventflags);
    VkSubmitInfo submitInfo = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &commandBuffer,
    };
    VK_CHECK(dispatch->vkEndCommandBuffer(commandBuffer));
    VK_CHECK(dispatch->vkQueueSubmit(stateBlock->queue, 1, &submitInfo, fence));
    VK_CHECK(dispatch->vkWaitForFences(stateBlock->device, 1, &fence, VK_TRUE, 3000000000L));
    VK_CHECK(dispatch->vkResetFences(stateBlock->device, 1, &fence));
    GFXSTREAM_DEBUG("load in queue 0x%llx: event 0x%llx", (unsigned long long)(stateBlock->queue),
                    (unsigned long long)event);

    dispatch->vkDestroyFence(stateBlock->device, fence, nullptr);
    dispatch->vkFreeCommandBuffers(stateBlock->device, stateBlock->commandPool, 1, &commandBuffer);
}

void signalSemaphore(StateBlock* stateBlock, VkSemaphore unboxed_semaphore) {
    VulkanDispatch* dispatch = stateBlock->deviceDispatch;
    VkFence fence;
    VkFenceCreateInfo fenceCreateInfo{
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
    };
    VK_CHECK(dispatch->vkCreateFence(stateBlock->device, &fenceCreateInfo, nullptr, &fence));
    VkSubmitInfo submitInfo = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 0,
        .pCommandBuffers = nullptr,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &unboxed_semaphore,
    };
    VK_CHECK(dispatch->vkQueueSubmit(stateBlock->queue, 1, &submitInfo, fence));
    VK_CHECK(dispatch->vkWaitForFences(stateBlock->device, 1, &fence, VK_TRUE, 3000000000L));
    VK_CHECK(dispatch->vkResetFences(stateBlock->device, 1, &fence));
    dispatch->vkDestroyFence(stateBlock->device, fence, nullptr);
}

bool loadBufferContent(gfxstream::Stream* stream, StateBlock* stateBlock,
                      SnapshotUploadBatch* batch, VkBuffer buffer,
                      const BufferInfo* bufferInfo) {
    VkBufferUsageFlags requiredUsages =
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if ((bufferInfo->usage & requiredUsages) != requiredUsages) {
        return true;
    }
    VulkanDispatch* dispatch = stateBlock->deviceDispatch;

    SnapshotStagingContext* staging = nullptr;
    VkDeviceSize batchOffset = 0;
    if (!reserveSnapshotUpload(stateBlock, batch, static_cast<VkDeviceSize>(bufferInfo->size),
                               &staging, &batchOffset)) {
        return false;
    }
    VkCommandBuffer commandBuffer = staging->commandBuffer;

    size_t bufferSize = stream->getBe64();
    if (bufferSize != bufferInfo->size) {
        GFXSTREAM_ERROR("Failed to read buffer on snapshot load");
        return false;
    }
    stream->read(static_cast<char*>(staging->mapped) + batchOffset, bufferInfo->size);

    VkBufferCopy bufferCopy = {
        .srcOffset = batchOffset,
        .dstOffset = 0,
        .size = bufferInfo->size,
    };

    dispatch->vkCmdCopyBuffer(commandBuffer, staging->buffer, buffer, 1, &bufferCopy);
    VkBufferMemoryBarrier barrier{.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                                  .pNext = nullptr,
                                  .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                  .dstAccessMask = kAllMemoryAccess,
                                  .srcQueueFamilyIndex = 0xFFFFFFFF,
                                  .dstQueueFamilyIndex = 0xFFFFFFFF,
                                  .buffer = buffer,
                                  .offset = 0,
                                  .size = bufferInfo->size};
    dispatch->vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 1, &barrier,
                                   0, nullptr);
    return true;
}

}  // namespace vk
}  // namespace host
}  // namespace gfxstream
