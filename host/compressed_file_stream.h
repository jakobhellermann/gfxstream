// Snapshot file stream with zstd block compression on worker threads (libTAS GPU savestates keep
// several snapshots in /tmp, a tmpfs: compressed they take ~3.5x less RAM).
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "render-utils/stream.h"

namespace gfxstream {
namespace host {

// Write: with GFXSTREAM_SNAPSHOT_ZSTD_LEVEL (default 1; 0 = off) the file is kMagic, a small header
// and blocks of [u32 raw size][u32 stored size][zstd frame], ended by a [0][0] block; with 0 the
// bytes go to the file as they are, like StdioStream. Read: files starting with kMagic are
// decompressed (blocks ahead on the worker threads), any other file is read as it is, so older
// snapshots keep loading. GFXSTREAM_SNAPSHOT_ZSTD_THREADS sets the worker count (default 8).
class CompressedFileStream : public gfxstream::StreamWithErrorLogger {
  public:
    enum Mode { kRead, kWrite };

    // Takes ownership of `file` (may be null: every read and write then fails).
    CompressedFileStream(FILE* file, Mode mode, const char* label);
    ~CompressedFileStream() override;

    CompressedFileStream(const CompressedFileStream&) = delete;
    CompressedFileStream& operator=(const CompressedFileStream&) = delete;

    ssize_t read(void* buffer, size_t size) override;
    ssize_t write(const void* buffer, size_t size) override;

    // Write mode: writes the remaining blocks and flushes the FILE. Call before using get().
    bool finish();
    FILE* get() const { return mFile; }

  private:
    struct Block {
        std::unique_ptr<char[]> raw;
        size_t rawSize = 0;
        std::unique_ptr<char[]> stored;
        size_t storedSize = 0;
        bool done = false;
        bool failed = false;
    };

    Block* takeBlock();
    void submit(Block* block);
    Block* waitOldest();
    bool writeOldest();
    bool readAhead();
    void workerLoop();
    void logTiming();

    FILE* mFile;
    Mode mMode;
    std::string mLabel;
    bool mCompressed = false;
    int mLevel = 0;
    size_t mBlockBytes = 0;
    size_t mStoredCapacity = 0;
    size_t mMaxInflight = 0;
    bool mFinished = false;
    bool mEof = false;

    std::vector<std::unique_ptr<Block>> mBlocks;
    std::vector<Block*> mFree;
    std::deque<Block*> mInflight;  // in file order
    Block* mCurrent = nullptr;     // write: being filled; read: being consumed
    size_t mCurrentPos = 0;

    std::mutex mMutex;
    std::condition_variable mJobReady;
    std::condition_variable mBlockDone;
    std::deque<Block*> mJobs;
    bool mStop = false;
    std::vector<std::thread> mWorkers;

    uint64_t mRawBytes = 0;
    uint64_t mFileBytes = 0;
    double mWaitMs = 0;
    double mWorkerMs = 0;
    std::chrono::steady_clock::time_point mStart;
};

}  // namespace host
}  // namespace gfxstream
