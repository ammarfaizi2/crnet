// SPDX-License-Identifier: BSD-3-Clause
//
// Asynchronous transfers, cancellation, flow control, concurrency,
// client shutdown and the completion queue.
//
#include <poll.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <set>
#include <thread>

#include "common.h"

using namespace crtest;

namespace {

struct Recorder {
	std::mutex mu;
	std::vector<std::string> events;
	std::string data;
	std::atomic<int> completes{0};
	int error = 999;
	int status = 0;
	int abort_on_data_after = -1;	// abort once this many bytes seen
	bool pause_on_response = false;
	std::thread::id cb_thread;
	int nested_perform_rc = 0;
	crnet_client *client = nullptr;
	bool try_nested_perform = false;

	void Add(const std::string &e)
	{
		std::lock_guard<std::mutex> lk(mu);
		events.push_back(e);
	}
};

int OnResponse(crnet_transfer *t, const crnet_response *resp, void *ud)
{
	auto *r = static_cast<Recorder *>(ud);
	r->Add("response");
	r->cb_thread = std::this_thread::get_id();
	r->status = crnet_response_status(resp);
	if (r->pause_on_response)
		crnet_transfer_pause(t);
	if (r->try_nested_perform) {
		crnet_request *req = crnet_request_new("GET", HttpsUrl("/echo").c_str());
		crnet_response *x = nullptr;
		r->nested_perform_rc = crnet_perform(r->client, req, &x);
		crnet_response_free(x);
		crnet_request_free(req);
	}
	return 0;
}

int OnData(crnet_transfer *, const void *d, size_t n, void *ud)
{
	auto *r = static_cast<Recorder *>(ud);
	{
		std::lock_guard<std::mutex> lk(r->mu);
		if (r->events.empty() || r->events.back() != "data")
			r->events.push_back("data");
		r->data.append(static_cast<const char *>(d), n);
	}
	if (r->abort_on_data_after >= 0 &&
	    r->data.size() >= static_cast<size_t>(r->abort_on_data_after))
		return 1;
	return 0;
}

void OnComplete(crnet_transfer *, int err, const crnet_response *, void *ud)
{
	auto *r = static_cast<Recorder *>(ud);
	r->Add("complete");
	r->error = err;
	r->completes++;
}

const crnet_callbacks kAll = {OnResponse, OnData, OnComplete};
const crnet_callbacks kNoData = {OnResponse, nullptr, OnComplete};

crnet_transfer *StartGet(crnet_client *c, const std::string &url,
			 const crnet_callbacks *cb, void *ud)
{
	crnet_request *req = crnet_request_new("GET", url.c_str());
	crnet_transfer *t = crnet_start(c, req, cb, ud);
	crnet_request_free(req);  // the transfer has its own copy
	return t;
}

} // namespace

TEST_F(ClientTest, CallbackSequence)
{
	Recorder rec;
	crnet_transfer *t = StartGet(client_, HttpsUrl("/bytes/300000"), &kAll, &rec);
	ASSERT_NE(t, nullptr);
	EXPECT_EQ(crnet_transfer_wait(t, UINT64_MAX), CRNET_OK);
	EXPECT_TRUE(crnet_transfer_done(t));
	EXPECT_EQ(rec.events, (std::vector<std::string>{"response", "data", "complete"}));
	EXPECT_EQ(rec.completes, 1);
	EXPECT_EQ(rec.error, CRNET_OK);
	EXPECT_EQ(rec.status, 200);
	EXPECT_EQ(rec.data.size(), 300000u);
	EXPECT_NE(rec.cb_thread, std::this_thread::get_id());
	// Streamed bodies are not accumulated.
	size_t n = 1;
	crnet_response_body(crnet_transfer_response(t), &n);
	EXPECT_EQ(n, 0u);
	crnet_transfer_free(t);
}

TEST_F(ClientTest, BufferedWhenNoDataCallback)
{
	Recorder rec;
	crnet_transfer *t = StartGet(client_, HttpsUrl("/bytes/1000"), &kNoData, &rec);
	ASSERT_EQ(crnet_transfer_wait(t, UINT64_MAX), CRNET_OK);
	crnet_response *resp = crnet_transfer_take_response(t);
	ASSERT_NE(resp, nullptr);
	EXPECT_EQ(crnet_transfer_response(t), nullptr);
	size_t n = 0;
	crnet_response_body(resp, &n);
	EXPECT_EQ(n, 1000u);
	crnet_response_free(resp);
	crnet_transfer_free(t);
}

TEST_F(ClientTest, NoCallbacksAtAll)
{
	crnet_transfer *t = StartGet(client_, HttpsUrl("/echo"), nullptr, nullptr);
	ASSERT_NE(t, nullptr);
	EXPECT_EQ(crnet_transfer_wait(t, UINT64_MAX), CRNET_OK);
	EXPECT_EQ(crnet_response_status(crnet_transfer_response(t)), 200);
	crnet_transfer_free(t);
}

TEST_F(ClientTest, WaitTimeoutDoesNotStopTransfer)
{
	crnet_transfer *t = StartGet(client_, HttpsUrl("/delay/600"), nullptr, nullptr);
	EXPECT_EQ(crnet_transfer_wait(t, 0), CRNET_E_TIMEOUT);
	EXPECT_EQ(crnet_transfer_wait(t, 50), CRNET_E_TIMEOUT);
	EXPECT_FALSE(crnet_transfer_done(t));
	EXPECT_EQ(crnet_transfer_take_response(t), nullptr);
	EXPECT_EQ(crnet_transfer_wait(t, UINT64_MAX), CRNET_OK);
	crnet_transfer_free(t);
}

bool Readable(int fd, int timeout_ms = 0)
{
	pollfd p = {fd, POLLIN, 0};
	return poll(&p, 1, timeout_ms) == 1;
}

TEST_F(ClientTest, WaitFdsFollowTheTransfer)
{
	crnet_transfer *t = StartGet(client_, HttpsUrl("/delay/600"), nullptr, nullptr);
	int resp = crnet_transfer_response_fd(t);
	int done = crnet_transfer_done_fd(t);
	ASSERT_GE(resp, 0);
	ASSERT_GE(done, 0);
	EXPECT_EQ(crnet_transfer_response_fd(t), resp);
	EXPECT_EQ(crnet_transfer_done_fd(t), done);
	EXPECT_FALSE(Readable(resp));
	EXPECT_FALSE(Readable(done));
	ASSERT_TRUE(Readable(done, 5000));
	EXPECT_TRUE(crnet_transfer_done(t));
	EXPECT_TRUE(Readable(resp));
	EXPECT_EQ(crnet_transfer_wait(t, 0), CRNET_OK);
	EXPECT_TRUE(Readable(done));	// and stays so
	crnet_transfer_free(t);
}

TEST_F(ClientTest, ResponseFdBeforeTheBody)
{
	crnet_request *req = crnet_request_new(
		"GET", HttpsUrl("/stream/100000?chunk=1000&delay_ms=10").c_str());
	crnet_request_set_response_stream(req, 1);
	crnet_transfer *t = crnet_start(client_, req, nullptr, nullptr);
	crnet_request_free(req);
	ASSERT_NE(t, nullptr);
	ASSERT_TRUE(Readable(crnet_transfer_response_fd(t), 5000));
	EXPECT_EQ(crnet_transfer_wait_response(t, 0), CRNET_OK);
	EXPECT_EQ(crnet_response_status(crnet_transfer_response(t)), 200);
	// About a second of body is still to come, and unread.
	EXPECT_FALSE(Readable(crnet_transfer_done_fd(t)));
	crnet_transfer_free(t);
}

TEST_F(ClientTest, WaitFdsOfAFinishedTransferAreReadable)
{
	crnet_transfer *t = StartGet(client_, HttpsUrl("/echo"), nullptr, nullptr);
	ASSERT_EQ(crnet_transfer_wait(t, UINT64_MAX), CRNET_OK);
	EXPECT_TRUE(Readable(crnet_transfer_response_fd(t)));
	EXPECT_TRUE(Readable(crnet_transfer_done_fd(t)));
	crnet_transfer_free(t);

	// Failed before any response.
	t = StartGet(client_, "http://127.0.0.1:1/", nullptr, nullptr);
	int resp = crnet_transfer_response_fd(t);
	ASSERT_TRUE(Readable(resp, 5000));
	EXPECT_EQ(crnet_transfer_wait_response(t, 0), CRNET_E_NET);
	EXPECT_TRUE(Readable(crnet_transfer_done_fd(t), 5000));
	crnet_transfer_free(t);

	EXPECT_EQ(crnet_transfer_response_fd(nullptr), -1);
	EXPECT_EQ(crnet_transfer_done_fd(nullptr), -1);
}

TEST_F(ClientTest, WaitFdIsMadeOnceForManyThreads)
{
	crnet_transfer *t = StartGet(client_, HttpsUrl("/delay/300"), nullptr, nullptr);
	std::vector<int> fds(16, -1);
	std::vector<std::thread> threads;
	for (size_t i = 0; i < fds.size(); i++)
		threads.emplace_back([&, i] { fds[i] = crnet_transfer_done_fd(t); });
	for (auto &th : threads)
		th.join();
	for (int fd : fds)
		EXPECT_EQ(fd, fds[0]);
	EXPECT_TRUE(Readable(fds[0], 5000));
	crnet_transfer_free(t);
}

TEST_F(ClientTest, CancelMidStream)
{
	Recorder rec;
	crnet_transfer *t = StartGet(
		client_, HttpsUrl("/stream/1000000?chunk=1000&delay_ms=5"), &kAll, &rec);
	while (true) {
		std::lock_guard<std::mutex> lk(rec.mu);
		if (rec.data.size() > 5000)
			break;
	}
	crnet_transfer_cancel(t);
	EXPECT_EQ(crnet_transfer_wait(t, UINT64_MAX), CRNET_E_CANCELED);
	EXPECT_EQ(rec.error, CRNET_E_CANCELED);
	EXPECT_EQ(rec.completes, 1);
	EXPECT_LT(rec.data.size(), 1000000u);
	crnet_transfer_cancel(t);  // no-op after completion
	EXPECT_EQ(rec.completes, 1);
	crnet_transfer_free(t);
}

TEST_F(ClientTest, CancelBeforeResponse)
{
	Recorder rec;
	crnet_transfer *t = StartGet(client_, HttpsUrl("/delay/2000"), &kAll, &rec);
	crnet_transfer_cancel(t);
	EXPECT_EQ(crnet_transfer_wait(t, 5000), CRNET_E_CANCELED);
	EXPECT_EQ(rec.events, std::vector<std::string>{"complete"});
	crnet_transfer_free(t);
}

TEST_F(ClientTest, DataCallbackAbort)
{
	Recorder rec;
	rec.abort_on_data_after = 1;
	crnet_transfer *t = StartGet(client_, HttpsUrl("/bytes/500000"), &kAll, &rec);
	EXPECT_EQ(crnet_transfer_wait(t, UINT64_MAX), CRNET_E_CALLBACK);
	EXPECT_EQ(rec.completes, 1);
	crnet_transfer_free(t);
}

TEST_F(ClientTest, ResponseCallbackAbort)
{
	crnet_callbacks cb = kAll;
	cb.on_response = [](crnet_transfer *, const crnet_response *, void *) { return 1; };
	Recorder rec;
	crnet_transfer *t = StartGet(client_, HttpsUrl("/bytes/500000"), &cb, &rec);
	EXPECT_EQ(crnet_transfer_wait(t, UINT64_MAX), CRNET_E_CALLBACK);
	EXPECT_TRUE(rec.data.empty());
	crnet_transfer_free(t);
}

TEST_F(ClientTest, PauseAndResume)
{
	Recorder rec;
	rec.pause_on_response = true;
	crnet_transfer *t = StartGet(client_, HttpsUrl("/bytes/200000"), &kAll, &rec);
	std::this_thread::sleep_for(std::chrono::milliseconds(300));
	{
		std::lock_guard<std::mutex> lk(rec.mu);
		EXPECT_TRUE(rec.data.empty()) << "paused before the first read";
	}
	EXPECT_FALSE(crnet_transfer_done(t));
	crnet_transfer_resume(t);
	EXPECT_EQ(crnet_transfer_wait(t, UINT64_MAX), CRNET_OK);
	EXPECT_EQ(rec.data.size(), 200000u);
	crnet_transfer_free(t);
}

TEST_F(ClientTest, FreeWhileRunningCancelsAndWaits)
{
	auto *rec = new Recorder;
	crnet_transfer *t = StartGet(client_, HttpsUrl("/delay/3000"), &kAll, rec);
	auto t0 = std::chrono::steady_clock::now();
	crnet_transfer_free(t);
	auto dt = std::chrono::steady_clock::now() - t0;
	// on_complete has run by now; user data can go.
	EXPECT_EQ(rec->completes, 1);
	EXPECT_EQ(rec->error, CRNET_E_CANCELED);
	EXPECT_LT(dt, std::chrono::milliseconds(2000));
	delete rec;
}

TEST_F(ClientTest, FreeFromOnComplete)
{
	std::atomic<bool> freed{false};
	crnet_callbacks cb = {};
	cb.on_complete = [](crnet_transfer *t, int, const crnet_response *, void *ud) {
		crnet_transfer_free(t);
		static_cast<std::atomic<bool> *>(ud)->store(true);
	};
	crnet_transfer *t = StartGet(client_, HttpsUrl("/echo"), &cb, &freed);
	ASSERT_NE(t, nullptr);
	for (int i = 0; i < 500 && !freed; i++)
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	EXPECT_TRUE(freed);
}

TEST_F(ClientTest, ResultIsPendingWhileRunning)
{
	crnet_transfer *t = StartGet(client_, HttpsUrl("/delay/300"), nullptr, nullptr);
	ASSERT_NE(t, nullptr);
	EXPECT_EQ(crnet_transfer_result(t), CRNET_E_PENDING);
	EXPECT_EQ(crnet_transfer_wait(t, UINT64_MAX), CRNET_OK);
	EXPECT_EQ(crnet_transfer_result(t), CRNET_OK);
	crnet_transfer_free(t);

	t = StartGet(client_, "https://127.0.0.1:1/", nullptr, nullptr);
	ASSERT_NE(t, nullptr);
	EXPECT_EQ(crnet_transfer_wait(t, UINT64_MAX), CRNET_E_NET);
	EXPECT_EQ(crnet_transfer_result(t), CRNET_E_NET);
	crnet_transfer_free(t);
	EXPECT_EQ(crnet_transfer_result(nullptr), CRNET_E_INVAL);
}

// on_complete hands the transfer to another thread and stays inside the
// callback until that thread is done with it: the result and the
// response must already be readable there, although the transfer does
// not count as done until on_complete returns.
TEST_F(ClientTest, ResultIsReadableOnceOnCompleteIsEntered)
{
	struct HandOff {
		std::mutex mu;
		std::condition_variable cv;
		crnet_transfer *t = nullptr;
		int in_callback = 999;
		bool checked = false;
	} h;
	crnet_callbacks cb = {};
	cb.on_complete = [](crnet_transfer *t, int, const crnet_response *, void *ud) {
		auto *h = static_cast<HandOff *>(ud);
		std::unique_lock<std::mutex> lk(h->mu);
		h->in_callback = crnet_transfer_result(t);
		h->t = t;
		h->cv.notify_all();
		h->cv.wait(lk, [h] { return h->checked; });
	};
	crnet_transfer *t = StartGet(client_, HttpsUrl("/echo"), &cb, &h);
	ASSERT_NE(t, nullptr);
	{
		std::unique_lock<std::mutex> lk(h.mu);
		h.cv.wait(lk, [&h] { return h.t != nullptr; });
	}
	EXPECT_EQ(h.in_callback, CRNET_OK);
	EXPECT_EQ(crnet_transfer_result(t), CRNET_OK);
	EXPECT_EQ(crnet_response_status(crnet_transfer_response(t)), 200);
	EXPECT_FALSE(crnet_transfer_done(t));
	EXPECT_EQ(crnet_transfer_wait(t, 0), CRNET_E_TIMEOUT);
	{
		std::lock_guard<std::mutex> lk(h.mu);
		h.checked = true;
	}
	h.cv.notify_all();
	EXPECT_EQ(crnet_transfer_wait(t, UINT64_MAX), CRNET_OK);
	EXPECT_TRUE(crnet_transfer_done(t));
	crnet_transfer_free(t);
}

TEST_F(ClientTest, BlockingCallInsideCallbackIsRefused)
{
	Recorder rec;
	rec.try_nested_perform = true;
	rec.client = client_;
	crnet_transfer *t = StartGet(client_, HttpsUrl("/echo"), &kAll, &rec);
	EXPECT_EQ(crnet_transfer_wait(t, UINT64_MAX), CRNET_OK);
	EXPECT_EQ(rec.nested_perform_rc, CRNET_E_WOULD_DEADLOCK);
	crnet_transfer_free(t);
}

TEST_F(ClientTest, ManyConcurrentRequestsShareOneHttp2Connection)
{
	const int kN = 200;
	std::vector<crnet_transfer *> ts;
	for (int i = 0; i < kN; i++)
		ts.push_back(StartGet(client_, HttpsUrl("/delay/50?i=" + std::to_string(i)),
				      nullptr, nullptr));
	std::set<int> ports;
	for (int i = 0; i < kN; i++) {
		ASSERT_EQ(crnet_transfer_wait(ts[i], UINT64_MAX), CRNET_OK);
		json e = json::parse(crnet_response_body(crnet_transfer_response(ts[i]), nullptr));
		EXPECT_EQ(e["target"], "/delay/50?i=" + std::to_string(i));
		ports.insert(e["peer_port"].get<int>());
		crnet_transfer_free(ts[i]);
	}
	EXPECT_EQ(ports.size(), 1u) << "all streams multiplexed on one connection";
}

TEST_F(ClientTest, ConcurrentRequestsAreParallel)
{
	// 40 x 500 ms server delay must not take 20 s.
	auto t0 = std::chrono::steady_clock::now();
	std::vector<crnet_transfer *> ts;
	for (int i = 0; i < 40; i++)
		ts.push_back(StartGet(client_, HttpsUrl("/delay/500"), nullptr, nullptr));
	for (auto *t : ts) {
		EXPECT_EQ(crnet_transfer_wait(t, UINT64_MAX), CRNET_OK);
		crnet_transfer_free(t);
	}
	EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::seconds(4));
}

TEST_F(ClientTest, PerformFromManyThreads)
{
	std::atomic<int> ok{0};
	std::vector<std::thread> th;
	for (int i = 0; i < 16; i++)
		th.emplace_back([&, i] {
			for (int j = 0; j < 10; j++) {
				std::string url = (j % 2 ? HttpsUrl("/echo?t=") : HttpsH1Url("/echo?t=")) +
						  std::to_string(i * 100 + j);
				Result r = Get(client_, url);
				if (r.rc == CRNET_OK && r.echo()["target"] ==
								  "/echo?t=" + std::to_string(i * 100 + j))
					ok++;
			}
		});
	for (auto &t : th)
		t.join();
	EXPECT_EQ(ok, 160);
}

TEST(Shutdown, ClientFreeCompletesInFlightTransfers)
{
	crnet_client *c = NewTestClient();
	Recorder rec[5];
	crnet_transfer *ts[5];
	for (int i = 0; i < 5; i++)
		ts[i] = StartGet(c, HttpsUrl("/delay/5000"), &kAll, &rec[i]);
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	crnet_client_free(c);
	for (int i = 0; i < 5; i++) {
		EXPECT_EQ(rec[i].completes, 1);
		EXPECT_EQ(rec[i].error, CRNET_E_SHUTDOWN);
		// Handles outlive the client.
		EXPECT_TRUE(crnet_transfer_done(ts[i]));
		EXPECT_EQ(crnet_transfer_wait(ts[i], 0), CRNET_E_SHUTDOWN);
		crnet_transfer_free(ts[i]);
	}
}

TEST(Shutdown, ManyClientsInParallel)
{
	std::vector<std::thread> th;
	std::atomic<int> ok{0};
	for (int i = 0; i < 8; i++)
		th.emplace_back([&] {
			crnet_client *c = NewTestClient();
			for (int j = 0; j < 5; j++)
				if (Get(c, HttpsUrl("/echo")).rc == CRNET_OK)
					ok++;
			crnet_client_free(c);
		});
	for (auto &t : th)
		t.join();
	EXPECT_EQ(ok, 40);
}

TEST_F(ClientTest, CompletionQueue)
{
	crnet_queue *q = crnet_queue_new();
	ASSERT_NE(q, nullptr);
	int fd = crnet_queue_fd(q);
	ASSERT_GE(fd, 0);

	crnet_transfer *t;
	void *ud;
	EXPECT_EQ(crnet_queue_pop(q, &t, &ud, 0), CRNET_E_STATE) << "nothing pending";

	const int kN = 50;
	std::vector<int> tags(kN);
	for (int i = 0; i < kN; i++) {
		tags[i] = i;
		crnet_request *req = crnet_request_new(
			"GET", HttpsUrl("/echo?q=" + std::to_string(i)).c_str());
		ASSERT_EQ(crnet_queue_start(q, client_, req, &tags[i]), CRNET_OK);
		crnet_request_free(req);
	}
	EXPECT_EQ(crnet_queue_pending(q), static_cast<size_t>(kN));

	std::set<int> seen;
	while (crnet_queue_pending(q) > 0) {
		struct pollfd pfd = {fd, POLLIN, 0};
		ASSERT_EQ(poll(&pfd, 1, 10000), 1);
		while (crnet_queue_pop(q, &t, &ud, 0) == CRNET_OK) {
			int i = *static_cast<int *>(ud);
			EXPECT_EQ(crnet_transfer_wait(t, 0), CRNET_OK);
			json e = json::parse(crnet_response_body(crnet_transfer_response(t), nullptr));
			EXPECT_EQ(e["target"], "/echo?q=" + std::to_string(i));
			seen.insert(i);
			crnet_transfer_free(t);
		}
	}
	EXPECT_EQ(seen.size(), static_cast<size_t>(kN));
	struct pollfd pfd = {fd, POLLIN, 0};
	EXPECT_EQ(poll(&pfd, 1, 0), 0) << "fd drained";
	crnet_queue_free(q);
}

TEST_F(ClientTest, CompletionQueueBlockingPopAndTimeout)
{
	crnet_queue *q = crnet_queue_new();
	crnet_request *req = crnet_request_new("GET", HttpsUrl("/delay/300").c_str());
	crnet_queue_start(q, client_, req, nullptr);
	crnet_request_free(req);
	crnet_transfer *t = nullptr;
	EXPECT_EQ(crnet_queue_pop(q, &t, nullptr, 20), CRNET_E_TIMEOUT);
	EXPECT_EQ(crnet_queue_pop(q, &t, nullptr, UINT64_MAX), CRNET_OK);
	ASSERT_NE(t, nullptr);
	crnet_transfer_free(t);
	crnet_queue_free(q);
}

TEST_F(ClientTest, CompletionQueueFreeWithRunningTransfers)
{
	crnet_queue *q = crnet_queue_new();
	for (int i = 0; i < 5; i++) {
		crnet_request *req = crnet_request_new("GET", HttpsUrl("/delay/5000").c_str());
		crnet_queue_start(q, client_, req, nullptr);
		crnet_request_free(req);
	}
	auto t0 = std::chrono::steady_clock::now();
	crnet_queue_free(q);
	EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::seconds(3));
}

TEST_F(ClientTest, ManyThreadsWaitOnOneTransfer)
{
	crnet_transfer *t = StartGet(client_, HttpsUrl("/delay/300"), nullptr, nullptr);
	std::atomic<int> ok{0}, timed_out{0};
	std::vector<std::thread> th;
	for (int i = 0; i < 16; i++)
		th.emplace_back([&, i] {
			// Half wait forever, half poll with short timeouts first.
			if (i % 2 && crnet_transfer_wait(t, 10) == CRNET_E_TIMEOUT)
				timed_out++;
			if (crnet_transfer_wait(t, i % 2 ? 5000 : UINT64_MAX) == CRNET_OK)
				ok++;
		});
	for (auto &x : th)
		x.join();
	EXPECT_EQ(ok, 16);
	EXPECT_EQ(timed_out, 8);
	// Late waiters return immediately.
	auto t0 = std::chrono::steady_clock::now();
	EXPECT_EQ(crnet_transfer_wait(t, UINT64_MAX), CRNET_OK);
	EXPECT_EQ(crnet_transfer_wait(t, 0), CRNET_OK);
	EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::milliseconds(50));
	crnet_transfer_free(t);
}
