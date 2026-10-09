/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Compiled as C (not C++) to keep crnet.h valid C. Runs one request
 * against the URL given on the command line with an insecure client.
 */
#include <stdio.h>
#include <string.h>

#include <crnet/crnet.h>

int main(int argc, char *argv[])
{
	crnet_client_config *cfg;
	crnet_client *c;
	crnet_request *req;
	crnet_response *resp = NULL;
	int rc;

	if (argc != 2)
		return 2;
	printf("%s\n", crnet_version());
	cfg = crnet_client_config_new();
	crnet_client_config_set_insecure(cfg, 1);
	c = crnet_client_new(cfg);
	crnet_client_config_free(cfg);
	if (!c)
		return 1;
	req = crnet_request_new("GET", argv[1]);
	rc = crnet_perform(c, req, &resp);
	printf("rc=%d status=%d proto=%s\n", rc, crnet_response_status(resp),
	       crnet_response_protocol(resp));
	crnet_response_free(resp);
	crnet_request_free(req);
	crnet_client_free(c);
	return rc == CRNET_OK ? 0 : 1;
}
