// SPDX-License-Identifier: BSD-3-Clause
/*
 * crget - a tiny curl-like tool on top of crnet.
 *
 *   crget [-x proxy] [-X method] [-H "Name: value"]... [-d body]
 *         [-m navigate|fetch|raw] [-k] [-i] [-L max] [-t ms] URL
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <crnet/crnet.h>

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
	if (!strcmp(mode, "fetch"))
		crnet_request_set_mode(req, CRNET_MODE_FETCH);
	else if (!strcmp(mode, "raw"))
		crnet_request_set_mode(req, CRNET_MODE_RAW);
	crnet_request_set_max_redirects(req, max_redirects);
	if (timeout)
		crnet_request_set_timeout(req, timeout);

	crnet_response *resp = NULL;
	int rc = crnet_perform(client, req, &resp);
	if (rc != CRNET_OK) {
		fprintf(stderr, "error: %s (%s)\n", crnet_strerror(rc),
			crnet_response_error_string(resp));
	} else {
		if (show_headers) {
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
		size_t len;
		const char *data = crnet_response_body(resp, &len);
		fwrite(data, 1, len, stdout);
	}

	crnet_response_free(resp);
	crnet_request_free(req);
	crnet_client_free(client);
	return rc == CRNET_OK ? 0 : 1;
}
