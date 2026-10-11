// SPDX-License-Identifier: BSD-3-Clause
#include "crnet/fd_flag.h"

#include <errno.h>
#include <stdint.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace crnet {

FdFlag::FdFlag() : fd_(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) {}

FdFlag::~FdFlag()
{
	if (fd_ >= 0)
		close(fd_);
}

void FdFlag::Set(bool readable)
{
	if (readable == readable_ || fd_ < 0)
		return;
	uint64_t v = 1;
	ssize_t r;
	do {
		r = readable ? write(fd_, &v, sizeof(v))
			     : read(fd_, &v, sizeof(v));
	} while (r < 0 && errno == EINTR);
	readable_ = readable;
}

} // namespace crnet
