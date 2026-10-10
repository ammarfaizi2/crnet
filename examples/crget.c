// SPDX-License-Identifier: BSD-3-Clause
/*
 * crget - a tiny curl-like tool on top of crnet.
 *
 *   crget [-x proxy] [-X method] [-H "Name: value"]... [-d body]
 *         [-T file|-] [-o file] [-m navigate|fetch|raw] [-k] [-i]
 *         [-L max] [-t ms] URL
 *
 * Bodies are streamed both ways, so files of any size use constant
 * memory: -T uploads a file (crnet_request_set_body_file()), or stdin
 * with "-T -" (crnet_transfer_write()); the response is read with
 * crnet_transfer_read() and written to stdout or the -o file.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <crnet/crnet.h>

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s [-x proxy] [-X method] [-H 'Name: value']... "
		"[-d body] [-T file|-] [-o file] [-m navigate|fetch|raw] [-k] "
		"[-i] [-L max] [-t timeout_ms] [-n netlog.json] URL\n",
		argv0);
	exit(2);
}

int main(int argc, char *argv[])
{
	const char *proxy = NULL, *method = NULL, *body = NULL;
	const char *mode = "navigate", *netlog = NULL;
	const char *upload = NULL, *output = NULL;
	const char *headers[64];
	int nheaders = 0, insecure = 0, show_headers = 0, max_redirects = 20;
	unsigned long timeout = 0;
	int c;

	while ((c = getopt(argc, argv, "x:X:H:d:T:o:m:kiL:t:n:")) != -1) {
		switch (c) {
		case 'x': proxy = optarg; break;
		case 'X': method = optarg; break;
		case 'H':
			if (nheaders < 64)
				headers[nheaders++] = optarg;
			break;
		case 'd': body = optarg; break;
		case 'T': upload = optarg; break;
		case 'o': output = optarg; break;
		case 'm': mode = optarg; break;
		case 'k': insecure = 1; break;
		case 'i': show_headers = 1; break;
		case 'L': max_redirects = atoi(optarg); break;
		case 't': timeout = strtoul(optarg, NULL, 10); break;
		case 'n': netlog = optarg; break;
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
	if (netlog)
		crnet_client_config_set_netlog(cfg, netlog);
	crnet_client *client = crnet_client_new(cfg);
	crnet_client_config_free(cfg);
	if (!client) {
		fprintf(stderr, "crnet_client_new failed\n");
		return 1;
	}

	if (!method)
		method = upload ? "PUT" : body ? "POST" : "GET";
	crnet_request *req = crnet_request_new(method, argv[optind]);
	if (!req) {
		fprintf(stderr, "bad URL or method\n");
		return 2;
	}
	for (int i = 0; i < nheaders; i++) {
		char *h = strdup(headers[i]);
		char *colon = strchr(h, ':');
		if (!colon) {
			fprintf(stderr, "bad header: %s\n", headers[i]);
			return 2;
		}
		*colon++ = '\0';
		while (*colon == ' ')
			colon++;
		/* "Name:" with no value removes a default header. */
		crnet_request_set_header(req, h, *colon ? colon : NULL);
		free(h);
	}
	if (body)
		crnet_request_set_body(req, body, strlen(body));
	if (upload && !strcmp(upload, "-"))
		crnet_request_set_body_stream(req, CRNET_LENGTH_UNKNOWN);
	else if (upload)
		crnet_request_set_body_file(req, upload, 0, UINT64_MAX);
	if (!strcmp(mode, "fetch"))
		crnet_request_set_mode(req, CRNET_MODE_FETCH);
	else if (!strcmp(mode, "raw"))
		crnet_request_set_mode(req, CRNET_MODE_RAW);
	crnet_request_set_max_redirects(req, max_redirects);
	if (timeout)
		crnet_request_set_timeout(req, timeout);
	crnet_request_set_response_stream(req, 1);

	int out = STDOUT_FILENO;
	if (output && strcmp(output, "-")) {
		out = open(output, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (out < 0) {
			perror(output);
			return 1;
		}
	}

	crnet_transfer *t = crnet_start(client, req, NULL, NULL);
	crnet_request_free(req);
	if (!t) {
		fprintf(stderr, "crnet_start failed\n");
		return 1;
	}

	/* Feed stdin as the request body ("-T -"). */
	static char buf[1 << 16];
	if (upload && !strcmp(upload, "-")) {
		ssize_t n;
		while ((n = read(STDIN_FILENO, buf, sizeof(buf))) > 0)
			if (crnet_transfer_write(t, buf, n, NULL, UINT64_MAX))
				break;
		crnet_transfer_write_end(t);
	}

	int rc = crnet_transfer_wait_response(t, UINT64_MAX);
	const crnet_response *resp = crnet_transfer_response(t);
	if (rc == CRNET_OK && show_headers) {
		fprintf(stderr, "%s %d %s\n", crnet_response_protocol(resp),
			crnet_response_status(resp),
			crnet_response_status_text(resp));
		for (size_t i = 0; i < crnet_response_header_count(resp); i++) {
			const char *k, *v;
			crnet_response_header_at(resp, i, &k, &v);
			fprintf(stderr, "%s: %s\n", k, v);
		}
		fprintf(stderr, "\n");
	}
	while (rc == CRNET_OK) {
		size_t n;
		rc = crnet_transfer_read(t, buf, sizeof(buf), &n, UINT64_MAX);
		if (rc != CRNET_OK || n == 0)
			break;
		for (size_t off = 0; off < n;) {
			ssize_t w = write(out, buf + off, n - off);
			if (w <= 0) {
				perror("write");
				rc = CRNET_E_CANCELED;
				break;
			}
			off += w;
		}
	}
	if (rc != CRNET_OK)
		fprintf(stderr, "error: %s (%s)\n", crnet_strerror(rc),
			crnet_response_error_string(resp));

	if (out != STDOUT_FILENO)
		close(out);
	crnet_transfer_free(t);
	crnet_client_free(client);
	return rc == CRNET_OK ? 0 : 1;
}
