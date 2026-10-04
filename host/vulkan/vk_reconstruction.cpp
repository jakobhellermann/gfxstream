// Copyright (C) 2019 The Android Open Source Project
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

#include "vk_reconstruction.h"

#include <string.h>

#include <unordered_map>

#include "vk_decoder.h"
#include "vk_recording_order.h"
#include "vulkan_boxed_handles.h"
#include "gfxstream/containers/EntityManager.h"

#define DEBUG_RECONSTRUCTION 0

#if DEBUG_RECONSTRUCTION

#define DEBUG_RECON(fmt, ...) GFXSTREAM_INFO(fmt, ##__VA_ARGS__);

#else

#define DEBUG_RECON(fmt, ...)

#endif

namespace gfxstream {
namespace host {
namespace vk {
namespace {

#if DEBUG_RECONSTRUCTION
uint32_t GetOpcode(const VkSnapshotApiCallInfo& info) {
    if (info.packet.size() <= 4) return -1;

    return *(reinterpret_cast<const uint32_t*>(info.packet.data()));
}
#endif

}  // namespace

VkReconstruction::VkReconstruction() = default;

void VkReconstruction::clear() {
    mGraph.clear();
    mApiCallManager.clear();
    mSubDecodeApiCalls.clear();
}

void VkReconstruction::saveReplayBuffers(gfxstream::Stream* stream) {
    DEBUG_RECON("start")

#if DEBUG_RECONSTRUCTION
    dump();
#endif

    std::vector<uint64_t> uniqApiRefsByTopoOrder;

    mGraph.getIdsByTimestamp(uniqApiRefsByTopoOrder);

    size_t totalApiTraceSize = 0;

    for (auto apiHandle : uniqApiRefsByTopoOrder) {
        const VkSnapshotApiCallInfo* info = mApiCallManager.get(apiHandle);
        totalApiTraceSize += info->packet.size();
    }

    DEBUG_RECON("total api trace size: %zu", totalApiTraceSize);

    std::vector<uint64_t> createdHandleBuffer;

    for (auto apiHandle : uniqApiRefsByTopoOrder) {
        auto item = mApiCallManager.get(apiHandle);
        for (auto createdHandle : item->createdHandles) {
            DEBUG_RECON("save handle: 0x%lx", createdHandle);
            createdHandleBuffer.push_back(createdHandle);
        }
    }

    std::vector<uint8_t> apiTraceBuffer;
    apiTraceBuffer.resize(totalApiTraceSize);

    uint8_t* apiTracePtr = apiTraceBuffer.data();

    for (auto apiHandle : uniqApiRefsByTopoOrder) {
        auto item = mApiCallManager.get(apiHandle);
        // 4 bytes for opcode, and 4 bytes for saveBufferRaw's size field
        DEBUG_RECON("saving api handle 0x%lx op code %d name %s", apiHandle, GetOpcode(*item),
                api_opcode_to_string(GetOpcode(*item)));
        memcpy(apiTracePtr, item->packet.data(), item->packet.size());
        apiTracePtr += item->packet.size();
    }

    DEBUG_RECON("created handle buffer size: %zu trace: %zu", createdHandleBuffer.size(),
                apiTraceBuffer.size());

    // TODO(ai-review): generated, not yet audited
    // Append sub-decoded (command buffer recording) api packets after the
    // graph-derived stream. These calls create no handles, so appending does
    // not disturb the created-handle pairing the loader derives from the
    // create-calls; the loader's linear decode just re-applies the recording
    // after all objects exist.
    size_t subTraceBytes = 0;
    size_t subResolved = 0;
    std::vector<uint64_t> subCmdBuffer;
    std::vector<uint8_t> subPacketBuffer;
    std::vector<uint8_t> subTraceBuffer;  // rewritten packets, main-stream format
    std::map<uint64_t, std::vector<const VkSnapshotApiCallInfo*>> recordings;
    std::vector<uint64_t> recordingOrder;
    for (const auto& [apiHandle, boxedCmd] : mSubDecodeApiCalls) {
        // Drop entries for command buffers that no longer exist (freed, or
        // freed via vkResetCommandPool): replaying their packets would unbox
        // dead handles and abort the restore.
        if (!mGraph.getDepNode(boxedCmd)) continue;
        const VkSnapshotApiCallInfo* info = mApiCallManager.get(apiHandle);
        if (!info || info->packet.size() < 8 ||
            *(const uint32_t*)(info->packet.data() + 4) != info->packet.size()) {
            continue;
        }
        // Raw sub-stream packet (blob format: [op][len][args], no seqno, no
        // dispatchable handle) saved together with its boxed command buffer;
        // the loader re-registers it so the next save keeps the content.
        subCmdBuffer.push_back(boxedCmd);
        subPacketBuffer.insert(subPacketBuffer.end(), info->packet.begin(), info->packet.end());
        if (recordings[boxedCmd].empty()) recordingOrder.push_back(boxedCmd);
        recordings[boxedCmd].push_back(info);
        subResolved++;
    }

    // TODO(ai-review): generated, not yet audited
    // vkCmdExecuteCommands records a reference to an executable secondary
    // command buffer. The guest can record the primary first and later flush
    // the secondary recording, so arrival order is not a valid replay order.
    // Replay each buffer's packets in order, but finish its dependencies first.
    std::map<uint64_t, std::vector<uint64_t>> dependencies;
    for (uint64_t boxedCmd : recordingOrder) {
        for (const auto* info : recordings[boxedCmd]) {
            const auto& packet = info->packet;
            uint32_t opcode;
            memcpy(&opcode, packet.data(), sizeof(opcode));
            if (opcode != OP_vkCmdExecuteCommands || packet.size() < 12) continue;
            uint32_t count;
            memcpy(&count, packet.data() + 8, sizeof(count));
            if (count > (packet.size() - 12) / sizeof(uint64_t)) {
                GFXSTREAM_WARNING("snapshot save: malformed vkCmdExecuteCommands in 0x%llx",
                                  (unsigned long long)boxedCmd);
                continue;
            }
            for (uint32_t i = 0; i < count; ++i) {
                uint64_t secondary;
                memcpy(&secondary, packet.data() + 12 + i * sizeof(secondary), sizeof(secondary));
                if (recordings.count(secondary))
                    dependencies[boxedCmd].push_back(secondary);
                else
                    GFXSTREAM_WARNING("snapshot save: secondary 0x%llx has no recording",
                                      (unsigned long long)secondary);
            }
        }
        dependencies.try_emplace(boxedCmd);
    }
    const auto replayOrder = orderCommandBufferRecordings(recordingOrder, dependencies);

    for (uint64_t boxedCmd : replayOrder) {
        for (const auto* info : recordings[boxedCmd]) {
            // Rewritten into main-stream format for the loader's replay: main
            // stream packets carry a 4-byte seqno (QueueSubmitWithCommands) and
            // the dispatchable VkCommandBuffer, both of which the sub-stream
            // omits (the seqno lives in the decode loop, the handle in the flush
            // call).
            const uint32_t rewrittenLen = (uint32_t)info->packet.size() + 12;
            const uint32_t seqno = 0;
            const size_t base = subTraceBuffer.size();
            subTraceBuffer.resize(base + rewrittenLen);
            memcpy(subTraceBuffer.data() + base, info->packet.data(), 4);  // opcode
            memcpy(subTraceBuffer.data() + base + 4, &rewrittenLen, 4);    // length
            memcpy(subTraceBuffer.data() + base + 8, &seqno, 4);           // seqno
            memcpy(subTraceBuffer.data() + base + 12, &boxedCmd, 8);       // VkCommandBuffer
            memcpy(subTraceBuffer.data() + base + 20, info->packet.data() + 8,
                   info->packet.size() - 8);
            subTraceBytes += rewrittenLen;
        }
    }
    apiTraceBuffer.resize(totalApiTraceSize + subTraceBytes);
    memcpy(apiTraceBuffer.data() + totalApiTraceSize, subTraceBuffer.data(), subTraceBuffer.size());
    GFXSTREAM_INFO(
        "snapshot save: %zu graph api calls, %zu sub-decoded recording calls (%zu bytes, %zu "
        "resolved)",
        uniqApiRefsByTopoOrder.size(), mSubDecodeApiCalls.size(), subTraceBytes, subResolved);

    gfxstream::host::saveBuffer(stream, createdHandleBuffer);
    gfxstream::host::saveBuffer(stream, apiTraceBuffer);
    gfxstream::host::saveBuffer(stream, subCmdBuffer);
    gfxstream::host::saveBuffer(stream, subPacketBuffer);
}

/*static*/
void VkReconstruction::loadReplayBuffers(gfxstream::Stream* stream,
                                         std::vector<uint64_t>* outHandleBuffer,
                                         std::vector<uint8_t>* outDecoderBuffer,
                                         std::vector<uint64_t>* outSubCmdBuffer,
                                         std::vector<uint8_t>* outSubPacketBuffer) {
    DEBUG_RECON("starting to unpack decoder replay buffer");

    gfxstream::host::loadBuffer(stream, outHandleBuffer);
    gfxstream::host::loadBuffer(stream, outDecoderBuffer);
    gfxstream::host::loadBuffer(stream, outSubCmdBuffer);
    gfxstream::host::loadBuffer(stream, outSubPacketBuffer);

    DEBUG_RECON("finished unpacking decoder replay buffer");
}

VkSnapshotApiCallHandle VkReconstruction::createApiCallInfo() {
    VkSnapshotApiCallHandle handle = mApiCallManager.add(VkSnapshotApiCallInfo(), 1);

    auto* info = mApiCallManager.get(handle);
    info->handle = handle;

    return handle;
}

void VkReconstruction::removeHandleFromApiInfo(VkSnapshotApiCallHandle h, uint64_t toRemove) {}

void VkReconstruction::destroyApiCallInfoIfUnused(VkSnapshotApiCallHandle handle) {
    VkSnapshotApiCallInfo* info = mApiCallManager.get(handle);
    if (!info) return;

    if (info->packet.empty()) {
        mApiCallManager.remove(handle);
        mGraph.removeApiNode(handle);
        return;
    }

    if (!info->extraCreatedHandles.empty()) {
        info->createdHandles.insert(info->createdHandles.end(),
                                    info->extraCreatedHandles.begin(),
                                    info->extraCreatedHandles.end());
        info->extraCreatedHandles.clear();
    }
}

void VkReconstruction::setApiTrace(VkSnapshotApiCallHandle apiCallHandle, const uint8_t* packet,
                                   size_t packetLenBytes) {
    VkSnapshotApiCallInfo* info = mApiCallManager.get(apiCallHandle);
    if (info && packet && packetLenBytes > 0) {
        info->packet.assign(packet, packet + packetLenBytes);
    }
}

void VkReconstruction::dump() { DEBUG_RECON("%s: dep graph dump", __func__); }

void VkReconstruction::addHandles(const uint64_t* toAdd, uint32_t count) {
    if (!toAdd) return;
    mGraph.addNodes(toAdd, count);
}

void VkReconstruction::removeHandles(const uint64_t* toRemove, uint32_t count, bool recursive) {
    if (!toRemove) return;

    mGraph.removeNodesAndDescendants(toRemove, count);
}

void VkReconstruction::forEachHandleAddApi(const uint64_t* toProcess, uint32_t count,
                                           uint64_t apiHandle, HandleState state) {
    if (!toProcess) return;

    if (state == VkReconstruction::CREATED) {
        mGraph.associateWithApiCall(toProcess, count, apiHandle);
    }
}

void VkReconstruction::removeDescendantsOfHandle(const uint64_t handle) {
    mGraph.removeDescendantsOfHandle(handle);
}

void VkReconstruction::removeGrandChildren(const uint64_t handle) {
    mGraph.removeGrandChildren(handle);
}

void VkReconstruction::addApiCallDependencyOnVkObject(VkSnapshotApiCallHandle handle,
                                                      VkObjectHandle object) {
    VkSnapshotApiCallInfo* info = mApiCallManager.get(handle);
    if (!info) return;

    info->depends.push_back(object);
}

void VkReconstruction::addHandleDependenciesForApiCallDependencies(VkSnapshotApiCallHandle apiCallHandle,
                                                                   VkObjectHandle child) {
    VkSnapshotApiCallInfo* apiCallInfo = mApiCallManager.get(apiCallHandle);
    if (!apiCallInfo) return;

    for (const VkObjectHandle parent : apiCallInfo->depends) {
        addHandleDependency(&child, 1, parent);
    }
}

void VkReconstruction::addHandleDependency(const uint64_t* handles, uint32_t count,
                                           uint64_t parentHandle, HandleState childState,
                                           HandleState parentState) {
    if (!handles) return;

    if (!parentHandle) return;

    mGraph.addNodeIdDependencies(handles, count, parentHandle);
}

void VkReconstruction::setCreatedHandlesForApi(uint64_t apiHandle, const uint64_t* created,
                                               uint32_t count) {
    if (!created) return;

    mGraph.setCreatedNodeIdsForApi(apiHandle, created, count);
    auto item = mApiCallManager.get(apiHandle);

    if (!item) return;

    item->createdHandles.insert(item->createdHandles.end(), created, created + count);
}


void VkReconstruction::addOrderedBoxedHandlesCreatedByCall(VkSnapshotApiCallHandle handle,
                                            const VkObjectHandle* boxedHandles,
                                            uint32_t boxedHandlesCount) {
    VkSnapshotApiCallInfo* info = mApiCallManager.get(handle);
    if (!info) return;

    info->extraCreatedHandles.insert(info->extraCreatedHandles.end(),
                                     boxedHandles,
                                     boxedHandles + boxedHandlesCount);
}

}  // namespace vk
}  // namespace host
}  // namespace gfxstream
