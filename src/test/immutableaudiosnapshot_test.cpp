#include "util/immutableaudiosnapshot.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <memory>
#include <thread>
#include <vector>

namespace {

struct SnapshotValue {
    explicit SnapshotValue(int number)
            : number(number), check(number ^ 0x234567) {
    }

    const int number;
    const int check;
};

TEST(ImmutableAudioSnapshotTest, HeldGenerationsSurvivePublicationAndSlotExhaustion) {
    mixxx::ImmutableAudioSnapshot<SnapshotValue, 4> snapshot;
    std::array<decltype(snapshot)::ReadGuard, 4> readers;
    std::array<std::weak_ptr<const SnapshotValue>, 4> weakValues;
    for (int i = 0; i < 4; ++i) {
        auto value = std::make_shared<const SnapshotValue>(i);
        weakValues[i] = value;
        snapshot.publish(std::move(value));
        readers[i] = snapshot.acquire();
        ASSERT_TRUE(readers[i].acquired());
    }
    EXPECT_FALSE(snapshot.acquire().acquired());
    for (int i = 4; i < 1000; ++i) {
        snapshot.publish(std::make_shared<const SnapshotValue>(i));
    }
    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(readers[i]->number, i);
        EXPECT_FALSE(weakValues[i].expired());
    }
    readers[0] = {};
    EXPECT_FALSE(weakValues[0].expired());
    auto current = snapshot.acquire();
    ASSERT_TRUE(current);
    EXPECT_EQ(current->number, 999);
    snapshot.publish(nullptr);
    EXPECT_TRUE(weakValues[0].expired());
    EXPECT_EQ(current->number, 999);
    current = {};
    snapshot.publish(nullptr);
    auto empty = snapshot.acquire();
    ASSERT_TRUE(empty.acquired());
    EXPECT_FALSE(empty);
}

TEST(ImmutableAudioSnapshotTest, ReleasingReaderNeverDestroysRetiredValue) {
    std::atomic<int> destructionCount{0};
    const auto publisherThread = std::this_thread::get_id();
    std::atomic<bool> wrongDestructorThread{false};
    struct TrackedValue {
        TrackedValue(std::atomic<int>* pCount,
                std::atomic<bool>* pWrongThread,
                std::thread::id publisher)
                : pCount(pCount), pWrongThread(pWrongThread), publisher(publisher) {
        }
        ~TrackedValue() {
            if (std::this_thread::get_id() != publisher) {
                pWrongThread->store(true);
            }
            pCount->fetch_add(1);
        }
        std::atomic<int>* pCount;
        std::atomic<bool>* pWrongThread;
        std::thread::id publisher;
    };
    mixxx::ImmutableAudioSnapshot<TrackedValue> snapshot;
    snapshot.publish(std::make_shared<const TrackedValue>(
            &destructionCount, &wrongDestructorThread, publisherThread));
    auto reader = snapshot.acquire();
    snapshot.publish(nullptr);
    std::thread readerThread([guard = std::move(reader)]() {});
    readerThread.join();
    EXPECT_EQ(destructionCount.load(), 0);
    snapshot.publish(nullptr);
    EXPECT_EQ(destructionCount.load(), 1);
    EXPECT_FALSE(wrongDestructorThread.load());
}

TEST(ImmutableAudioSnapshotTest, ConcurrentPublishersAndReadersSeeImmutableValues) {
    mixxx::ImmutableAudioSnapshot<SnapshotValue> snapshot;
    snapshot.publish(std::make_shared<const SnapshotValue>(0));
    std::atomic<bool> start{false};
    std::atomic<int> activePublishers{2};
    std::atomic<int> errors{0};
    std::atomic<int> reads{0};
    std::vector<std::thread> readers;
    for (int i = 0; i < 8; ++i) {
        readers.emplace_back([&]() {
            while (!start.load()) {
                std::this_thread::yield();
            }
            do {
                const auto value = snapshot.acquire();
                if (value) {
                    const int number = value->number;
                    std::this_thread::yield();
                    if (value->check != (number ^ 0x234567)) {
                        errors.fetch_add(1);
                    }
                    reads.fetch_add(1);
                }
            } while (activePublishers.load() != 0);
        });
    }
    const auto publish = [&](int offset) {
        while (!start.load()) {
            std::this_thread::yield();
        }
        for (int i = 0; i < 10000; ++i) {
            snapshot.publish(std::make_shared<const SnapshotValue>(offset + i));
        }
        activePublishers.fetch_sub(1);
    };
    std::thread firstPublisher(publish, 0);
    std::thread secondPublisher(publish, 10000);
    start.store(true);
    firstPublisher.join();
    secondPublisher.join();
    for (auto& reader : readers) {
        reader.join();
    }
    EXPECT_EQ(errors.load(), 0);
    EXPECT_GT(reads.load(), 0);
}

}
