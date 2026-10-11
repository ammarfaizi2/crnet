// SPDX-License-Identifier: BSD-3-Clause
#include "crnet/socks_auth.h"

#include <map>
#include <mutex>
#include <utility>

#include "base/no_destructor.h"
#include "net/base/host_port_pair.h"
#include "net/base/proxy_server.h"
#include "net/base/proxy_string_util.h"
#include "net/socket/socks5_client_socket.h"

namespace crnet {
namespace {

struct Entry {
	std::string username;
	std::string password;
	int refs = 0;
};

struct Table {
	std::mutex mu;
	std::map<std::string, Entry> entries;
};

Table &GetTable()
{
	static base::NoDestructor<Table> table;
	return *table;
}

// Called on network threads, by every SOCKS5 connection attempt.
bool Lookup(const net::HostPortPair &proxy, std::string *username,
	    std::string *password)
{
	Table &t = GetTable();
	std::lock_guard<std::mutex> lk(t.mu);
	auto it = t.entries.find(proxy.ToString());
	if (it == t.entries.end())
		return false;
	*username = it->second.username;
	*password = it->second.password;
	return true;
}

} // namespace

void InstallSocksCredentialsLookup()
{
	net::SOCKS5ClientSocket::SetCredentialsLookup(&Lookup);
}

std::string SocksProxyKey(const std::string &proxy)
{
	net::ProxyServer server = net::ProxyUriToProxyServer(
		proxy, net::ProxyServer::SCHEME_HTTP);
	if (!server.is_valid() || !server.is_socks() ||
	    server.scheme() != net::ProxyServer::SCHEME_SOCKS5)
		return std::string();
	return server.host_port_pair().ToString();
}

bool RegisterSocksCredentials(const std::string &key,
			      const std::string &username,
			      const std::string &password)
{
	Table &t = GetTable();
	std::lock_guard<std::mutex> lk(t.mu);
	Entry &e = t.entries[key];
	if (e.refs && (e.username != username || e.password != password))
		return false;
	e.username = username;
	e.password = password;
	e.refs++;
	return true;
}

void UnregisterSocksCredentials(const std::string &key)
{
	Table &t = GetTable();
	std::lock_guard<std::mutex> lk(t.mu);
	auto it = t.entries.find(key);
	if (it != t.entries.end() && --it->second.refs <= 0)
		t.entries.erase(it);
}

} // namespace crnet
