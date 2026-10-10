// SPDX-License-Identifier: BSD-3-Clause
//
// OneShotEvent: signalled once, waited for by any number of threads.
//
// Completion of a transfer or a WebSocket is published through one of
// these instead of a mutex and condition variable. It is a futex on a
// single word: checking it is one atomic load, signalling is a store
// plus a wake-up only when a thread is actually asleep on it.
//
#ifndef CRNET_SRC_EVENT_H
#define CRNET_SRC_EVENT_H

#include <stdint.h>

#include <atomic>

namespace crnet {

class OneShotEvent {
public:
	OneShotEvent() = default;
	OneShotEvent(const OneShotEvent &) = delete;
	OneShotEvent &operator=(const OneShotEvent &) = delete;

	// Everything written before Signal() is visible to a thread that
	// sees IsSignaled() or returns true from Wait().
	bool IsSignaled() const
	{
		return state_.load(std::memory_order_acquire) != 0;
	}

	// Idempotent.
	void Signal();

	// Waits up to |timeout_ms| (0 = just check, UINT64_MAX = forever).
	// Returns whether the event is signalled.
	bool Wait(uint64_t timeout_ms) const;

private:
	mutable std::atomic<uint32_t> state_{0};
	mutable std::atomic<uint32_t> waiters_{0};
};

} // namespace crnet

#endif // CRNET_SRC_EVENT_H
