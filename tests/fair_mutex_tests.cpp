/*
 * Semantics of the host mutex backing sys_lwmutex (runtime/ppu/ppu_fair_mutex.h).
 *
 * Covers the three properties the guest depends on and that are easy to get
 * wrong: recursive re-entry by the owner, FIFO fairness under contention, and
 * cross-thread release -- which lv2 permits and which used to be silently
 * ignored, leaving the mutex held forever.
 */
#include "ppu_fair_mutex.h"

#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

static int g_failures;
static void check(bool ok, const char* what)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++g_failures; }
}

/* A thread that owns the mutex recursively still holds it until the last
 * unlock; another thread must not acquire in between. */
static void test_recursive_ownership()
{
    FairRecursiveTimedMutex m;
    m.lock(1, 0);
    m.lock(1, 0);
    check(!m.try_lock(2, 0), "recursive: other thread must not acquire");
    m.unlock(1);
    check(!m.try_lock(2, 0), "recursive: still held at depth 1");
    m.unlock(1);
    check(m.try_lock(2, 0), "recursive: released at depth 0");
    m.unlock(2);
}

/* lv2 lets a thread release an lwmutex it does not own. Before this was
 * supported the unlock was dropped and every waiter hung forever. */
static void test_cross_thread_release()
{
    FairRecursiveTimedMutex m;
    m.lock(1, 0);
    m.unlock(2);                                   /* thread 2 releases thread 1's lock */
    check(m.try_lock(3, 0), "cross-thread release must free the mutex");
    m.unlock(3);
}

/* An acquire that times out runs WITHOUT the lock, and the guest still calls
 * unlock for it. That unlock must not release somebody else's lock. */
static void test_timed_out_acquire_does_not_release_owner()
{
    FairRecursiveTimedMutex m;
    m.lock(1, 0);
    check(!m.try_lock_for(2, 0, std::chrono::milliseconds(5)),
          "contended timed acquire should time out");
    m.unlock(2);                                   /* balances thread 2's failed acquire */
    check(!m.try_lock(3, 0), "timed-out unlock must not release the owner");
    m.unlock(1);
    check(m.try_lock(3, 0), "owner's own unlock still works");
    m.unlock(3);
}

/* Contended acquisitions are served in arrival order; a busy thread must not
 * barge ahead of an older waiter (the starvation this class exists to fix). */
static void test_fifo_fairness()
{
    FairRecursiveTimedMutex m;
    std::atomic<int> next{0};
    std::vector<int> order;
    std::mutex order_mtx;

    m.lock(100, 0);
    std::vector<std::thread> ts;
    for (int i = 0; i < 8; ++i) {
        ts.emplace_back([&, i] {
            while (next.load() != i) std::this_thread::yield();
            ++next;                                  /* enqueue in a known order */
            m.lock((uint32_t)(200 + i), 0);
            { std::lock_guard<std::mutex> g(order_mtx); order.push_back(i); }
            m.unlock((uint32_t)(200 + i));
        });
        while (next.load() != i + 1) std::this_thread::yield();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    m.unlock(100);
    for (auto& t : ts) t.join();

    check(order.size() == 8, "fifo: every waiter ran");
    for (size_t i = 0; i < order.size(); ++i)
        check(order[i] == (int)i, "fifo: waiters served in arrival order");
}

int main()
{
    test_recursive_ownership();
    test_cross_thread_release();
    test_timed_out_acquire_does_not_release_owner();
    test_fifo_fairness();
    if (g_failures == 0) std::printf("fair_mutex_tests: ok\n");
    return g_failures != 0;
}
