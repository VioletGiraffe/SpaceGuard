#include "snapshot_scan_runner.h"

#include <algorithm>
#include <assert.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <utility>

namespace {

constexpr std::chrono::milliseconds ProgressPublicationInterval{100};
constexpr uint64_t ScanJobTag = 1;

uint32_t scanParticipantCount() noexcept
{
	return std::clamp(std::thread::hardware_concurrency(), 1u, 8u);
}

int nextProgressQueueTag()
{
	static std::atomic_int nextTag{-2};
	return nextTag.fetch_sub(1, std::memory_order_relaxed);
}

} // namespace

struct SnapshotScanRunner::RequestState
{
	std::atomic_bool canceled = false;
};

SnapshotScanRunner::SnapshotScanRunner(CExecutionQueue& publicationQueue, SnapshotScanRunnerCallbacks callbacks)
	: m_publicationQueue{publicationQueue},
	  m_callbacks{std::move(callbacks)},
	  m_progressQueueTag{nextProgressQueueTag()},
	  m_scanPool{scanParticipantCount(), "SpaceGuard snapshot scan"}
{
	assert(m_callbacks.completed);
}

SnapshotScanRunner::~SnapshotScanRunner()
{
	(void)cancel();
	m_scanPool.retire(ScanJobTag);
}

std::optional<uint64_t> SnapshotScanRunner::start(const NativePath& normalizedRootPath)
{
	std::lock_guard lock{m_stateMutex};
	if (m_scanInProgress)
		return {};

	const uint64_t generation = ++m_lastGeneration;
	auto request = std::make_shared<RequestState>();
	m_scanInProgress = true;
	m_activeRequest = request;
	try
	{
		m_scanPool.enqueue([this, rootPath{normalizedRootPath}, generation, request{std::move(request)}]() mutable {
			runScan(std::move(rootPath), generation, request);
		}, ScanJobTag);
	}
	catch (...)
	{
		m_scanInProgress = false;
		m_activeRequest.reset();
		throw;
	}
	return generation;
}

bool SnapshotScanRunner::cancel()
{
	std::lock_guard lock{m_stateMutex};
	if (!m_scanInProgress)
		return false;

	assert(m_activeRequest);
	m_activeRequest->canceled.store(true, std::memory_order_relaxed);
	return true;
}

bool SnapshotScanRunner::scanInProgress() const
{
	std::lock_guard lock{m_stateMutex};
	return m_scanInProgress;
}

void SnapshotScanRunner::runScan(
	NativePath rootPath, const uint64_t generation, const std::shared_ptr<RequestState>& request)
{
	SnapshotScanProgress latestProgress;
	std::optional<SnapshotScanProgress> lastEnqueuedProgress;
	std::mutex progressMutex;
	auto lastProgressPublication = std::chrono::steady_clock::now() - ProgressPublicationInterval;
	const auto reportProgress = [this, generation, &latestProgress, &lastEnqueuedProgress, &progressMutex, &lastProgressPublication](
		const SnapshotScanProgress& progress) {
		std::lock_guard lock{progressMutex};
		latestProgress = progress;
		const auto now = std::chrono::steady_clock::now();
		if (now - lastProgressPublication < ProgressPublicationInterval)
			return;
		enqueueProgress(generation, progress);
		lastEnqueuedProgress = progress;
		lastProgressPublication = now;
	};

	SnapshotScanResult result = SnapshotScanCanceled{};
	try
	{
		result = scanSnapshot(rootPath, request->canceled, m_scanPool, reportProgress);
	}
	catch (...)
	{
		result = SnapshotScanFailure{SnapshotScanFailureCode::unexpected_error, rootPath, {}};
	}

	{
		std::lock_guard lock{progressMutex};
		if (!lastEnqueuedProgress || *lastEnqueuedProgress != latestProgress)
			enqueueProgress(generation, latestProgress);
	}

	std::lock_guard lock{m_stateMutex};
	assert(m_scanInProgress && m_activeRequest == request);
	if (request->canceled.load(std::memory_order_relaxed))
		result = SnapshotScanCanceled{};

	auto publishedResult = std::make_shared<const SnapshotScanResult>(std::move(result));
	const auto completed = m_callbacks.completed;
	m_publicationQueue.enqueue([completed, generation, publishedResult{std::move(publishedResult)}] {
		completed(generation, publishedResult);
	});
	m_activeRequest.reset();
	m_scanInProgress = false;
}

void SnapshotScanRunner::enqueueProgress(const uint64_t generation, const SnapshotScanProgress& progress)
{
	if (!m_callbacks.progress)
		return;

	const auto progressCallback = m_callbacks.progress;
	m_publicationQueue.enqueue([progressCallback, generation, progress] {
		progressCallback(generation, progress);
	}, m_progressQueueTag);
}
