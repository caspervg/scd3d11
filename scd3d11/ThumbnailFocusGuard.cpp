/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

// Keeps the region view thumbnail when SC4 loses focus while saving.
//
// SC4WriteCityRegionViewThumbnail (0x005DDEC0) renders the city and reads it
// back with CopyColorBuffer. The Windows build then checks focus with
// 0x005F85F0 (GetActiveWindow() == GetForegroundWindow()). If focus was lost,
// it waits up to about 10 s and then writes a blank thumbnail instead. SCD3D11
// renders to an off-screen texture, so focus cannot spoil the capture. Two
// patches make the check after rendering ask whether the window is usable:
//
//   0x005F85F0  push esi; call [GetForegroundWindow]     ->  jmp CaptureIsUsable
//   0x005DE976  mov al,[esi+9]; test al,al; jz 005DE9F7  ->  mov al,[esi+8]; test al,al; jnz 005DE9F7
//
// [esi+9] is set by any loss of focus, [esi+8] holds the result of the check
// after rendering. A minimized window still counts as unusable and falls back
// to SC4's own wait and retry.
//
// SimCity 4 Deluxe 1.1.641 only, image base 0x00400000.

#include "ThumbnailFocusGuard.h"

#include "Diagnostics.h"
#include "GameVersion.h"

#include <cstring>
#include <intrin.h>
#include <vector>

#include <windows.h>

#pragma intrinsic(_ReturnAddress)

namespace nSCD3D11::ThumbnailFocusGuard {
	namespace {

		constexpr uintptr_t kFocusTestVA = 0x005F85F0;
		constexpr size_t kFocusTestLength = 7;
		const uint8_t kFocusTestExpected[kFocusTestLength] = {0x56, 0xFF, 0x15, 0x0C, 0x05, 0xA8, 0x00};

		constexpr uintptr_t kRetryCheckVA = 0x005DE976;
		constexpr size_t kRetryCheckLength = 7;
		const uint8_t kRetryCheckExpected[kRetryCheckLength] = {0x8A, 0x46, 0x09, 0x84, 0xC0, 0x74, 0x7A};
		const uint8_t kRetryCheckPatched[kRetryCheckLength] = {0x8A, 0x46, 0x08, 0x84, 0xC0, 0x75, 0x7A};

		// Return addresses of the focus test calls that matter; the other two are SC4's wait loop.
		constexpr uintptr_t kBeforeRenderReturnVA = 0x005DDF07;
		constexpr uintptr_t kAfterRenderReturnVA = 0x005DE967;

		uint8_t *gFocusTest = nullptr;
		uint8_t *gRetryCheck = nullptr;
		uint8_t gFocusTestSaved[kFocusTestLength]{};
		bool gInstalled = false;
		bool gAttempted = false;

		// Main thread only: the driver window and SC4's thumbnail code both run there.
		HWND gWindow = nullptr;
		unsigned gMinimizes = 0;
		unsigned gDeactivations = 0;
		unsigned gMinimizesBeforeRender = 0;
		unsigned gDeactivationsBeforeRender = 0;

		bool IsUsableWindow() {
			return gWindow == nullptr || !IsWindow(gWindow) || !IsIconic(gWindow);
		}

		// Replaces the focus test. Reached by a jmp, so the return address is SC4's call site.
		bool __cdecl CaptureIsUsable() {
			uintptr_t const caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
			bool const usable = IsUsableWindow();
			if (caller == kBeforeRenderReturnVA) {
				gMinimizesBeforeRender = gMinimizes;
				gDeactivationsBeforeRender = gDeactivations;
				return usable;
			}
			if (caller != kAfterRenderReturnVA) return usable;

			if (!usable || gMinimizes != gMinimizesBeforeRender) {
				Log(LogCategory::Window, "thumbnail: window minimized while rendering, leaving SC4 to wait and retry");
				return false;
			}
			if (gDeactivations != gDeactivationsBeforeRender || GetActiveWindow() != GetForegroundWindow()) {
				Log(LogCategory::Window,
				    "thumbnail: kept the render although SC4 lost focus during it (%u deactivations, foreground %p)",
				    gDeactivations - gDeactivationsBeforeRender, GetForegroundWindow());
			}
			return true;
		}

		bool WriteCode(uint8_t *target, uint8_t const *bytes, size_t length) {
			DWORD protection = 0;
			if (!VirtualProtect(target, length, PAGE_EXECUTE_READWRITE, &protection)) return false;
			std::memcpy(target, bytes, length);
			DWORD ignored = 0;
			VirtualProtect(target, length, protection, &ignored);
			FlushInstructionCache(GetCurrentProcess(), target, length);
			return true;
		}

	} // namespace

	bool Install(void *window) {
		gWindow = static_cast<HWND>(window);
		if (gInstalled) return true;
		if (gAttempted) return false;
		gAttempted = true;

		if (!IsGameVersion641()) {
			Log(LogCategory::Initialization, "thumbnail focus guard: requires SimCity 4 1.1.641, skipping");
			return false;
		}
		// IsGameVersion641 only accepts an executable at its preferred base, so
		// the game's addresses are used as they are.
		gFocusTest = reinterpret_cast<uint8_t *>(kFocusTestVA);
		gRetryCheck = reinterpret_cast<uint8_t *>(kRetryCheckVA);

		if (std::memcmp(gFocusTest, kFocusTestExpected, kFocusTestLength) != 0 ||
		    std::memcmp(gRetryCheck, kRetryCheckExpected, kRetryCheckLength) != 0) {
			Log(LogCategory::Initialization, "thumbnail focus guard: SC4WriteCityRegionViewThumbnail already modified, skipping");
			return false;
		}

		std::memcpy(gFocusTestSaved, gFocusTest, kFocusTestLength);
		uint8_t jump[kFocusTestLength] = {0xE9, 0, 0, 0, 0, 0x90, 0x90};
		int32_t const displacement = static_cast<int32_t>(
			reinterpret_cast<uintptr_t>(&CaptureIsUsable) - (reinterpret_cast<uintptr_t>(gFocusTest) + 5));
		std::memcpy(jump + 1, &displacement, sizeof(displacement));
		if (!WriteCode(gRetryCheck, kRetryCheckPatched, kRetryCheckLength)) return false;
		if (!WriteCode(gFocusTest, jump, kFocusTestLength)) {
			WriteCode(gRetryCheck, kRetryCheckExpected, kRetryCheckLength);
			return false;
		}

		gInstalled = true;
		Log(LogCategory::Initialization,
		    "thumbnail focus guard installed: losing focus while saving no longer blanks the region thumbnail");
		return true;
	}

	void Uninstall() {
		if (!gInstalled) return;
		WriteCode(gFocusTest, gFocusTestSaved, kFocusTestLength);
		WriteCode(gRetryCheck, kRetryCheckExpected, kRetryCheckLength);
		gInstalled = false;
		gWindow = nullptr;
		Log(LogCategory::Initialization, "thumbnail focus guard uninstalled");
	}

	void NoteWindowMessage(uint32_t message, uintptr_t wParam) {
		if (message == WM_SIZE && wParam == SIZE_MINIMIZED) ++gMinimizes;
		else if (message == WM_ACTIVATEAPP && wParam == FALSE) ++gDeactivations;
	}

} // namespace nSCD3D11::ThumbnailFocusGuard
