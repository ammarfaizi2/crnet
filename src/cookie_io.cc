// SPDX-License-Identifier: BSD-3-Clause
#include "crnet/cookie_io.h"

#include <math.h>

#include <optional>
#include <utility>

#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/time/time.h"
#include "base/values.h"
#include "net/base/registry_controlled_domains/registry_controlled_domain.h"
#include "net/cookies/cookie_constants.h"
#include "net/cookies/cookie_partition_key.h"
#include "url/third_party/mozilla/url_parse.h"

namespace crnet {
namespace {

// Times are microseconds since the Unix epoch, 0 for "none"; doubles
// hold them exactly for the next couple of centuries.
double ToMicros(base::Time t)
{
	if (t.is_null())
		return 0;
	return static_cast<double>((t - base::Time::UnixEpoch()).InMicroseconds());
}

bool FromMicros(std::optional<double> us, base::Time *t)
{
	if (!us || !isfinite(*us) || fabs(*us) > 0x1p62)
		return false;
	*t = *us == 0 ? base::Time()
		      : base::Time::UnixEpoch() +
				base::Microseconds(static_cast<int64_t>(*us));
	return true;
}

struct Name {
	int value;
	const char *name;
};

constexpr Name kSameSite[] = {
	{static_cast<int>(net::CookieSameSite::UNSPECIFIED), "unspecified"},
	{static_cast<int>(net::CookieSameSite::NO_RESTRICTION), "none"},
	{static_cast<int>(net::CookieSameSite::LAX_MODE), "lax"},
	{static_cast<int>(net::CookieSameSite::STRICT_MODE), "strict"},
};
constexpr Name kPriority[] = {
	{net::COOKIE_PRIORITY_LOW, "low"},
	{net::COOKIE_PRIORITY_MEDIUM, "medium"},
	{net::COOKIE_PRIORITY_HIGH, "high"},
};
constexpr Name kSourceScheme[] = {
	{static_cast<int>(net::CookieSourceScheme::kUnset), "unset"},
	{static_cast<int>(net::CookieSourceScheme::kNonSecure), "nonsecure"},
	{static_cast<int>(net::CookieSourceScheme::kSecure), "secure"},
};

template <size_t N>
const char *NameOf(const Name (&table)[N], int value)
{
	for (const Name &n : table) {
		if (n.value == value)
			return n.name;
	}
	return table[0].name;
}

template <size_t N>
bool ValueOf(const Name (&table)[N], const std::string *name, int *value)
{
	if (!name)
		return false;
	for (const Name &n : table) {
		if (*name == n.name) {
			*value = n.value;
			return true;
		}
	}
	return false;
}

bool Expired(const net::CanonicalCookie &c, base::Time now)
{
	return c.IsPersistent() && c.ExpiryDate() <= now;
}

// Storage only checks that a cookie's domain matches the host it is set
// from, so it would take a cookie for ".com" and send it everywhere.
// Refuse what Set-Cookie refuses: a domain cookie for a public suffix.
bool AcceptableDomain(const net::CanonicalCookie &c)
{
	if (!c.IsDomainCookie())
		return true;
	return !net::registry_controlled_domains::GetDomainAndRegistry(
			std::string_view(c.Domain()).substr(1),
			net::registry_controlled_domains::INCLUDE_PRIVATE_REGISTRIES)
			.empty();
}

// ------------------------------------------------------------------
// JSON
// ------------------------------------------------------------------

std::string ToJson(const net::CookieList &cookies)
{
	base::ListValue list;
	for (const net::CanonicalCookie &c : cookies) {
		base::DictValue d;
		if (c.PartitionKey()) {
			auto key = net::CookiePartitionKey::Serialize(c.PartitionKey());
			if (!key.has_value())
				continue;	// nonce-based, cannot be stored
			d.Set("partition",
			      base::DictValue()
				      .Set("top_level_site", key->TopLevelSite())
				      .Set("cross_site_ancestor",
					   key->has_cross_site_ancestor()));
		} else {
			d.Set("partition", base::Value());
		}
		d.Set("name", c.Name());
		d.Set("value", c.Value());
		d.Set("domain", c.Domain());
		d.Set("path", c.Path());
		d.Set("secure", c.SecureAttribute());
		d.Set("http_only", c.IsHttpOnly());
		d.Set("same_site",
		      NameOf(kSameSite, static_cast<int>(c.SameSite())));
		d.Set("priority", NameOf(kPriority, c.Priority()));
		d.Set("expires", c.IsPersistent()
					 ? base::Value(ToMicros(c.ExpiryDate()))
					 : base::Value());
		d.Set("creation", ToMicros(c.CreationDate()));
		d.Set("last_access", ToMicros(c.LastAccessDate()));
		d.Set("last_update", ToMicros(c.LastUpdateDate()));
		d.Set("source_scheme",
		      NameOf(kSourceScheme, static_cast<int>(c.SourceScheme())));
		d.Set("source_port", c.SourcePort());
		list.Append(std::move(d));
	}
	if (list.empty())
		return "[]\n";
	std::string out;
	base::JSONWriter::WriteWithOptions(
		list, base::JSONWriter::OPTIONS_PRETTY_PRINT, &out);
	return out;
}

std::unique_ptr<net::CanonicalCookie> FromJson(const base::DictValue &d)
{
	const std::string *name = d.FindString("name");
	const std::string *value = d.FindString("value");
	const std::string *domain = d.FindString("domain");
	const std::string *path = d.FindString("path");
	std::optional<bool> secure = d.FindBool("secure");
	std::optional<bool> http_only = d.FindBool("http_only");
	int same_site, priority, source_scheme;
	if (!name || !value || !domain || !path || !secure || !http_only ||
	    !ValueOf(kSameSite, d.FindString("same_site"), &same_site) ||
	    !ValueOf(kPriority, d.FindString("priority"), &priority) ||
	    !ValueOf(kSourceScheme, d.FindString("source_scheme"),
		     &source_scheme))
		return nullptr;

	base::Time expires, creation, last_access, last_update;
	const base::Value *exp = d.Find("expires");
	if (!exp)
		return nullptr;
	if (!exp->is_none() && !FromMicros(exp->GetIfDouble(), &expires))
		return nullptr;
	if (!FromMicros(d.FindDouble("creation"), &creation) ||
	    !FromMicros(d.FindDouble("last_access"), &last_access) ||
	    !FromMicros(d.FindDouble("last_update"), &last_update))
		return nullptr;
	if (creation.is_null())
		creation = base::Time::Now();
	std::optional<int> source_port = d.FindInt("source_port");
	if (!source_port)
		return nullptr;

	std::optional<net::CookiePartitionKey> partition;
	const base::Value *p = d.Find("partition");
	if (!p)
		return nullptr;
	if (!p->is_none()) {
		const base::DictValue *pd = p->GetIfDict();
		const std::string *site =
			pd ? pd->FindString("top_level_site") : nullptr;
		std::optional<bool> cross =
			pd ? pd->FindBool("cross_site_ancestor") : std::nullopt;
		if (!site || !cross)
			return nullptr;
		auto key = net::CookiePartitionKey::FromStorage(*site, *cross);
		if (!key.has_value() || !key->has_value())
			return nullptr;
		partition = std::move(**key);
	}

	return net::CanonicalCookie::FromStorage(
		*name, *value, *domain, *path, creation, expires, last_access,
		last_update, *secure, *http_only,
		static_cast<net::CookieSameSite>(same_site),
		static_cast<net::CookiePriority>(priority), std::move(partition),
		static_cast<net::CookieSourceScheme>(source_scheme), *source_port,
		net::CookieSourceType::kOther,
		net::CanonicalCookieFromStorageCallSite::kCookieManager);
}

bool ParseJson(const std::string &data,
	       std::vector<std::unique_ptr<net::CanonicalCookie>> *out)
{
	std::optional<base::Value> v =
		base::JSONReader::Read(data, base::JSON_PARSE_RFC);
	if (!v || !v->is_list())
		return false;
	const base::Time now = base::Time::Now();
	for (const base::Value &item : v->GetList()) {
		if (!item.is_dict())
			return false;
		std::unique_ptr<net::CanonicalCookie> c = FromJson(item.GetDict());
		if (!c || !AcceptableDomain(*c))
			return false;
		if (!Expired(*c, now))
			out->push_back(std::move(c));
	}
	return true;
}

// ------------------------------------------------------------------
// Netscape cookies.txt
// ------------------------------------------------------------------

constexpr char kHttpOnlyPrefix[] = "#HttpOnly_";

std::string ToNetscape(const net::CookieList &cookies)
{
	std::string out = "# Netscape HTTP Cookie File\n";
	for (const net::CanonicalCookie &c : cookies) {
		if (c.PartitionKey())
			continue;	// not representable
		int64_t expires =
			c.IsPersistent()
				? (c.ExpiryDate() - base::Time::UnixEpoch())
					  .InSeconds()
				: 0;
		base::StrAppend(
			&out,
			{c.IsHttpOnly() ? kHttpOnlyPrefix : "", c.Domain(), "\t",
			 c.IsDomainCookie() ? "TRUE" : "FALSE", "\t", c.Path(),
			 "\t", c.SecureAttribute() ? "TRUE" : "FALSE", "\t",
			 base::NumberToString(expires), "\t", c.Name(), "\t",
			 c.Value(), "\n"});
	}
	return out;
}

bool ParseBool(const std::string &s, bool *b)
{
	if (base::EqualsCaseInsensitiveASCII(s, "TRUE"))
		*b = true;
	else if (base::EqualsCaseInsensitiveASCII(s, "FALSE"))
		*b = false;
	else
		return false;
	return true;
}

bool ParseNetscape(const std::string &data,
		   std::vector<std::unique_ptr<net::CanonicalCookie>> *out)
{
	const base::Time now = base::Time::Now();
	for (std::string_view line : base::SplitStringPiece(
		     data, "\n", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL)) {
		if (!line.empty() && line.back() == '\r')
			line.remove_suffix(1);
		bool http_only = false;
		if (line.starts_with(kHttpOnlyPrefix)) {
			http_only = true;
			line.remove_prefix(sizeof(kHttpOnlyPrefix) - 1);
		} else if (line.empty() || line.front() == '#' ||
			   base::TrimWhitespaceASCII(line, base::TRIM_ALL)
				   .empty()) {
			continue;
		}
		std::vector<std::string> f = base::SplitString(
			line, "\t", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL);
		bool subdomains, secure;
		int64_t expires_s;
		if (f.size() != 7 || !ParseBool(f[1], &subdomains) ||
		    !ParseBool(f[3], &secure) ||
		    !base::StringToInt64(f[4], &expires_s) || expires_s < 0)
			return false;
		std::string domain = f[0];
		// Chromium marks domain cookies with a leading dot.
		if (subdomains && !domain.starts_with("."))
			domain.insert(0, ".");
		base::Time expires;
		if (expires_s)
			expires = base::Time::UnixEpoch() + base::Seconds(expires_s);
		std::unique_ptr<net::CanonicalCookie> c =
			net::CanonicalCookie::FromStorage(
				f[5], f[6], domain, f[2], now, expires, now, now,
				secure, http_only, net::CookieSameSite::UNSPECIFIED,
				net::COOKIE_PRIORITY_DEFAULT, std::nullopt,
				secure ? net::CookieSourceScheme::kSecure
				       : net::CookieSourceScheme::kNonSecure,
				url::PORT_UNSPECIFIED, net::CookieSourceType::kOther,
				net::CanonicalCookieFromStorageCallSite::kCookieManager);
		if (!c || !AcceptableDomain(*c))
			return false;
		if (!Expired(*c, now))
			out->push_back(std::move(c));
	}
	return true;
}

} // namespace

std::string ExportCookies(const net::CookieList &cookies,
			  crnet_cookie_format format)
{
	return format == CRNET_COOKIES_NETSCAPE ? ToNetscape(cookies)
						: ToJson(cookies);
}

bool ParseCookies(const std::string &data, crnet_cookie_format format,
		  std::vector<std::unique_ptr<net::CanonicalCookie>> *out)
{
	switch (format) {
	case CRNET_COOKIES_JSON:
		return ParseJson(data, out);
	case CRNET_COOKIES_NETSCAPE:
		return ParseNetscape(data, out);
	}
	return false;
}

} // namespace crnet
