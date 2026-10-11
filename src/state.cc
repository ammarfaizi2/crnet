// SPDX-License-Identifier: BSD-3-Clause
#include "crnet/state.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/files/file_util.h"
#include "base/files/important_file_writer.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/memory/weak_ptr.h"
#include "base/synchronization/waitable_event.h"
#include "base/task/single_thread_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/values.h"

namespace crnet {
namespace {

std::optional<base::DictValue> ReadDict(const base::FilePath &path)
{
	std::string data;
	if (!base::ReadFileToString(path, &data))
		return std::nullopt;
	std::optional<base::Value> v = base::JSONReader::Read(data, 0);
	if (!v || !v->is_dict())
		return std::nullopt;
	return std::move(*v).TakeDict();
}

class ServerPropertiesStore : public net::HttpServerProperties::PrefDelegate {
public:
	ServerPropertiesStore(const base::FilePath &path,
			      scoped_refptr<base::SequencedTaskRunner> runner)
		: path_(path), runner_(std::move(runner))
	{
		runner_->PostTaskAndReplyWithResult(
			FROM_HERE, base::BindOnce(&ReadDict, path_),
			base::BindOnce(&ServerPropertiesStore::OnLoaded,
				       weak_factory_.GetWeakPtr()));
	}

	const base::DictValue &GetServerProperties() const override
	{
		return props_;
	}

	void SetServerProperties(base::DictValue dict,
				 base::OnceClosure callback) override
	{
		props_ = std::move(dict);
		std::string json;
		base::JSONWriter::Write(props_, &json);
		auto write = base::BindOnce(
			[](const base::FilePath &path, const std::string &json) {
				base::ImportantFileWriter::WriteFileAtomically(
					path, json);
			},
			path_, std::move(json));
		if (callback)
			runner_->PostTaskAndReply(FROM_HERE, std::move(write),
						  std::move(callback));
		else
			runner_->PostTask(FROM_HERE, std::move(write));
	}

	void WaitForPrefLoad(base::OnceClosure callback) override
	{
		if (loaded_) {
			base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
				FROM_HERE, std::move(callback));
			return;
		}
		waiting_.push_back(std::move(callback));
	}

private:
	void OnLoaded(std::optional<base::DictValue> dict)
	{
		if (dict)
			props_ = std::move(*dict);
		loaded_ = true;
		for (base::OnceClosure &cb : waiting_)
			std::move(cb).Run();
		waiting_.clear();
	}

	const base::FilePath path_;
	const scoped_refptr<base::SequencedTaskRunner> runner_;
	base::DictValue props_;
	bool loaded_ = false;
	std::vector<base::OnceClosure> waiting_;
	base::WeakPtrFactory<ServerPropertiesStore> weak_factory_{this};
};

} // namespace

scoped_refptr<base::SequencedTaskRunner> CreateStateRunner()
{
	return base::ThreadPool::CreateSequencedTaskRunner(
		{base::MayBlock(), base::TaskPriority::BEST_EFFORT,
		 base::TaskShutdownBehavior::BLOCK_SHUTDOWN});
}

void FlushStateRunner(base::SequencedTaskRunner *runner)
{
	base::WaitableEvent done;
	runner->PostTask(FROM_HERE, base::BindOnce(&base::WaitableEvent::Signal,
						   base::Unretained(&done)));
	done.Wait();
}

std::unique_ptr<net::HttpServerProperties::PrefDelegate>
CreateServerPropertiesStore(const base::FilePath &path,
			    scoped_refptr<base::SequencedTaskRunner> runner)
{
	return std::make_unique<ServerPropertiesStore>(path, std::move(runner));
}

} // namespace crnet
