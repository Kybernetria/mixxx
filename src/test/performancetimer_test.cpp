#include <gtest/gtest.h>
#include <ctime>

#include "util/performancetimer.h"

TEST(PerformanceTimerTest, StoppedTimerElapsedIsZero) {
    PerformanceTimer timer;
    EXPECT_FALSE(timer.running());
    EXPECT_EQ(0, timer.elapsed().toIntegerNanos());
    EXPECT_FALSE(timer.running());
}

TEST(PerformanceTimerTest, RestartStartsStoppedTimer) {
    PerformanceTimer timer;
    EXPECT_EQ(0, timer.restart().toIntegerNanos());
    EXPECT_TRUE(timer.running());
    EXPECT_GE(timer.elapsed().toIntegerNanos(), 0);
    EXPECT_GE(timer.restart().toIntegerNanos(), 0);
}

TEST(PerformanceTimerTest, DifferenceWithStoppedTimerIsZero) {
    PerformanceTimer stopped;
    PerformanceTimer running;
    running.start();
    EXPECT_EQ(0, stopped.difference(running).toIntegerNanos());
    EXPECT_EQ(0, running.difference(stopped).toIntegerNanos());
    EXPECT_EQ(0, stopped.difference(stopped).toIntegerNanos());
}

// This test was added because of an signed/unsigned underflow bug that
// affected Windows and (presumably) Symbian.
// See https://github.com/mixxxdj/mixxx/issues/7397
TEST(PerformanceTimerTest, DifferenceCanBeNegative) {
    PerformanceTimer early;
    early.start();

    std::time_t start = time(0);
    while (1) {
        // use the standard clock to make sure we don't run forever
        double seconds = difftime(time(0), start);

        PerformanceTimer late;
        late.start();
        mixxx::Duration difference = early.difference(late);

        // If the high-res clock hasn't ticked yet, the difference should be 0.
        // If it has ticked, then the difference better be negative.
        ASSERT_LE(difference.toIntegerNanos(), 0);

        if (difference < mixxx::Duration::fromNanos(0)) {
            break; // test passed
        }

        if (seconds > 0.9) {
            // The standard clock ticked, but difference is still zero.
            // Assume that there is no high-resolution clock on this system.
            ASSERT_EQ(0, difference.toIntegerNanos());

            // It would be nice if gtest printed this, but it currently doesn't.
            // https://code.google.com/p/googletest/wiki/AdvancedGuide
            SUCCEED() << "inconclusive: no high-resolution timer on this system?";
            break;
        }
    }
}
