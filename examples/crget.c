// SPDX-License-Identifier: BSD-3-Clause
/*
 * crget - a tiny curl-like tool on top of crhttp.
 *
 *   crget [-x proxy] [-X method] [-H "Name: value"]... [-d body]
 *         [-m navigate|fetch|raw] [-k] [-i] [-L max] [-t ms] URL
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <crhttp/crhttp.h>

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s [-x proxy] [-X method] [-H 'Name: value']... "
		"[-d body] [-m navigate|fetch|raw] [-k] [-i] [-L max] "
		"[-t timeout_ms] [-n netlog.json] URL\n",
		argv0);
	exit(2);
}

int main(int argc, char *argv[])
{
	const char *proxy = NULL, *method = "GET", *body = NULL;
	const char *mode = "navigate", *netlog = NULL;
	const char *headers[64];
	int nheaders = 0, insecure = 0, show_headers = 0, max_redirects = 20;
	unsigned long timeout = 0;
	int c;

	while ((c = getopt(argc, argv, "x:X:H:d:m:kiL:t:n:")) != -1) {
		switch (c) {
		case 'x': proxy = optarg; break;
		case 'X': method = optarg; break;
		case 'H':
			if (nheaders < 64)
				headers[nheaders++] = optarg;
			break;
		case 'd': body = optarg; break;
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

	crhttp_client_config *cfg = crhttp_client_config_new();
	if (proxy && crhttp_client_config_set_proxy(cfg, proxy)) {
		fprintf(stderr, "bad proxy: %s\n", proxy);
		return 2;
	}
	crhttp_client_config_set_insecure(cfg, insecure);
	if (netlog)
		crhttp_client_config_set_netlog(cfg, netlog);
	crhttp_client *client = crhttp_client_new(cfg);
	crhttp_client_config_free(cfg);
	if (!client) {
		fprintf(stderr, "crhttp_client_new failed\n");
		return 1;
	}

	crhttp_request *req = crhttp_request_new(method, argv[optind]);
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
		crhttp_request_set_header(req, h, *colon ? colon : NULL);
		free(h);
	}
	if (body)
		crhttp_request_set_body(req, body, strlen(body));
	if (!strcmp(mode, "fetch"))
		crhttp_request_set_mode(req, CRHTTP_MODE_FETCH);
	else if (!strcmp(mode, "raw"))
		crhttp_request_set_mode(req, CRHTTP_MODE_RAW);
	crhttp_request_set_max_redirects(req, max_redirects);
	if (timeout)
		crhttp_request_set_timeout(req, timeout);

	crhttp_response *resp = NULL;
	int rc = crhttp_perform(client, req, &resp);
	if (rc != CRHTTP_OK) {
		fprintf(stderr, "error: %s (%s)\n", crhttp_strerror(rc),
			crhttp_response_error_string(resp));
	} else {
		if (show_headers) {
			fprintf(stderr, "%s %d %s\n", crhttp_response_protocol(resp),
				crhttp_response_status(resp),
				crhttp_response_status_text(resp));
			for (size_t i = 0; i < crhttp_response_header_count(resp); i++) {
				const char *k, *v;
				crhttp_response_header_at(resp, i, &k, &v);
				fprintf(stderr, "%s: %s\n", k, v);
			}
			fprintf(stderr, "\n");
		}
		size_t len;
		const char *data = crhttp_response_body(resp, &len);
		fwrite(data, 1, len, stdout);
	}

	crhttp_response_free(resp);
	crhttp_request_free(req);
	crhttp_client_free(client);
	return rc == CRHTTP_OK ? 0 : 1;
}
