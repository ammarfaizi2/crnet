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
#include "base/synchronization/waitable_event.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/task/thread_pool/thread_pool_instance.h"
#include "crnet/internal.h"
#include "crnet/socks_auth.h"
#include "net/base/network_change_notifier.h"

namespace crnet {
namespace {

std::once_flag g_init_once;
// Written once inside call_once, which orders it before every return.
bool g_init_ok = false;

thread_local bool tls_network_thread = false;

int LogLevelFromEnv()
{
	const char *v = getenv("CRNET_LOG_LEVEL");
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
	// it, and running them while the host process exits (with crnet
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
		FromEnv(enable_features, "CRNET_ENABLE_FEATURES"),
		FromEnv(disable_features, "CRNET_DISABLE_FEATURES"));
	base::FeatureList::SetInstance(std::move(fl));

	base::ThreadPoolInstance::CreateAndStartWithDefaultParams("crnet");
	InstallSocksCredentialsLookup();

	// The NetworkChangeNotifier feeds the system DNS configuration to the
	// built-in resolver and lets //net react to network changes, as in
	// Chrome. It needs a sequence to live on, not a thread of its own
	// (its netlink watcher already runs on the ThreadPool), so it gets a
	// ThreadPool sequence, kept alive for the process lifetime. It must
	// exist before the first HostResolverManager, hence the wait.
	scoped_refptr<base::SequencedTaskRunner> ncn_runner =
		base::ThreadPool::CreateSequencedTaskRunner(
			{base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN});
	ncn_runner->AddRef();	// leaked with the notifier
	base::WaitableEvent ev;
	ncn_runner->PostTask(
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
	// After the first call this is a single atomic load.
	bool ran = false;
	std::call_once(g_init_once, [&] {
		ran = true;
		g_init_ok = DoInit(enable_features, disable_features);
	});
	if (already)
		*already = !ran;
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

} // namespace crnet
