// SPDX-License-Identifier: BSD-3-Clause
//
// crnet_queue: completed transfers delivered to the application's own
// thread through an eventfd, so they can be multiplexed with anything
// else the application waits on.
//
#include <errno.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <chrono>

#include "crnet/internal.h"

crnet_queue::crnet_queue()
{
	efd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK | EFD_SEMAPHORE);
}

crnet_queue::~crnet_queue()
{
	if (efd_ >= 0)
		close(efd_);
}

void crnet_queue::Started(crnet_transfer *t)
{
	std::lock_guard<std::mutex> lk(mu_);
	running_.insert(base::WrapRefCounted(t));
}

// Network thread, from crnet_transfer::Finish(). Everything happens
// under the lock so that Drain() cannot return (and the queue be freed)
// while this is still touching it.
void crnet_queue::Completed(crnet_transfer *t)
{
	std::lock_guard<std::mutex> lk(mu_);
	auto it = running_.find(base::WrapRefCounted(t));
	if (it == running_.end())
		return;
	done_.push_back(*it);
	running_.erase(it);
	uint64_t one = 1;
	ssize_t r;
	do {
		r = write(efd_, &one, sizeof(one));
	} while (r < 0 && errno == EINTR);
	cv_.notify_all();
}

int crnet_queue::Pop(crnet_transfer **t, void **user_data,
		      uint64_t timeout_ms)
{
	std::unique_lock<std::mutex> lk(mu_);
	auto ready = [this] { return !done_.empty() || running_.empty(); };
	if (timeout_ms == UINT64_MAX) {
		cv_.wait(lk, ready);
	} else if (timeout_ms > 0) {
		uint64_t ms = std::min<uint64_t>(timeout_ms, 1ull << 40);
		cv_.wait_for(lk, std::chrono::milliseconds(ms), ready);
	}
	if (done_.empty())
		return running_.empty() ? CRNET_E_STATE : CRNET_E_TIMEOUT;

	scoped_refptr<crnet_transfer> tr = std::move(done_.front());
	done_.pop_front();
	lk.unlock();

	// Consume one eventfd count (semaphore mode) to keep the fd level
	// in sync with done_.
	uint64_t v;
	ssize_t r;
	do {
		r = read(efd_, &v, sizeof(v));
	} while (r < 0 && errno == EINTR);

	if (user_data)
		*user_data = tr->user_data();
	// Hand our reference to the caller as their handle.
	*t = tr.release();
	return CRNET_OK;
}

size_t crnet_queue::pending() const
{
	std::lock_guard<std::mutex> lk(mu_);
	return running_.size() + done_.size();
}

void crnet_queue::Drain()
{
	std::unique_lock<std::mutex> lk(mu_);
	for (const auto &t : running_)
		t->Cancel(CRNET_E_CANCELED);
	cv_.wait(lk, [this] { return running_.empty(); });
	done_.clear();
}
