/*
 * ps3recomp - runtime/ppu/ppu_fair_mutex.h
 *
 * Host mutex backing the guest's sys_lwmutex.
 *
 * std::recursive_timed_mutex permits barging: a busy thread can release and
 * immediately reacquire while an older waiter is still waking.  That is fatal
 * for Taiko's allocator lock -- service threads perform runs of small
 * allocations and a starved thread crawls forward one allocation per timeout.
 * So contended first-level acquisitions queue in arrival order (FIFO), while
 * recursive re-entry by the owner stays free.
 *
 * Extracted from ppu_sysprx.cpp so the semantics can be unit-tested; see
 * tests/fair_mutex_tests.cpp.
 */
#ifndef PS3RECOMP_PPU_FAIR_MUTEX_H
#define PS3RECOMP_PPU_FAIR_MUTEX_H

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>

class FairRecursiveTimedMutex {
public:
    void lock(uint32_t self, uint32_t lr)
    {
        std::unique_lock<std::mutex> lock(gate_);
        if (owner_ == self) {
            ++depth_;
            return;
        }
        if (owner_ == 0 && waiters_.empty()) {
            owner_ = self;
            depth_ = 1;
            owner_lr_ = lr;
            return;
        }

        const uint64_t ticket = next_ticket_++;
        waiters_.push_back(ticket);
        cv_.wait(lock, [&] {
            return owner_ == 0 &&
                   !waiters_.empty() && waiters_.front() == ticket;
        });
        waiters_.pop_front();
        owner_ = self;
        depth_ = 1;
        owner_lr_ = lr;
    }

    bool try_lock(uint32_t self, uint32_t lr)
    {
        std::lock_guard<std::mutex> lock(gate_);
        if (owner_ == self) {
            ++depth_;
            return true;
        }
        if (owner_ == 0 && waiters_.empty()) {
            owner_ = self;
            depth_ = 1;
            owner_lr_ = lr;
            return true;
        }
        return false;
    }

    template<class Rep, class Period>
    bool try_lock_for(uint32_t self, uint32_t lr,
                      const std::chrono::duration<Rep, Period>& timeout)
    {
        std::unique_lock<std::mutex> lock(gate_);
        if (owner_ == self) {
            ++depth_;
            return true;
        }
        if (owner_ == 0 && waiters_.empty()) {
            owner_ = self;
            depth_ = 1;
            owner_lr_ = lr;
            return true;
        }

        const uint64_t ticket = next_ticket_++;
        waiters_.push_back(ticket);
        const auto ready = [&] {
            return owner_ == 0 &&
                   !waiters_.empty() && waiters_.front() == ticket;
        };
        if (!cv_.wait_for(lock, timeout, ready)) {
            const auto it = std::find(waiters_.begin(), waiters_.end(), ticket);
            const bool was_front = it != waiters_.end() && it == waiters_.begin();
            if (it != waiters_.end()) waiters_.erase(it);
            if (was_front && owner_ == 0) cv_.notify_all();
            /* The caller proceeds WITHOUT the lock, and the guest will still
             * call unlock for it.  Remember that so the matching unlock is a
             * no-op instead of being mistaken for a cross-thread release. */
            ++timed_out_[self];
            return false;
        }

        waiters_.pop_front();
        owner_ = self;
        depth_ = 1;
        owner_lr_ = lr;
        return true;
    }

    /* lv2 permits a thread that does not own an lwmutex to release it, and
     * the guest's own recursion counter in sys_lwmutex_unlock decrements
     * regardless of caller.  Refusing that here left the mutex held forever
     * and hung every waiter -- a failure that looks random and is close to
     * impossible to attribute.  Mirror the guest instead. */
    void unlock(uint32_t self)
    {
        std::unique_lock<std::mutex> lock(gate_);
        if (owner_ == self && depth_ != 0) {
            if (--depth_ != 0) return;
        } else {
            /* Balance an acquire that timed out and ran unlocked; that thread
             * never held the mutex, so its unlock must not touch the owner. */
            const auto it = timed_out_.find(self);
            if (it != timed_out_.end()) {
                if (--it->second == 0) timed_out_.erase(it);
                return;
            }
            if (depth_ == 0 || owner_ == 0) return;
            if (--depth_ != 0) return;      /* genuine cross-thread release */
        }
        owner_ = 0;
        owner_lr_ = 0;
        lock.unlock();
        cv_.notify_all();
    }

    /* Condition waits must drop every recursive level atomically.  Keeping
     * this beside owner_/depth_ avoids a second per-thread ownership table. */
    int release_all(uint32_t self)
    {
        std::unique_lock<std::mutex> lock(gate_);
        if (owner_ != self || depth_ == 0) return 0;
        const int depth = (int)depth_;
        owner_ = 0;
        depth_ = 0;
        owner_lr_ = 0;
        lock.unlock();
        cv_.notify_all();
        return depth;
    }

    void owner_snapshot(uint32_t& tid, uint32_t& lr)
    {
        std::lock_guard<std::mutex> lock(gate_);
        tid = owner_;
        lr = owner_lr_;
    }

private:
    std::mutex gate_;
    std::condition_variable cv_;
    uint32_t owner_ = 0;
    uint32_t depth_ = 0;
    uint32_t owner_lr_ = 0;
    uint64_t next_ticket_ = 0;
    std::deque<uint64_t> waiters_;
    /* guest tid -> outstanding acquires that timed out and ran unlocked */
    std::map<uint32_t, uint32_t> timed_out_;
};

#endif /* PS3RECOMP_PPU_FAIR_MUTEX_H */
