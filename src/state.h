// SPDX-License-Identifier: BSD-3-Clause
//
// Network state kept across runs in a state directory, as Chrome keeps
// it in a profile: HTTP server properties (Alt-Svc, HTTP/2 and QUIC
// support, broken alternative services) and dynamic HSTS.
//
#ifndef CRNET_SRC_STATE_H
#define CRNET_SRC_STATE_H

#include <memory>

#include "base/files/file_path.h"
#include "base/memory/scoped_refptr.h"
#include "base/task/sequenced_task_runner.h"
#include "net/http/http_server_properties.h"

namespace crnet {

// The file names Chrome uses in a profile.
inline constexpr char kServerPropertiesFile[] = "Network Persistent State";
inline constexpr char kTransportSecurityFile[] = "TransportSecurity";

// A sequence for the state files' I/O. Everything the persisters write
// is posted to it, so waiting for it to drain (FlushStateRunner()) means
// the files are on disk.
scoped_refptr<base::SequencedTaskRunner> CreateStateRunner();
void FlushStateRunner(base::SequencedTaskRunner *runner);

// Backs HttpServerProperties with a JSON file: read in the background
// at start, rewritten whenever Chromium saves (it batches changes, and
// saves once more when HttpServerProperties is destroyed).
std::unique_ptr<net::HttpServerProperties::PrefDelegate>
CreateServerPropertiesStore(const base::FilePath &path,
			    scoped_refptr<base::SequencedTaskRunner> runner);

} // namespace crnet

#endif // CRNET_SRC_STATE_H
