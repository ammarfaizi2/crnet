// SPDX-License-Identifier: BSD-3-Clause
//
// Browser emulation: the request headers Chrome sends, and their order.
//
// //net produces Chrome's TLS and HTTP/2 behaviour on its own, but the
// browser-level request headers (client hints, fetch metadata, accept)
// are added above //net by Blink and the network service. They are
// reproduced here from captures of Chrome 155 (see tests/fingerprint)
// and the Chromium sources referenced below.
//
#include <algorithm>
#include <array>
#include <string_view>

#include "base/containers/span.h"
#include "base/no_destructor.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "crnet/internal.h"
#include "net/base/registry_controlled_domains/registry_controlled_domain.h"

namespace crnet {
namespace {

constexpr int kMajor = CRNET_CHROMIUM_MAJOR;

// content/common/content_constants_internal.h kFrameAcceptHeaderValueWithJxl
// plus the signed-exchange suffix added by Chrome.
constexpr char kNavigateAccept[] =
	"text/html,application/xhtml+xml,application/xml;q=0.9,image/jxl,"
	"image/avif,image/webp,image/apng,*/*;q=0.8,"
	"application/signed-exchange;v=b3;q=0.7";
constexpr char kFetchAccept[] = "*/*";

// Header slots in the order Chrome emits them. Names carry the casing
// Chrome uses on HTTP/1.1 (HTTP/2 lower-cases everything anyway).
// "*" is where headers unknown to the template go. "accept@user" is the
// position Accept takes in fetch() when the page set it itself.
constexpr auto kNavigateOrder = std::to_array<std::string_view>({
	"Cache-Control",
	"sec-ch-ua",
	"sec-ch-ua-mobile",
	"sec-ch-ua-platform",
	"Upgrade-Insecure-Requests",
	"Content-Type",
	"User-Agent",
	"Origin",
	"Accept",
	"Sec-Fetch-Site",
	"Sec-Fetch-Mode",
	"Sec-Fetch-User",
	"Sec-Fetch-Dest",
	"*",
	"Referer",
	"Accept-Encoding",
	"Accept-Language",
	"Cookie",
});

constexpr auto kFetchOrder = std::to_array<std::string_view>({
	"*",
	"sec-ch-ua-platform",
	"X-Requested-With",
	"User-Agent",
	"accept@user",
	"sec-ch-ua",
	"Content-Type",
	"sec-ch-ua-mobile",
	"Accept",
	"Origin",
	"Sec-Fetch-Site",
	"Sec-Fetch-Mode",
	"Sec-Fetch-User",
	"Sec-Fetch-Dest",
	"Referer",
	"Accept-Encoding",
	"Accept-Language",
	"Cookie",
});

// components/embedder_support/user_agent_utils.cc
// GenerateBrandVersionList() / GetGreasedUserAgentBrandVersion() /
// GetRandomOrder(), seeded with the major version.
std::string BuildSecChUa(int seed, const std::string &brand,
			 const std::string &version)
{
	static constexpr std::array<const char *, 11> kGreaseChars = {
		" ", "(", ":", "-", ".", "/", ")", ";", "=", "?", "_"};
	static constexpr std::array<const char *, 3> kGreaseVersions = {
		"8", "99", "24"};
	static constexpr std::array<std::array<size_t, 3>, 6> kOrders = {
		{{0, 1, 2}, {0, 2, 1}, {1, 0, 2},
		 {1, 2, 0}, {2, 0, 1}, {2, 1, 0}}};

	std::string grease = base::StrCat(
		{"Not", kGreaseChars[seed % kGreaseChars.size()], "A",
		 kGreaseChars[(seed + 1) % kGreaseChars.size()], "Brand"});
	const std::array<std::pair<std::string, std::string>, 3> list = {{
		{grease, kGreaseVersions[seed % kGreaseVersions.size()]},
		{"Chromium", version},
		{brand, version},
	}};
	std::array<std::pair<std::string, std::string>, 3> shuffled;
	const auto &order = kOrders[seed % kOrders.size()];
	for (size_t i = 0; i < 3; i++)
		shuffled[order[i]] = list[i];

	std::string out;
	for (const auto &bv : shuffled) {
		if (!out.empty())
			out += ", ";
		base::StrAppend(&out, {"\"", bv.first, "\";v=\"", bv.second,
				       "\""});
	}
	return out;
}

bool IsSafeMethod(const std::string &m)
{
	return m == "GET" || m == "HEAD";
}

// services/network/sec_header_helpers.cc: the most "distant" relation
// between the initiator and any URL in the redirect chain.
std::string ComputeSecFetchSite(const std::optional<url::Origin> &initiator,
				const std::vector<GURL> &chain)
{
	if (!initiator)
		return "none";
	int worst = 0;	// 0 same-origin, 1 same-site, 2 cross-site
	for (const GURL &u : chain) {
		url::Origin o = url::Origin::Create(u);
		int rel;
		if (initiator->IsSameOriginWith(o))
			rel = 0;
		else if (initiator->scheme() == o.scheme() &&
			 net::registry_controlled_domains::SameDomainOrHost(
				 *initiator, o,
				 net::registry_controlled_domains::
					 INCLUDE_PRIVATE_REGISTRIES))
			rel = 1;
		else
			rel = 2;
		worst = std::max(worst, rel);
	}
	static constexpr std::array<const char *, 3> kNames = {
		"same-origin", "same-site", "cross-site"};
	return kNames[worst];
}

std::vector<std::pair<std::string, std::string>> ParseCookieLine(
	std::string_view line)
{
	std::vector<std::pair<std::string, std::string>> out;
	for (std::string_view part : base::SplitStringPiece(
		     line, ";", base::TRIM_WHITESPACE,
		     base::SPLIT_WANT_NONEMPTY)) {
		size_t eq = part.find('=');
		if (eq == std::string_view::npos)
			out.emplace_back("", std::string(part));
		else
			out.emplace_back(std::string(part.substr(0, eq)),
					 std::string(part.substr(eq + 1)));
	}
	return out;
}

std::string MergeCookies(const std::string &user, const std::string &jar)
{
	auto u = ParseCookieLine(user);
	std::string out = user;
	for (const auto &kv : ParseCookieLine(jar)) {
		bool dup = std::any_of(u.begin(), u.end(), [&](const auto &x) {
			return x.first == kv.first;
		});
		if (dup)
			continue;
		if (!out.empty())
			out += "; ";
		out += kv.first.empty() ? kv.second
					: base::StrCat({kv.first, "=", kv.second});
	}
	return out;
}

} // namespace

const std::optional<std::string> *RequestSpec::FindHeader(
	std::string_view name) const
{
	for (const auto &h : headers)
		if (base::EqualsCaseInsensitiveASCII(h.first, name))
			return &h.second;
	return nullptr;
}

const std::string &DefaultUserAgent()
{
	static const base::NoDestructor<std::string> ua(base::StrCat(
		{"Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, "
		 "like Gecko) Chrome/",
		 base::NumberToString(kMajor), ".0.0.0 Safari/537.36"}));
	return *ua;
}

const std::string &DefaultSecChUa()
{
	static const base::NoDestructor<std::string> v(BuildSecChUa(
		kMajor, "Google Chrome", base::NumberToString(kMajor)));
	return *v;
}

BrowserHeaders BuildBrowserHeaders(const ClientConfig &cfg,
				   const RequestSpec &spec)
{
	BrowserHeaders bh;
	net::HttpRequestHeaders &h = bh.extra;

	const std::string &ua =
		cfg.user_agent.empty() ? DefaultUserAgent() : cfg.user_agent;
	const std::string &ch_ua =
		cfg.sec_ch_ua.empty() ? DefaultSecChUa() : cfg.sec_ch_ua;
	const std::string platform = base::StrCat({"\"", cfg.platform, "\""});

	// The initiator is the page the request pretends to come from: the
	// Referer's origin when one is given; for fetch() without one, the
	// target's own origin (a same-origin API call).
	const std::optional<std::string> *ref = spec.FindHeader("Referer");
	if (ref && *ref) {
		GURL r(**ref);
		if (r.is_valid())
			bh.initiator = url::Origin::Create(r);
		bh.referrer = **ref;
	}
	if (!bh.initiator && spec.mode == CRNET_MODE_FETCH)
		bh.initiator = url::Origin::Create(spec.url);

	const bool safe = IsSafeMethod(spec.method);
	const bool cross_origin =
		bh.initiator && !bh.initiator->IsSameOriginWith(spec.url);

	switch (spec.mode) {
	case CRNET_MODE_NAVIGATE:
		bh.priority = net::HIGHEST;
		if (!safe)
			h.SetHeader("Cache-Control", "max-age=0");
		h.SetHeader("sec-ch-ua", ch_ua);
		h.SetHeader("sec-ch-ua-mobile", "?0");
		h.SetHeader("sec-ch-ua-platform", platform);
		h.SetHeader("Upgrade-Insecure-Requests", "1");
		h.SetHeader("User-Agent", ua);
		if (!safe)
			h.SetHeader("Origin", bh.initiator
						     ? bh.initiator->Serialize()
						     : "null");
		h.SetHeader("Accept", kNavigateAccept);
		h.SetHeader("Sec-Fetch-Site", "none");
		h.SetHeader("Sec-Fetch-Mode", "navigate");
		h.SetHeader("Sec-Fetch-User", "?1");
		h.SetHeader("Sec-Fetch-Dest", "document");
		bh.auto_fetch_site = true;
		break;
	case CRNET_MODE_FETCH:
		bh.priority = net::MEDIUM;
		h.SetHeader("sec-ch-ua-platform", platform);
		h.SetHeader("User-Agent", ua);
		h.SetHeader("sec-ch-ua", ch_ua);
		h.SetHeader("sec-ch-ua-mobile", "?0");
		h.SetHeader("Accept", kFetchAccept);
		// CORS: Origin on cross-origin requests and on unsafe methods.
		if (cross_origin || !safe)
			h.SetHeader("Origin", bh.initiator->Serialize());
		h.SetHeader("Sec-Fetch-Site", "same-origin");
		h.SetHeader("Sec-Fetch-Mode", "cors");
		h.SetHeader("Sec-Fetch-Dest", "empty");
		bh.auto_fetch_site = true;
		break;
	case CRNET_MODE_RAW:
	default:
		bh.priority = net::MEDIUM;
		break;
	}

	// Caller headers override the defaults in place or are appended.
	for (const auto &[name, value] : spec.headers) {
		std::string lower = base::ToLowerASCII(name);
		if (lower == "referer")
			continue;	// URLRequest::SetReferrer()
		if (lower == "sec-fetch-site")
			bh.auto_fetch_site = false;
		if (!value) {
			h.RemoveHeader(name);
			bh.suppressed.insert(lower);
			continue;
		}
		if (lower == "cookie")
			bh.user_cookie = *value;
		h.SetHeader(name, *value);
	}

	switch (spec.priority) {
	case CRNET_PRIORITY_DEFAULT:
		break;
	default:
		bh.priority = static_cast<net::RequestPriority>(spec.priority);
		break;
	}
	return bh;
}

net::HttpRequestHeaders FinalizeHeaders(const RequestSpec &spec,
					const BrowserHeaders &bh,
					const net::URLRequest &req,
					const net::HttpRequestHeaders &in)
{
	struct Entry {
		std::string name;
		std::string value;
		bool used = false;
	};
	std::vector<Entry> entries;
	for (const auto &kv : in.GetHeaderVector()) {
		std::string lower = base::ToLowerASCII(kv.key);
		if (bh.suppressed.count(lower))
			continue;
		entries.push_back({kv.key, kv.value});
	}

	auto find = [&](std::string_view name) -> Entry * {
		for (auto &e : entries)
			if (!e.used &&
			    base::EqualsCaseInsensitiveASCII(e.name, name))
				return &e;
		return nullptr;
	};

	// The cookie jar replaces a caller-supplied Cookie header; keep both,
	// the caller's values winning on name clashes.
	if (bh.user_cookie) {
		Entry *c = find("Cookie");
		if (c && c->value != *bh.user_cookie)
			c->value = MergeCookies(*bh.user_cookie, c->value);
	}

	if (bh.auto_fetch_site) {
		if (Entry *s = find("Sec-Fetch-Site"))
			s->value = ComputeSecFetchSite(bh.initiator,
						       req.url_chain());
	}

	if (spec.mode == CRNET_MODE_RAW) {
		// No browser order to restore: keep //net's.
		net::HttpRequestHeaders out;
		for (const auto &e : entries)
			out.SetHeader(e.name, e.value);
		return out;
	}

	base::span<const std::string_view> order =
		spec.mode == CRNET_MODE_FETCH ? base::span(kFetchOrder)
					       : base::span(kNavigateOrder);

	auto known = [&](std::string_view name) {
		for (std::string_view slot : order) {
			if (slot == "accept@user")
				slot = "Accept";
			if (base::EqualsCaseInsensitiveASCII(slot, name))
				return true;
		}
		return false;
	};

	const std::optional<std::string> *user_accept =
		spec.FindHeader("Accept");
	const bool accept_by_user = user_accept && *user_accept;

	net::HttpRequestHeaders out;
	// Known headers take Chrome's spelling, others keep the caller's.
	auto emit = [&](Entry *e, std::string_view spelling) {
		if (!e)
			return;
		e->used = true;
		out.SetHeader(spelling, e->value);
	};

	for (std::string_view slot : order) {
		if (slot == "*") {
			for (auto &e : entries)
				if (!e.used && !known(e.name))
					emit(&e, e.name);
			continue;
		}
		if (slot == "accept@user") {
			if (accept_by_user)
				emit(find("Accept"), "Accept");
			continue;
		}
		emit(find(slot), slot);
	}
	// Anything left (should not happen) keeps //net's order.
	for (auto &e : entries)
		if (!e.used)
			emit(&e, e.name);
	return out;
}

std::string NetErrorName(int net_error)
{
	return net_error == 0 ? std::string() : net::ErrorToString(net_error);
}

} // namespace crnet
