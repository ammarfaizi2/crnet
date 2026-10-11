// SPDX-License-Identifier: BSD-3-Clause
//
// FdFlag: a boolean exposed as a pollable file descriptor.
//
// The fds crnet hands out for poll()/epoll (a streamed download, a
// WebSocket's receive queue, a streamed upload) are level-triggered:
// readable exactly while an operation would not block. FdFlag keeps an
// eventfd in that state, touching it only when the state flips.
//
#ifndef CRNET_SRC_FD_FLAG_H
#define CRNET_SRC_FD_FLAG_H

namespace crnet {

class FdFlag {
public:
	// Creates the eventfd, initially not readable; fd() is -1 if that
	// failed.
	FdFlag();
	~FdFlag();

	FdFlag(const FdFlag &) = delete;
	FdFlag &operator=(const FdFlag &) = delete;

	int fd() const { return fd_; }

	// Not thread-safe: the owner calls it under its own lock.
	void Set(bool readable);

private:
	int fd_;
	bool readable_ = false;
};

} // namespace crnet

#endif // CRNET_SRC_FD_FLAG_H
