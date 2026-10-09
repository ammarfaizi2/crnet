// SPDX-License-Identifier: BSD-3-Clause
//
// Fetch many URLs concurrently on one client with the C++ wrapper, then
// the same with a completion queue polled from this thread.
//
//   parallel URL [URL...]
//
#include <poll.h>

#include <iostream>
#include <vector>

#include <crhttp/crhttp.hpp>

int main(int argc, char *argv[])
{
	if (argc < 2) {
		std::cerr << "usage: " << argv[0] << " URL [URL...]\n";
		return 2;
	}

	crhttp::Client client;
	std::cout << "crhttp " << crhttp::version() << "\n";

	// 1) Asynchronous transfers with callbacks (network thread).
	std::vector<crhttp::Transfer> transfers;
	for (int i = 1; i < argc; i++) {
		crhttp::Handlers h;
		std::string url = argv[i];
		h.on_complete = [url](int err, const crhttp::ResponseView &r) {
			if (err)
				std::cout << "[cb]    " << url << " -> " << crhttp_strerror(err)
					  << " " << r.error_string() << "\n";
			else
				std::cout << "[cb]    " << url << " -> " << r.status() << " "
					  << r.protocol() << " " << r.body().size() << " bytes\n";
		};
		transfers.push_back(client.start(crhttp::Request(url), std::move(h)));
	}
	for (auto &t : transfers)
		t.wait();
	transfers.clear();

	// 2) Completion queue: results are collected on this thread.
	crhttp_queue *q = crhttp_queue_new();
	for (int i = 1; i < argc; i++)
		crhttp_queue_start(q, client.get(), crhttp::Request(argv[i]).get(), argv[i]);

	struct pollfd pfd = {crhttp_queue_fd(q), POLLIN, 0};
	while (crhttp_queue_pending(q) > 0) {
		poll(&pfd, 1, -1);
		crhttp_transfer *t;
		void *ud;
		while (crhttp_queue_pop(q, &t, &ud, 0) == CRHTTP_OK) {
			crhttp::ResponseView r(crhttp_transfer_response(t));
			std::cout << "[queue] " << static_cast<const char *>(ud) << " -> "
				  << r.status() << " " << r.protocol() << "\n";
			crhttp_transfer_free(t);
		}
	}
	crhttp_queue_free(q);
	return 0;
}
