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

#include <crnet/crnet.hpp>

int main(int argc, char *argv[])
{
	if (argc < 2) {
		std::cerr << "usage: " << argv[0] << " URL [URL...]\n";
		return 2;
	}

	crnet::Client client;
	std::cout << "crnet " << crnet::version() << "\n";

	// 1) Asynchronous transfers with callbacks (network thread).
	std::vector<crnet::Transfer> transfers;
	for (int i = 1; i < argc; i++) {
		crnet::Handlers h;
		std::string url = argv[i];
		h.on_complete = [url](int err, const crnet::ResponseView &r) {
			if (err)
				std::cout << "[cb]    " << url << " -> " << crnet_strerror(err)
					  << " " << r.error_string() << "\n";
			else
				std::cout << "[cb]    " << url << " -> " << r.status() << " "
					  << r.protocol() << " " << r.body().size() << " bytes\n";
		};
		transfers.push_back(client.start(crnet::Request(url), std::move(h)));
	}
	for (auto &t : transfers)
		t.wait();
	transfers.clear();

	// 2) Completion queue: results are collected on this thread.
	crnet_queue *q = crnet_queue_new();
	for (int i = 1; i < argc; i++)
		crnet_queue_start(q, client.get(), crnet::Request(argv[i]).get(), argv[i]);

	struct pollfd pfd = {crnet_queue_fd(q), POLLIN, 0};
	while (crnet_queue_pending(q) > 0) {
		poll(&pfd, 1, -1);
		crnet_transfer *t;
		void *ud;
		while (crnet_queue_pop(q, &t, &ud, 0) == CRNET_OK) {
			crnet::ResponseView r(crnet_transfer_response(t));
			std::cout << "[queue] " << static_cast<const char *>(ud) << " -> "
				  << r.status() << " " << r.protocol() << "\n";
			crnet_transfer_free(t);
		}
	}
	crnet_queue_free(q);
	return 0;
}
