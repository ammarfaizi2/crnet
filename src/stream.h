// SPDX-License-Identifier: BSD-3-Clause
//
// Streamed request and response bodies: bounded byte pipes between the
// application's threads and a transfer on the network thread.
//
#ifndef CRNET_SRC_STREAM_H
#define CRNET_SRC_STREAM_H

#include <stdint.h>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>

#include "base/memory/ref_counted.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/task/single_thread_task_runner.h"
#include "crnet/fd_flag.h"
#include "net/base/io_buffer.h"
#include "net/base/upload_data_stream.h"

namespace crnet {

// Bytes buffered per direction before the producer is held back.
inline constexpr size_t kStreamBufferSize = 1024 * 1024;

class PipeUploadDataStream;

// A request body written by the application (any thread) and consumed by
// Chromium (network thread) through PipeUploadDataStream.
class UploadPipe : public base::RefCountedThreadSafe<UploadPipe> {
public:
	// |length| < 0: unknown (chunked).
	UploadPipe(int64_t length,
		   scoped_refptr<base::SingleThreadTaskRunner> runner);

	UploadPipe(const UploadPipe &) = delete;
	UploadPipe &operator=(const UploadPipe &) = delete;

	// Application side; see crnet_transfer_write()/_write_end().
	int Write(const char *data, size_t len, size_t *written,
		  uint64_t timeout_ms);
	int End();

	// Transfer side, when it finishes: refuse further data and wake
	// writers, which then get |result|.
	void Close(int result);

	// Stream side (network thread).
	void Attach(base::WeakPtr<PipeUploadDataStream> stream);
	// Copies up to |len| bytes: > 0 bytes copied; 0 at the end of the
	// body (*eof); -1 when nothing is buffered yet, in which case the
	// stream is notified once data or the end arrives.
	int Take(char *buf, size_t len, bool *eof);
	uint64_t consumed() const;

private:
	friend class base::RefCountedThreadSafe<UploadPipe>;
	~UploadPipe();

	const int64_t length_;
	const scoped_refptr<base::SingleThreadTaskRunner> runner_;

	mutable std::mutex mu_;
	std::condition_variable space_cv_;
	std::deque<std::string> chunks_;
	size_t front_offset_ = 0;
	size_t buffered_ = 0;
	uint64_t written_ = 0;
	uint64_t consumed_ = 0;
	bool ended_ = false;
	bool closed_ = false;
	int close_result_ = 0;
	bool reader_waiting_ = false;
	base::WeakPtr<PipeUploadDataStream> stream_;
};

// The UploadDataStream handed to the URLRequest for a streamed body. Not
// rewindable once data was consumed.
class PipeUploadDataStream : public net::UploadDataStream {
public:
	PipeUploadDataStream(scoped_refptr<UploadPipe> pipe, int64_t length);
	~PipeUploadDataStream() override;

	// Data or the end arrived for a read that returned ERR_IO_PENDING.
	void OnPipeReady();

	bool IsInMemory() const override { return false; }

private:
	int InitInternal(const net::NetLogWithSource &net_log) override;
	int ReadInternal(net::IOBuffer *buf, int buf_len) override;
	void ResetInternal() override;

	const scoped_refptr<UploadPipe> pipe_;
	const int64_t length_;
	scoped_refptr<net::IOBuffer> pending_buf_;
	int pending_len_ = 0;
	base::WeakPtrFactory<PipeUploadDataStream> weak_factory_{this};
};

// A response body produced by the network thread and read by the
// application (crnet_transfer_read()).
class DownloadBuffer {
public:
	DownloadBuffer();
	~DownloadBuffer();

	DownloadBuffer(const DownloadBuffer &) = delete;
	DownloadBuffer &operator=(const DownloadBuffer &) = delete;

	bool ok() const { return readable_.fd() >= 0; }
	int fd() const { return readable_.fd(); }

	// Network thread. Returns true when the buffer is full and reading
	// from the network should stop until the reader asks to resume.
	bool Push(const char *data, size_t len);
	// Network thread: no more data will come; |result| for the reader.
	void Finish(int result);

	// Any thread. *resume is set when the network side stopped reading
	// and should now continue.
	int Read(char *buf, size_t len, size_t *nread, uint64_t timeout_ms,
		 bool *resume);

private:
	void UpdateFdLocked();

	FdFlag readable_;
	std::mutex mu_;
	std::condition_variable cv_;
	std::deque<std::string> chunks_;
	size_t front_offset_ = 0;
	size_t buffered_ = 0;
	bool stopped_ = false;
	bool finished_ = false;
	int result_ = 0;
};

} // namespace crnet

#endif // CRNET_SRC_STREAM_H
