// SPDX-License-Identifier: BSD-3-Clause
//
// Streamed request bodies (from a file, or written by the application)
// and streamed response bodies (pull with crnet_transfer_read()).
//
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <random>
#include <thread>

#include <openssl/sha.h>

#include "common.h"

using namespace crtest;

namespace {

std::string Sha256Hex(const std::string &data)
{
	unsigned char md[SHA256_DIGEST_LENGTH];
	SHA256(reinterpret_cast<const unsigned char *>(data.data()), data.size(), md);
	static const char *hex = "0123456789abcdef";
	std::string out;
	for (unsigned char c : md) {
		out += hex[c >> 4];
		out += hex[c & 15];
	}
	return out;
}

std::string RandomData(size_t n, unsigned seed = 7)
{
	std::mt19937 rng(seed);
	std::string s(n, '\0');
	for (auto &c : s)
		c = static_cast<char>(rng());
	return s;
}

std::string Pattern(size_t n)
{
	std::string s(n, '\0');
	for (size_t i = 0; i < n; i++)
		s[i] = static_cast<char>(i & 0xff);
	return s;
}

// Writes |data| in random-sized pieces, then ends the body.
void WriteAll(crnet_transfer *t, const std::string &data, bool end = true)
{
	std::mt19937 rng(3);
	size_t off = 0;
	while (off < data.size()) {
		size_t n = std::min<size_t>(data.size() - off, 1 + rng() % 200000);
		size_t w = 0;
		ASSERT_EQ(crnet_transfer_write(t, data.data() + off, n, &w, UINT64_MAX), CRNET_OK);
		ASSERT_EQ(w, n);
		off += n;
	}
	if (end) {
		ASSERT_EQ(crnet_transfer_write_end(t), CRNET_OK);
	}
}

json Digest(crnet_transfer *t)
{
	EXPECT_EQ(crnet_transfer_wait(t, 30000), CRNET_OK);
	const crnet_response *r = crnet_transfer_response(t);
	EXPECT_EQ(crnet_response_status(r), 200);
	return json::parse(crnet_response_body(r, nullptr));
}

crnet_transfer *StartUpload(crnet_client *c, const std::string &url, int64_t length,
			    bool stream_response = false)
{
	crnet_request *req = crnet_request_new("POST", url.c_str());
	crnet_request_set_mode(req, CRNET_MODE_RAW);
	crnet_request_set_body_stream(req, length);
	if (stream_response)
		crnet_request_set_response_stream(req, 1);
	crnet_transfer *t = crnet_start(c, req, nullptr, nullptr);
	crnet_request_free(req);
	return t;
}

std::string TempFile(const std::string &data)
{
	std::string path = ::testing::TempDir() + "crnet-upload-" +
			   std::to_string(getpid()) + "-" + std::to_string(rand());
	int fd = open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
	EXPECT_GE(fd, 0);
	size_t off = 0;
	while (off < data.size()) {
		ssize_t w = write(fd, data.data() + off, data.size() - off);
		if (w <= 0) {
			break;
		}
		off += w;
	}
	close(fd);
	return path;
}

} // namespace

// ------------------------------------------------------------------
// Uploads written by the application
// ------------------------------------------------------------------

TEST_F(ClientTest, StreamUploadKnownLengthSendsContentLength)
{
	std::string data = RandomData(5 * 1024 * 1024 + 11);
	for (const std::string &url : {HttpsUrl("/upload-digest"), HttpsH1Url("/upload-digest")}) {
		crnet_transfer *t = StartUpload(client_, url, data.size());
		ASSERT_NE(t, nullptr);
		WriteAll(t, data);
		json d = Digest(t);
		crnet_transfer_free(t);
		EXPECT_EQ(d["len"], data.size()) << url;
		EXPECT_EQ(d["sha256"], Sha256Hex(data));
		EXPECT_EQ(d["content_length"], std::to_string(data.size()));
		EXPECT_TRUE(d["transfer_encoding"].is_null());
	}
}

TEST_F(ClientTest, StreamUploadUnknownLengthHttp11IsChunked)
{
	std::string data = RandomData(3 * 1024 * 1024 + 5, 9);
	crnet_transfer *t = StartUpload(client_, HttpsH1Url("/upload-digest"),
					CRNET_LENGTH_UNKNOWN);
	WriteAll(t, data);
	json d = Digest(t);
	crnet_transfer_free(t);
	EXPECT_EQ(d["protocol"], "http/1.1");
	EXPECT_EQ(d["transfer_encoding"], "chunked");
	EXPECT_TRUE(d["content_length"].is_null());
	EXPECT_EQ(d["sha256"], Sha256Hex(data));
}

TEST_F(ClientTest, StreamUploadUnknownLengthHttp2HasNoFraming)
{
	std::string data = RandomData(2 * 1024 * 1024 + 3, 10);
	crnet_transfer *t = StartUpload(client_, HttpsUrl("/upload-digest"),
					CRNET_LENGTH_UNKNOWN);
	WriteAll(t, data);
	json d = Digest(t);
	crnet_transfer_free(t);
	EXPECT_EQ(d["protocol"], "h2");
	EXPECT_TRUE(d["transfer_encoding"].is_null()) << "no chunked in HTTP/2";
	EXPECT_TRUE(d["content_length"].is_null());
	EXPECT_EQ(d["sha256"], Sha256Hex(data));
}

TEST_F(ClientTest, StreamUploadEmptyBody)
{
	for (int64_t len : {int64_t(0), int64_t(CRNET_LENGTH_UNKNOWN)}) {
		crnet_transfer *t = StartUpload(client_, HttpsH1Url("/upload-digest"), len);
		ASSERT_EQ(crnet_transfer_write_end(t), CRNET_OK);
		json d = Digest(t);
		crnet_transfer_free(t);
		EXPECT_EQ(d["len"], 0) << len;
	}
}

TEST_F(ClientTest, StreamUploadFromAnotherThreadWhileTransferRuns)
{
	std::string data = RandomData(4 * 1024 * 1024, 11);
	crnet_transfer *t = StartUpload(client_, HttpsUrl("/upload-digest"), data.size());
	std::thread writer([&] {
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		WriteAll(t, data);
	});
	json d = Digest(t);
	writer.join();
	crnet_transfer_free(t);
	EXPECT_EQ(d["sha256"], Sha256Hex(data));
}

TEST_F(ClientTest, StreamUploadBackpressure)
{
	// The server does not read for a while: once our 1 MiB buffer and
	// the sockets are full, writing must block. 32 MiB exceeds what
	// loopback socket buffers can absorb.
	std::string data = RandomData(32 * 1024 * 1024, 12);
	crnet_transfer *t = StartUpload(client_, HttpsH1Url("/slow-upload?pause_ms=1500"),
					CRNET_LENGTH_UNKNOWN);
	size_t w = 0;
	EXPECT_EQ(crnet_transfer_write(t, data.data(), data.size(), &w, 0),
		  CRNET_E_TIMEOUT);
	EXPECT_GT(w, 0u);
	EXPECT_LE(w, 1024u * 1024u) << "never more than the buffer at once";
	size_t w2 = 0;
	EXPECT_EQ(crnet_transfer_write(t, data.data() + w, data.size() - w, &w2, 300),
		  CRNET_E_TIMEOUT);
	EXPECT_LT(w + w2, data.size()) << "still held back while the server waits";
	size_t off = w + w2;
	ASSERT_EQ(crnet_transfer_write(t, data.data() + off, data.size() - off, nullptr,
				       UINT64_MAX), CRNET_OK);
	ASSERT_EQ(crnet_transfer_write_end(t), CRNET_OK);
	json d = Digest(t);
	crnet_transfer_free(t);
	EXPECT_EQ(d["sha256"], Sha256Hex(data));
}

TEST_F(ClientTest, StreamUploadLengthMismatch)
{
	crnet_transfer *t = StartUpload(client_, HttpsH1Url("/upload-digest"), 10);
	EXPECT_EQ(crnet_transfer_write(t, "0123456789AB", 12, nullptr, 0), CRNET_E_INVAL)
		<< "more than declared";
	EXPECT_EQ(crnet_transfer_write(t, "01234", 5, nullptr, UINT64_MAX), CRNET_OK);
	EXPECT_EQ(crnet_transfer_write_end(t), CRNET_E_INVAL) << "fewer than declared";
	EXPECT_EQ(crnet_transfer_wait(t, 5000), CRNET_E_INVAL);
	EXPECT_EQ(crnet_transfer_write(t, "x", 1, nullptr, 0), CRNET_E_STATE);
	crnet_transfer_free(t);
}

TEST_F(ClientTest, StreamUploadStateErrors)
{
	// Not a streamed body.
	crnet_request *req = crnet_request_new("GET", HttpsUrl("/echo").c_str());
	crnet_transfer *plain = crnet_start(client_, req, nullptr, nullptr);
	crnet_request_free(req);
	EXPECT_EQ(crnet_transfer_write(plain, "x", 1, nullptr, 0), CRNET_E_STATE);
	EXPECT_EQ(crnet_transfer_write_end(plain), CRNET_E_STATE);
	EXPECT_EQ(crnet_transfer_read(plain, nullptr, 0, nullptr, 0), CRNET_E_STATE);
	EXPECT_EQ(crnet_transfer_fd(plain), -1);
	crnet_transfer_free(plain);

	// Writing after the end.
	crnet_transfer *t = StartUpload(client_, HttpsH1Url("/upload-digest"),
					CRNET_LENGTH_UNKNOWN);
	EXPECT_EQ(crnet_transfer_write(t, "abc", 3, nullptr, UINT64_MAX), CRNET_OK);
	EXPECT_EQ(crnet_transfer_write_end(t), CRNET_OK);
	EXPECT_EQ(crnet_transfer_write(t, "d", 1, nullptr, 0), CRNET_E_STATE);
	EXPECT_EQ(crnet_transfer_write_end(t), CRNET_E_STATE);
	EXPECT_EQ(Digest(t)["len"], 3);
	crnet_transfer_free(t);
}

TEST_F(ClientTest, StreamUploadWriterIsReleasedWhenTransferFails)
{
	// Nobody reads the body of a request to a refused port: the blocked
	// writer must return with the transfer's error.
	crnet_request *req = crnet_request_new("POST", "http://127.0.0.1:1/");
	crnet_request_set_body_stream(req, CRNET_LENGTH_UNKNOWN);
	crnet_transfer *t = crnet_start(client_, req, nullptr, nullptr);
	crnet_request_free(req);
	std::string big(4 * 1024 * 1024, 'x');
	int rc = crnet_transfer_write(t, big.data(), big.size(), nullptr, 10000);
	EXPECT_EQ(rc, CRNET_E_NET);
	crnet_transfer_free(t);
}

TEST_F(ClientTest, StreamUploadCannotBeResentOnRedirect)
{
	std::string target = HttpsH1Url("/upload-digest");
	crnet_transfer *t = StartUpload(
		client_, HttpsH1Url("/redirect-to?code=307&url=" + target), CRNET_LENGTH_UNKNOWN);
	WriteAll(t, "payload");
	EXPECT_EQ(crnet_transfer_wait(t, 5000), CRNET_E_NET);
	EXPECT_EQ(crnet_response_net_error(crnet_transfer_response(t)),
		  -25);  // ERR_UPLOAD_STREAM_REWIND_NOT_SUPPORTED
	crnet_transfer_free(t);
}

TEST_F(ClientTest, StreamedRequestsNeedTheAsyncApi)
{
	crnet_request *req = crnet_request_new("POST", HttpsUrl("/echo").c_str());
	crnet_response *resp = nullptr;
	crnet_request_set_body_stream(req, 1);
	EXPECT_EQ(crnet_perform(client_, req, &resp), CRNET_E_INVAL);
	crnet_request_set_body(req, "x", 1);
	crnet_request_set_response_stream(req, 1);
	EXPECT_EQ(crnet_perform(client_, req, &resp), CRNET_E_INVAL);
	crnet_queue *q = crnet_queue_new();
	EXPECT_EQ(crnet_queue_start(q, client_, req, nullptr), CRNET_E_INVAL);
	crnet_queue_free(q);
	crnet_request_free(req);
	EXPECT_EQ(crnet_request_set_body_stream(req = crnet_request_new(nullptr, HttpsUrl("/").c_str()), -2),
		  CRNET_E_INVAL);
	crnet_request_free(req);
}

// ------------------------------------------------------------------
// Uploads from a file
// ------------------------------------------------------------------

TEST_F(ClientTest, FileUploadWholeAndSlice)
{
	std::string data = RandomData(12 * 1024 * 1024 + 123, 13);
	std::string path = TempFile(data);
	struct {
		uint64_t off, len;
	} cases[] = {{0, UINT64_MAX}, {1000, 4 * 1024 * 1024}, {data.size() - 10, UINT64_MAX}};
	for (auto c : cases) {
		for (const std::string &url : {HttpsUrl("/upload-digest"), HttpsH1Url("/upload-digest")}) {
			crnet_request *req = crnet_request_new("PUT", url.c_str());
			ASSERT_EQ(crnet_request_set_body_file(req, path.c_str(), c.off, c.len), CRNET_OK);
			crnet_response *resp = nullptr;
			ASSERT_EQ(crnet_perform(client_, req, &resp), CRNET_OK);
			crnet_request_free(req);
			json d = json::parse(crnet_response_body(resp, nullptr));
			crnet_response_free(resp);
			std::string expect = data.substr(c.off, c.len == UINT64_MAX ? std::string::npos : c.len);
			EXPECT_EQ(d["len"], expect.size()) << url << " " << c.off;
			EXPECT_EQ(d["sha256"], Sha256Hex(expect));
			EXPECT_EQ(d["content_length"], std::to_string(expect.size()));
		}
	}
	unlink(path.c_str());
}

TEST_F(ClientTest, FileUploadIsResentOnRedirect)
{
	std::string data = RandomData(100000, 14);
	std::string path = TempFile(data);
	std::string target = HttpsH1Url("/upload-digest");
	crnet_request *req = crnet_request_new(
		"POST", HttpsH1Url("/redirect-to?code=307&url=" + target).c_str());
	crnet_request_set_body_file(req, path.c_str(), 0, UINT64_MAX);
	crnet_response *resp = nullptr;
	ASSERT_EQ(crnet_perform(client_, req, &resp), CRNET_OK);
	EXPECT_EQ(json::parse(crnet_response_body(resp, nullptr))["sha256"], Sha256Hex(data));
	crnet_response_free(resp);
	crnet_request_free(req);
	unlink(path.c_str());
}

TEST_F(ClientTest, FileUploadMissingFile)
{
	crnet_request *req = crnet_request_new("POST", HttpsUrl("/upload-digest").c_str());
	crnet_request_set_body_file(req, "/nonexistent/crnet-test-file", 0, UINT64_MAX);
	crnet_response *resp = nullptr;
	EXPECT_EQ(crnet_perform(client_, req, &resp), CRNET_E_NET);
	EXPECT_EQ(crnet_response_net_error(resp), -6);  // ERR_FILE_NOT_FOUND
	crnet_response_free(resp);
	crnet_request_free(req);
}

// ------------------------------------------------------------------
// Streamed response bodies
// ------------------------------------------------------------------

namespace {

crnet_transfer *StartDownload(crnet_client *c, const std::string &url,
			      uint64_t max_body = 0)
{
	crnet_request *req = crnet_request_new("GET", url.c_str());
	crnet_request_set_mode(req, CRNET_MODE_RAW);
	crnet_request_set_response_stream(req, 1);
	if (max_body)
		crnet_request_set_max_body_size(req, max_body);
	crnet_transfer *t = crnet_start(c, req, nullptr, nullptr);
	crnet_request_free(req);
	return t;
}

// Reads the whole body; returns the final read status.
int ReadAll(crnet_transfer *t, std::string *out, size_t chunk = 65536)
{
	std::string buf(chunk, '\0');
	for (;;) {
		size_t n = 0;
		int rc = crnet_transfer_read(t, buf.data(), buf.size(), &n, 30000);
		if (rc != CRNET_OK)
			return rc;
		if (n == 0)
			return CRNET_OK;
		out->append(buf.data(), n);
	}
}

} // namespace

TEST_F(ClientTest, StreamDownloadLargeBody)
{
	const size_t kSize = 40 * 1024 * 1024 + 17;
	for (const std::string &url : {HttpsUrl("/bytes/" + std::to_string(kSize)),
				       HttpsH1Url("/bytes/" + std::to_string(kSize))}) {
		crnet_transfer *t = StartDownload(client_, url);
		ASSERT_EQ(crnet_transfer_wait_response(t, 10000), CRNET_OK);
		const crnet_response *r = crnet_transfer_response(t);
		EXPECT_EQ(crnet_response_status(r), 200);
		EXPECT_EQ(std::string(crnet_response_header(r, "content-length")),
			  std::to_string(kSize));
		std::string body;
		EXPECT_EQ(ReadAll(t, &body), CRNET_OK);
		EXPECT_EQ(body.size(), kSize) << url;
		EXPECT_TRUE(body == Pattern(kSize));
		EXPECT_EQ(crnet_transfer_wait(t, 5000), CRNET_OK);
		size_t n = 7;
		crnet_response_body(crnet_transfer_response(t), &n);
		EXPECT_EQ(n, 0u) << "streamed bodies are not buffered";
		crnet_transfer_free(t);
	}
}

TEST_F(ClientTest, StreamDownloadAppliesBackpressure)
{
	crnet_transfer *t = StartDownload(client_, HttpsH1Url("/bytes/104857600"));
	ASSERT_EQ(crnet_transfer_wait_response(t, 10000), CRNET_OK);
	std::this_thread::sleep_for(std::chrono::milliseconds(500));
	EXPECT_FALSE(crnet_transfer_done(t)) << "100 MB cannot fit while nobody reads";
	std::string body;
	EXPECT_EQ(ReadAll(t, &body, 1 << 20), CRNET_OK);
	EXPECT_EQ(body.size(), 104857600u);
	crnet_transfer_free(t);
}

TEST_F(ClientTest, StreamDownloadHeadersBeforeBody)
{
	crnet_transfer *t = StartDownload(client_,
					  HttpsH1Url("/stream/200000?chunk=20000&delay_ms=50"));
	ASSERT_EQ(crnet_transfer_wait_response(t, 5000), CRNET_OK);
	EXPECT_EQ(crnet_response_status(crnet_transfer_response(t)), 200);
	EXPECT_FALSE(crnet_transfer_done(t));
	std::string body;
	EXPECT_EQ(ReadAll(t, &body), CRNET_OK);
	EXPECT_EQ(body, Pattern(200000));
	crnet_transfer_free(t);
}

TEST_F(ClientTest, StreamDownloadWithPollableFd)
{
	crnet_transfer *t = StartDownload(client_,
					  HttpsUrl("/stream/300000?chunk=10000&delay_ms=5"));
	int fd = crnet_transfer_fd(t);
	ASSERT_GE(fd, 0);
	std::string body, buf(8192, '\0');
	for (;;) {
		struct pollfd pfd = {fd, POLLIN, 0};
		ASSERT_EQ(poll(&pfd, 1, 10000), 1);
		size_t n = 0;
		ASSERT_EQ(crnet_transfer_read(t, buf.data(), buf.size(), &n, 0), CRNET_OK);
		if (n == 0)
			break;
		body.append(buf.data(), n);
	}
	EXPECT_EQ(body, Pattern(300000));
	crnet_transfer_free(t);
}

TEST_F(ClientTest, StreamDownloadReadTimeout)
{
	crnet_transfer *t = StartDownload(client_, HttpsH1Url("/delay/1000"));
	char buf[16];
	size_t n = 1;
	EXPECT_EQ(crnet_transfer_read(t, buf, sizeof(buf), &n, 50), CRNET_E_TIMEOUT);
	EXPECT_EQ(n, 0u);
	crnet_transfer_free(t);
}

TEST_F(ClientTest, StreamDownloadErrorAfterData)
{
	crnet_transfer *t = StartDownload(client_, HttpsH1Url("/bytes/1000000"), 100000);
	std::string body;
	EXPECT_EQ(ReadAll(t, &body), CRNET_E_TOO_LARGE);
	EXPECT_LE(body.size(), 100000u + 65536u);
	crnet_transfer_free(t);
}

TEST_F(ClientTest, StreamDownloadFailureBeforeHeaders)
{
	crnet_transfer *t = StartDownload(client_, "https://does-not-exist.invalid/");
	EXPECT_EQ(crnet_transfer_wait_response(t, 10000), CRNET_E_NET);
	char c;
	size_t n;
	EXPECT_EQ(crnet_transfer_read(t, &c, 1, &n, 1000), CRNET_E_NET);
	crnet_transfer_free(t);
}

TEST_F(ClientTest, OnDataTakesPrecedenceOverStreaming)
{
	std::atomic<size_t> got{0};
	crnet_callbacks cb = {};
	cb.on_data = [](crnet_transfer *, const void *, size_t n, void *ud) {
		*static_cast<std::atomic<size_t> *>(ud) += n;
		return 0;
	};
	crnet_request *req = crnet_request_new("GET", HttpsUrl("/bytes/5000").c_str());
	crnet_request_set_response_stream(req, 1);
	crnet_transfer *t = crnet_start(client_, req, &cb, &got);
	crnet_request_free(req);
	EXPECT_EQ(crnet_transfer_wait(t, 5000), CRNET_OK);
	EXPECT_EQ(got, 5000u);
	EXPECT_EQ(crnet_transfer_fd(t), -1);
	crnet_transfer_free(t);
}

TEST_F(ClientTest, StreamUploadAndDownloadTogether)
{
	std::string data = RandomData(2 * 1024 * 1024, 15);
	crnet_transfer *t = StartUpload(client_, HttpsUrl("/upload-digest"),
					CRNET_LENGTH_UNKNOWN, /*stream_response=*/true);
	WriteAll(t, data);
	std::string body;
	ASSERT_EQ(ReadAll(t, &body), CRNET_OK);
	EXPECT_EQ(json::parse(body)["sha256"], Sha256Hex(data));
	crnet_transfer_free(t);
}

TEST_F(ClientTest, ManyConcurrentStreamedTransfers)
{
	const int kN = 16;
	std::atomic<int> ok{0};
	std::vector<std::thread> th;
	for (int i = 0; i < kN; i++)
		th.emplace_back([&, i] {
			std::string data = RandomData(2 * 1024 * 1024 + i, 100 + i);
			std::string url = i % 2 ? HttpsUrl("/upload-digest") : HttpsH1Url("/upload-digest");
			crnet_transfer *t = StartUpload(client_, url, i % 3 ? CRNET_LENGTH_UNKNOWN
									    : int64_t(data.size()),
							/*stream_response=*/true);
			std::mt19937 rng(i);
			size_t off = 0;
			while (off < data.size()) {
				size_t n = std::min<size_t>(data.size() - off, 1 + rng() % 300000);
				size_t w = 0;
				// Mix non-blocking and blocking writes.
				int rc = crnet_transfer_write(t, data.data() + off, n, &w,
							      rng() % 2 ? 0 : UINT64_MAX);
				if (rc != CRNET_OK && rc != CRNET_E_TIMEOUT)
					break;
				off += w;
			}
			crnet_transfer_write_end(t);
			std::string body;
			if (ReadAll(t, &body) == CRNET_OK &&
			    json::parse(body)["sha256"] == Sha256Hex(data))
				ok++;
			crnet_transfer_free(t);
		});
	for (auto &x : th)
		x.join();
	EXPECT_EQ(ok, kN);
}

// ------------------------------------------------------------------
// Progress
// ------------------------------------------------------------------

namespace {

int64_t P(crnet_transfer *t, crnet_progress which)
{
	return crnet_transfer_progress(t, which);
}

} // namespace

TEST_F(ClientTest, ProgressOfAStreamedUpload)
{
	const std::string data = RandomData(8 * 1024 * 1024, 21);
	const std::string half = data.substr(0, data.size() / 2);
	crnet_transfer *t = StartUpload(client_, HttpsUrl("/upload-digest"), data.size());
	ASSERT_NE(t, nullptr);
	EXPECT_EQ(P(t, CRNET_PROGRESS_UPLOAD_TOTAL), static_cast<int64_t>(data.size()));
	WriteAll(t, half, /*end=*/false);
	// The pipe holds at most 1 MiB, so most of the first half is with
	// Chromium by now; the sampled position must show it within a few
	// sampling intervals while the body is still incomplete.
	int64_t sent = 0;
	for (int i = 0; i < 100 && sent == 0; i++) {
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		sent = P(t, CRNET_PROGRESS_UPLOAD_SENT);
	}
	EXPECT_GT(sent, 0);
	EXPECT_LE(sent, static_cast<int64_t>(half.size()));
	WriteAll(t, data.substr(half.size()));
	json d = Digest(t);
	EXPECT_EQ(d["len"], data.size());
	EXPECT_EQ(P(t, CRNET_PROGRESS_UPLOAD_SENT), static_cast<int64_t>(data.size()));
	EXPECT_EQ(P(t, CRNET_PROGRESS_UPLOAD_TOTAL), static_cast<int64_t>(data.size()));
	crnet_transfer_free(t);
}

TEST_F(ClientTest, ProgressOfAnUploadOfUnknownLength)
{
	const std::string data = RandomData(3 * 1024 * 1024, 22);
	crnet_transfer *t = StartUpload(client_, HttpsH1Url("/upload-digest"),
					CRNET_LENGTH_UNKNOWN);
	ASSERT_NE(t, nullptr);
	WriteAll(t, data);
	Digest(t);
	EXPECT_EQ(P(t, CRNET_PROGRESS_UPLOAD_TOTAL), -1);
	EXPECT_EQ(P(t, CRNET_PROGRESS_UPLOAD_SENT), static_cast<int64_t>(data.size()));
	crnet_transfer_free(t);
}

TEST_F(ClientTest, ProgressOfAFileUpload)
{
	const std::string data = RandomData(3 * 1024 * 1024 + 5, 23);
	std::string path = TempFile(data);
	crnet_request *req = crnet_request_new("PUT", HttpsUrl("/upload-digest").c_str());
	// Whole file: the size is only known once Chromium opens it.
	ASSERT_EQ(crnet_request_set_body_file(req, path.c_str(), 0, UINT64_MAX), CRNET_OK);
	crnet_transfer *t = crnet_start(client_, req, nullptr, nullptr);
	crnet_request_free(req);
	ASSERT_NE(t, nullptr);
	Digest(t);
	EXPECT_EQ(P(t, CRNET_PROGRESS_UPLOAD_TOTAL), static_cast<int64_t>(data.size()));
	EXPECT_EQ(P(t, CRNET_PROGRESS_UPLOAD_SENT), static_cast<int64_t>(data.size()));
	crnet_transfer_free(t);
	unlink(path.c_str());
}

TEST_F(ClientTest, ProgressOfADownload)
{
	crnet_transfer *t = StartDownload(client_, HttpsUrl("/bytes/3000000"));
	ASSERT_NE(t, nullptr);
	EXPECT_EQ(P(t, CRNET_PROGRESS_UPLOAD_TOTAL), 0);
	ASSERT_EQ(crnet_transfer_wait_response(t, 10000), CRNET_OK);
	EXPECT_EQ(P(t, CRNET_PROGRESS_DOWNLOAD_TOTAL), 3000000);
	char buf[100000];
	size_t got = 0, n = 0;
	while (got < 500000) {
		ASSERT_EQ(crnet_transfer_read(t, buf, sizeof(buf), &n, 10000), CRNET_OK);
		ASSERT_GT(n, 0u);
		got += n;
	}
	// Received counts what was pulled off the network, ahead of what
	// the application has read.
	EXPECT_GE(P(t, CRNET_PROGRESS_DOWNLOAD_DECODED), static_cast<int64_t>(got));
	EXPECT_LE(P(t, CRNET_PROGRESS_DOWNLOAD_DECODED), 3000000);
	std::string rest;
	EXPECT_EQ(ReadAll(t, &rest), CRNET_OK);
	EXPECT_EQ(crnet_transfer_wait(t, 5000), CRNET_OK);
	EXPECT_EQ(P(t, CRNET_PROGRESS_DOWNLOAD_DECODED), 3000000);
	EXPECT_EQ(P(t, CRNET_PROGRESS_DOWNLOAD_RECEIVED), 3000000);
	EXPECT_EQ(P(t, CRNET_PROGRESS_UPLOAD_SENT), 0);
	EXPECT_EQ(P(t, CRNET_PROGRESS_COUNT), -1);
	EXPECT_EQ(crnet_transfer_progress(nullptr, CRNET_PROGRESS_UPLOAD_SENT), -1);
	crnet_transfer_free(t);
}

TEST_F(ClientTest, ProgressCountsEncodedAndDecodedBytes)
{
	crnet_request *req = crnet_request_new("GET", HttpsUrl("/encoded/gzip").c_str());
	crnet_transfer *t = crnet_start(client_, req, nullptr, nullptr);
	crnet_request_free(req);
	ASSERT_EQ(crnet_transfer_wait(t, 10000), CRNET_OK);
	const crnet_response *r = crnet_transfer_response(t);
	size_t len = 0;
	crnet_response_body(r, &len);
	int64_t wire = std::stoll(crnet_response_header(r, "content-length"));
	EXPECT_EQ(P(t, CRNET_PROGRESS_DOWNLOAD_DECODED), static_cast<int64_t>(len));
	EXPECT_EQ(P(t, CRNET_PROGRESS_DOWNLOAD_RECEIVED), wire);
	EXPECT_EQ(P(t, CRNET_PROGRESS_DOWNLOAD_TOTAL), wire);
	EXPECT_LT(wire, static_cast<int64_t>(len));
	crnet_transfer_free(t);
}

// ------------------------------------------------------------------
// Event-loop uploads (crnet_transfer_write_fd)
// ------------------------------------------------------------------

namespace {

bool Readable(int fd, int timeout_ms)
{
	pollfd p = {fd, POLLIN, 0};
	return poll(&p, 1, timeout_ms) == 1 && (p.revents & POLLIN);
}

} // namespace

// The whole body is fed from a poll() loop with non-blocking writes;
// the fd must report room exactly when a write can make progress.
TEST_F(ClientTest, StreamUploadDrivenByWriteFd)
{
	const std::string data = RandomData(12 * 1024 * 1024 + 3, 31);
	for (const std::string &url : {HttpsUrl("/upload-digest"), HttpsH1Url("/upload-digest")}) {
		crnet_transfer *t = StartUpload(client_, url, data.size());
		ASSERT_NE(t, nullptr);
		int fd = crnet_transfer_write_fd(t);
		ASSERT_GE(fd, 0);
		EXPECT_EQ(crnet_transfer_fd(t), -1) << "download fd is separate";
		size_t off = 0;
		int would_block = 0;
		while (off < data.size()) {
			ASSERT_TRUE(Readable(fd, 10000)) << "no room after 10 s at " << off;
			size_t w = 0;
			int rc = crnet_transfer_write(t, data.data() + off, data.size() - off, &w, 0);
			ASSERT_TRUE(rc == CRNET_OK || rc == CRNET_E_TIMEOUT) << rc;
			EXPECT_GT(w, 0u) << "readable fd but no progress";
			off += w;
			if (rc == CRNET_E_TIMEOUT)
				would_block++;
		}
		EXPECT_GT(would_block, 0) << "12 MB should not fit in the 1 MiB buffer";
		ASSERT_EQ(crnet_transfer_write_end(t), CRNET_OK);
		EXPECT_TRUE(Readable(fd, 0)) << "an ended body never blocks";
		json d = Digest(t);
		EXPECT_EQ(d["len"], data.size()) << url;
		EXPECT_EQ(d["sha256"], Sha256Hex(data));
		crnet_transfer_free(t);
	}
}

// While the server does not read, the buffer stays full and the fd must
// stay quiet; it wakes up once the server starts reading.
TEST_F(ClientTest, WriteFdIsQuietWhileTheBufferIsFull)
{
	const std::string data = RandomData(48 * 1024 * 1024, 32);
	crnet_transfer *t = StartUpload(client_, HttpsH1Url("/slow-upload?pause_ms=3000"),
					data.size());
	ASSERT_NE(t, nullptr);
	int fd = crnet_transfer_write_fd(t);
	size_t off = 0;
	bool saw_quiet = false;
	while (off < data.size()) {
		size_t w = 0;
		int rc = crnet_transfer_write(t, data.data() + off, data.size() - off, &w, 0);
		off += w;
		if (rc == CRNET_OK)
			break;
		ASSERT_EQ(rc, CRNET_E_TIMEOUT);
		if (!saw_quiet && !Readable(fd, 300)) {
			// Full, and nothing drained it for 300 ms.
			saw_quiet = true;
			size_t w2 = 1;
			EXPECT_EQ(crnet_transfer_write(t, data.data() + off, 1, &w2, 0),
				  CRNET_E_TIMEOUT);
			EXPECT_EQ(w2, 0u);
		}
		ASSERT_TRUE(Readable(fd, 10000)) << "never woke up at " << off;
	}
	EXPECT_TRUE(saw_quiet) << "the server pause should have filled the buffer";
	ASSERT_EQ(crnet_transfer_write_end(t), CRNET_OK);
	json d = Digest(t);
	EXPECT_EQ(d["sha256"], Sha256Hex(data));
	crnet_transfer_free(t);
}

TEST_F(ClientTest, WriteFdIsReadableWhenTheTransferFailed)
{
	crnet_request *req = crnet_request_new("POST", "https://127.0.0.1:1/");
	crnet_request_set_body_stream(req, CRNET_LENGTH_UNKNOWN);
	crnet_transfer *t = crnet_start(client_, req, nullptr, nullptr);
	crnet_request_free(req);
	ASSERT_NE(t, nullptr);
	int fd = crnet_transfer_write_fd(t);
	ASSERT_GE(fd, 0);
	std::string chunk(256 * 1024, 'x');
	for (int i = 0; i < 4; i++)	// fill the 1 MiB buffer
		crnet_transfer_write(t, chunk.data(), chunk.size(), nullptr, 0);
	EXPECT_EQ(crnet_transfer_wait(t, 10000), CRNET_E_NET);
	EXPECT_TRUE(Readable(fd, 0));
	EXPECT_NE(crnet_transfer_write(t, "x", 1, nullptr, 0), CRNET_OK);
	crnet_transfer_free(t);
}

TEST_F(ClientTest, WriteFdOnlyForStreamedUploads)
{
	crnet_transfer *t = StartDownload(client_, HttpsUrl("/echo"));
	EXPECT_EQ(crnet_transfer_write_fd(t), -1);
	EXPECT_EQ(crnet_transfer_write_fd(nullptr), -1);
	crnet_transfer_wait(t, 10000);
	crnet_transfer_free(t);
}
