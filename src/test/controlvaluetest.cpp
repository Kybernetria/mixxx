#include <QObject>

#include "control/controlvalue.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <future>
#include <thread>

namespace {

struct AssignmentGate {
    explicit AssignmentGate(int blockedValue)
            : blockedValue(blockedValue), release(releasePromise.get_future().share()) {
    }

    const int blockedValue;
    std::promise<void> entered;
    std::promise<void> releasePromise;
    std::shared_future<void> release;
};

struct PausableValue {
    explicit PausableValue(int value = 0, AssignmentGate* pGate = nullptr)
            : value(value), pGate(pGate) {
    }

    PausableValue& operator=(const PausableValue& other) {
        const auto newValue = other.value.load();
        auto* gate = pGate ? pGate : other.pGate;
        if (gate && newValue == gate->blockedValue) {
            gate->entered.set_value();
            gate->release.wait();
        }
        value.store(newValue);
        return *this;
    }

    std::atomic<int> value;
    AssignmentGate* const pGate;
};

TEST(ControlRingValueTest, WriterExcludesConcurrentReadersAndOtherWriters) {
    ControlRingValue<PausableValue> slot;
    ASSERT_TRUE(slot.trySet(PausableValue(10)));
    AssignmentGate gate(100);
    std::atomic<bool> writerSucceeded{false};
    std::thread writer([&] {
        writerSucceeded.store(slot.trySet(PausableValue(100, &gate)));
    });
    gate.entered.get_future().wait();

    std::atomic<int> admittedReaders{0};
    std::atomic<int> admittedWriters{0};
    std::array<std::thread, 8> contenders;
    for (std::size_t i = 0; i < contenders.size(); ++i) {
        contenders[i] = std::thread([&, i] {
            for (int attempt = 0; attempt < 10000; ++attempt) {
                if (i < 6) {
                    PausableValue value;
                    if (slot.tryGet(&value)) {
                        ++admittedReaders;
                    }
                } else if (slot.trySet(PausableValue(200))) {
                    ++admittedWriters;
                }
            }
        });
    }
    for (auto& contender : contenders) {
        contender.join();
    }
    gate.releasePromise.set_value();
    writer.join();

    EXPECT_TRUE(writerSucceeded.load());
    EXPECT_EQ(0, admittedReaders.load());
    EXPECT_EQ(0, admittedWriters.load());
    PausableValue value;
    ASSERT_TRUE(slot.tryGet(&value));
    EXPECT_EQ(100, value.value.load());
    ASSERT_TRUE(slot.trySet(PausableValue(300)));
    ASSERT_TRUE(slot.tryGet(&value));
    EXPECT_EQ(300, value.value.load());
}

TEST(ControlRingValueTest, ReaderExcludesWritersUntilItsCopyCompletes) {
    ControlRingValue<PausableValue> slot;
    ASSERT_TRUE(slot.trySet(PausableValue(10)));
    AssignmentGate gate(10);
    PausableValue value(0, &gate);
    std::atomic<bool> readerSucceeded{false};
    std::thread reader([&] {
        readerSucceeded.store(slot.tryGet(&value));
    });
    gate.entered.get_future().wait();

    const bool writerWasAdmitted = slot.trySet(PausableValue(20));
    gate.releasePromise.set_value();
    reader.join();

    EXPECT_FALSE(writerWasAdmitted);
    EXPECT_TRUE(readerSucceeded.load());
    EXPECT_EQ(10, value.value.load());
    ASSERT_TRUE(slot.trySet(PausableValue(20)));
    PausableValue latest;
    ASSERT_TRUE(slot.tryGet(&latest));
    EXPECT_EQ(20, latest.value.load());
}

TEST(ControlValueAtomicTest, ConcurrentMultiwordSnapshotsRemainCoherent) {
    struct Value {
        std::uint64_t number;
        std::uint64_t complement;
        std::uint64_t doubled;
        std::uint64_t writer;
    };
    ControlValueAtomic<Value> value(Value{0, ~std::uint64_t(0), 0, 0});
    std::atomic<int> finishedWriters{0};
    std::atomic<int> inconsistentReads{0};
    std::array<std::thread, 2> writers;
    std::array<std::thread, 4> readers;
    for (std::size_t i = 0; i < writers.size(); ++i) {
        writers[i] = std::thread([&, i] {
            for (std::uint64_t number = 1; number <= 10000; ++number) {
                value.setValue(Value{number, ~number, number * 2, i});
            }
            ++finishedWriters;
        });
    }
    for (auto& reader : readers) {
        reader = std::thread([&] {
            do {
                const auto snapshot = value.getValue();
                if (snapshot.complement != ~snapshot.number ||
                        snapshot.doubled != snapshot.number * 2 || snapshot.writer > 1) {
                    ++inconsistentReads;
                }
            } while (finishedWriters.load() != static_cast<int>(writers.size()));
        });
    }
    for (auto& writer : writers) {
        writer.join();
    }
    for (auto& reader : readers) {
        reader.join();
    }
    EXPECT_EQ(0, inconsistentReads.load());
}

}
