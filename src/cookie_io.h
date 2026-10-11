// SPDX-License-Identifier: BSD-3-Clause
//
// Cookie jar export and import: crnet's lossless JSON format and the
// Netscape cookies.txt format used by curl and wget.
//
#ifndef CRNET_SRC_COOKIE_IO_H
#define CRNET_SRC_COOKIE_IO_H

#include <memory>
#include <string>
#include <vector>

#include "crnet/crnet.h"
#include "net/cookies/canonical_cookie.h"

namespace crnet {

// Serializes |cookies|. Cookies the format cannot represent are left
// out: partitioned cookies in the Netscape format, and cookies with a
// nonce-based (opaque) partition in either.
std::string ExportCookies(const net::CookieList &cookies,
			  crnet_cookie_format format);

// Parses |data|. Returns false if it is malformed or holds a cookie
// Chromium would not store; expired cookies are dropped silently.
bool ParseCookies(const std::string &data, crnet_cookie_format format,
		  std::vector<std::unique_ptr<net::CanonicalCookie>> *out);

} // namespace crnet

#endif // CRNET_SRC_COOKIE_IO_H
