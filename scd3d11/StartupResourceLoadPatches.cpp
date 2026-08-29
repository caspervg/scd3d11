#include "StartupResourceLoadPatches.h"

#include "Diagnostics.h"
#include "StartupProgress.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <windows.h>

namespace nSCD3D11::StartupResourceLoadPatches {
	namespace {
		constexpr uintptr_t kImageBase = 0x00400000;
		constexpr uintptr_t kPackedFileOpenAddress = 0x00972A17;
		constexpr uintptr_t kPackedFileOpenRva = kPackedFileOpenAddress - kImageBase;
		constexpr size_t kPatchSize = 5;
		constexpr std::array<uint8_t, kPatchSize> kExpectedPrologue{0x55, 0x8B, 0xEC, 0x51, 0x51};

		using PackedFileOpen = bool (__thiscall *)(void *, bool, bool);
		PackedFileOpen originalPackedFileOpen = nullptr;
		uint8_t *target = nullptr;
		uint8_t *trampoline = nullptr;
		bool installed = false;

		bool IsSupportedGameVersion(void) {
			char path[MAX_PATH]{};
			if (GetModuleFileNameA(nullptr, path, MAX_PATH) == 0) return false;
			DWORD ignored = 0;
			DWORD const size = GetFileVersionInfoSizeA(path, &ignored);
			if (size == 0) return false;
			std::unique_ptr<uint8_t[]> data(new (std::nothrow) uint8_t[size]);
			if (!data || !GetFileVersionInfoA(path, 0, size, data.get())) return false;
			VS_FIXEDFILEINFO *version = nullptr;
			UINT versionSize = 0;
			if (!VerQueryValueA(data.get(), "\\", reinterpret_cast<void **>(&version), &versionSize) ||
			    version == nullptr || versionSize < sizeof(VS_FIXEDFILEINFO)) return false;
			return HIWORD(version->dwFileVersionMS) == 1 && LOWORD(version->dwFileVersionMS) == 1 &&
			       HIWORD(version->dwFileVersionLS) == 641;
		}

		bool HasExpectedPrologue(uint8_t const *address) {
			return address != nullptr &&
			       std::memcmp(address, kExpectedPrologue.data(), kExpectedPrologue.size()) == 0;
		}

		void WriteRelativeJump(uint8_t *source, void const *destination) {
			source[0] = 0xE9;
			auto const displacement = static_cast<int32_t>(
				reinterpret_cast<uintptr_t>(destination) - reinterpret_cast<uintptr_t>(source + kPatchSize));
			std::memcpy(source + 1, &displacement, sizeof(displacement));
		}

		bool __fastcall ObservePackedFileOpen(void *segment, void *, bool openRead, bool openWrite) {
			bool const result = originalPackedFileOpen(segment, openRead, openWrite);
			if (result && openRead && !openWrite) StartupProgress::RecordPackageLoaded();
			return result;
		}

		bool CreateTrampoline(void) {
			trampoline = static_cast<uint8_t *>(VirtualAlloc(
				nullptr, kPatchSize * 2, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
			if (trampoline == nullptr) return false;
			std::memcpy(trampoline, target, kPatchSize);
			WriteRelativeJump(trampoline + kPatchSize, target + kPatchSize);
			originalPackedFileOpen = reinterpret_cast<PackedFileOpen>(trampoline);
			return true;
		}

		bool ActivateHook(void) {
			DWORD previousProtection = 0;
			if (!VirtualProtect(target, kPatchSize, PAGE_EXECUTE_READWRITE, &previousProtection)) return false;
			WriteRelativeJump(target, reinterpret_cast<void *>(&ObservePackedFileOpen));
			FlushInstructionCache(GetCurrentProcess(), target, kPatchSize);
			DWORD ignored = 0;
			VirtualProtect(target, kPatchSize, previousProtection, &ignored);
			return true;
		}

		void ReleaseTrampoline(void) {
			originalPackedFileOpen = nullptr;
			if (trampoline != nullptr) VirtualFree(trampoline, 0, MEM_RELEASE);
			trampoline = nullptr;
		}
	}

	bool Install(void) {
		if (installed) return true;
		StartupProgress::Reset();
		if (!IsSupportedGameVersion()) {
			Log(LogCategory::Initialization, "startup resource progress patch skipped: requires SimCity 4 1.1.641");
			return false;
		}
		auto const module = reinterpret_cast<uint8_t *>(GetModuleHandleA(nullptr));
		target = module == nullptr ? nullptr : module + kPackedFileOpenRva;
		if (!HasExpectedPrologue(target)) {
			Log(LogCategory::Initialization,
			    "startup resource progress patch skipped: cGZDBSegmentPackedFile::Open is already modified");
			target = nullptr;
			return false;
		}
		if (!CreateTrampoline() || !ActivateHook()) {
			Log(LogCategory::Initialization, "startup resource progress patch installation failed");
			ReleaseTrampoline();
			target = nullptr;
			return false;
		}
		installed = true;
		Log(LogCategory::Initialization,
		    "startup resource progress patch installed at cGZDBSegmentPackedFile::Open (00972A17)");
		return true;
	}

	void Uninstall(void) {
		if (!installed) return;
		DWORD previousProtection = 0;
		if (VirtualProtect(target, kPatchSize, PAGE_EXECUTE_READWRITE, &previousProtection)) {
			std::memcpy(target, kExpectedPrologue.data(), kPatchSize);
			FlushInstructionCache(GetCurrentProcess(), target, kPatchSize);
			DWORD ignored = 0;
			VirtualProtect(target, kPatchSize, previousProtection, &ignored);
		}
		installed = false;
		ReleaseTrampoline();
		target = nullptr;
	}
}
