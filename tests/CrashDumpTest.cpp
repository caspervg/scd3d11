/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

#include "CrashDump.h"

#include <windows.h>

#include <cassert>
#include <cstring>
#include <filesystem>
#include <string>

using nSCD3D11::CrashDump::IsDriverStorePath;

namespace {
	std::wstring gMarker;

	// Stands in for SC4's exception report filter, installed before SCD3D11's.
	LONG WINAPI PreviousFilter(EXCEPTION_POINTERS *) {
		HANDLE const file = CreateFileW(gMarker.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
		if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
		return EXCEPTION_EXECUTE_HANDLER;
	}

	// The child: crashes outside any display driver, which only the unhandled exception filter sees.
	int Crash(wchar_t const *marker) {
		gMarker = marker;
		SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
		SetUnhandledExceptionFilter(PreviousFilter);
		nSCD3D11::CrashDump::Arm();
		// A fault code catches itself is not a crash: no dump, and the session's one dump is still unused.
		__try {
			*static_cast<int volatile *>(nullptr) = 1;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
		}
		*static_cast<int volatile *>(nullptr) = 2;
		return 0;
	}

	void TestUnhandledCrashWritesDump(wchar_t const *self) {
		namespace fs = std::filesystem;
		wchar_t temp[MAX_PATH]{};
		GetTempPathW(MAX_PATH, temp);
		fs::path const root = fs::path(temp) / (L"scd3d11-crashdump-test-" + std::to_wstring(GetCurrentProcessId()));
		fs::remove_all(root);
		fs::create_directories(root);
		fs::path const marker = root / L"previous-filter-ran";

		// The child writes under %LOCALAPPDATA%, so point that at the scratch folder.
		SetEnvironmentVariableW(L"LOCALAPPDATA", root.c_str());
		std::wstring commandLine = L"\"" + std::wstring(self) + L"\" crash \"" + marker.wstring() + L"\"";
		STARTUPINFOW startup{sizeof(startup)};
		PROCESS_INFORMATION process{};
		BOOL const started = CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
		                                    &startup, &process);
		assert(started);
		assert(WaitForSingleObject(process.hProcess, 120000) == WAIT_OBJECT_0);
		DWORD exitCode = 0;
		GetExitCodeProcess(process.hProcess, &exitCode);
		CloseHandle(process.hThread);
		CloseHandle(process.hProcess);

		// The crash still reached SC4's filter and ended the process with the fault's code.
		assert(exitCode == static_cast<DWORD>(EXCEPTION_ACCESS_VIOLATION));
		assert(fs::exists(marker));
		size_t dumps = 0;
		for (auto const &entry : fs::directory_iterator(root / L"SCD3D11" / L"CrashDumps")) {
			assert(entry.path().extension() == L".dmp");
			assert(entry.file_size() > 0);
			++dumps;
		}
		assert(dumps == 1);
		fs::remove_all(root);
	}
} // namespace

int wmain(int argc, wchar_t **argv) {
	if (argc == 3 && std::wcscmp(argv[1], L"crash") == 0) return Crash(argv[2]);

	// GetMappedFileName gives NT device paths.
	assert(IsDriverStorePath(L"\\Device\\HarddiskVolume3\\Windows\\System32\\DriverStore\\FileRepository\\"
	                         L"u0205306.inf_amd64_ed4bdb51fc506ec7\\B026676\\amdxx32.dll"));
	assert(IsDriverStorePath(L"C:\\WINDOWS\\SYSTEM32\\DRIVERSTORE\\FILEREPOSITORY\\nv_dispi.inf_amd64\\nvwgf2um.dll"));
	assert(!IsDriverStorePath(L"\\Device\\HarddiskVolume3\\Windows\\System32\\d3d11.dll"));
	assert(!IsDriverStorePath(L"C:\\Program Files (x86)\\SimCity 4 Deluxe Edition\\Apps\\dxgi.dll"));
	assert(!IsDriverStorePath(L"D:\\Games\\DriverStore\\amdxx32.dll"));

	TestUnhandledCrashWritesDump(argv[0]);
	return 0;
}
