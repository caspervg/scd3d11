/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

#include "RemoteRenderer.h"

#include "Diagnostics.h"
#include "Settings.h"

#include <string>

namespace nSCD3D11 {
	namespace {
		// The stream's buffer in shared memory. It counts against SC4's 32-bit address space.
		constexpr uint64_t kRingBytes = 64ull << 20;
		// How long a frame may take before the server counts as stuck.
		constexpr DWORD kFrameTimeoutMs = 3000;

		std::wstring ModuleFolder() {
			HMODULE module = nullptr;
			GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			                   reinterpret_cast<LPCWSTR>(&ModuleFolder), &module);
			wchar_t path[MAX_PATH]{};
			GetModuleFileNameW(module, path, MAX_PATH);
			std::wstring folder = path;
			return folder.substr(0, folder.find_last_of(L"\\/") + 1);
		}
	}

	RemoteRenderer::~RemoteRenderer() {
		if (ring.Control() != nullptr) ring.Close();
		if (process != nullptr) CloseHandle(process);
		// Closing the job ends the server if it is still running.
		if (job != nullptr) CloseHandle(job);
	}

	bool RemoteRenderer::Start() {
		DWORD const self = GetCurrentProcessId();
		std::wstring const name = L"Local\\SCD3D12-" + std::to_wstring(self);
		if (!ring.Create(name, kRingBytes)) {
			Log(LogCategory::Initialization, "server: cannot create the shared ring");
			return false;
		}
		ring.Control()->presentMode = Settings::Get().serverComposition ? nSCDWire::kPresentComposition
		                                                                : nSCDWire::kPresentCopy;
		// SC4 ignores subfolders of Plugins when it loads DLLs, so the server and its own DLLs live in one.
		std::wstring folder = ModuleFolder() + L"SCD3D12\\";
		if (GetFileAttributesW((folder + L"SCD3D12Server.exe").c_str()) == INVALID_FILE_ATTRIBUTES) folder = ModuleFolder();
		std::wstring const server = folder + L"SCD3D12Server.exe";
		std::wstring const log = ModuleFolder() + L"..\\Logs\\SCD3D12Server.log";
		std::wstring command = L"\"" + server + L"\" serve --ring " + name + L" --parent " + std::to_wstring(self) +
		                       L" --log \"" + log + L"\"";
		std::string const &relight = Settings::Get().serverRelight;
		if (!relight.empty() && relight != "off") {
			command += L" --relight " + std::wstring(relight.begin(), relight.end());
			std::string const &light = Settings::Get().serverLight;
			if (!light.empty() && light != "off") command += L" --light " + std::wstring(light.begin(), light.end());
		}
		std::string const &scene = Settings::Get().serverScene;
		if (!scene.empty() && scene != "off") command += L" --scene " + std::wstring(scene.begin(), scene.end());
		if (DrawStatsEnabled()) command += L" --stats on";

		job = CreateJobObjectW(nullptr, nullptr);
		JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
		limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
		if (job != nullptr) SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));

		STARTUPINFOW startup{sizeof(startup)};
		PROCESS_INFORMATION info{};
		if (!CreateProcessW(server.c_str(), command.data(), nullptr, nullptr, FALSE,
		                    CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, folder.c_str(), &startup, &info)) {
			Log(LogCategory::Initialization, "server: cannot start SCD3D12Server.exe (error %lu), drawing locally",
			    GetLastError());
			return false;
		}
		if (job == nullptr || !AssignProcessToJobObject(job, info.hProcess)) {
			Log(LogCategory::Initialization, "server: cannot tie the server's lifetime to the game (error %lu)",
			    GetLastError());
		}
		ResumeThread(info.hThread);
		CloseHandle(info.hThread);
		process = info.hProcess;
		active = true;
		Log(LogCategory::Initialization, "server: started SCD3D12Server.exe (process %lu)", info.dwProcessId);
		return true;
	}

	void RemoteRenderer::Stop(char const *reason) {
		if (!active) return;
		active = false;
		Log(LogCategory::Initialization, "server: %s, drawing locally from now on", reason);
		if (ring.Control() != nullptr) ring.Close();
		if (process != nullptr) TerminateProcess(process, 1);
	}

	bool RemoteRenderer::ServerAlive() const {
		return process != nullptr && WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
	}

	nSCDWire::Writer::Sink RemoteRenderer::Sink() {
		return [this](void const *data, size_t size) {
			if (!active) return false;
			if (!ring.Write(data, size, [this] { return ServerAlive(); })) {
				Stop("the server stopped reading");
				return false;
			}
			return true;
		};
	}

	void RemoteRenderer::SetAdapter(LUID adapter) {
		nSCDWire::RingControl *const control = ring.Control();
		if (control == nullptr) return;
		control->adapterLow = adapter.LowPart;
		control->adapterHigh = adapter.HighPart;
		control->adapterSet.store(1, std::memory_order_release);
	}

	bool RemoteRenderer::OpenGeneration(ID3D11Device *device) {
		nSCDWire::RingControl *const control = ring.Control();
		uint32_t const current = control->frameGeneration.load(std::memory_order_acquire);
		if (current == generation) return frameFence != nullptr;
		generation = current;
		for (auto &texture: textures) texture.Reset();
		frameFence.Reset();
		consumedFence.Reset();

		Microsoft::WRL::ComPtr<ID3D11Device1> device1;
		Microsoft::WRL::ComPtr<ID3D11Device5> device5;
		if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device1))) || FAILED(device->QueryInterface(IID_PPV_ARGS(&device5)))) {
			Stop("this Direct3D 11 runtime cannot open shared fences");
			return false;
		}
		auto const handle = [](uint64_t value) { return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(value)); };
		HRESULT result = S_OK;
		char const *step = "texture";
		for (int i = 0; i < 2 && SUCCEEDED(result); ++i) {
			result = device1->OpenSharedResource1(handle(control->frameTextures[i]), IID_PPV_ARGS(&textures[i]));
		}
		if (SUCCEEDED(result)) {
			step = "frame fence";
			result = device5->OpenSharedFence(handle(control->frameFence), IID_PPV_ARGS(&frameFence));
		}
		if (SUCCEEDED(result)) {
			step = "consumed fence";
			result = device5->OpenSharedFence(handle(control->consumedFence), IID_PPV_ARGS(&consumedFence));
		}
		// The opened objects hold their own references.
		for (uint64_t value: {control->frameTextures[0], control->frameTextures[1], control->frameFence,
		                      control->consumedFence}) {
			if (value != 0) CloseHandle(handle(value));
		}
		if (FAILED(result)) {
			Log(LogCategory::Initialization, "server: opening its shared %s failed (HRESULT 0x%08lX)", step,
			    static_cast<unsigned long>(result));
			Stop("the server's frames cannot be opened");
			return false;
		}
		Log(LogCategory::Initialization, "server: frames at %ux%u (generation %u)", control->frameWidth,
		    control->frameHeight, generation);
		return true;
	}

	bool RemoteRenderer::CopyFrame(ID3D11Device *device, ID3D11DeviceContext *context, ID3D11Texture2D *target,
	                               uint64_t frame) {
		if (!active) return false;
		if (!ServerAlive()) {
			Stop("the server exited");
			return false;
		}
		ULONGLONG const deadline = GetTickCount64() + kFrameTimeoutMs;
		// The server publishes its frames once it has seen the video mode, shortly after the first frames.
		while (!OpenGeneration(device)) {
			if (!active || GetTickCount64() > deadline) return false;
			Sleep(1);
		}
		while (frameFence->GetCompletedValue() < frame + 1) {
			WaitForSingleObject(ring.FrameEvent(), 50);
			if (!ServerAlive()) {
				Stop("the server exited");
				return false;
			}
			if (GetTickCount64() > deadline) {
				Stop("the server fell more than 3 seconds behind");
				return false;
			}
			// A resize brings new textures and fences.
			if (ring.Control()->frameGeneration.load(std::memory_order_acquire) != generation) return false;
		}
		D3D11_TEXTURE2D_DESC source{}, destination{};
		textures[frame % 2]->GetDesc(&source);
		target->GetDesc(&destination);
		if (source.Width != destination.Width || source.Height != destination.Height) return false;
		context->CopyResource(target, textures[frame % 2].Get());
		return true;
	}

	bool RemoteRenderer::Composite(HWND window) {
		nSCDWire::RingControl *const control = ring.Control();
		if (!active || control == nullptr || control->presentMode != nSCDWire::kPresentComposition) return false;
		uint32_t const current = control->compositionGeneration.load(std::memory_order_acquire);
		if (current == compositionGeneration) return compositionVisual != nullptr;
		compositionGeneration = current;
		HANDLE const surface = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(control->compositionSurface));
		HRESULT result = S_OK;
		if (!composition) {
			result = DCompositionCreateDevice2(nullptr, IID_PPV_ARGS(&composition));
			// Topmost, so the visual covers this side's own swap chain, which stops presenting.
			if (SUCCEEDED(result)) result = composition->CreateTargetForHwnd(window, TRUE, &compositionTarget);
			if (SUCCEEDED(result)) result = composition->CreateVisual(&compositionVisual);
			if (SUCCEEDED(result)) result = compositionTarget->SetRoot(compositionVisual.Get());
		}
		compositionSurface.Reset();
		if (SUCCEEDED(result)) result = composition->CreateSurfaceFromHandle(surface, &compositionSurface);
		if (SUCCEEDED(result)) result = compositionVisual->SetContent(compositionSurface.Get());
		if (SUCCEEDED(result)) result = composition->Commit();
		CloseHandle(surface);
		if (FAILED(result)) {
			Log(LogCategory::Initialization, "server: DirectComposition failed (HRESULT 0x%08lX), presenting here",
			    static_cast<unsigned long>(result));
			compositionVisual.Reset();
			return false;
		}
		Log(LogCategory::Initialization, "server: presenting through DirectComposition");
		return true;
	}

	void RemoteRenderer::EndFrame(ID3D11Device *device, ID3D11DeviceContext *context, uint64_t frame) {
		if (!active || !OpenGeneration(device)) return;
		Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context4;
		if (SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&context4)))) context4->Signal(consumedFence.Get(), frame + 1);
	}
}
