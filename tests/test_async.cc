// SPDX-License-Identifier: BSD-3-Clause
//
// Asynchronous transfers, cancellation, flow control, concurrency,
// client shutdown and the completion queue.
//
#include <poll.h>

#include <atomic>
#include <chrono>
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
	crhttp_client *client = nullptr;
	bool try_nested_perform = false;

	void Add(const std::string &e)
	{
		std::lock_guard<std::mutex> lk(mu);
		events.push_back(e);
	}
};

int OnResponse(crhttp_transfer *t, const crhttp_response *resp, void *ud)
{
	auto *r = static_cast<Recorder *>(ud);
	r->Add("response");
	r->cb_thread = std::this_thread::get_id();
	r->status = crhttp_response_status(resp);
	if (r->pause_on_response)
		crhttp_transfer_pause(t);
	if (r->try_nested_perform) {
		crhttp_request *req = crhttp_request_new("GET", HttpsUrl("/echo").c_str());
		crhttp_response *x = nullptr;
		r->nested_perform_rc = crhttp_perform(r->client, req, &x);
		crhttp_response_free(x);
		crhttp_request_free(req);
	}
	return 0;
}

int OnData(crhttp_transfer *, const void *d, size_t n, void *ud)
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

void OnComplete(crhttp_transfer *, int err, const crhttp_response *, void *ud)
{
	auto *r = static_cast<Recorder *>(ud);
	r->Add("complete");
	r->error = err;
	r->completes++;
}

const crhttp_callbacks kAll = {OnResponse, OnData, OnComplete};
const crhttp_callbacks kNoData = {OnResponse, nullptr, OnComplete};

crhttp_transfer *StartGet(crhttp_client *c, const std::string &url,
			  const crhttp_callbacks *cb, void *ud)
{
	crhttp_request *req = crhttp_request_new("GET", url.c_str());
	crhttp_transfer *t = crhttp_start(c, req, cb, ud);
	crhttp_request_free(req);  // the transfer has its own copy
	return t;
}

} // namespace

TEST_F(ClientTest, CallbackSequence)
{
	Recorder rec;
	crhttp_transfer *t = StartGet(client_, HttpsUrl("/bytes/300000"), &kAll, &rec);
	ASSERT_NE(t, nullptr);
	EXPECT_EQ(crhttp_transfer_wait(t, UINT64_MAX), CRHTTP_OK);
	EXPECT_TRUE(crhttp_transfer_done(t));
	EXPECT_EQ(rec.events, (std::vector<std::string>{"response", "data", "complete"}));
	EXPECT_EQ(rec.completes, 1);
	EXPECT_EQ(rec.error, CRHTTP_OK);
	EXPECT_EQ(rec.status, 200);
	EXPECT_EQ(rec.data.size(), 300000u);
	EXPECT_NE(rec.cb_thread, std::this_thread::get_id());
	// Streamed bodies are not accumulated.
	size_t n = 1;
	crhttp_response_body(crhttp_transfer_response(t), &n);
	EXPECT_EQ(n, 0u);
	crhttp_transfer_free(t);
}

TEST_F(ClientTest, BufferedWhenNoDataCallback)
{
	Recorder rec;
	crhttp_transfer *t = StartGet(client_, HttpsUrl("/bytes/1000"), &kNoData, &rec);
	ASSERT_EQ(crhttp_transfer_wait(t, UINT64_MAX), CRHTTP_OK);
	crhttp_response *resp = crhttp_transfer_take_response(t);
	ASSERT_NE(resp, nullptr);
	EXPECT_EQ(crhttp_transfer_response(t), nullptr);
	size_t n = 0;
	crhttp_response_body(resp, &n);
	EXPECT_EQ(n, 1000u);
	crhttp_response_free(resp);
	crhttp_transfer_free(t);
}

TEST_F(ClientTest, NoCallbacksAtAll)
{
	crhttp_transfer *t = StartGet(client_, HttpsUrl("/echo"), nullptr, nullptr);
	ASSERT_NE(t, nullptr);
	EXPECT_EQ(crhttp_transfer_wait(t, UINT64_MAX), CRHTTP_OK);
	EXPECT_EQ(crhttp_response_status(crhttp_transfer_response(t)), 200);
	crhttp_transfer_free(t);
}

TEST_F(ClientTest, WaitTimeoutDoesNotStopTransfer)
{
	crhttp_transfer *t = StartGet(client_, HttpsUrl("/delay/600"), nullptr, nullptr);
	EXPECT_EQ(crhttp_transfer_wait(t, 0), CRHTTP_E_TIMEOUT);
	EXPECT_EQ(crhttp_transfer_wait(t, 50), CRHTTP_E_TIMEOUT);
	EXPECT_FALSE(crhttp_transfer_done(t));
	EXPECT_EQ(crhttp_transfer_take_response(t), nullptr);
	EXPECT_EQ(crhttp_transfer_wait(t, UINT64_MAX), CRHTTP_OK);
	crhttp_transfer_free(t);
}

TEST_F(ClientTest, CancelMidStream)
{
	Recorder rec;
	crhttp_transfer *t = StartGet(
		client_, HttpsUrl("/stream/1000000?chunk=1000&delay_ms=5"), &kAll, &rec);
	while (true) {
		std::lock_guard<std::mutex> lk(rec.mu);
		if (rec.data.size() > 5000)
			break;
	}
	crhttp_transfer_cancel(t);
	EXPECT_EQ(crhttp_transfer_wait(t, UINT64_MAX), CRHTTP_E_CANCELED);
	EXPECT_EQ(rec.error, CRHTTP_E_CANCELED);
	EXPECT_EQ(rec.completes, 1);
	EXPECT_LT(rec.data.size(), 1000000u);
	crhttp_transfer_cancel(t);  // no-op after completion
	EXPECT_EQ(rec.completes, 1);
	crhttp_transfer_free(t);
}

TEST_F(ClientTest, CancelBeforeResponse)
{
	Recorder rec;
	crhttp_transfer *t = StartGet(client_, HttpsUrl("/delay/2000"), &kAll, &rec);
	crhttp_transfer_cancel(t);
	EXPECT_EQ(crhttp_transfer_wait(t, 5000), CRHTTP_E_CANCELED);
	EXPECT_EQ(rec.events, std::vector<std::string>{"complete"});
	crhttp_transfer_free(t);
}

TEST_F(ClientTest, DataCallbackAbort)
{
	Recorder rec;
	rec.abort_on_data_after = 1;
	crhttp_transfer *t = StartGet(client_, HttpsUrl("/bytes/500000"), &kAll, &rec);
	EXPECT_EQ(crhttp_transfer_wait(t, UINT64_MAX), CRHTTP_E_CALLBACK);
	EXPECT_EQ(rec.completes, 1);
	crhttp_transfer_free(t);
}

TEST_F(ClientTest, ResponseCallbackAbort)
{
	crhttp_callbacks cb = kAll;
	cb.on_response = [](crhttp_transfer *, const crhttp_response *, void *) { return 1; };
	Recorder rec;
	crhttp_transfer *t = StartGet(client_, HttpsUrl("/bytes/500000"), &cb, &rec);
	EXPECT_EQ(crhttp_transfer_wait(t, UINT64_MAX), CRHTTP_E_CALLBACK);
	EXPECT_TRUE(rec.data.empty());
	crhttp_transfer_free(t);
}

TEST_F(ClientTest, PauseAndResume)
{
	Recorder rec;
	rec.pause_on_response = true;
	crhttp_transfer *t = StartGet(client_, HttpsUrl("/bytes/200000"), &kAll, &rec);
	std::this_thread::sleep_for(std::chrono::milliseconds(300));
	{
		std::lock_guard<std::mutex> lk(rec.mu);
		EXPECT_TRUE(rec.data.empty()) << "paused before the first read";
	}
	EXPECT_FALSE(crhttp_transfer_done(t));
	crhttp_transfer_resume(t);
	EXPECT_EQ(crhttp_transfer_wait(t, UINT64_MAX), CRHTTP_OK);
	EXPECT_EQ(rec.data.size(), 200000u);
	crhttp_transfer_free(t);
}

TEST_F(ClientTest, FreeWhileRunningCancelsAndWaits)
{
	auto *rec = new Recorder;
	crhttp_transfer *t = StartGet(client_, HttpsUrl("/delay/3000"), &kAll, rec);
	auto t0 = std::chrono::steady_clock::now();
	crhttp_transfer_free(t);
	auto dt = std::chrono::steady_clock::now() - t0;
	// on_complete has run by now; user data can go.
	EXPECT_EQ(rec->completes, 1);
	EXPECT_EQ(rec->error, CRHTTP_E_CANCELED);
	EXPECT_LT(dt, std::chrono::milliseconds(2000));
	delete rec;
}

TEST_F(ClientTest, FreeFromOnComplete)
{
	std::atomic<bool> freed{false};
	crhttp_callbacks cb = {};
	cb.on_complete = [](crhttp_transfer *t, int, const crhttp_response *, void *ud) {
		crhttp_transfer_free(t);
		static_cast<std::atomic<bool> *>(ud)->store(true);
	};
	crhttp_transfer *t = StartGet(client_, HttpsUrl("/echo"), &cb, &freed);
	ASSERT_NE(t, nullptr);
	for (int i = 0; i < 500 && !freed; i++)
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	EXPECT_TRUE(freed);
}

TEST_F(ClientTest, BlockingCallInsideCallbackIsRefused)
{
	Recorder rec;
	rec.try_nested_perform = true;
	rec.client = client_;
	crhttp_transfer *t = StartGet(client_, HttpsUrl("/echo"), &kAll, &rec);
	EXPECT_EQ(crhttp_transfer_wait(t, UINT64_MAX), CRHTTP_OK);
	EXPECT_EQ(rec.nested_perform_rc, CRHTTP_E_WOULD_DEADLOCK);
	crhttp_transfer_free(t);
}

TEST_F(ClientTest, ManyConcurrentRequestsShareOneHttp2Connection)
{
	const int kN = 200;
	std::vector<crhttp_transfer *> ts;
	for (int i = 0; i < kN; i++)
		ts.push_back(StartGet(client_, HttpsUrl("/delay/50?i=" + std::to_string(i)),
				      nullptr, nullptr));
	std::set<int> ports;
	for (int i = 0; i < kN; i++) {
		ASSERT_EQ(crhttp_transfer_wait(ts[i], UINT64_MAX), CRHTTP_OK);
		json e = json::parse(crhttp_response_body(crhttp_transfer_response(ts[i]), nullptr));
		EXPECT_EQ(e["target"], "/delay/50?i=" + std::to_string(i));
		ports.insert(e["peer_port"].get<int>());
		crhttp_transfer_free(ts[i]);
	}
	EXPECT_EQ(ports.size(), 1u) << "all streams multiplexed on one connection";
}

TEST_F(ClientTest, ConcurrentRequestsAreParallel)
{
	// 40 x 500 ms server delay must not take 20 s.
	auto t0 = std::chrono::steady_clock::now();
	std::vector<crhttp_transfer *> ts;
	for (int i = 0; i < 40; i++)
		ts.push_back(StartGet(client_, HttpsUrl("/delay/500"), nullptr, nullptr));
	for (auto *t : ts) {
		EXPECT_EQ(crhttp_transfer_wait(t, UINT64_MAX), CRHTTP_OK);
		crhttp_transfer_free(t);
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
				if (r.rc == CRHTTP_OK && r.echo()["target"] ==
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
	crhttp_client *c = NewTestClient();
	Recorder rec[5];
	crhttp_transfer *ts[5];
	for (int i = 0; i < 5; i++)
		ts[i] = StartGet(c, HttpsUrl("/delay/5000"), &kAll, &rec[i]);
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	crhttp_client_free(c);
	for (int i = 0; i < 5; i++) {
		EXPECT_EQ(rec[i].completes, 1);
		EXPECT_EQ(rec[i].error, CRHTTP_E_SHUTDOWN);
		// Handles outlive the client.
		EXPECT_TRUE(crhttp_transfer_done(ts[i]));
		EXPECT_EQ(crhttp_transfer_wait(ts[i], 0), CRHTTP_E_SHUTDOWN);
		crhttp_transfer_free(ts[i]);
	}
}

TEST(Shutdown, ManyClientsInParallel)
{
	std::vector<std::thread> th;
	std::atomic<int> ok{0};
	for (int i = 0; i < 8; i++)
		th.emplace_back([&] {
			crhttp_client *c = NewTestClient();
			for (int j = 0; j < 5; j++)
				if (Get(c, HttpsUrl("/echo")).rc == CRHTTP_OK)
					ok++;
			crhttp_client_free(c);
		});
	for (auto &t : th)
		t.join();
	EXPECT_EQ(ok, 40);
}

TEST_F(ClientTest, CompletionQueue)
{
	crhttp_queue *q = crhttp_queue_new();
	ASSERT_NE(q, nullptr);
	int fd = crhttp_queue_fd(q);
	ASSERT_GE(fd, 0);

	crhttp_transfer *t;
	void *ud;
	EXPECT_EQ(crhttp_queue_pop(q, &t, &ud, 0), CRHTTP_E_STATE) << "nothing pending";

	const int kN = 50;
	std::vector<int> tags(kN);
	for (int i = 0; i < kN; i++) {
		tags[i] = i;
		crhttp_request *req = crhttp_request_new(
			"GET", HttpsUrl("/echo?q=" + std::to_string(i)).c_str());
		ASSERT_EQ(crhttp_queue_start(q, client_, req, &tags[i]), CRHTTP_OK);
		crhttp_request_free(req);
	}
	EXPECT_EQ(crhttp_queue_pending(q), static_cast<size_t>(kN));

	std::set<int> seen;
	while (crhttp_queue_pending(q) > 0) {
		struct pollfd pfd = {fd, POLLIN, 0};
		ASSERT_EQ(poll(&pfd, 1, 10000), 1);
		while (crhttp_queue_pop(q, &t, &ud, 0) == CRHTTP_OK) {
			int i = *static_cast<int *>(ud);
			EXPECT_EQ(crhttp_transfer_wait(t, 0), CRHTTP_OK);
			json e = json::parse(crhttp_response_body(crhttp_transfer_response(t), nullptr));
			EXPECT_EQ(e["target"], "/echo?q=" + std::to_string(i));
			seen.insert(i);
			crhttp_transfer_free(t);
		}
	}
	EXPECT_EQ(seen.size(), static_cast<size_t>(kN));
	struct pollfd pfd = {fd, POLLIN, 0};
	EXPECT_EQ(poll(&pfd, 1, 0), 0) << "fd drained";
	crhttp_queue_free(q);
}

TEST_F(ClientTest, CompletionQueueBlockingPopAndTimeout)
{
	crhttp_queue *q = crhttp_queue_new();
	crhttp_request *req = crhttp_request_new("GET", HttpsUrl("/delay/300").c_str());
	crhttp_queue_start(q, client_, req, nullptr);
	crhttp_request_free(req);
	crhttp_transfer *t = nullptr;
	EXPECT_EQ(crhttp_queue_pop(q, &t, nullptr, 20), CRHTTP_E_TIMEOUT);
	EXPECT_EQ(crhttp_queue_pop(q, &t, nullptr, UINT64_MAX), CRHTTP_OK);
	ASSERT_NE(t, nullptr);
	crhttp_transfer_free(t);
	crhttp_queue_free(q);
}

TEST_F(ClientTest, CompletionQueueFreeWithRunningTransfers)
{
	crhttp_queue *q = crhttp_queue_new();
	for (int i = 0; i < 5; i++) {
		crhttp_request *req = crhttp_request_new("GET", HttpsUrl("/delay/5000").c_str());
		crhttp_queue_start(q, client_, req, nullptr);
		crhttp_request_free(req);
	}
	auto t0 = std::chrono::steady_clock::now();
	crhttp_queue_free(q);
	EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::seconds(3));
}
