// SPDX-License-Identifier: BSD-3-Clause
//
// SOCKS5 username/password credentials (RFC 1929) for Chromium's SOCKS5
// client (chromium/patches/*-SOCKS5-username-password-*). Chromium's
// proxy configuration carries no credentials, so the client looks them
// up by proxy in this process-wide table, which crnet clients fill while
// they exist.
//
#ifndef CRNET_SRC_SOCKS_AUTH_H
#define CRNET_SRC_SOCKS_AUTH_H

#include <string>

namespace crnet {

// Hooks the table into Chromium's SOCKS5 client; once, at global init.
void InstallSocksCredentialsLookup();

// "host:port" of a SOCKS5 proxy in Chromium's proxy rules syntax, "" for
// anything else.
std::string SocksProxyKey(const std::string &proxy);

// Adds a reference to |key|'s credentials. Fails if other clients
// registered different ones for the same proxy, since the SOCKS5 client
// could not tell whose connection it is making.
bool RegisterSocksCredentials(const std::string &key,
			      const std::string &username,
			      const std::string &password);
void UnregisterSocksCredentials(const std::string &key);

// The table key for |key|'s credentials in connection partition
// |partition|: a connection of a partition takes these, if registered,
// before the proxy's own.
std::string SocksPartitionKey(const std::string &key, int partition);

} // namespace crnet

#endif // CRNET_SRC_SOCKS_AUTH_H
