#include "StartupProgress.h"

#include <cassert>

int main() {
	nSCD3D11::StartupProgress::Reset();
	auto snapshot = nSCD3D11::StartupProgress::GetSnapshot();
	assert(snapshot.loadedPackageCount == 0);
	assert(!snapshot.resourceLoadingComplete);
	uint32_t const initialRevision = snapshot.revision;

	nSCD3D11::StartupProgress::RecordPackageLoaded();
	nSCD3D11::StartupProgress::RecordPackageLoaded();
	snapshot = nSCD3D11::StartupProgress::GetSnapshot();
	assert(snapshot.loadedPackageCount == 2);
	assert(snapshot.revision > initialRevision);

	nSCD3D11::StartupProgress::MarkResourceLoadingComplete();
	snapshot = nSCD3D11::StartupProgress::GetSnapshot();
	assert(snapshot.loadedPackageCount == 2);
	assert(snapshot.resourceLoadingComplete);
	return 0;
}
