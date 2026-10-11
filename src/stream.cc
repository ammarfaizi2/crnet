// SPDX-License-Identifier: BSD-3-Clause
#include "crnet/stream.h"

#include <string.h>

#include <algorithm>
#include <chrono>

#include "base/compiler_specific.h"
#include "base/functional/bind.h"
#include "crnet/crnet.h"
#include "net/base/net_errors.h"

namespace crnet {
namespace {

using Clock = std::chrono::steady_clock;

Clock::time_point Deadline(uint64_t timeout_ms)
{
	uint64_t ms = std::min<uint64_t>(timeout_ms, 1ull << 40);
	return Clock::now() + std::chrono::milliseconds(ms);
}

// Copies up to |len| bytes out of a chunk queue.
size_t Drain(std::deque<std::string> *chunks, size_t *front_offset,
	     char *out, size_t len)
{
	size_t n = 0;
	while (n < len && !chunks->empty()) {
		const std::string &c = chunks->front();
		size_t take = std::min(len - n, c.size() - *front_offset);
		UNSAFE_BUFFERS(memcpy(out + n, c.data() + *front_offset, take));
		n += take;
		*front_offset += take;
		if (*front_offset == c.size()) {
			chunks->pop_front();
			*front_offset = 0;
		}
	}
	return n;
}

} // namespace

// ------------------------------------------------------------------
// UploadPipe
// ------------------------------------------------------------------

UploadPipe::UploadPipe(int64_t length,
		       scoped_refptr<base::SingleThreadTaskRunner> runner)
	: length_(length), runner_(std::move(runner))
{
	writable_.Set(true);
}

UploadPipe::~UploadPipe() = default;

void UploadPipe::UpdateFdLocked()
{
	// Write() returns at once when there is room, and also when it can
	// only fail.
	writable_.Set(buffered_ < kStreamBufferSize || ended_ || closed_);
}

int UploadPipe::Write(const char *data, size_t len, size_t *written,
		      uint64_t timeout_ms)
{
	size_t done = 0;
	int rc = CRNET_OK;
	const bool forever = timeout_ms == UINT64_MAX;
	const Clock::time_point deadline = forever ? Clock::time_point()
						   : Deadline(timeout_ms);
	{
		std::unique_lock<std::mutex> lk(mu_);
		if (ended_)
			rc = CRNET_E_STATE;
		else if (length_ >= 0 &&
			 written_ + len > static_cast<uint64_t>(length_))
			rc = CRNET_E_INVAL;
		while (rc == CRNET_OK && done < len) {
			if (closed_) {
				rc = close_result_;
				break;
			}
			size_t space = kStreamBufferSize - buffered_;
			if (space == 0) {
				auto ready = [this] {
					return closed_ ||
					       buffered_ < kStreamBufferSize;
				};
				if (forever)
					space_cv_.wait(lk, ready);
				else if (!space_cv_.wait_until(lk, deadline, ready))
					rc = CRNET_E_TIMEOUT;
				continue;
			}
			size_t n = std::min(space, len - done);
			UNSAFE_BUFFERS(chunks_.emplace_back(data + done, n));
			buffered_ += n;
			written_ += n;
			done += n;
			// Wake the reader now, not after the loop: the next
			// iteration may block until it has drained the buffer.
			if (reader_waiting_) {
				reader_waiting_ = false;
				runner_->PostTask(
					FROM_HERE,
					base::BindOnce(&PipeUploadDataStream::OnPipeReady,
						       stream_));
			}
		}
		UpdateFdLocked();
	}
	if (written)
		*written = done;
	return rc;
}

int UploadPipe::End()
{
	std::lock_guard<std::mutex> lk(mu_);
	if (ended_ || closed_)
		return CRNET_E_STATE;
	ended_ = true;
	UpdateFdLocked();
	if (reader_waiting_) {
		reader_waiting_ = false;
		runner_->PostTask(FROM_HERE,
				  base::BindOnce(&PipeUploadDataStream::OnPipeReady,
						 stream_));
	}
	if (length_ >= 0 && written_ < static_cast<uint64_t>(length_))
		return CRNET_E_INVAL;
	return CRNET_OK;
}

void UploadPipe::Close(int result)
{
	{
		std::lock_guard<std::mutex> lk(mu_);
		closed_ = true;
		close_result_ = result;
		UpdateFdLocked();
	}
	space_cv_.notify_all();
}

void UploadPipe::Attach(base::WeakPtr<PipeUploadDataStream> stream)
{
	std::lock_guard<std::mutex> lk(mu_);
	stream_ = std::move(stream);
}

int UploadPipe::Take(char *buf, size_t len, bool *eof)
{
	size_t n;
	{
		std::lock_guard<std::mutex> lk(mu_);
		n = Drain(&chunks_, &front_offset_, buf, len);
		buffered_ -= n;
		consumed_ += n;
		UpdateFdLocked();
		*eof = ended_ && buffered_ == 0;
		if (n == 0 && !*eof) {
			reader_waiting_ = true;
			return -1;
		}
	}
	if (n)
		space_cv_.notify_all();
	return static_cast<int>(n);
}

uint64_t UploadPipe::consumed() const
{
	std::lock_guard<std::mutex> lk(mu_);
	return consumed_;
}

// ------------------------------------------------------------------
// PipeUploadDataStream (network thread)
// ------------------------------------------------------------------

PipeUploadDataStream::PipeUploadDataStream(scoped_refptr<UploadPipe> pipe,
					   int64_t length)
	: net::UploadDataStream(/*is_chunked=*/length < 0, /*identifier=*/0),
	  pipe_(std::move(pipe)),
	  length_(length)
{
}

PipeUploadDataStream::~PipeUploadDataStream() = default;

int PipeUploadDataStream::InitInternal(const net::NetLogWithSource &net_log)
{
	// Data handed to Chromium is gone; a body cannot be sent twice.
	if (pipe_->consumed())
		return net::ERR_UPLOAD_STREAM_REWIND_NOT_SUPPORTED;
	if (!is_chunked())
		SetSize(static_cast<uint64_t>(length_));
	pipe_->Attach(weak_factory_.GetWeakPtr());
	return net::OK;
}

int PipeUploadDataStream::ReadInternal(net::IOBuffer *buf, int buf_len)
{
	bool eof = false;
	int n = pipe_->Take(buf->data(), static_cast<size_t>(buf_len), &eof);
	if (n > 0) {
		if (eof && is_chunked())
			SetIsFinalChunk();
		return n;
	}
	if (n == 0 && is_chunked()) {
		SetIsFinalChunk();
		return 0;
	}
	// Nothing yet, or a declared-length body ended short (the API fails
	// the transfer for that): wait. Chromium only asks for more of a
	// declared-length body while bytes are still owed.
	pending_buf_ = buf;
	pending_len_ = buf_len;
	return net::ERR_IO_PENDING;
}

void PipeUploadDataStream::ResetInternal()
{
	pending_buf_ = nullptr;
	pending_len_ = 0;
}

void PipeUploadDataStream::OnPipeReady()
{
	if (!pending_buf_)
		return;
	// Take the pending read out first: completing it may synchronously
	// start the next read, which stores its own pending state.
	scoped_refptr<net::IOBuffer> buf = std::move(pending_buf_);
	int len = std::exchange(pending_len_, 0);
	int rv = ReadInternal(buf.get(), len);
	if (rv == net::ERR_IO_PENDING)
		return;	// ReadInternal() stored (buf, len) again
	OnReadCompleted(rv);
}

// ------------------------------------------------------------------
// DownloadBuffer
// ------------------------------------------------------------------

DownloadBuffer::DownloadBuffer() = default;

DownloadBuffer::~DownloadBuffer() = default;

void DownloadBuffer::UpdateFdLocked()
{
	readable_.Set(buffered_ > 0 || finished_);
}

bool DownloadBuffer::Push(const char *data, size_t len)
{
	bool full;
	{
		std::lock_guard<std::mutex> lk(mu_);
		UNSAFE_BUFFERS(chunks_.emplace_back(data, len));
		buffered_ += len;
		full = buffered_ >= kStreamBufferSize;
		if (full)
			stopped_ = true;
		UpdateFdLocked();
	}
	cv_.notify_all();
	return full;
}

void DownloadBuffer::Finish(int result)
{
	{
		std::lock_guard<std::mutex> lk(mu_);
		finished_ = true;
		result_ = result;
		UpdateFdLocked();
	}
	cv_.notify_all();
}

int DownloadBuffer::Read(char *buf, size_t len, size_t *nread,
			 uint64_t timeout_ms, bool *resume)
{
	*nread = 0;
	*resume = false;
	std::unique_lock<std::mutex> lk(mu_);
	auto ready = [this] { return buffered_ > 0 || finished_; };
	if (timeout_ms == UINT64_MAX)
		cv_.wait(lk, ready);
	else if (!cv_.wait_until(lk, Deadline(timeout_ms), ready))
		return CRNET_E_TIMEOUT;

	if (buffered_ == 0)
		return result_;	// finished: OK means end of body

	size_t n = Drain(&chunks_, &front_offset_, buf, len);
	buffered_ -= n;
	*nread = n;
	// Resume with hysteresis, once half the buffer is free again.
	if (stopped_ && buffered_ <= kStreamBufferSize / 2) {
		stopped_ = false;
		*resume = true;
	}
	UpdateFdLocked();
	return CRNET_OK;
}

} // namespace crnet
