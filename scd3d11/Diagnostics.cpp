/*
 *  SCD3D11 - a free graphics driver for SimCity 4's SimGL interface
 *  Copyright (C) 2025  Nelson Gomez (nsgomez) <nelson@ngomez.me>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

#include "Diagnostics.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <thread>
#include <unordered_set>

namespace nSCD3D11
{
	namespace
	{
		// Window and swap chain events repeat on every focus change, so they get a larger budget.
		constexpr unsigned int kMessagesPerCategory[] = { 128, 128, 1024, 128, 128, 128, 1024 };
		std::atomic<unsigned int> messageCounts[static_cast<unsigned int>(LogCategory::Count)]{};
		std::unordered_set<uint64_t> observed[static_cast<unsigned int>(ObservedCategory::Count)];

		char const* CategoryName(LogCategory category) {
			static char const* names[] = { "init", "caps", "swapchain", "resource", "grid", "unsupported", "window" };
			return names[static_cast<unsigned int>(category)];
		}

		// The log goes next to the user Plugins folder, i.e. the parent of the folder holding SCD3D11.dll.
		// Falls back to the working directory if the module path is unavailable.
		void WriteLogLine(char const* message) {
			static std::filesystem::path const logPath = [] {
				HMODULE module = nullptr;
				wchar_t path[MAX_PATH]{};
				if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				                        reinterpret_cast<LPCWSTR>(&WriteLogLine), &module) ||
				    GetModuleFileNameW(module, path, MAX_PATH) == 0) {
					return std::filesystem::path(L"SC4D3D11.log");
				}
				return std::filesystem::path(path).parent_path().parent_path() / L"SC4D3D11.log";
			}();

			// _wfopen_s does not share the file, so a concurrent writer (the watchdog) would lose its line.
			static std::mutex mutex;
			std::lock_guard<std::mutex> const lock(mutex);
			static bool opened = false;
			wchar_t const* const mode = opened ? L"a" : L"w";
			opened = true;
			FILE* file = nullptr;
			if (_wfopen_s(&file, logPath.c_str(), mode) == 0) {
				fputs(message, file);
				fclose(file);
			}
		}
	}

	namespace {
		void LogV(LogCategory category, char const* format, va_list arguments) {
			unsigned int const index = static_cast<unsigned int>(category);
			if (index >= static_cast<unsigned int>(LogCategory::Count)) {
				return;
			}

			unsigned int const count = messageCounts[index]++;
			if (count > kMessagesPerCategory[index]) {
				return;
			}

			char message[1024]{};
			if (count == kMessagesPerCategory[index]) {
				sprintf_s(message, "[SC4D3D11][%s] further messages suppressed\n", CategoryName(category));
			}
			else {
				char detail[896]{};
				vsnprintf_s(detail, sizeof(detail), _TRUNCATE, format, arguments);
				sprintf_s(message, "[SC4D3D11][%s] %s\n", CategoryName(category), detail);
			}

			OutputDebugStringA(message);
			WriteLogLine(message);
		}
	}

	void Log(LogCategory category, char const* format, ...) {
		va_list arguments;
		va_start(arguments, format);
		LogV(category, format, arguments);
		va_end(arguments);
	}

	void LogTrace(LogCategory category, char const* format, ...) {
		static bool const enabled = std::strstr(GetCommandLineA(), "-LogLevel:trace") != nullptr;
		if (!enabled) return;
		va_list arguments;
		va_start(arguments, format);
		LogV(category, format, arguments);
		va_end(arguments);
	}

	void LogHRESULT(LogCategory category, char const* operation, HRESULT result) {
		Log(category, "%s failed (HRESULT 0x%08lX)", operation, static_cast<unsigned long>(result));
	}

	void RecordEncountered(ObservedCategory category, uint64_t value) {
#ifndef NDEBUG
		unsigned int const index = static_cast<unsigned int>(category);
		if (index >= static_cast<unsigned int>(ObservedCategory::Count)) return;
		auto& values = observed[index];
		if (values.size() >= 4096 || !values.insert(value).second) return;

		static char const* names[] = { "render-state", "vertex-format", "texture-format" };
		char message[128]{};
		sprintf_s(message, "[SC4D3D11][state] %s=0x%016llX\n", names[index], static_cast<unsigned long long>(value));
		WriteLogLine(message);
#else
		(void)category;
		(void)value;
#endif
	}

	namespace {
		constexpr ULONGLONG kStallMilliseconds = 5000;
		constexpr ULONGLONG kStallRepeatMilliseconds = 15000;
		constexpr unsigned kStallReportsPerStall = 5;
		constexpr size_t kStackCandidates = 12;

		std::atomic<char const*> renderPhase{"none"};
		std::atomic<ULONGLONG> lastRenderFrame{0};
		std::atomic<DWORD> renderThreadId{0};
		std::atomic<HWND> watchedWindow{nullptr};
		std::mutex watchdogMutex;
		std::thread watchdogThread;
		HANDLE watchdogStop = nullptr;

		// "module+0xRVA", or the bare address outside any module.
		void DescribeAddress(uintptr_t address, char* text, size_t size) {
			HMODULE module = nullptr;
			char path[MAX_PATH]{};
			if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			                        reinterpret_cast<LPCSTR>(address), &module) ||
			    GetModuleFileNameA(module, path, MAX_PATH) == 0) {
				sprintf_s(text, size, "%p", reinterpret_cast<void*>(address));
				return;
			}
			char const* const slash = std::strrchr(path, '\\');
			sprintf_s(text, size, "%p %s+0x%IX", reinterpret_cast<void*>(address), slash ? slash + 1 : path,
			          address - reinterpret_cast<uintptr_t>(module));
		}

		bool IsLikelyReturnAddress(uintptr_t address) {
			MEMORY_BASIC_INFORMATION memory{};
			if (address < 6 || VirtualQuery(reinterpret_cast<LPCVOID>(address - 6), &memory, sizeof(memory)) != sizeof(memory) ||
			    memory.State != MEM_COMMIT || memory.Type != MEM_IMAGE ||
			    (memory.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) == 0) {
				return false;
			}
			auto const* const code = reinterpret_cast<uint8_t const*>(address);
			// call rel32, call [reg+disp8]/call reg, call [mem]/call [reg+disp32]
			return code[-5] == 0xE8 || code[-2] == 0xFF || code[-3] == 0xFF || code[-6] == 0xFF;
		}

		void LogRenderThreadLocation(ULONGLONG stalled) {
			uintptr_t instruction = 0;
			uintptr_t stackPointer = 0;
			uintptr_t stack[512]{};
			SIZE_T stackBytes = 0;
			DWORD const threadId = renderThreadId.load();
			HANDLE const thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, threadId);
			if (thread != nullptr) {
				// Only copy while suspended: the thread may hold the heap or loader lock.
				if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
					CONTEXT context{};
					context.ContextFlags = CONTEXT_CONTROL;
					if (GetThreadContext(thread, &context)) {
#ifdef _M_IX86
						instruction = context.Eip;
						stackPointer = context.Esp;
#else
						instruction = context.Rip;
						stackPointer = context.Rsp;
#endif
						ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(stackPointer), stack,
						                  sizeof(stack), &stackBytes);
					}
					ResumeThread(thread);
				}
				CloseHandle(thread);
			}

			HWND const window = watchedWindow.load();
			HWND const foreground = GetForegroundWindow();
			DWORD foregroundProcess = 0;
			if (foreground != nullptr) GetWindowThreadProcessId(foreground, &foregroundProcess);
			char location[MAX_PATH + 64]{};
			DescribeAddress(instruction, location, sizeof(location));
			Log(LogCategory::Window,
			    "watchdog: no frame for %llu ms; phase=%s thread=%lu at %s; window=%p iconic=%d visible=%d "
			    "foreground=%p (%s)",
			    static_cast<unsigned long long>(stalled), renderPhase.load(), threadId, location, window,
			    window != nullptr && IsIconic(window) ? 1 : 0, window != nullptr && IsWindowVisible(window) ? 1 : 0,
			    foreground, foreground == window ? "driver window" :
			                foregroundProcess == GetCurrentProcessId() ? "this process" : "other process");

			size_t found = 0;
			for (size_t index = 0; index < stackBytes / sizeof(uintptr_t) && found < kStackCandidates; ++index) {
				if (!IsLikelyReturnAddress(stack[index])) continue;
				DescribeAddress(stack[index], location, sizeof(location));
				Log(LogCategory::Window, "watchdog:   [esp+0x%zX] %s", index * sizeof(uintptr_t), location);
				++found;
			}
		}

		void RunRenderWatchdog(HANDLE stop) {
			ULONGLONG stalledFrame = 0;
			ULONGLONG nextReport = 0;
			unsigned reports = 0;
			// SC4 loads plugins on the render thread after its first Flush; that is not a stall. Arm only once
			// frames have kept coming for a while (10 half-second ticks).
			ULONGLONG previousFrame = 0;
			unsigned framesFlowing = 0;
			while (WaitForSingleObject(stop, 500) == WAIT_TIMEOUT) {
				ULONGLONG const frame = lastRenderFrame.load();
				if (frame == 0) continue;
				if (framesFlowing < 10) {
					framesFlowing = frame != previousFrame ? framesFlowing + 1 : 0;
					previousFrame = frame;
					continue;
				}
				ULONGLONG const now = GetTickCount64();
				if (stalledFrame != 0 && frame != stalledFrame) {
					Log(LogCategory::Window, "watchdog: frames resumed after %llu ms",
					    static_cast<unsigned long long>(frame - stalledFrame));
					stalledFrame = 0;
				}
				if (now - frame < kStallMilliseconds) continue;
				if (stalledFrame == 0) {
					stalledFrame = frame;
					reports = 0;
					nextReport = now;
				}
				if (now >= nextReport && reports < kStallReportsPerStall) {
					LogRenderThreadLocation(now - frame);
					++reports;
					nextReport = now + kStallRepeatMilliseconds;
				}
			}
		}
	}

	void NoteRenderPhase(char const* phase) {
		renderPhase.store(phase, std::memory_order_relaxed);
	}

	void NoteRenderFrame() {
		renderThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed);
		lastRenderFrame.store(GetTickCount64(), std::memory_order_relaxed);
	}

	void StartRenderWatchdog(HWND window) {
		watchedWindow.store(window);
		std::lock_guard<std::mutex> const lock(watchdogMutex);
		if (watchdogThread.joinable()) return;
		watchdogStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		if (watchdogStop == nullptr) return;
		watchdogThread = std::thread(RunRenderWatchdog, watchdogStop);
	}

	void StopRenderWatchdog() {
		std::lock_guard<std::mutex> const lock(watchdogMutex);
		if (watchdogThread.joinable()) {
			SetEvent(watchdogStop);
			watchdogThread.join();
		}
		if (watchdogStop != nullptr) {
			CloseHandle(watchdogStop);
			watchdogStop = nullptr;
		}
		watchedWindow.store(nullptr);
		lastRenderFrame.store(0);
	}
}
