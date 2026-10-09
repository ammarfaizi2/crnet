// SPDX-License-Identifier: BSD-3-Clause
//
// Process-wide Chromium initialisation.
//
// Chromium expects an embedder to set up a handful of singletons before
// //net is used: an AtExitManager, the command line, the FeatureList,
// the ThreadPool, ICU and (for the built-in DNS client and network
// change handling) a NetworkChangeNotifier. A library cannot own main(),
// so this happens lazily on first use and is never torn down: Chromium
// does not support re-initialising these within a process.
//
#include <stdlib.h>

#include <mutex>

#include "base/at_exit.h"
#include "base/command_line.h"
#include "base/feature_list.h"
#include "base/i18n/icu_util.h"
#include "base/logging.h"
#include "base/logging/logging_settings.h"
#include "base/message_loop/message_pump_type.h"
#include "base/synchronization/waitable_event.h"
#include "base/task/thread_pool/thread_pool_instance.h"
#include "base/threading/thread.h"
#include "crhttp/internal.h"
#include "net/base/network_change_notifier.h"

namespace crhttp {
namespace {

std::mutex g_init_mu;
bool g_init_done = false;
bool g_init_ok = false;

thread_local bool tls_network_thread = false;

int LogLevelFromEnv()
{
	const char *v = getenv("CRHTTP_LOG_LEVEL");
	if (!v || !*v)
		return logging::LOGGING_FATAL;
	// Same scale as Chrome's --log-level: 0=INFO 1=WARNING 2=ERROR 3=FATAL.
	int lvl = atoi(v);
	if (lvl < logging::LOGGING_INFO)
		lvl = logging::LOGGING_INFO;
	if (lvl > logging::LOGGING_FATAL)
		lvl = logging::LOGGING_FATAL;
	return lvl;
}

std::string FromEnv(const char *arg, const char *env)
{
	if (arg)
		return arg;
	const char *v = getenv(env);
	return v ? v : "";
}

bool DoInit(const char *enable_features, const char *disable_features)
{
	// Leaked on purpose: Chromium singletons register exit callbacks with
	// it, and running them while the host process exits (with crhttp
	// threads possibly still alive) is unsafe.
	new base::AtExitManager();

	base::CommandLine::Init(0, nullptr);

	logging::LoggingSettings ls;
	ls.logging_dest = logging::LOG_TO_STDERR;
	logging::InitLogging(ls);
	logging::SetMinLogLevel(LogLevelFromEnv());
	logging::SetLogItems(true, true, true, false);

	if (!base::i18n::InitializeICU())
		return false;

	auto fl = std::make_unique<base::FeatureList>();
	fl->InitFromCommandLine(
		FromEnv(enable_features, "CRHTTP_ENABLE_FEATURES"),
		FromEnv(disable_features, "CRHTTP_DISABLE_FEATURES"));
	base::FeatureList::SetInstance(std::move(fl));

	base::ThreadPoolInstance::CreateAndStartWithDefaultParams("crhttp");

	// The NetworkChangeNotifier feeds the system DNS configuration to the
	// built-in resolver and lets //net react to network changes, as in
	// Chrome. It lives on a dedicated IO thread for the process lifetime.
	auto *ncn_thread = new base::Thread("crhttp-ncn");
	base::Thread::Options opts(base::MessagePumpType::IO, 0);
	if (!ncn_thread->StartWithOptions(std::move(opts)))
		return false;
	base::WaitableEvent ev;
	ncn_thread->task_runner()->PostTask(
		FROM_HERE, base::BindOnce(
				   [](base::WaitableEvent *ev) {
					   net::NetworkChangeNotifier::CreateIfNeeded()
						   .release();
					   ev->Signal();
				   },
				   &ev));
	ev.Wait();
	return true;
}

} // namespace

bool EnsureGlobalInit(const char *enable_features,
		      const char *disable_features, bool *already)
{
	std::lock_guard<std::mutex> lk(g_init_mu);
	if (already)
		*already = g_init_done;
	if (!g_init_done) {
		g_init_ok = DoInit(enable_features, disable_features);
		g_init_done = true;
	}
	return g_init_ok;
}

bool OnNetworkThread()
{
	return tls_network_thread;
}

void MarkNetworkThread()
{
	tls_network_thread = true;
}

} // namespace crhttp
