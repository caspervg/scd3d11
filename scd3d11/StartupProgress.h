#pragma once

#include <cstdint>

namespace nSCD3D11::StartupProgress {

	struct Snapshot {
		uint32_t loadedPackageCount;
		bool resourceLoadingComplete;
		uint32_t revision;
	};

	void Reset(void);
	void RecordPackageLoaded(void);
	void MarkResourceLoadingComplete(void);
	Snapshot GetSnapshot(void);

}
