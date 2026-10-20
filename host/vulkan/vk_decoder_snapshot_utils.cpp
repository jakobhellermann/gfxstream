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

#include "gfxstream/common/logging.h"
#include "vk_common_operations.h"
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

VkExtent3D getMipmapExtent(VkExtent3D baseExtent, uint32_t mipLevel) {
    return VkExtent3D{
        .width = std::max<uint32_t>(baseExtent.width >> mipLevel, 1u),
        .height = std::max<uint32_t>(baseExtent.height >> mipLevel, 1u),
        .depth = std::max<uint32_t>(baseExtent.depth >> mipLevel, 1u),
    };
}

constexpr uint32_t kBadImageSnapshot = 0xbaadbeef;
constexpr uint32_t kGoodImageSnapshot = 0x900df00d;
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

void destroySnapshotStagingContext(StateBlock* stateBlock, SnapshotStagingContext* staging) {
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
                .srcAccessMask = static_cast<VkAccessFlags>(~VK_ACCESS_NONE_KHR),
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
                imgMemoryBarrier.dstAccessMask = ~VK_ACCESS_NONE_KHR;
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
    VK_RETURN_FALSE_ON_ERROR(
        dispatch->vkWaitForFences(stateBlock->device, 1, &staging->fence, VK_TRUE, 3000000000L));
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
                      SnapshotStagingContext* staging, VkImage image,
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
    VkCommandBuffer commandBuffer = staging->commandBuffer;
    VkFence fence = staging->fence;

    if (imageInfo->imageCreateInfoShallow.samples != VK_SAMPLE_COUNT_1_BIT) {
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
            .srcAccessMask = static_cast<VkAccessFlags>(~VK_ACCESS_NONE_KHR),
            .dstAccessMask = static_cast<VkAccessFlags>(~VK_ACCESS_NONE_KHR),
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
        VkCommandBufferBeginInfo beginInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        };
        VK_CHECK(dispatch->vkBeginCommandBuffer(commandBuffer, &beginInfo));

        dispatch->vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                       VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                                       nullptr, 1, &imgMemoryBarrier);

        VK_CHECK(dispatch->vkEndCommandBuffer(commandBuffer));

        // Execute the command to copy image
        VkSubmitInfo submitInfo = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1,
            .pCommandBuffers = &commandBuffer,
        };
        VK_CHECK(dispatch->vkQueueSubmit(stateBlock->queue, 1, &submitInfo, fence));
        VK_CHECK(
            dispatch->vkWaitForFences(stateBlock->device, 1, &fence, VK_TRUE, 3000000000L));
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
    if (!ensureSnapshotStagingCapacity(stateBlock, staging, stagingNeeded)) {
        return false;
    }

    VkDeviceSize regionBase = 0;
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

    VkCommandBufferBeginInfo beginInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    };
    if (dispatch->vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
        GFXSTREAM_ERROR("Failed to start command buffer on snapshot load");
        return false;
    }

    regionBase = 0;
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
                .srcAccessMask = static_cast<VkAccessFlags>(~VK_ACCESS_NONE_KHR),
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
                imgMemoryBarrier.dstAccessMask = ~VK_ACCESS_NONE_KHR;
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
    VK_CHECK(dispatch->vkEndCommandBuffer(commandBuffer));

    VkSubmitInfo submitInfo = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &commandBuffer,
    };
    VK_CHECK(dispatch->vkQueueSubmit(stateBlock->queue, 1, &submitInfo, fence));
    VK_CHECK(dispatch->vkWaitForFences(stateBlock->device, 1, &fence, VK_TRUE, 3000000000L));
    VK_CHECK(dispatch->vkResetFences(stateBlock->device, 1, &fence));
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
    VK_CHECK(
        dispatch->vkWaitForFences(stateBlock->device, 1, &staging->fence, VK_TRUE, 3000000000L));
    VK_CHECK(dispatch->vkResetFences(stateBlock->device, 1, &staging->fence));
    stream->putBe64(bufferInfo->size);
    stream->write(staging->mapped, bufferInfo->size);
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

    size_t bufferSize = stream->getBe64();
    if (bufferSize != bufferInfo->size) {
        GFXSTREAM_ERROR("Failed to read buffer on snapshot load");
        return false;
    }
    stream->read(staging->mapped, bufferInfo->size);

    VkBufferCopy bufferCopy = {
        .srcOffset = 0,
        .dstOffset = 0,
        .size = bufferInfo->size,
    };

    VkCommandBufferBeginInfo beginInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    };
    if (dispatch->vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
        GFXSTREAM_ERROR("Failed to start command buffer on snapshot load");
        return false;
    }
    dispatch->vkCmdCopyBuffer(commandBuffer, staging->buffer, buffer, 1, &bufferCopy);
    VkBufferMemoryBarrier barrier{.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                                  .pNext = nullptr,
                                  .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                  .dstAccessMask = static_cast<VkAccessFlags>(~VK_ACCESS_NONE_KHR),
                                  .srcQueueFamilyIndex = 0xFFFFFFFF,
                                  .dstQueueFamilyIndex = 0xFFFFFFFF,
                                  .buffer = buffer,
                                  .offset = 0,
                                  .size = bufferInfo->size};
    dispatch->vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 1, &barrier,
                                   0, nullptr);

    // Execute the command to copy buffer
    VkSubmitInfo submitInfo = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &commandBuffer,
    };
    VK_CHECK(dispatch->vkEndCommandBuffer(commandBuffer));
    VK_CHECK(dispatch->vkQueueSubmit(stateBlock->queue, 1, &submitInfo, staging->fence));
    VK_CHECK(
        dispatch->vkWaitForFences(stateBlock->device, 1, &staging->fence, VK_TRUE, 3000000000L));
    VK_CHECK(dispatch->vkResetFences(stateBlock->device, 1, &staging->fence));
    return true;
}

}  // namespace vk
}  // namespace host
}  // namespace gfxstream
