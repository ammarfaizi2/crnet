/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Compiled as C (not C++) to keep crhttp.h valid C. Runs one request
 * against the URL given on the command line with an insecure client.
 */
#include <stdio.h>
#include <string.h>

#include <crhttp/crhttp.h>

int main(int argc, char *argv[])
{
	crhttp_client_config *cfg;
	crhttp_client *c;
	crhttp_request *req;
	crhttp_response *resp = NULL;
	int rc;

	if (argc != 2)
		return 2;
	printf("%s\n", crhttp_version());
	cfg = crhttp_client_config_new();
	crhttp_client_config_set_insecure(cfg, 1);
	c = crhttp_client_new(cfg);
	crhttp_client_config_free(cfg);
	if (!c)
		return 1;
	req = crhttp_request_new("GET", argv[1]);
	rc = crhttp_perform(c, req, &resp);
	printf("rc=%d status=%d proto=%s\n", rc, crhttp_response_status(resp),
	       crhttp_response_protocol(resp));
	crhttp_response_free(resp);
	crhttp_request_free(req);
	crhttp_client_free(c);
	return rc == CRHTTP_OK ? 0 : 1;
}
