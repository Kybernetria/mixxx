#include "util/boundedatomicvalue.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <thread>

TEST(BoundedAtomicValueTest, ConcurrentSnapshotsAreCoherentOrRejected) {
    struct Value {
        std::uint64_t value;
        std::uint64_t complement;
        std::uint64_t multiple;
    };
    BoundedAtomicValue<Value> mailbox(Value{0, ~std::uint64_t(0), 0});
    std::atomic<bool> done{false};
    std::atomic<int> inconsistent{0};
    std::atomic<int> successes{0};
    std::thread writer([&] {
        for (std::uint64_t i = 1; i <= 100000; ++i)
            mailbox.setValue(Value{i, ~i, i * 3});
        done.store(true);
    });
    std::array<std::thread, 6> readers;
    for (auto& reader : readers) {
        reader = std::thread([&] {
            do {
                Value value;
                if (mailbox.tryGetValue(&value)) {
                    ++successes;
                    if (value.complement != ~value.value || value.multiple != value.value * 3)
                        ++inconsistent;
                }
            } while (!done.load());
        });
    }
    writer.join();
    for (auto& reader : readers)
        reader.join();
    EXPECT_EQ(0, inconsistent.load());
    EXPECT_GT(successes.load(), 0);
    Value final;
    ASSERT_TRUE(mailbox.tryGetValue(&final));
    EXPECT_EQ(100000, final.value);
}
