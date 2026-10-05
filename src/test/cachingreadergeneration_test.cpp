#include "engine/cachingreader/cachingreader.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <future>
#include <memory>
#include <thread>

#include "engine/engineworkerscheduler.h"
#include "test/callbackallocationcheck.h"
#include "test/mixxxtest.h"

using namespace std::chrono_literals;

class CachingReaderGenerationTest : public MixxxTest {
  protected:
    std::unique_ptr<CachingReader> makeReader(EngineWorkerScheduler* scheduler) {
        auto reader = std::make_unique<CachingReader>(QStringLiteral("[ReaderGenerationTest]"),
                config(), mixxx::audio::ChannelCount::stereo());
        reader->m_worker.setScheduler(scheduler);
        return reader;
    }

    void fillStatusQueue(CachingReader* reader) {
        const auto update = ReaderStatusUpdate::trackUnloaded(0);
        while (reader->m_readerStatusUpdateFIFO.write(&update, 1) == 1) {
        }
        EXPECT_EQ(0, reader->m_readerStatusUpdateFIFO.writeAvailable());
    }

    void requestUnload(CachingReader* reader) {
#ifdef __STEM__
        reader->newTrack({}, {});
#else
        reader->newTrack({});
#endif
        reader->m_worker.wakeIfReady();
    }

    bool requestDequeued(const CachingReader& reader) const {
        return reader.m_worker.m_newTrackAvailable.loadAcquire() == 0;
    }

    void stopWorker(CachingReader* reader) {
        reader->m_worker.quitWait();
    }

    void drainStatusQueue(CachingReader* reader) {
        ReaderStatusUpdate update;
        while (reader->m_readerStatusUpdateFIFO.read(&update, 1) == 1) {
        }
    }

    quint64 beginRequest(CachingReader* reader, bool loading) {
        return reader->beginTrackRequest(loading);
    }

    void postStatus(CachingReader* reader, const ReaderStatusUpdate& update) {
        ASSERT_EQ(1, reader->m_readerStatusUpdateFIFO.write(&update, 1));
    }

    void expectLoading(const CachingReader& reader, quint64 generation) {
        EXPECT_EQ(reader.packedState(generation, CachingReader::STATE_TRACK_LOADING),
                reader.m_requestState.load());
    }

    void expectUnloading(const CachingReader& reader, quint64 generation) {
        EXPECT_EQ(reader.packedState(generation, CachingReader::STATE_TRACK_UNLOADING),
                reader.m_requestState.load());
    }

    void expectIdle(const CachingReader& reader, quint64 generation) {
        EXPECT_EQ(reader.packedState(generation, CachingReader::STATE_IDLE),
                reader.m_requestState.load());
    }

    void expectLoaded(const CachingReader& reader, quint64 generation,
            const mixxx::IndexRange& range) {
        EXPECT_EQ(reader.packedState(generation, CachingReader::STATE_TRACK_LOADED),
                reader.m_requestState.load());
        EXPECT_EQ(generation, reader.m_cacheGeneration);
        EXPECT_EQ(range, reader.m_readableFrameIndexRange);
    }

    CachingReaderChunkForOwner* allocateChunk(CachingReader* reader, SINT index) {
        return reader->allocateChunk(index);
    }

    CachingReaderChunkForOwner* lookupChunk(CachingReader* reader, SINT index) {
        return reader->lookupChunk(index);
    }

    std::size_t freeChunks(const CachingReader& reader) const {
        return reader.m_freeChunks.size();
    }

    bool churnChunks(CachingReader* reader, int count) {
        constexpr int kPoolSize = 80;
        std::array<CachingReaderChunkForOwner*, kPoolSize> chunks{};
        while (count > 0) {
            const int batchSize = std::min(count, kPoolSize);
            for (int i = 0; i < batchSize; ++i) {
                const int index = (i * 37) % kPoolSize;
                chunks[i] = reader->allocateChunk(index);
                if (!chunks[i]) {
                    return false;
                }
            }
            for (int i = 0; i < batchSize; ++i) {
                if (reader->lookupChunk((i * 37) % kPoolSize) != chunks[i]) {
                    return false;
                }
            }
            for (int i = 0; i < batchSize; ++i) {
                reader->freeChunk(chunks[i]);
            }
            count -= batchSize;
        }
        return true;
    }

    void submitWorkerRequest(CachingReaderWorker* worker, quint64 generation) {
#ifdef __STEM__
        worker->newTrack({}, {}, generation);
#else
        worker->newTrack({}, generation);
#endif
    }

    void expectPendingWorkerRequest(const CachingReaderWorker& worker, quint64 generation) {
        EXPECT_EQ(generation, worker.m_requestedGeneration.load());
        EXPECT_EQ(generation, worker.m_pNewTrack.generation);
        EXPECT_EQ(1, worker.m_newTrackAvailable.loadAcquire());
    }
};

TEST_F(CachingReaderGenerationTest, OutOfOrderSubmitCannotReplaceNewerPendingRequest) {
    EngineWorkerScheduler scheduler;
    FIFO<CachingReaderChunkReadRequest> requests(20);
    FIFO<ReaderStatusUpdate> statuses(80);
    CachingReaderWorker worker(QStringLiteral("[ReaderGenerationTest]"),
            &requests,
            &statuses,
            mixxx::audio::ChannelCount::stereo());
    worker.setScheduler(&scheduler);

    submitWorkerRequest(&worker, 2);
    submitWorkerRequest(&worker, 1);
    expectPendingWorkerRequest(worker, 2);
    submitWorkerRequest(&worker, 2);
    expectPendingWorkerRequest(worker, 2);
    submitWorkerRequest(&worker, 3);
    expectPendingWorkerRequest(worker, 3);
}

TEST_F(CachingReaderGenerationTest, OldLoadedStatusCannotReplaceNewerEjectRequest) {
    EngineWorkerScheduler scheduler;
    const auto reader = makeReader(&scheduler);
    const auto load = beginRequest(reader.get(), true);
    const auto eject = beginRequest(reader.get(), false);
    postStatus(reader.get(), ReaderStatusUpdate::trackLoaded(mixxx::IndexRange::forward(0, 32768),
                                    load));
    reader->process();
    expectUnloading(*reader, eject);
    postStatus(reader.get(), ReaderStatusUpdate::trackUnloaded(eject));
    reader->process();
    expectIdle(*reader, eject);
}

TEST_F(CachingReaderGenerationTest, OldLoadedAndFailedStatusesCannotReplaceNewerTrack) {
    EngineWorkerScheduler scheduler;
    const auto reader = makeReader(&scheduler);
    const auto oldLoad = beginRequest(reader.get(), true);
    const auto newLoad = beginRequest(reader.get(), true);
    const auto oldRange = mixxx::IndexRange::forward(0, 32768);
    const auto newRange = mixxx::IndexRange::forward(100, 65536);
    postStatus(reader.get(), ReaderStatusUpdate::trackLoaded(oldRange, oldLoad));
    postStatus(reader.get(), ReaderStatusUpdate::trackUnloaded(oldLoad));
    reader->process();
    expectLoading(*reader, newLoad);

    postStatus(reader.get(), ReaderStatusUpdate::trackLoaded(newRange, newLoad));
    reader->process();
    expectLoaded(*reader, newLoad, newRange);
    postStatus(reader.get(), ReaderStatusUpdate::trackLoaded(oldRange, oldLoad));
    postStatus(reader.get(), ReaderStatusUpdate::trackUnloaded(oldLoad));
    reader->process();
    expectLoaded(*reader, newLoad, newRange);
}

TEST_F(CachingReaderGenerationTest, OldChunkReturnCannotEraseSameIndexInNewGeneration) {
    EngineWorkerScheduler scheduler;
    const auto reader = makeReader(&scheduler);
    const auto initialFreeChunks = freeChunks(*reader);
    const auto range = mixxx::IndexRange::forward(0, 65536);
    const auto oldLoad = beginRequest(reader.get(), true);
    postStatus(reader.get(), ReaderStatusUpdate::trackLoaded(range, oldLoad));
    reader->process();
    auto* oldChunk = allocateChunk(reader.get(), 0);
    ASSERT_NE(nullptr, oldChunk);
    oldChunk->giveToWorker();

    const auto newLoad = beginRequest(reader.get(), true);
    postStatus(reader.get(), ReaderStatusUpdate::trackLoaded(range, newLoad));
    reader->process();
    auto* newChunk = allocateChunk(reader.get(), 0);
    ASSERT_NE(nullptr, newChunk);
    ASSERT_NE(oldChunk, newChunk);
    postStatus(reader.get(), ReaderStatusUpdate::readDiscarded(oldChunk, oldLoad));
    reader->process();

    EXPECT_EQ(newChunk, lookupChunk(reader.get(), 0));
    EXPECT_EQ(CachingReaderChunkForOwner::READY, newChunk->getState());
    EXPECT_EQ(CachingReaderChunkForOwner::FREE, oldChunk->getState());
    EXPECT_EQ(initialFreeChunks - 1, freeChunks(*reader));
    expectLoaded(*reader, newLoad, range);
}

TEST_F(CachingReaderGenerationTest, OldSuccessfulChunkCannotNarrowNewTrackRange) {
    EngineWorkerScheduler scheduler;
    const auto reader = makeReader(&scheduler);
    const auto initialFreeChunks = freeChunks(*reader);
    const auto oldRange = mixxx::IndexRange::forward(0, 8192);
    const auto newRange = mixxx::IndexRange::forward(0, 65536);
    const auto oldLoad = beginRequest(reader.get(), true);
    postStatus(reader.get(), ReaderStatusUpdate::trackLoaded(oldRange, oldLoad));
    reader->process();
    auto* oldChunk = allocateChunk(reader.get(), 0);
    ASSERT_NE(nullptr, oldChunk);
    oldChunk->giveToWorker();

    const auto newLoad = beginRequest(reader.get(), true);
    postStatus(reader.get(), ReaderStatusUpdate::trackLoaded(newRange, newLoad));
    reader->process();
    auto* newChunk = allocateChunk(reader.get(), 0);
    ASSERT_NE(nullptr, newChunk);
    ReaderStatusUpdate completed;
    completed.init(CHUNK_READ_SUCCESS, oldChunk, oldRange, oldLoad);
    postStatus(reader.get(), completed);
    reader->process();

    EXPECT_EQ(newChunk, lookupChunk(reader.get(), 0));
    EXPECT_EQ(CachingReaderChunkForOwner::FREE, oldChunk->getState());
    EXPECT_EQ(initialFreeChunks - 1, freeChunks(*reader));
    expectLoaded(*reader, newLoad, newRange);
}

TEST_F(CachingReaderGenerationTest, CacheChunkChurnDoesNotAllocateFreePoolNodes) {
    EngineWorkerScheduler scheduler;
    const auto reader = makeReader(&scheduler);
    const auto initialFreeChunks = freeChunks(*reader);
    ASSERT_TRUE(churnChunks(reader.get(), 80));
    mixxxtest::callbackAllocations = 0;
    mixxxtest::callbackDeallocations = 0;
    mixxxtest::countCallbackAllocations = true;
    const bool completed = churnChunks(reader.get(), 10000);
    mixxxtest::countCallbackAllocations = false;
    EXPECT_TRUE(completed);
    EXPECT_EQ(initialFreeChunks, freeChunks(*reader));
    EXPECT_EQ(0u, mixxxtest::callbackAllocations);
    EXPECT_EQ(0u, mixxxtest::callbackDeallocations);
}

TEST_F(CachingReaderGenerationTest, ShutdownReturnsWhenStatusQueueHasNoConsumer) {
    EngineWorkerScheduler scheduler;
    const auto reader = makeReader(&scheduler);
    fillStatusQueue(reader.get());
    requestUnload(reader.get());
    const auto dequeuedDeadline = std::chrono::steady_clock::now() + 1s;
    while (!requestDequeued(*reader) && std::chrono::steady_clock::now() < dequeuedDeadline) {
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_TRUE(requestDequeued(*reader));

    auto stopped = std::async(std::launch::async, [this, &reader] {
        stopWorker(reader.get());
    });
    const bool returnedWithoutConsumer = stopped.wait_for(1s) == std::future_status::ready;
    if (!returnedWithoutConsumer) {
        drainStatusQueue(reader.get());
    }
    EXPECT_TRUE(returnedWithoutConsumer);
    stopped.get();
}
