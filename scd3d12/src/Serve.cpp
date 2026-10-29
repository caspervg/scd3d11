/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

// SCD3D12Server serve: draws SimCity 4 live. SCD3D11 starts it, streams the game's SimGL calls through a
// SharedRing, and shows the frames this side copies into two shared textures (see RingControl).

#include "Serve.h"

#include "AssetStore.h"
#include "cGDriver12.h"
#include "OwnedScene.h"
#include "SceneLane.h"
#include "wire/SharedRing.h"
#include "wire/WireReplay.h"

#include <dcomp.h>
#include <dxgi1_4.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace nSCD3D12 {
	namespace {
		class ServeHooks : public nSCDWire::ReplayHooks {
		public:
			ServeHooks(cGDriver12 &driver, nSCDWire::RingControl &control, HANDLE parent, HANDLE frameEvent,
			           bool stats, SceneMode sceneMode)
				: driver(driver), control(control), parent(parent), frameEvent(frameEvent), stats(stats),
				  sceneMode(sceneMode) {}

			~ServeHooks() {
				if (indexer.joinable()) indexer.join();
				if (copyEvent != nullptr) CloseHandle(copyEvent);
			}

			// The scene lane needs the game's data, which is indexed on a thread of its own once the game names
			// its folders. The game's own draws stand until then.
			void GameFolders(std::wstring const (&folders)[static_cast<size_t>(nSCDWire::GameFolder::Count)]) override {
				if (sceneMode == SceneMode::Off || indexer.joinable()) return;
				std::wstring copy[std::size(folders)];
				for (size_t i = 0; i < std::size(folders); ++i) copy[i] = folders[i];
				indexer = std::thread([this, copy] {
					auto const start = std::chrono::steady_clock::now();
					store.AddFolders(copy[0], copy[1], copy[2], copy[3], copy[4]);
					std::printf("scene: indexed %zu packages, %zu entries in %.2f s\n", store.Files().size(),
					            store.Index().size(),
					            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
					std::fflush(stdout);
					storeReady.store(true, std::memory_order_release);
				});
			}
			void ModelInstance(nSCDWire::ModelInstance const &model) override {
				if (scene) scene->ModelInstance(model);
			}
			void NetworkPiece(nSCDWire::NetworkPiece const &piece, nSCDWire::NetworkPart const *parts) override {
				if (scene) scene->NetworkPiece(piece, parts);
			}
			void CellTexture(nSCDWire::CellTexture const &cell) override {
				if (scene) scene->CellTexture(cell);
			}

			void VideoMode(bool show, uint32_t width, uint32_t height, bool fullscreen) override {
				driver.VideoMode(show, width, height, fullscreen);
				if (owned) owned->VideoMode();
				Share(width, height);
				if (control.presentMode == nSCDWire::kPresentComposition) Compose(width, height);
			}

			void Resize(uint32_t width, uint32_t height) override {
				driver.Resize(width, height);
				Share(width, height);
				if (control.presentMode == nSCDWire::kPresentComposition) Compose(width, height);
			}

			void Vertices(uint32_t name, void const *data, uint32_t bytes) override {
				driver.Vertices(name, data, bytes);
			}

			void OwnerBegin(nSCDWire::DrawOwner const &owner) override {
				if (owned) owned->OwnerBegin(owner);
				driver.OwnerBegin(owner);
				if (scene) scene->OwnerBegin(owner);
			}
			void OwnerEnd() override {
				driver.OwnerEnd();
				if (scene) scene->OwnerEnd();
				if (owned) owned->OwnerEnd();
			}
			void Occupant(nSCDWire::OccupantEvent const &event) override {
				if (owned) owned->Occupant(event);
			}
			void DrawBlobs(nSCDWire::BlobKey const &vertices, nSCDWire::BlobKey const &indices) override {
				driver.DrawBlobs(vertices, indices);
			}

			bool Frame() override {
				driver.Frame();
				if (scene) scene->EndFrame(frame, nullptr);
				if (owned) owned->EndFrame();
				// Between frames no bracket is open, so the lane can start here.
				if (!scene && storeReady.load(std::memory_order_acquire)) {
					scene = std::make_unique<SceneLane>(driver, store, sceneMode == SceneMode::Check);
					if (sceneMode == SceneMode::Owned) owned = std::make_unique<OwnedScene>(driver, *scene);
					std::printf("scene: %s from frame %llu\n", owned ? "owning the static view" : "substituting",
					            static_cast<unsigned long long>(frame));
					std::fflush(stdout);
				}
				uint64_t const index = frame++;
				if (frameFence) Publish(index);
				if (stats) Report();
				return true;
			}

		private:
			// Once a second: frames, this thread's CPU time and the GPU time per frame.
			void Report() {
				++reportFrames;
				ULONGLONG const now = GetTickCount64();
				if (reportStart == 0) reportStart = now;
				if (now - reportStart < 1000) return;
				FILETIME created, exited, kernel, user;
				GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user);
				auto const ticks = [](FILETIME const &time) {
					return static_cast<uint64_t>(time.dwHighDateTime) << 32 | time.dwLowDateTime;
				};
				uint64_t const cpu = ticks(kernel) + ticks(user);
				double gpu = 0.0;
				uint32_t gpuFrames = 0;
				driver.TakeGpuTime(gpu, gpuFrames);
				std::printf("%u frames in %.2f s, CPU %.2f ms and GPU %.2f ms per frame\n", reportFrames,
				            static_cast<double>(now - reportStart) / 1000.0,
				            static_cast<double>(cpu - reportCpu) / 1.0e4 / reportFrames,
				            gpuFrames != 0 ? gpu / gpuFrames : 0.0);
				if (scene && ++reports % 10 == 0) {
					scene->PrintTotals();
					if (owned) owned->PrintTotals();
				}
				std::fflush(stdout);
				reportStart = now;
				reportCpu = cpu;
				reportFrames = 0;
			}

			// New shared textures and fences for frames of this size, handed to the shim as handles it owns.
			void Share(uint32_t width, uint32_t height) {
				ID3D12Device *const device = driver.Device();
				if (device == nullptr) return;
				if (!list) {
					device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
					device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
					                          IID_PPV_ARGS(&list));
					list->Close();
					device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&copyFence));
					copyEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
				}
				WaitForCopies();

				D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
				D3D12_RESOURCE_DESC description{};
				description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
				description.Width = width;
				description.Height = height;
				description.DepthOrArraySize = 1;
				description.MipLevels = 1;
				description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
				description.SampleDesc.Count = 1;
				// Direct3D 11 only opens shared textures that could be render targets.
				description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
				HANDLE handles[4]{};
				HRESULT result = S_OK;
				for (int i = 0; i < 2 && SUCCEEDED(result); ++i) {
					textures[i].Reset();
					result = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &description,
					                                         D3D12_RESOURCE_STATE_COMMON, nullptr,
					                                         IID_PPV_ARGS(&textures[i]));
					if (SUCCEEDED(result)) {
						result = device->CreateSharedHandle(textures[i].Get(), nullptr, GENERIC_ALL, nullptr, &handles[i]);
					}
				}
				frameFence.Reset();
				consumedFence.Reset();
				if (SUCCEEDED(result)) result = device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&frameFence));
				if (SUCCEEDED(result)) result = device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&consumedFence));
				if (SUCCEEDED(result)) result = device->CreateSharedHandle(frameFence.Get(), nullptr, GENERIC_ALL, nullptr, &handles[2]);
				if (SUCCEEDED(result)) result = device->CreateSharedHandle(consumedFence.Get(), nullptr, GENERIC_ALL, nullptr, &handles[3]);

				uint64_t owned[4]{};
				for (int i = 0; i < 4 && SUCCEEDED(result); ++i) {
					HANDLE theirs = nullptr;
					if (!DuplicateHandle(GetCurrentProcess(), handles[i], parent, &theirs, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
						result = HRESULT_FROM_WIN32(GetLastError());
					}
					owned[i] = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(theirs));
				}
				for (HANDLE handle: handles) {
					if (handle != nullptr) CloseHandle(handle);
				}
				if (FAILED(result)) {
					std::printf("sharing frames failed: 0x%08lX\n", static_cast<unsigned long>(result));
					frameFence.Reset();
					return;
				}
				control.frameTextures[0] = owned[0];
				control.frameTextures[1] = owned[1];
				control.frameFence = owned[2];
				control.consumedFence = owned[3];
				control.frameWidth = width;
				control.frameHeight = height;
				control.frameGeneration.fetch_add(1, std::memory_order_release);
				// Fence values are frame numbers, so the new fences start at the current frame.
				generationStart = frame;
				std::printf("sharing %ux%u frames\n", width, height);
				std::fflush(stdout);
			}

			// ServerPresent=composition: a swap chain of our own, shown on SC4's window through a
			// DirectComposition surface the shim attaches.
			void Compose(uint32_t width, uint32_t height) {
				WaitForCopies();
				for (auto &buffer: swapBuffers) buffer.Reset();
				if (swapChain) {
					if (FAILED(swapChain->ResizeBuffers(2, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, 0))) swapChain.Reset();
				} else {
					HANDLE surface = nullptr;
					ComPtr<IDXGIFactoryMedia> media;
					DXGI_SWAP_CHAIN_DESC1 description{};
					description.Width = width;
					description.Height = height;
					description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
					description.SampleDesc.Count = 1;
					description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
					description.BufferCount = 2;
					description.Scaling = DXGI_SCALING_STRETCH;
					description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
					description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
					ComPtr<IDXGISwapChain1> created;
					HRESULT result = DCompositionCreateSurfaceHandle(COMPOSITIONOBJECT_ALL_ACCESS, nullptr, &surface);
					if (SUCCEEDED(result)) result = CreateDXGIFactory2(0, IID_PPV_ARGS(&media));
					if (SUCCEEDED(result)) {
						result = media->CreateSwapChainForCompositionSurfaceHandle(driver.Queue(), surface, &description,
						                                                           nullptr, &created);
					}
					if (SUCCEEDED(result)) result = created.As(&swapChain);
					HANDLE theirs = nullptr;
					if (SUCCEEDED(result) &&
					    !DuplicateHandle(GetCurrentProcess(), surface, parent, &theirs, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
						result = HRESULT_FROM_WIN32(GetLastError());
					}
					if (surface != nullptr) CloseHandle(surface);
					if (FAILED(result)) {
						std::printf("composition swap chain failed: 0x%08lX\n", static_cast<unsigned long>(result));
						swapChain.Reset();
						return;
					}
					control.compositionSurface = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(theirs));
					control.compositionGeneration.fetch_add(1, std::memory_order_release);
					std::puts("presenting through DirectComposition");
				}
				for (UINT i = 0; swapChain && i < 2; ++i) swapChain->GetBuffer(i, IID_PPV_ARGS(&swapBuffers[i]));
				std::fflush(stdout);
			}

			void WaitForCopies() {
				if (copyFence && copyFence->GetCompletedValue() < copyValue) {
					copyFence->SetEventOnCompletion(copyValue, copyEvent);
					WaitForSingleObject(copyEvent, INFINITE);
				}
			}

			// Copies frame `index` into its shared texture once the shim is done with the frame that used it
			// before, then tells the shim it is there.
			void Publish(uint64_t index) {
				ID3D12Resource *const output = driver.Output();
				if (output == nullptr) return;
				WaitForCopies();
				allocator->Reset();
				list->Reset(allocator.Get(), nullptr);
				ID3D12Resource *const target = textures[index % 2].Get();
				D3D12_RESOURCE_BARRIER barriers[2]{};
				barriers[0].Type = barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
				barriers[0].Transition = {output, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
				                          D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE};
				barriers[1].Transition = {target, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON,
				                          D3D12_RESOURCE_STATE_COPY_DEST};
				list->ResourceBarrier(2, barriers);
				list->CopyResource(target, output);
				ID3D12Resource *const presented = swapChain ? swapBuffers[swapChain->GetCurrentBackBufferIndex()].Get() : nullptr;
				D3D12_RESOURCE_BARRIER present{};
				if (presented != nullptr) {
					present.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
					present.Transition = {presented, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PRESENT,
					                      D3D12_RESOURCE_STATE_COPY_DEST};
					list->ResourceBarrier(1, &present);
					list->CopyResource(presented, output);
					std::swap(present.Transition.StateBefore, present.Transition.StateAfter);
					list->ResourceBarrier(1, &present);
				}
				for (auto &barrier: barriers) std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
				list->ResourceBarrier(2, barriers);
				list->Close();

				ID3D12CommandQueue *const queue = driver.Queue();
				// The texture was last used two frames ago. Wait on the GPU until the shim has copied it out.
				if (index >= generationStart + 2) queue->Wait(consumedFence.Get(), index - 1);
				ID3D12CommandList *const lists[]{list.Get()};
				queue->ExecuteCommandLists(1, lists);
				if (presented != nullptr) swapChain->Present(0, 0);
				queue->Signal(frameFence.Get(), index + 1);
				frameFence->SetEventOnCompletion(index + 1, frameEvent);
				queue->Signal(copyFence.Get(), ++copyValue);
			}

			cGDriver12 &driver;
			nSCDWire::RingControl &control;
			HANDLE parent;
			HANDLE frameEvent;
			uint64_t frame = 0;
			uint64_t generationStart = 0;
			ComPtr<ID3D12Resource> textures[2];
			ComPtr<ID3D12Fence> frameFence;
			ComPtr<ID3D12Fence> consumedFence;
			ComPtr<ID3D12CommandAllocator> allocator;
			ComPtr<ID3D12GraphicsCommandList> list;
			ComPtr<ID3D12Fence> copyFence;
			ComPtr<IDXGISwapChain3> swapChain;
			ComPtr<ID3D12Resource> swapBuffers[2];
			HANDLE copyEvent = nullptr;
			uint64_t copyValue = 0;
			bool stats;
			SceneMode sceneMode;
			nSCAssets::AssetStore store;
			std::atomic<bool> storeReady{false};
			std::thread indexer;
			// Declared after the store it reads, so it goes first.
			std::unique_ptr<SceneLane> scene;
			// Declared after the lane it takes meshes from.
			std::unique_ptr<OwnedScene> owned;
			uint32_t reports = 0;
			ULONGLONG reportStart = 0;
			uint64_t reportCpu = 0;
			uint32_t reportFrames = 0;
		};
	}

	int RunServe(std::wstring const &ringName, DWORD parentId, cGDriver12::Sun const *relight, bool logStats,
	             SceneMode scene) {
		HANDLE const parent = OpenProcess(PROCESS_DUP_HANDLE | SYNCHRONIZE, FALSE, parentId);
		if (parent == nullptr) {
			std::puts("cannot open the game's process");
			return 1;
		}
		auto const alive = [parent] { return WaitForSingleObject(parent, 0) == WAIT_TIMEOUT; };
		// Leave with the game, however this process is blocked (a GPU wait on a fence the game will never
		// signal, a full or empty ring).
		CreateThread(nullptr, 0, [](void *process) -> DWORD {
			WaitForSingleObject(static_cast<HANDLE>(process), INFINITE);
			std::puts("the game exited");
			std::fflush(stdout);
			TerminateProcess(GetCurrentProcess(), 0);
			return 0;
		}, parent, 0, nullptr);
		nSCDWire::SharedRing ring;
		if (!ring.Open(ringName)) {
			std::puts("cannot open the shared ring");
			return 1;
		}
		nSCDWire::RingControl &control = *ring.Control();

		// Shared textures need both devices on one adapter, which the shim reports once its device exists.
		while (control.adapterSet.load(std::memory_order_acquire) == 0) {
			if (!alive()) return 0;
			Sleep(5);
		}
		LUID const adapter{control.adapterLow, control.adapterHigh};
		cGDriver12 driver;
		if (relight != nullptr) driver.Relight(*relight);
		if (!driver.Create(false, &adapter)) {
			std::puts("cannot create a Direct3D 12 device on the game's adapter");
			return 1;
		}
		control.serverReady.store(1, std::memory_order_release);
		std::puts("serving");
		std::fflush(stdout);

		ServeHooks hooks(driver, control, parent, ring.FrameEvent(), logStats, scene);
		nSCDWire::Reader reader([&](void *data, size_t size) { return ring.Read(data, size, alive); });
		nSCDWire::ReplayStats stats;
		char const *error = nullptr;
		bool const ok = nSCDWire::Replay(reader, driver, hooks, &stats, &error);
		std::printf("%s after %llu frames, %llu records\n", ok ? "stream ended" : error,
		            static_cast<unsigned long long>(stats.frames), static_cast<unsigned long long>(stats.records));
		CloseHandle(parent);
		return ok ? 0 : 1;
	}
}
