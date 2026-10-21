// Copyright 2020 The Android Open Source Project
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

#include "gfxstream/host/file_stream.h"

#include <assert.h>
#include <errno.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <thread>

namespace gfxstream {
namespace host {
namespace {

// Large reads are split across threads: tmpfs page cache lookups and copies
// scale to a few cores, a single reader does not saturate memory bandwidth.
constexpr size_t kParallelReadMin = 1 << 20;
constexpr size_t kParallelReadThreads = 4;

// Returns the bytes read, short only at end of file, or -1 with errno set.
ssize_t preadFull(int fd, char* buffer, size_t size, off_t offset) {
    size_t done = 0;
    while (done < size) {
        ssize_t n = ::pread(fd, buffer + done, size - done, offset + done);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            return -1;
        }
        if (n == 0) {
            break;
        }
        done += n;
    }
    return done;
}

// Reads `size` bytes at the stream's position with parallel preads and
// moves the stream past them; same result contract as fread.
size_t readParallel(FILE* file, char* buffer, size_t size, off_t start) {
    const int fd = ::fileno(file);
    const size_t chunk = (size + kParallelReadThreads - 1) / kParallelReadThreads;
    std::array<ssize_t, kParallelReadThreads> got{};
    std::array<int, kParallelReadThreads> errors{};
    auto readChunk = [&](size_t i) {
        const size_t begin = std::min(size, i * chunk);
        got[i] = preadFull(fd, buffer + begin, std::min(chunk, size - begin), start + begin);
        errors[i] = got[i] < 0 ? errno : 0;
    };
    std::array<std::thread, kParallelReadThreads - 1> helpers;
    for (size_t i = 1; i < kParallelReadThreads; i++) {
        helpers[i - 1] = std::thread(readChunk, i);
    }
    readChunk(0);
    for (auto& helper : helpers) {
        helper.join();
    }
    size_t total = 0;
    for (size_t i = 0; i < kParallelReadThreads; i++) {
        if (got[i] < 0) {
            errno = errors[i];
            break;
        }
        total += got[i];
        if (static_cast<size_t>(got[i]) < std::min(chunk, size - std::min(size, i * chunk))) {
            break;
        }
    }
    if (::fseeko(file, start + total, SEEK_SET) != 0) {
        return 0;
    }
    return total;
}

}  // namespace

StdioStream::StdioStream(FILE* file, Ownership ownership) :
    mFile(file), mOwnership(ownership) {}

StdioStream::StdioStream(StdioStream&& other)
    : mFile(other.mFile), mOwnership(other.mOwnership) {
    other.mFile = nullptr;
}

StdioStream& StdioStream::operator=(StdioStream&& other) {
    assert(this != &other);
    close();
    mFile = other.mFile;
    mOwnership = other.mOwnership;
    other.mFile = nullptr;
    return *this;
}

StdioStream::~StdioStream() {
    close();
}

ssize_t StdioStream::read(void* buffer, size_t size) {
    if (size >= kParallelReadMin) {
        const off_t start = ::ftello(mFile);
        if (start >= 0) {
            return static_cast<ssize_t>(
                readParallel(mFile, static_cast<char*>(buffer), size, start));
        }
    }
    size_t res = ::fread(buffer, 1, size, mFile);
    if (res < size) {
        if (!::feof(mFile)) {
            errno = ::ferror(mFile);
        }
    }
    return static_cast<ssize_t>(res);
}

ssize_t StdioStream::write(const void* buffer, size_t size) {
    size_t res = ::fwrite(buffer, 1, size, mFile);
    if (res < size) {
        if (!::feof(mFile)) {
            errno = ::ferror(mFile);
        }
    }
    return static_cast<ssize_t>(res);
}

void StdioStream::close() {
    if (mOwnership == kOwner && mFile) {
        ::fclose(mFile);
        mFile = nullptr;
    }
}

}  // namespace host
}  // namespace gfxstream
