#include "engine/engineworkerscheduler.h"

#include <gtest/gtest.h>
#include <memory>

#include "engine/engineworker.h"
#include "test/mixxxtest.h"

class EngineWorkerSchedulerTest : public MixxxTest {
  protected:
    void consumeWake(EngineWorkerScheduler* scheduler) {
        ASSERT_TRUE(scheduler->m_wakeSemaphore.tryAcquire());
        scheduler->m_wakePending.store(false);
    }
    int pendingWakes(const EngineWorkerScheduler& scheduler) const {
        return scheduler.m_wakeSemaphore.available();
    }
};

TEST_F(EngineWorkerSchedulerTest, WakeBeforeWaitIsRetained) {
    EngineWorkerScheduler scheduler;
    consumeWake(&scheduler);
    scheduler.workerReady();
    scheduler.runWorkers();
    EXPECT_EQ(1, pendingWakes(scheduler));
    for (int i = 0; i < 100; ++i) {
        scheduler.runWorkers();
    }
    EXPECT_EQ(1, pendingWakes(scheduler));
    consumeWake(&scheduler);
    EXPECT_EQ(0, pendingWakes(scheduler));
}

TEST_F(EngineWorkerSchedulerTest, RepeatedNotificationsCoalesceUntilWakeConsumed) {
    EngineWorkerScheduler scheduler;
    consumeWake(&scheduler);
    for (int i = 0; i < 100; ++i) {
        scheduler.workerReady();
        scheduler.runWorkers();
    }
    EXPECT_EQ(1, pendingWakes(scheduler));
    consumeWake(&scheduler);
    scheduler.workerReady();
    scheduler.runWorkers();
    EXPECT_EQ(1, pendingWakes(scheduler));
}

TEST_F(EngineWorkerSchedulerTest, ReadyWorkersResumeWithoutUnrelatedWork) {
    class Worker : public EngineWorker {
      public:
        bool takeWake() {
            return m_semaRun.tryAcquire(1, 1000);
        }
    };
    Worker first;
    Worker second;
    EngineWorkerScheduler scheduler;
    first.setScheduler(&scheduler);
    second.setScheduler(&scheduler);
    scheduler.start();
    for (int i = 0; i < 100; ++i) {
        first.workReady();
        second.workReady();
        scheduler.runWorkers();
        ASSERT_TRUE(first.takeWake());
        ASSERT_TRUE(second.takeWake());
    }
}

TEST_F(EngineWorkerSchedulerTest, StopBeforeRegisteredWorkerDestruction) {
    class Worker : public EngineWorker {
      public:
        bool takeWake() {
            return m_semaRun.tryAcquire(1, 1000);
        }
        int pendingWakes() const {
            return m_semaRun.available();
        }
    };
    EngineWorkerScheduler scheduler;
    auto worker = std::make_unique<Worker>();
    worker->setScheduler(&scheduler);
    scheduler.start();
    worker->workReady();
    scheduler.runWorkers();
    ASSERT_TRUE(worker->takeWake());

    scheduler.stopAndWait();
    ASSERT_FALSE(scheduler.isRunning());
    worker->workReady();
    scheduler.runWorkers();
    scheduler.stopAndWait();
    EXPECT_EQ(0, worker->pendingWakes());

    worker.reset();
    scheduler.workerReady();
    scheduler.runWorkers();
    scheduler.stopAndWait();
    EXPECT_FALSE(scheduler.isRunning());
}

TEST_F(EngineWorkerSchedulerTest, StopWithoutStartingIsIdempotent) {
    EngineWorkerScheduler scheduler;
    scheduler.stopAndWait();
    const int wakeCount = pendingWakes(scheduler);
    scheduler.stopAndWait();
    EXPECT_EQ(wakeCount, pendingWakes(scheduler));
    EXPECT_FALSE(scheduler.isRunning());
}
