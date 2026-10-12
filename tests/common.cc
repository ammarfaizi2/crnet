// SPDX-License-Identifier: BSD-3-Clause
#include "common.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <mutex>
#include <strings.h>

#ifndef CRNET_TEST_SERVER
#error "CRNET_TEST_SERVER must point at tests/server/test_server.py"
#endif

namespace crtest {
namespace {

std::once_flag g_once;
Ports g_ports;
pid_t g_pid = -1;
int g_stdin = -1;

void StopServer()
{
	if (g_pid <= 0)
		return;
	close(g_stdin);
	kill(g_pid, SIGTERM);
	waitpid(g_pid, nullptr, 0);
	g_pid = -1;
}

void StartServer()
{
	int out[2], in[2];
	if (pipe(out) || pipe(in))
		abort();
	g_pid = fork();
	if (g_pid == 0) {
		dup2(in[0], 0);
		dup2(out[1], 1);
		close(in[1]);
		close(out[0]);
		execlp("python3", "python3", "-I", CRNET_TEST_SERVER, nullptr);
		_exit(127);
	}
	close(in[0]);
	close(out[1]);
	g_stdin = in[1];

	std::string line;
	char ch;
	while (read(out[0], &ch, 1) == 1 && ch != '\n')
		line += ch;
	close(out[0]);
	if (line.empty()) {
		fprintf(stderr, "test server failed to start\n");
		abort();
	}
	json j = json::parse(line);
	g_ports.https = j["https"];
	g_ports.https_h1 = j["https_h1"];
	g_ports.http = j["http"];
	g_ports.socks5 = j["socks5"];
	g_ports.http_proxy = j["http_proxy"];
	g_ports.socks5_auth = j["socks5_auth"];
	g_ports.https_ws_h2 = j["https_ws_h2"];
	g_ports.https_mtls = j["https_mtls"];
	g_ports.ca_file = j["ca"];
	g_ports.client_cert_file = j["client_cert"];
	g_ports.client_key_file = j["client_key"];
	atexit(StopServer);
}

} // namespace

const Ports &ServerPorts()
{
	std::call_once(g_once, StartServer);
	return g_ports;
}

std::string HttpsUrl(const std::string &path)
{
	return "https://localhost:" + std::to_string(ServerPorts().https) + path;
}

std::string HttpsH1Url(const std::string &path)
{
	return "https://localhost:" + std::to_string(ServerPorts().https_h1) + path;
}

std::string HttpUrl(const std::string &path)
{
	return "http://localhost:" + std::to_string(ServerPorts().http) + path;
}

crnet_client *NewTestClient(crnet_client_config *cfg)
{
	ServerPorts();
	bool own = !cfg;
	if (own)
		cfg = crnet_client_config_new();
	crnet_client_config_set_insecure(cfg, 1);
	// Interfaces coming and going on the host (containers) must not
	// fail unrelated tests; tests/test_network_change.cc covers that.
	crnet_client_config_set_ignore_ip_address_changes(cfg, 1);
	crnet_client *c = crnet_client_new(cfg);
	if (own)
		crnet_client_config_free(cfg);
	return c;
}

Result Perform(crnet_client *c, crnet_request *req)
{
	Result r;
	r.rc = crnet_perform(c, req, &r.resp);
	return r;
}

Result Get(crnet_client *c, const std::string &url)
{
	crnet_request *req = crnet_request_new("GET", url.c_str());
	Result r = Perform(c, req);
	crnet_request_free(req);
	return r;
}

std::vector<std::string> HeaderNames(const json &echo)
{
	std::vector<std::string> names;
	for (const auto &h : echo["headers"])
		names.push_back(h[0].get<std::string>());
	return names;
}

std::string EchoHeader(const json &echo, const std::string &name)
{
	for (const auto &h : echo["headers"])
		if (!strcasecmp(h[0].get<std::string>().c_str(), name.c_str()))
			return h[1].get<std::string>();
	return "";
}

bool EchoHasHeader(const json &echo, const std::string &name)
{
	for (const auto &h : echo["headers"])
		if (!strcasecmp(h[0].get<std::string>().c_str(), name.c_str()))
			return true;
	return false;
}

} // namespace crtest
