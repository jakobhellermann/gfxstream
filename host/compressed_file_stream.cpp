#include "compressed_file_stream.h"

#include <zstd.h>

#include "gfxstream/common/logging.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace gfxstream {
namespace host {

namespace {

constexpr char kMagic[8] = {'\x89', 'G', 'F', 'X', 'Z', 'S', 'T', '\n'};
constexpr uint32_t kVersion = 1;
constexpr size_t kDefaultBlockBytes = 2 << 20;
// a header's block size above this is a broken file
constexpr size_t kMaxBlockBytes = 64 << 20;

int envInt(const char* name, int fallback) {
    const char* env = getenv(name);
    return env && *env ? atoi(env) : fallback;
}

void putLe32(char* out, uint32_t value) {
    for (int i = 0; i < 4; i++) {
        out[i] = static_cast<char>(value >> (8 * i));
    }
}

uint32_t getLe32(const char* in) {
    uint32_t value = 0;
    for (int i = 0; i < 4; i++) {
        value |= static_cast<uint32_t>(static_cast<uint8_t>(in[i])) << (8 * i);
    }
    return value;
}

double msSince(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

}  // namespace

CompressedFileStream::CompressedFileStream(FILE* file, Mode mode, const char* label)
    : mFile(file), mMode(mode), mLabel(label), mStart(std::chrono::steady_clock::now()) {
    if (!mFile) {
        return;
    }
    if (mMode == kWrite) {
        mLevel = envInt("GFXSTREAM_SNAPSHOT_ZSTD_LEVEL", 1);
        mCompressed = mLevel != 0;
        mBlockBytes = kDefaultBlockBytes;
        if (mCompressed) {
            char header[16];
            memcpy(header, kMagic, 8);
            putLe32(header + 8, kVersion);
            putLe32(header + 12, static_cast<uint32_t>(mBlockBytes));
            if (fwrite(header, 1, sizeof(header), mFile) != sizeof(header)) {
                addError("%s: header write failed", mLabel.c_str());
            }
            mFileBytes += sizeof(header);
        }
    } else {
        char header[16];
        size_t got = fread(header, 1, sizeof(header), mFile);
        if (got == sizeof(header) && memcmp(header, kMagic, 8) == 0 &&
            getLe32(header + 8) == kVersion && getLe32(header + 12) > 0 &&
            getLe32(header + 12) <= kMaxBlockBytes) {
            mCompressed = true;
            mBlockBytes = getLe32(header + 12);
            mFileBytes += sizeof(header);
        } else if (fseek(mFile, 0, SEEK_SET) != 0) {
            addError("%s: rewind failed", mLabel.c_str());
        }
    }
    if (!mCompressed) {
        return;
    }
    mStoredCapacity = ZSTD_compressBound(mBlockBytes);
    unsigned threads = static_cast<unsigned>(std::max(1, envInt("GFXSTREAM_SNAPSHOT_ZSTD_THREADS", 8)));
    mMaxInflight = 2 * threads + 1;
    for (unsigned i = 0; i < threads; i++) {
        mWorkers.emplace_back([this] { workerLoop(); });
    }
}

CompressedFileStream::~CompressedFileStream() {
    if (mMode == kWrite && !mFinished) {
        finish();
    }
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mStop = true;
    }
    mJobReady.notify_all();
    for (auto& worker : mWorkers) {
        worker.join();
    }
    if (mMode == kRead && mCompressed) {
        logTiming();
    }
    if (mFile) {
        fclose(mFile);
    }
}

void CompressedFileStream::workerLoop() {
    ZSTD_CCtx* cctx = mMode == kWrite ? ZSTD_createCCtx() : nullptr;
    ZSTD_DCtx* dctx = mMode == kRead ? ZSTD_createDCtx() : nullptr;
    std::unique_lock<std::mutex> lock(mMutex);
    while (true) {
        mJobReady.wait(lock, [this] { return mStop || !mJobs.empty(); });
        if (mJobs.empty()) {
            break;
        }
        Block* block = mJobs.front();
        mJobs.pop_front();
        lock.unlock();
        auto start = std::chrono::steady_clock::now();
        bool failed = false;
        if (mMode == kWrite) {
            size_t r = ZSTD_compressCCtx(cctx, block->stored.get(), mStoredCapacity,
                                         block->raw.get(), block->rawSize, mLevel);
            failed = ZSTD_isError(r);
            block->storedSize = failed ? 0 : r;
        } else {
            size_t r = ZSTD_decompressDCtx(dctx, block->raw.get(), mBlockBytes,
                                           block->stored.get(), block->storedSize);
            failed = ZSTD_isError(r) || r != block->rawSize;
        }
        double ms = msSince(start);
        lock.lock();
        block->failed = failed;
        block->done = true;
        mWorkerMs += ms;
        mBlockDone.notify_all();
    }
    lock.unlock();
    ZSTD_freeCCtx(cctx);
    ZSTD_freeDCtx(dctx);
}

CompressedFileStream::Block* CompressedFileStream::takeBlock() {
    if (!mFree.empty()) {
        Block* block = mFree.back();
        mFree.pop_back();
        return block;
    }
    auto block = std::make_unique<Block>();
    block->raw.reset(new char[mBlockBytes]);
    block->stored.reset(new char[mStoredCapacity]);
    mBlocks.push_back(std::move(block));
    return mBlocks.back().get();
}

void CompressedFileStream::submit(Block* block) {
    block->done = false;
    block->failed = false;
    mInflight.push_back(block);
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mJobs.push_back(block);
    }
    mJobReady.notify_one();
}

CompressedFileStream::Block* CompressedFileStream::waitOldest() {
    Block* block = mInflight.front();
    mInflight.pop_front();
    auto start = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> lock(mMutex);
    mBlockDone.wait(lock, [block] { return block->done; });
    mWaitMs += msSince(start);
    return block;
}

bool CompressedFileStream::writeOldest() {
    Block* block = waitOldest();
    bool ok = !block->failed;
    if (ok) {
        char header[8];
        putLe32(header, static_cast<uint32_t>(block->rawSize));
        putLe32(header + 4, static_cast<uint32_t>(block->storedSize));
        ok = fwrite(header, 1, sizeof(header), mFile) == sizeof(header) &&
             fwrite(block->stored.get(), 1, block->storedSize, mFile) == block->storedSize;
        mFileBytes += sizeof(header) + block->storedSize;
    }
    if (!ok) {
        addError("%s: block %s failed", mLabel.c_str(), block->failed ? "compression" : "write");
    }
    mFree.push_back(block);
    return ok;
}

ssize_t CompressedFileStream::write(const void* buffer, size_t size) {
    if (!mFile || mMode != kWrite || mFinished) {
        return -EBADF;
    }
    if (!mCompressed) {
        size_t written = fwrite(buffer, 1, size, mFile);
        mRawBytes += written;
        mFileBytes += written;
        return written == size || size == 0 ? static_cast<ssize_t>(written) : -EIO;
    }
    const char* in = static_cast<const char*>(buffer);
    size_t left = size;
    while (left > 0) {
        if (!mCurrent) {
            mCurrent = takeBlock();
            mCurrent->rawSize = 0;
        }
        size_t n = std::min(left, mBlockBytes - mCurrent->rawSize);
        memcpy(mCurrent->raw.get() + mCurrent->rawSize, in, n);
        mCurrent->rawSize += n;
        in += n;
        left -= n;
        if (mCurrent->rawSize == mBlockBytes) {
            submit(mCurrent);
            mCurrent = nullptr;
            if (mInflight.size() >= mMaxInflight && !writeOldest()) {
                return -EIO;
            }
        }
    }
    mRawBytes += size;
    return static_cast<ssize_t>(size);
}

bool CompressedFileStream::finish() {
    if (mMode != kWrite || mFinished) {
        return !hasErrors();
    }
    mFinished = true;
    if (!mFile) {
        return false;
    }
    bool ok = true;
    if (mCompressed) {
        if (mCurrent && mCurrent->rawSize > 0) {
            submit(mCurrent);
        } else if (mCurrent) {
            mFree.push_back(mCurrent);
        }
        mCurrent = nullptr;
        while (!mInflight.empty()) {
            ok = writeOldest() && ok;
        }
        char end[8] = {};
        ok = fwrite(end, 1, sizeof(end), mFile) == sizeof(end) && ok;
        mFileBytes += sizeof(end);
    }
    ok = fflush(mFile) == 0 && ok;
    if (!ok) {
        addError("%s: finishing the file failed", mLabel.c_str());
    }
    if (mCompressed) {
        logTiming();
    }
    return ok && !hasErrors();
}

// Reads blocks ahead while there is room; false on a broken file.
bool CompressedFileStream::readAhead() {
    while (!mEof && mInflight.size() < mMaxInflight) {
        char header[8];
        if (fread(header, 1, sizeof(header), mFile) != sizeof(header)) {
            addError("%s: truncated file", mLabel.c_str());
            return false;
        }
        uint32_t rawSize = getLe32(header);
        uint32_t storedSize = getLe32(header + 4);
        mFileBytes += sizeof(header);
        if (rawSize == 0) {
            mEof = true;
            break;
        }
        if (rawSize > mBlockBytes || storedSize > mStoredCapacity) {
            addError("%s: bad block header", mLabel.c_str());
            return false;
        }
        Block* block = takeBlock();
        block->rawSize = rawSize;
        block->storedSize = storedSize;
        if (fread(block->stored.get(), 1, storedSize, mFile) != storedSize) {
            mFree.push_back(block);
            addError("%s: truncated block", mLabel.c_str());
            return false;
        }
        mFileBytes += storedSize;
        submit(block);
    }
    return true;
}

ssize_t CompressedFileStream::read(void* buffer, size_t size) {
    if (!mFile || mMode != kRead) {
        return -EBADF;
    }
    if (!mCompressed) {
        size_t got = fread(buffer, 1, size, mFile);
        mRawBytes += got;
        return static_cast<ssize_t>(got);
    }
    char* out = static_cast<char*>(buffer);
    size_t copied = 0;
    while (copied < size) {
        if (mCurrent && mCurrentPos < mCurrent->rawSize) {
            size_t n = std::min(size - copied, mCurrent->rawSize - mCurrentPos);
            memcpy(out + copied, mCurrent->raw.get() + mCurrentPos, n);
            mCurrentPos += n;
            copied += n;
            continue;
        }
        if (mCurrent) {
            mFree.push_back(mCurrent);
            mCurrent = nullptr;
        }
        if (!readAhead()) {
            return -EIO;
        }
        if (mInflight.empty()) {
            break;  // end of the stream
        }
        mCurrent = waitOldest();
        mCurrentPos = 0;
        if (mCurrent->failed) {
            addError("%s: block decompression failed", mLabel.c_str());
            return -EIO;
        }
    }
    mRawBytes += copied;
    return static_cast<ssize_t>(copied);
}

void CompressedFileStream::logTiming() {
    GFXSTREAM_DEBUG(
        "snapshot zstd %s %s: %.1f MB <-> %.1f MB in the file, level %d, %zu threads, %.1f ms open, "
        "workers %.1f ms, waited %.1f ms",
        mMode == kWrite ? "save" : "load", mLabel.c_str(), mRawBytes / 1048576.0,
        mFileBytes / 1048576.0, mLevel, mWorkers.size(), msSince(mStart), mWorkerMs, mWaitMs);
}

}  // namespace host
}  // namespace gfxstream
