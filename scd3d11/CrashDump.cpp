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

#include "Diagnostics.h"
#include "Settings.h"

#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>

#include <atomic>
#include <cwctype>
#include <string>

namespace nSCD3D11::CrashDump {
	namespace {
		using MiniDumpWriteDumpFunction = BOOL(WINAPI *)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
		                                                 PMINIDUMP_EXCEPTION_INFORMATION,
		                                                 PMINIDUMP_USER_STREAM_INFORMATION,
		                                                 PMINIDUMP_CALLBACK_INFORMATION);

		// How long the faulting thread waits for the dump. A full dump of a 32-bit process takes seconds; this
		// only bounds a writer that deadlocked on a lock the faulting thread holds.
		constexpr DWORD kWriteTimeoutMilliseconds = 120000;

		MiniDumpWriteDumpFunction gWriteDump = nullptr;
		PVOID gHandler = nullptr;
		HANDLE gWriter = nullptr;
		HANDLE gRequest = nullptr;
		HANDLE gDone = nullptr;
		HANDLE gStop = nullptr;
		std::wstring gDirectory;
		std::atomic<bool> gFired{false};
		EXCEPTION_POINTERS *gException = nullptr;
		DWORD gFaultingThread = 0;

		std::string Narrow(std::wstring const &text) {
			int const size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
			if (size <= 1) return {};
			std::string result(static_cast<size_t>(size), '\0');
			WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, result.data(), size, nullptr, nullptr);
			result.resize(static_cast<size_t>(size - 1));
			return result;
		}

		// "amdxx32.dll+0x3F5470" for the log. Uses the mapped file name, which needs no loader lock.
		std::wstring DescribeAddress(void *address) {
			MEMORY_BASIC_INFORMATION memory{};
			if (VirtualQuery(address, &memory, sizeof(memory)) != sizeof(memory) || memory.Type != MEM_IMAGE) return L"?";
			wchar_t path[MAX_PATH]{};
			if (K32GetMappedFileNameW(GetCurrentProcess(), memory.AllocationBase, path, MAX_PATH) == 0) return L"?";
			wchar_t const *const slash = wcsrchr(path, L'\\');
			wchar_t offset[32]{};
			swprintf_s(offset, L"+0x%IX", reinterpret_cast<uintptr_t>(address) -
			                               reinterpret_cast<uintptr_t>(memory.AllocationBase));
			return std::wstring(slash != nullptr ? slash + 1 : path) + offset;
		}

		bool IsInDisplayDriver(void *address) {
			MEMORY_BASIC_INFORMATION memory{};
			if (VirtualQuery(address, &memory, sizeof(memory)) != sizeof(memory) || memory.Type != MEM_IMAGE) return false;
			wchar_t path[MAX_PATH]{};
			if (K32GetMappedFileNameW(GetCurrentProcess(), memory.AllocationBase, path, MAX_PATH) == 0) return false;
			return IsDriverStorePath(path);
		}

		void WriteDump() {
			SYSTEMTIME now{};
			GetLocalTime(&now);
			wchar_t name[96]{};
			swprintf_s(name, L"SimCity4-%04u%02u%02u-%02u%02u%02u-%lu.dmp", now.wYear, now.wMonth, now.wDay, now.wHour,
			           now.wMinute, now.wSecond, GetCurrentProcessId());
			std::wstring const path = gDirectory + L"\\" + name;
			std::wstring const where = DescribeAddress(gException->ExceptionRecord->ExceptionAddress);
			Log(LogCategory::Initialization, "CrashDump: exception 0x%08lX at %s on thread %lu, writing %s",
			    gException->ExceptionRecord->ExceptionCode, Narrow(where).c_str(), gFaultingThread,
			    Narrow(path).c_str());

			HANDLE const file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL,
			                                nullptr);
			if (file == INVALID_HANDLE_VALUE) {
				Log(LogCategory::Initialization, "CrashDump: could not create the dump file (error %lu)", GetLastError());
				return;
			}
			MINIDUMP_EXCEPTION_INFORMATION exception{};
			exception.ThreadId = gFaultingThread;
			exception.ExceptionPointers = gException;
			exception.ClientPointers = FALSE;
			auto const type = static_cast<MINIDUMP_TYPE>(MiniDumpWithFullMemory | MiniDumpWithFullMemoryInfo |
			                                             MiniDumpWithHandleData | MiniDumpWithThreadInfo |
			                                             MiniDumpWithUnloadedModules);
			BOOL const written = gWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, type, &exception, nullptr,
			                                nullptr);
			DWORD const error = written ? 0 : GetLastError();
			CloseHandle(file);
			if (written) {
				Log(LogCategory::Initialization, "CrashDump: dump written");
			} else {
				Log(LogCategory::Initialization, "CrashDump: MiniDumpWriteDump failed (0x%08lX)", error);
			}
		}

		// Writes from its own thread: MiniDumpWriteDump should not run on the thread that faulted, and a thread
		// started ahead of time needs no loader lock when the fault comes.
		DWORD WINAPI RunWriter(void *) {
			HANDLE const events[] = {gRequest, gStop};
			if (WaitForMultipleObjects(2, events, FALSE, INFINITE) == WAIT_OBJECT_0) {
				WriteDump();
				SetEvent(gDone);
			}
			return 0;
		}

		// Writes the session's one dump, from the writer thread, and waits for it.
		void RequestDump(EXCEPTION_POINTERS *info) {
			if (gFired.exchange(true)) return;
			gException = info;
			gFaultingThread = GetCurrentThreadId();
			SetEvent(gRequest);
			WaitForSingleObject(gDone, kWriteTimeoutMilliseconds);
		}

		// First chance, for faults inside the display driver: still works when another plugin replaces the
		// unhandled exception filter after SCD3D11.
		LONG CALLBACK OnException(EXCEPTION_POINTERS *info) {
			DWORD const code = info->ExceptionRecord->ExceptionCode;
			if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION) return EXCEPTION_CONTINUE_SEARCH;
			if (gFired.load(std::memory_order_relaxed)) return EXCEPTION_CONTINUE_SEARCH;
			if (!IsInDisplayDriver(info->ExceptionRecord->ExceptionAddress)) return EXCEPTION_CONTINUE_SEARCH;
			RequestDump(info);
			// SC4's own exception report still runs.
			return EXCEPTION_CONTINUE_SEARCH;
		}

		// Every crash nothing handled, anywhere in the process. Exceptions code catches on purpose (SC4CPUOptions'
		// cull workers, for one) never get here. Runs ahead of SC4's own report filter, then hands over to it.
		LPTOP_LEVEL_EXCEPTION_FILTER gPreviousFilter = nullptr;
		bool gFilterInstalled = false;

		LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS *info) {
			RequestDump(info);
			return gPreviousFilter != nullptr ? gPreviousFilter(info) : EXCEPTION_CONTINUE_SEARCH;
		}

		std::wstring DumpDirectory() {
			wchar_t localAppData[MAX_PATH]{};
			DWORD const length = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
			if (length == 0 || length >= MAX_PATH) return {};
			std::wstring directory = std::wstring(localAppData) + L"\\SCD3D11";
			CreateDirectoryW(directory.c_str(), nullptr);
			directory += L"\\CrashDumps";
			CreateDirectoryW(directory.c_str(), nullptr);
			DWORD const attributes = GetFileAttributesW(directory.c_str());
			if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) return {};
			return directory;
		}
	} // namespace

	bool IsDriverStorePath(std::wstring_view path) {
		std::wstring lower(path);
		for (wchar_t &character : lower) character = static_cast<wchar_t>(std::towlower(character));
		return lower.find(L"\\driverstore\\filerepository\\") != std::wstring::npos;
	}

	void Install() {
		if (Settings::Get().crashDump) Arm();
	}

	void Arm() {
		if (gHandler != nullptr) return;
		gDirectory = DumpDirectory();
		if (gDirectory.empty()) {
			Log(LogCategory::Initialization, "CrashDump: no folder under %%LOCALAPPDATA%%, not installed");
			return;
		}
		// By full path: SC4 ships an old DbgHelp.dll in Apps that predates most dump types.
		wchar_t system[MAX_PATH]{};
		UINT const length = GetSystemDirectoryW(system, MAX_PATH);
		if (length == 0 || length >= MAX_PATH) return;
		HMODULE const dbghelp = LoadLibraryW((std::wstring(system) + L"\\dbghelp.dll").c_str());
		gWriteDump = dbghelp != nullptr
			             ? reinterpret_cast<MiniDumpWriteDumpFunction>(GetProcAddress(dbghelp, "MiniDumpWriteDump"))
			             : nullptr;
		if (gWriteDump == nullptr) {
			Log(LogCategory::Initialization, "CrashDump: could not load the system dbghelp.dll, not installed");
			return;
		}
		gRequest = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		gDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		gStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		if (gRequest != nullptr && gDone != nullptr && gStop != nullptr) {
			gWriter = CreateThread(nullptr, 0, RunWriter, nullptr, 0, nullptr);
		}
		if (gWriter == nullptr) {
			Log(LogCategory::Initialization, "CrashDump: could not start the writer thread, not installed");
			Uninstall();
			return;
		}
		gHandler = AddVectoredExceptionHandler(1, OnException);
		gPreviousFilter = SetUnhandledExceptionFilter(OnUnhandledException);
		gFilterInstalled = true;
		Log(LogCategory::Initialization, "CrashDump: the first crash writes a full dump to %s",
		    Narrow(gDirectory).c_str());
	}

	void Uninstall() {
		if (gFilterInstalled) {
			// Put back SC4's filter, unless another plugin has replaced SCD3D11's since.
			LPTOP_LEVEL_EXCEPTION_FILTER const current = SetUnhandledExceptionFilter(gPreviousFilter);
			if (current != OnUnhandledException) SetUnhandledExceptionFilter(current);
			gFilterInstalled = false;
			gPreviousFilter = nullptr;
		}
		if (gHandler != nullptr) {
			RemoveVectoredExceptionHandler(gHandler);
			gHandler = nullptr;
		}
		if (gWriter != nullptr) {
			SetEvent(gStop);
			WaitForSingleObject(gWriter, INFINITE);
			CloseHandle(gWriter);
			gWriter = nullptr;
		}
		for (HANDLE *event : {&gRequest, &gDone, &gStop}) {
			if (*event != nullptr) {
				CloseHandle(*event);
				*event = nullptr;
			}
		}
	}
} // namespace nSCD3D11::CrashDump
