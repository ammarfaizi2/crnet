// SPDX-License-Identifier: BSD-3-Clause
//
// SOCKS5 username/password credentials (RFC 1929) for Chromium's SOCKS5
// client (chromium/patches/*-SOCKS5-username-password-*). Chromium's
// proxy configuration carries no credentials, so the SOCKS5 client looks
// them up in this process-wide table, by proxy and by the connection
// partition of the proxy chain (chromium/patches/*-proxy-identities-*).
// Every connection of a crnet client is in a partition of that client,
// so a client only ever finds credentials it was given.
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

// Registers credentials for the connections of connection partition
// |partition| to the SOCKS5 proxy |proxy_key| (SocksProxyKey()).
// Returns the key to unregister them with.
std::string RegisterSocksCredentials(const std::string &proxy_key,
				     int partition, const std::string &username,
				     const std::string &password);
void UnregisterSocksCredentials(const std::string &key);

// Connections of |partition| to a proxy it has no credentials for use
// those of |parent| (a fresh connection's partition, its client's).
void SetSocksPartitionParent(int partition, int parent);
void ClearSocksPartitionParent(int partition);

} // namespace crnet

#endif // CRNET_SRC_SOCKS_AUTH_H
