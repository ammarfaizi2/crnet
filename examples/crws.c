// SPDX-License-Identifier: BSD-3-Clause
/*
 * crws - a tiny wscat-like WebSocket client on top of crnet.
 *
 *   crws [-x proxy] [-k] [-p protocols] [-H "Name: value"]... URL
 *
 * Lines read from stdin are sent as text messages; received messages
 * are printed (binary ones as a size). After EOF on stdin, the
 * connection is closed cleanly once no message arrived for a second
 * (like Chrome, crnet discards messages that arrive after it sent its
 * Close frame). Shows the queue mode: stdin and the socket's fd
 * are multiplexed with poll() on one thread, no callbacks involved.
 */
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <crnet/crnet.h>

static void usage(const char *argv0)
{
	fprintf(stderr, "usage: %s [-x proxy] [-k] [-p protocols] "
			"[-H 'Name: value']... URL\n", argv0);
	exit(2);
}

static int drain(crnet_ws *ws)
{
	crnet_ws_msg_type type;
	void *data;
	size_t len;
	int rc;

	while ((rc = crnet_ws_recv(ws, &type, &data, &len, 0)) == CRNET_OK) {
		if (type == CRNET_WS_TEXT)
			printf("< %.*s\n", (int)len, (char *)data);
		else
			printf("< [binary, %zu bytes]\n", len);
		fflush(stdout);
		crnet_free(data);
	}
	return rc;
}

int main(int argc, char *argv[])
{
	const char *proxy = NULL, *protocols = NULL;
	const char *headers[32];
	int nheaders = 0, insecure = 0, c;

	while ((c = getopt(argc, argv, "x:kp:H:")) != -1) {
		switch (c) {
		case 'x': proxy = optarg; break;
		case 'k': insecure = 1; break;
		case 'p': protocols = optarg; break;
		case 'H':
			if (nheaders < 32)
				headers[nheaders++] = optarg;
			break;
		default: usage(argv[0]);
		}
	}
	if (optind != argc - 1)
		usage(argv[0]);

	crnet_client_config *cfg = crnet_client_config_new();
	if (proxy && crnet_client_config_set_proxy(cfg, proxy)) {
		fprintf(stderr, "bad proxy: %s\n", proxy);
		return 2;
	}
	crnet_client_config_set_insecure(cfg, insecure);
	crnet_client *client = crnet_client_new(cfg);
	crnet_client_config_free(cfg);

	crnet_request *req = crnet_request_new(NULL, argv[optind]);
	if (!client || !req) {
		fprintf(stderr, "bad URL\n");
		return 2;
	}
	for (int i = 0; i < nheaders; i++) {
		char *h = strdup(headers[i]), *v = strchr(h, ':');
		if (v) {
			*v++ = '\0';
			while (*v == ' ')
				v++;
			crnet_request_set_header(req, h, *v ? v : NULL);
		}
		free(h);
	}

	crnet_ws *ws = crnet_ws_connect(client, req, protocols, NULL, NULL);
	crnet_request_free(req);
	if (!ws || crnet_ws_wait_open(ws, 30000) != CRNET_OK) {
		fprintf(stderr, "connect failed: %s\n",
			ws ? crnet_ws_close_reason(ws) : "invalid arguments");
		return 1;
	}
	fprintf(stderr, "connected (protocol \"%s\", extensions \"%s\")\n",
		crnet_ws_protocol(ws), crnet_ws_extensions(ws));

	struct pollfd pfd[2] = {
		{ STDIN_FILENO, POLLIN, 0 },
		{ crnet_ws_fd(ws), POLLIN, 0 },
	};
	char line[65536];
	int stdin_open = 1;

	for (;;) {
		/* After stdin's EOF, linger for late replies. */
		int n = poll(pfd, 2, stdin_open ? -1 : 1000);
		if (n < 0)
			break;
		if (n == 0) {
			crnet_ws_close(ws, 1000, NULL);
			continue;
		}
		if (pfd[1].revents && drain(ws) == CRNET_E_STATE)
			break;		/* closed and drained */
		if (stdin_open && pfd[0].revents) {
			if (!fgets(line, sizeof(line), stdin)) {
				stdin_open = 0;
				pfd[0].fd = -1;
				continue;
			}
			line[strcspn(line, "\n")] = '\0';
			if (crnet_ws_send(ws, CRNET_WS_TEXT, line, strlen(line)))
				fprintf(stderr, "send failed (not open)\n");
		}
	}

	int rc = crnet_ws_wait_closed(ws, 0);
	fprintf(stderr, "closed: %s, code %d \"%s\"\n", crnet_strerror(rc),
		crnet_ws_close_code(ws), crnet_ws_close_reason(ws));
	crnet_ws_free(ws);
	crnet_client_free(client);
	return rc == CRNET_OK ? 0 : 1;
}
