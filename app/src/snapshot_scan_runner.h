#pragma once

#include "snapshot_scanner.h"

#include "threading/cexecutionqueue.h"
#include "threading/cworkerthread.h"

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdint.h>

struct SnapshotScanRunnerCallbacks
{
	std::function<void(uint64_t generation, const SnapshotScanProgress& progress)> progress;
	std::function<void(uint64_t generation, const std::shared_ptr<const SnapshotScanResult>& result)> completed;
};

class SnapshotScanRunner
{
public:
	// The publication queue and callback targets must remain alive until the runner is destroyed and its queued callbacks
	// have been drained or discarded with their owning queue.
	SnapshotScanRunner(CExecutionQueue& publicationQueue, SnapshotScanRunnerCallbacks callbacks);
	~SnapshotScanRunner();

	SnapshotScanRunner(const SnapshotScanRunner&) = delete;
	SnapshotScanRunner& operator=(const SnapshotScanRunner&) = delete;

	[[nodiscard]] std::optional<uint64_t> start(const NativePath& normalizedRootPath);
	[[nodiscard]] bool cancel();
	[[nodiscard]] bool scanInProgress() const;

private:
	struct RequestState;

	void runScan(NativePath rootPath, uint64_t generation, const std::shared_ptr<RequestState>& request);
	void enqueueProgress(uint64_t generation, const SnapshotScanProgress& progress);

private:
	CExecutionQueue& m_publicationQueue;
	SnapshotScanRunnerCallbacks m_callbacks;
	const int m_progressQueueTag;
	mutable std::mutex m_stateMutex;
	uint64_t m_lastGeneration = 0;
	bool m_scanInProgress = false;
	std::shared_ptr<RequestState> m_activeRequest;
	// Keep last: the scan job and its nested parallelFor helpers access the runner state above. The destructor retires
	// the scan job while the pool can still run those helpers; reverse destruction then joins the workers before other state is released.
	CWorkerThreadPool m_scanPool;
};
