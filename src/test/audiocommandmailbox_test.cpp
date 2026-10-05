#include "util/audiocommandmailbox.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <thread>

namespace mixxx {

class AudioCommandMailboxTest : public testing::Test {
  protected:
    struct Command {
        std::uint64_t position;
        std::uint64_t complement;
    };

    using Mailbox = AudioCommandMailbox<Command>;

    static Command command(std::uint64_t position) {
        return {position, ~position};
    }

    static std::unique_lock<std::mutex> lockNonAudioWriter(Mailbox* mailbox) {
        return std::unique_lock<std::mutex>(mailbox->m_nonAudioWriterMutex);
    }

    static std::uint64_t reserveTicket(Mailbox* mailbox) {
        return mailbox->m_newestAssignedTicket.fetch_add(1) + 1;
    }

    static void finishNonAudioPublication(Mailbox* mailbox,
            std::uint64_t ticket,
            Command value) {
        mailbox->m_nonAudioLane.setValue(Mailbox::Snapshot{value, ticket});
    }
};

TEST_F(AudioCommandMailboxTest, ConsumingCapturedCommandPreservesNewerPublication) {
    Mailbox mailbox(command(0));
    mailbox.publish(command(100));
    Mailbox::Snapshot first{};
    ASSERT_TRUE(mailbox.tryGetPending(&first));

    const AudioCallbackScope scope;
    mailbox.publish(command(200));
    mailbox.consume(first);

    Mailbox::Snapshot second{};
    ASSERT_TRUE(mailbox.tryGetPending(&second));
    EXPECT_EQ(200, second.value.position);
    EXPECT_GT(second.ticket, first.ticket);
    mailbox.consume(second);
    EXPECT_FALSE(mailbox.tryGetPending(&second));
}

TEST_F(AudioCommandMailboxTest, DelayedOlderPublisherCannotReplaceNewerAudioCommand) {
    Mailbox mailbox(command(0));
    const auto lock = lockNonAudioWriter(&mailbox);
    const auto olderTicket = reserveTicket(&mailbox);
    Mailbox::Snapshot pending{};
    EXPECT_TRUE(mailbox.hasPending());
    EXPECT_FALSE(mailbox.tryGetPending(&pending));

    const AudioCallbackScope scope;
    mailbox.publish(command(200));
    ASSERT_TRUE(mailbox.tryGetPending(&pending));
    EXPECT_EQ(200, pending.value.position);
    finishNonAudioPublication(&mailbox, olderTicket, command(100));
    ASSERT_TRUE(mailbox.tryGetPending(&pending));
    EXPECT_EQ(200, pending.value.position);
    mailbox.consume(pending);
    EXPECT_FALSE(mailbox.hasPending());
    EXPECT_FALSE(mailbox.tryGetPending(&pending));
}

TEST_F(AudioCommandMailboxTest, NewestUnpublishedTicketDefersAnOlderCommand) {
    Mailbox mailbox(command(0));
    const AudioCallbackScope scope;
    mailbox.publish(command(100));
    const auto lock = lockNonAudioWriter(&mailbox);
    const auto newerTicket = reserveTicket(&mailbox);
    Mailbox::Snapshot pending{};
    EXPECT_FALSE(mailbox.tryGetPending(&pending));
    finishNonAudioPublication(&mailbox, newerTicket, command(200));
    ASSERT_TRUE(mailbox.tryGetPending(&pending));
    EXPECT_EQ(200, pending.value.position);
}

TEST_F(AudioCommandMailboxTest, AudioPublisherDoesNotWaitForNonAudioWriterMutex) {
    Mailbox mailbox(command(0));
    auto lock = lockNonAudioWriter(&mailbox);
    auto result = std::async(std::launch::async, [&] {
        const AudioCallbackScope scope;
        mailbox.publish(command(100));
    });
    const auto completed = result.wait_for(std::chrono::seconds(1));
    lock.unlock();
    result.get();
    EXPECT_EQ(std::future_status::ready, completed);
    Mailbox::Snapshot pending{};
    ASSERT_TRUE(mailbox.tryGetPending(&pending));
    EXPECT_EQ(100, pending.value.position);
}

TEST_F(AudioCommandMailboxTest, MultipleNonAudioWritersAndAudioWriterRemainCoherent) {
    Mailbox mailbox(command(0));
    std::array<std::thread, 4> writers;
    std::atomic<int> finishedWriters{0};
    for (std::size_t writerIndex = 0; writerIndex < writers.size(); ++writerIndex) {
        writers[writerIndex] = std::thread([&, writerIndex] {
            for (std::uint64_t i = 1; i <= 10000; ++i) {
                mailbox.publish(command(writerIndex * 10000 + i));
            }
            ++finishedWriters;
        });
    }
    const AudioCallbackScope scope;
    std::uint64_t lastConsumed = 0;
    std::uint64_t audioPosition = 50000;
    int inconsistentReads = 0;
    do {
        mailbox.publish(command(++audioPosition));
        Mailbox::Snapshot pending{};
        if (mailbox.tryGetPending(&pending)) {
            if (pending.value.complement != ~pending.value.position ||
                    pending.ticket <= lastConsumed) {
                ++inconsistentReads;
            }
            lastConsumed = pending.ticket;
            mailbox.consume(pending);
        }
    } while (finishedWriters.load() != static_cast<int>(writers.size()));
    for (auto& writer : writers) {
        writer.join();
    }
    EXPECT_EQ(0, inconsistentReads);
    mailbox.publish(command(99999));
    Mailbox::Snapshot pending{};
    ASSERT_TRUE(mailbox.tryGetPending(&pending));
    EXPECT_EQ(99999, pending.value.position);
}

}
