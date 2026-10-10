// SPDX-License-Identifier: BSD-3-Clause
#include "crnet/event.h"

#include <limits.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

namespace crnet {
namespace {

static_assert(sizeof(std::atomic<uint32_t>) == sizeof(uint32_t),
	      "the futex word is the atomic itself");

long Futex(std::atomic<uint32_t> *word, int op, uint32_t val,
	   const struct timespec *rel)
{
	return syscall(SYS_futex, reinterpret_cast<uint32_t *>(word),
		       op | FUTEX_PRIVATE_FLAG, val, rel, nullptr, 0);
}

struct timespec Now()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts;
}

} // namespace

// The waiter count and the state are both accessed sequentially
// consistently: either Signal() sees a waiter and wakes it, or the
// waiter, having registered first, sees the signalled state and never
// sleeps.
void OneShotEvent::Signal()
{
	state_.store(1, std::memory_order_seq_cst);
	if (waiters_.load(std::memory_order_seq_cst))
		Futex(&state_, FUTEX_WAKE, INT_MAX, nullptr);
}

bool OneShotEvent::Wait(uint64_t timeout_ms) const
{
	if (IsSignaled())
		return true;
	if (timeout_ms == 0)
		return false;

	const bool forever = timeout_ms == UINT64_MAX;
	struct timespec deadline = Now();
	if (!forever) {
		// Clamp absurd timeouts (~35 years) to keep the maths in range.
		uint64_t ms = timeout_ms < (1ull << 40) ? timeout_ms : (1ull << 40);
		deadline.tv_sec += ms / 1000;
		deadline.tv_nsec += (ms % 1000) * 1000000;
		if (deadline.tv_nsec >= 1000000000) {
			deadline.tv_sec++;
			deadline.tv_nsec -= 1000000000;
		}
	}

	waiters_.fetch_add(1, std::memory_order_seq_cst);
	while (state_.load(std::memory_order_seq_cst) == 0) {
		struct timespec rel, *prel = nullptr;
		if (!forever) {
			struct timespec now = Now();
			rel.tv_sec = deadline.tv_sec - now.tv_sec;
			rel.tv_nsec = deadline.tv_nsec - now.tv_nsec;
			if (rel.tv_nsec < 0) {
				rel.tv_sec--;
				rel.tv_nsec += 1000000000;
			}
			if (rel.tv_sec < 0)
				break;
			prel = &rel;
		}
		// Returns on a wake-up, a changed value (EAGAIN), a signal
		// (EINTR) or the timeout; the loop re-checks in every case.
		Futex(&state_, FUTEX_WAIT, 0, prel);
	}
	waiters_.fetch_sub(1, std::memory_order_seq_cst);
	return IsSignaled();
}

} // namespace crnet
