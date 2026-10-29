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
#include "wire/ImageFile.h"
#include "wire/SharedRing.h"
#include "wire/WireReplay.h"

#include <dcomp.h>
#include <dxgi1_4.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace nSCD3D12 {
	namespace {
		class ServeHooks : public nSCDWire::ReplayHooks {
		public:
			ServeHooks(cGDriver12 &driver, nSCDWire::RingControl &control, HANDLE parent, HANDLE frameEvent,
			           bool stats, SceneMode sceneMode, WorldLight::Options const *light)
				: driver(driver), control(control), parent(parent), frameEvent(frameEvent), stats(stats),
				  sceneMode(sceneMode) {
				if (light != nullptr) world = std::make_unique<WorldLight>(driver, *light);
			}

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
			void OccupantExemplar(nSCDWire::OccupantExemplar const &occupant, uint32_t const *values) override {
				if (owned) owned->OccupantExemplar(occupant, values);
			}
			void FreeCamera(nSCDWire::FreeCamera const &camera) override {
				freeCamera = camera;
				if (owned) owned->SetFreeCamera(camera);
			}

			void VideoMode(bool show, uint32_t width, uint32_t height, bool fullscreen) override {
				driver.VideoMode(show, width, height, fullscreen);
				if (owned) owned->VideoMode();
				if (world) world->VideoMode();
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
				if (world) world->OwnerBegin(owner);
				driver.OwnerBegin(owner);
				if (scene) scene->OwnerBegin(owner);
			}
			void OwnerEnd() override {
				driver.OwnerEnd();
				if (scene) scene->OwnerEnd();
				if (owned) owned->OwnerEnd();
				if (world) world->OwnerEnd();
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
				control.serverOwnsView.store(owned && owned->Owning() ? 1u : 0u, std::memory_order_release);
				// Between frames no bracket is open, so the lane can start here.
				if (!scene && storeReady.load(std::memory_order_acquire)) {
					scene = std::make_unique<SceneLane>(driver, store, sceneMode == SceneMode::Check);
					if (sceneMode == SceneMode::Owned) {
						owned = std::make_unique<OwnedScene>(driver, *scene);
						owned->SetFreeCamera(freeCamera);
					}
					if (world) world->SetScene(owned.get(), scene.get());
					std::printf("scene: %s from frame %llu\n", owned ? "owning the static view" : "substituting",
					            static_cast<unsigned long long>(frame));
					std::fflush(stdout);
				}
				uint64_t const index = frame++;
				if (frameFence) Publish(index);
				if (stats) Report();
				return true;
			}

			void PrintTotals() const {
				if (scene) scene->PrintTotals();
				if (owned) owned->PrintTotals();
				if (world) world->PrintTotals();
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
				if (++reports % 10 == 0) PrintTotals();
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
			nSCDWire::FreeCamera freeCamera{};
			// Declared after the scenes it reads.
			std::unique_ptr<WorldLight> world;
			uint32_t reports = 0;
			ULONGLONG reportStart = 0;
			uint64_t reportCpu = 0;
			uint32_t reportFrames = 0;
		};
	}

	int RunServe(std::wstring const &ringName, DWORD parentId, cGDriver12::Sun const *relight, bool logStats,
	             SceneMode scene, WorldLight::Options const *light) {
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

		ServeHooks hooks(driver, control, parent, ring.FrameEvent(), logStats, scene, relight != nullptr ? light : nullptr);
		nSCDWire::Reader reader([&](void *data, size_t size) { return ring.Read(data, size, alive); });
		nSCDWire::ReplayStats stats;
		char const *error = nullptr;
		bool const ok = nSCDWire::Replay(reader, driver, hooks, &stats, &error);
		std::printf("%s after %llu frames, %llu records\n", ok ? "stream ended" : error,
		            static_cast<unsigned long long>(stats.frames), static_cast<unsigned long long>(stats.records));
		hooks.PrintTotals();
		std::fflush(stdout);
		CloseHandle(parent);
		return ok ? 0 : 1;
	}

	namespace {
		// The shim's side of the frames in RunFeed: signals each frame consumed as soon as it is there, and copies
		// one of them out first when asked.
		class FeedFrames {
		public:
			FeedFrames(nSCDWire::RingControl &control, ID3D12Device *device, uint64_t saveFrame, std::wstring savePath)
				: control(control), device(device), saveFrame(saveFrame), savePath(std::move(savePath)),
				  event(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {}
			~FeedFrames() { CloseHandle(event); }

			void Run(std::atomic<bool> const &done) {
				while (!done.load(std::memory_order_acquire)) {
					uint32_t const now = control.frameGeneration.load(std::memory_order_acquire);
					if (now != generation) Open(now);
					uint64_t const shown = frames ? frames->GetCompletedValue() : 0;
					if (shown <= consumed) {
						// Woken by the next frame, or now and then to look for new fences and the end.
						if (frames) frames->SetEventOnCompletion(consumed + 1, event);
						WaitForSingleObject(event, 5);
						continue;
					}
					// Frame n is there once the fence reaches n + 1, and stays until it is signalled consumed.
					if (!savePath.empty() && !saved && shown > saveFrame && consumed <= saveFrame) {
						Save(textures[saveFrame % 2].Get());
						saved = true;
					}
					uint64_t const upTo = savePath.empty() || saved ? shown : (std::min)(shown, saveFrame);
					if (upTo > consumed) {
						consumed = upTo;
						consumedFence->Signal(consumed);
					}
				}
			}

			bool saved = false;

		private:
			void Open(uint32_t now) {
				generation = now;
				auto const handle = [](uint64_t value) { return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(value)); };
				frames.Reset();
				consumedFence.Reset();
				for (int i = 0; i < 2; ++i) {
					textures[i].Reset();
					device->OpenSharedHandle(handle(control.frameTextures[i]), IID_PPV_ARGS(&textures[i]));
				}
				device->OpenSharedHandle(handle(control.consumedFence), IID_PPV_ARGS(&consumedFence));
				device->OpenSharedHandle(handle(control.frameFence), IID_PPV_ARGS(&frames));
				// New fences start at 0, so the frames already there get signalled consumed again.
				consumed = 0;
			}

			void Save(ID3D12Resource *texture) {
				if (texture == nullptr) return;
				ComPtr<ID3D12CommandQueue> queue;
				ComPtr<ID3D12CommandAllocator> allocator;
				ComPtr<ID3D12GraphicsCommandList> list;
				ComPtr<ID3D12Fence> fence;
				ComPtr<ID3D12Resource> readback;
				D3D12_COMMAND_QUEUE_DESC const queueDescription{D3D12_COMMAND_LIST_TYPE_DIRECT};
				D3D12_RESOURCE_DESC const description = texture->GetDesc();
				D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
				uint64_t bytes = 0;
				device->GetCopyableFootprints(&description, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
				D3D12_HEAP_PROPERTIES const heap{D3D12_HEAP_TYPE_READBACK};
				D3D12_RESOURCE_DESC buffer{};
				buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
				buffer.Width = bytes;
				buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
				buffer.SampleDesc.Count = 1;
				buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
				if (FAILED(device->CreateCommandQueue(&queueDescription, IID_PPV_ARGS(&queue))) ||
				    FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
				    FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
				                                     IID_PPV_ARGS(&list))) ||
				    FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) ||
				    FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
				                                           D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)))) {
					return;
				}
				D3D12_TEXTURE_COPY_LOCATION const to{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {footprint}};
				D3D12_TEXTURE_COPY_LOCATION from{texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
				from.SubresourceIndex = 0;
				list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
				list->Close();
				ID3D12CommandList *const lists[]{list.Get()};
				queue->ExecuteCommandLists(1, lists);
				queue->Signal(fence.Get(), 1);
				HANDLE const event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
				fence->SetEventOnCompletion(1, event);
				WaitForSingleObject(event, INFINITE);
				CloseHandle(event);
				uint8_t *mapped = nullptr;
				if (FAILED(readback->Map(0, nullptr, reinterpret_cast<void **>(&mapped)))) return;
				uint32_t const width = static_cast<uint32_t>(description.Width), height = description.Height;
				std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4);
				for (uint32_t y = 0; y < height; ++y) {
					std::memcpy(rgba.data() + static_cast<size_t>(y) * width * 4,
					            mapped + footprint.Offset + static_cast<size_t>(y) * footprint.Footprint.RowPitch, width * 4);
				}
				readback->Unmap(0, nullptr);
				bool const ok = nSCDWire::SavePng(savePath.c_str(), width, height, rgba.data());
				std::printf("feed: frame %llu %s\n", static_cast<unsigned long long>(saveFrame), ok ? "saved" : "not saved");
				std::fflush(stdout);
			}

			nSCDWire::RingControl &control;
			ID3D12Device *device;
			uint64_t saveFrame;
			std::wstring savePath;
			uint32_t generation = 0;
			uint64_t consumed = 0;
			ComPtr<ID3D12Resource> textures[2];
			ComPtr<ID3D12Fence> frames, consumedFence;
			HANDLE event;
		};
	}

	int RunFeed(std::wstring const &capture, std::wstring const &serveArguments, uint64_t saveFrame,
	            std::wstring const &savePath) {
		std::FILE *file = nullptr;
		if (_wfopen_s(&file, capture.c_str(), L"rb") != 0 || file == nullptr) {
			std::puts("feed: cannot open the capture");
			return 1;
		}
		ComPtr<IDXGIFactory1> factory;
		ComPtr<IDXGIAdapter1> adapter;
		ComPtr<ID3D12Device> device;
		DXGI_ADAPTER_DESC1 adapterDescription{};
		if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || FAILED(factory->EnumAdapters1(0, &adapter)) ||
		    FAILED(adapter->GetDesc1(&adapterDescription)) ||
		    FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
			std::puts("feed: no Direct3D 12 device");
			std::fclose(file);
			return 1;
		}
		DWORD const self = GetCurrentProcessId();
		std::wstring const name = L"Local\\SCD3D12-feed-" + std::to_wstring(self);
		nSCDWire::SharedRing ring;
		if (!ring.Create(name, 64ull << 20)) {
			std::puts("feed: cannot create the shared ring");
			std::fclose(file);
			return 1;
		}
		nSCDWire::RingControl &control = *ring.Control();
		control.presentMode = nSCDWire::kPresentCopy;
		control.adapterLow = adapterDescription.AdapterLuid.LowPart;
		control.adapterHigh = adapterDescription.AdapterLuid.HighPart;
		control.adapterSet.store(1, std::memory_order_release);

		wchar_t program[MAX_PATH]{};
		GetModuleFileNameW(nullptr, program, MAX_PATH);
		std::wstring command = L"\"" + std::wstring(program) + L"\" serve --ring " + name + L" --parent " +
		                       std::to_wstring(self) + L" " + serveArguments;
		STARTUPINFOW startup{sizeof(startup)};
		PROCESS_INFORMATION info{};
		if (!CreateProcessW(program, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &info)) {
			std::puts("feed: cannot start serve");
			std::fclose(file);
			return 1;
		}
		CloseHandle(info.hThread);
		auto const alive = [&] { return WaitForSingleObject(info.hProcess, 0) == WAIT_TIMEOUT; };

		std::atomic<bool> done{false};
		FeedFrames frames(control, device.Get(), saveFrame, savePath);
		std::thread consumer([&] { frames.Run(done); });
		auto const start = std::chrono::steady_clock::now();
		std::vector<uint8_t> chunk(1 << 20);
		uint64_t total = 0;
		bool written = true;
		for (size_t size; written && (size = std::fread(chunk.data(), 1, chunk.size(), file)) != 0;) {
			written = ring.Write(chunk.data(), size, alive);
			total += size;
		}
		std::fclose(file);
		ring.Close();
		WaitForSingleObject(info.hProcess, INFINITE);
		DWORD code = 1;
		GetExitCodeProcess(info.hProcess, &code);
		CloseHandle(info.hProcess);
		done.store(true, std::memory_order_release);
		consumer.join();
		std::printf("feed: %.1f MB in %.1f s, serve %s with %lu\n", static_cast<double>(total) / (1 << 20),
		            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(),
		            written ? "exited" : "stopped reading and exited", code);
		return written && code == 0 && (savePath.empty() || frames.saved) ? 0 : 1;
	}
}
