#include "StartupProgress.h"

#include <atomic>

namespace nSCD3D11::StartupProgress {
	namespace {
		std::atomic<uint32_t> loadedPackageCount{0};
		std::atomic<bool> resourceLoadingComplete{false};
		std::atomic<uint32_t> revision{1};

		void PublishChange(void) {
			revision.fetch_add(1, std::memory_order_release);
		}
	}

	void Reset(void) {
		loadedPackageCount.store(0, std::memory_order_relaxed);
		resourceLoadingComplete.store(false, std::memory_order_relaxed);
		PublishChange();
	}

	void RecordPackageLoaded(void) {
		loadedPackageCount.fetch_add(1, std::memory_order_relaxed);
		PublishChange();
	}

	void MarkResourceLoadingComplete(void) {
		resourceLoadingComplete.store(true, std::memory_order_relaxed);
		PublishChange();
	}

	Snapshot GetSnapshot(void) {
		Snapshot snapshot{};
		uint32_t finalRevision;
		do {
			snapshot.revision = revision.load(std::memory_order_acquire);
			snapshot.loadedPackageCount = loadedPackageCount.load(std::memory_order_relaxed);
			snapshot.resourceLoadingComplete = resourceLoadingComplete.load(std::memory_order_relaxed);
			finalRevision = revision.load(std::memory_order_acquire);
		} while (snapshot.revision != finalRevision);
		return snapshot;
	}
}
