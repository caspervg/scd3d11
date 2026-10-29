/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

#include "Dlss.h"

#include <windows.h>
#include <DirectXMath.h>
#include <wrl/client.h>

#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss.h>
#include <sl_dlss_g.h>
#include <sl_helpers.h>
#include <sl_pcl.h>
#include <sl_reflex.h>
#include <sl_security.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {
	// Streamline's entry points, from sl.interposer.dll. The server links no Streamline library, so it starts and
	// draws without the DLLs.
	struct Api {
		PFun_slInit *init = nullptr;
		PFun_slShutdown *shutdown = nullptr;
		PFun_slIsFeatureSupported *isFeatureSupported = nullptr;
		PFun_slSetD3DDevice *setD3DDevice = nullptr;
		PFun_slGetFeatureVersion *getFeatureVersion = nullptr;
		PFun_slGetNewFrameToken *getNewFrameToken = nullptr;
		PFun_slSetTagForFrame *setTagForFrame = nullptr;
		PFun_slSetConstants *setConstants = nullptr;
		PFun_slEvaluateFeature *evaluateFeature = nullptr;
		PFun_slUpgradeInterface *upgradeInterface = nullptr;
		PFun_slGetNativeInterface *getNativeInterface = nullptr;
		PFun_slGetFeatureFunction *getFeatureFunction = nullptr;
	} api;
}

// The feature helpers in sl_dlss.h and its kind look their functions up through this.
extern "C" sl::Result slGetFeatureFunction(sl::Feature feature, char const *functionName, void *&function) {
	return api.getFeatureFunction != nullptr ? api.getFeatureFunction(feature, functionName, function)
	                                         : sl::Result::eErrorNotInitialized;
}

namespace nSCD3D12 {
	namespace {
		constexpr uint32_t kSlots = 3;
		// Our project's identity for NGX, which needs one when there is no NVIDIA application id.
		constexpr char kProjectId[] = "5c3d3d12-7a1e-4f0b-9d2c-5c4d1a3e0b12";

		void Log(sl::LogType type, char const *message) {
			if (type == sl::LogType::eInfo) return;
			std::printf("streamline %s: %s", type == sl::LogType::eError ? "error" : "warning", message);
			size_t const length = std::strlen(message);
			if (length == 0 || message[length - 1] != '\n') std::putchar('\n');
			std::fflush(stdout);
		}

		bool Check(sl::Result result, char const *what) {
			if (result == sl::Result::eOk) return true;
			std::printf("dlss: %s failed: %s\n", what, sl::getResultAsStr(result));
			std::fflush(stdout);
			return false;
		}

		// Swaps object for Streamline's wrapper. The wrapper holds its own reference to what it wraps, so ours goes.
		template<typename T>
		bool UpgradeInterface(T *&object, char const *what) {
			T *const original = object;
			if (!Check(api.upgradeInterface(reinterpret_cast<void **>(&object)), what)) return false;
			if (object != original) original->Release();
			return true;
		}

		sl::DLSSMode ToSl(DlssMode mode) {
			switch (mode) {
				case DlssMode::Dlaa: return sl::DLSSMode::eDLAA;
				case DlssMode::Quality: return sl::DLSSMode::eMaxQuality;
				case DlssMode::Balanced: return sl::DLSSMode::eBalanced;
				case DlssMode::Performance: return sl::DLSSMode::eMaxPerformance;
				case DlssMode::UltraPerformance: return sl::DLSSMode::eUltraPerformance;
				default: return sl::DLSSMode::eOff;
			}
		}

		// An OpenGL matrix (column major, column vectors) is the row major, row vector matrix Streamline and
		// DirectXMath use, read as it is.
		DirectX::XMMATRIX LoadMatrix(float const *m) {
			return DirectX::XMLoadFloat4x4(reinterpret_cast<DirectX::XMFLOAT4X4 const *>(m));
		}

		void Store(sl::float4x4 &out, DirectX::XMMATRIX const &m) {
			DirectX::XMStoreFloat4x4(reinterpret_cast<DirectX::XMFLOAT4X4 *>(&out), m);
		}

		// OpenGL's projection to Direct3D's: depth from 0 to 1 instead of -1 to 1, as the driver draws it.
		DirectX::XMMATRIX ToDirect3D(float const *projection) {
			DirectX::XMMATRIX const depth(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0.5f, 0, 0, 0, 0.5f, 1);
			return DirectX::XMMatrixMultiply(LoadMatrix(projection), depth);
		}

		float Halton(uint32_t index, uint32_t base) {
			float result = 0.0f, fraction = 1.0f;
			for (; index > 0; index /= base) {
				fraction /= static_cast<float>(base);
				result += fraction * static_cast<float>(index % base);
			}
			return result;
		}
	}

	bool ParseDlssMode(std::wstring const &text, DlssMode &mode) {
		if (text == L"off") mode = DlssMode::Off;
		else if (text == L"dlaa") mode = DlssMode::Dlaa;
		else if (text == L"quality") mode = DlssMode::Quality;
		else if (text == L"balanced") mode = DlssMode::Balanced;
		else if (text == L"performance") mode = DlssMode::Performance;
		else if (text == L"ultra") mode = DlssMode::UltraPerformance;
		else return false;
		return true;
	}

	struct Dlss::Impl {
		HMODULE module = nullptr;
		bool initialised = false;
		DlssMode mode = DlssMode::Off;
		bool frameGeneration = false;
		ID3D12Device *device = nullptr;
		ID3D12CommandQueue *queue = nullptr;
		sl::ViewportHandle viewport{0};
		sl::FrameToken *token = nullptr;
		uint64_t frame = 0;
		uint32_t jitterIndex = 0;
		uint32_t jitterPhases = 8;
		sl::DLSSOptions options{};
		bool optionsSet = false;
		uint32_t frameLimit = 0;

		// The upscaled frame.
		ComPtr<ID3D12Resource> output;
		uint32_t outputWidth = 0, outputHeight = 0;

		// Command lists for the tags and the DLSS pass, one per frame in flight.
		ComPtr<ID3D12CommandAllocator> allocators[kSlots];
		ComPtr<ID3D12GraphicsCommandList> list;
		ComPtr<ID3D12Fence> fence;
		HANDLE fenceEvent = nullptr;
		uint64_t fenceValue = 0;
		uint64_t slotFence[kSlots]{};
		uint32_t slot = 0;
		ComPtr<ID3D12QueryHeap> queries;
		ComPtr<ID3D12Resource> queryReadback;
		uint64_t *queryValues = nullptr;
		bool slotTimed[kSlots]{};
		double tickMilliseconds = 0.0;
		double gpuMilliseconds = 0.0;
		uint32_t gpuFrames = 0;

		// Presenting.
		ID3D12Device *proxyDevice = nullptr;
		ComPtr<ID3D12CommandQueue> presentQueue, nativePresentQueue;

		~Impl() {
			if (fence && fence->GetCompletedValue() < fenceValue) {
				fence->SetEventOnCompletion(fenceValue, fenceEvent);
				WaitForSingleObject(fenceEvent, INFINITE);
			}
			if (fenceEvent != nullptr) CloseHandle(fenceEvent);
			nativePresentQueue.Reset();
			presentQueue.Reset();
			if (proxyDevice != nullptr) proxyDevice->Release();
			if (initialised) api.shutdown();
			if (module != nullptr) FreeLibrary(module);
		}

		bool Load() {
			wchar_t path[MAX_PATH];
			DWORD const length = GetModuleFileNameW(nullptr, path, MAX_PATH);
			if (length == 0 || length == MAX_PATH) return false;
			std::wstring folder(path, length);
			folder.resize(folder.find_last_of(L'\\'));
			std::wstring const interposer = folder + L"\\sl.interposer.dll";
			if (GetFileAttributesW(interposer.c_str()) == INVALID_FILE_ATTRIBUTES) {
				std::puts("dlss: sl.interposer.dll is not next to the server, drawing without DLSS");
				return false;
			}
			// Only NVIDIA's own signed build is loaded.
			if (!sl::security::verifyEmbeddedSignature(interposer.c_str())) {
				std::puts("dlss: sl.interposer.dll is not signed by NVIDIA, drawing without DLSS");
				return false;
			}
			module = LoadLibraryW(interposer.c_str());
			if (module == nullptr) return false;
			auto const get = [this](auto &function, char const *name) {
				function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(GetProcAddress(module, name));
				return function != nullptr;
			};
			if (!get(api.init, "slInit") || !get(api.shutdown, "slShutdown") ||
			    !get(api.isFeatureSupported, "slIsFeatureSupported") || !get(api.setD3DDevice, "slSetD3DDevice") ||
			    !get(api.getFeatureVersion, "slGetFeatureVersion") ||
			    !get(api.getNewFrameToken, "slGetNewFrameToken") || !get(api.setTagForFrame, "slSetTagForFrame") ||
			    !get(api.setConstants, "slSetConstants") || !get(api.evaluateFeature, "slEvaluateFeature") ||
			    !get(api.upgradeInterface, "slUpgradeInterface") ||
			    !get(api.getNativeInterface, "slGetNativeInterface") ||
			    !get(api.getFeatureFunction, "slGetFeatureFunction")) {
				std::puts("dlss: sl.interposer.dll lacks a function the server needs");
				return false;
			}

			std::vector<sl::Feature> features;
			if (mode != DlssMode::Off) features.push_back(sl::kFeatureDLSS);
			// Frame generation needs Reflex and its latency markers.
			if (frameGeneration) features.insert(features.end(), {sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL});
			wchar_t const *paths[]{folder.c_str()};
			sl::Preferences preferences{};
			preferences.logLevel = sl::LogLevel::eDefault;
			preferences.logMessageCallback = Log;
			preferences.pathsToPlugins = paths;
			preferences.numPathsToPlugins = 1;
			// The driver keeps its own device and queue, and nothing is downloaded behind the user's back.
			preferences.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseManualHooking |
			                    sl::PreferenceFlags::eUseFrameBasedResourceTagging;
			preferences.featuresToLoad = features.data();
			preferences.numFeaturesToLoad = static_cast<uint32_t>(features.size());
			preferences.engine = sl::EngineType::eCustom;
			preferences.engineVersion = "1.0";
			preferences.projectId = kProjectId;
			preferences.renderAPI = sl::RenderAPI::eD3D12;
			if (!Check(api.init(preferences, sl::kSDKVersion), "slInit")) return false;
			initialised = true;
			return true;
		}

		bool Supported(sl::Feature feature, LUID luid) {
			sl::AdapterInfo adapter{};
			adapter.deviceLUID = reinterpret_cast<uint8_t *>(&luid);
			adapter.deviceLUIDSizeInBytes = sizeof(luid);
			sl::Result const result = api.isFeatureSupported(feature, adapter);
			sl::FeatureVersion version{};
			api.getFeatureVersion(feature, version);
			std::printf("dlss: %s %s (Streamline %s, NGX %s)\n", sl::getFeatureAsStr(feature),
			            result == sl::Result::eOk ? "supported" : sl::getResultAsStr(result),
			            version.versionSL.toStr().c_str(), version.versionNGX.toStr().c_str());
			return result == sl::Result::eOk;
		}

		bool CreateGpu() {
			for (auto &allocator: allocators) {
				if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)))) return false;
			}
			if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[0].Get(), nullptr,
			                                     IID_PPV_ARGS(&list)))) return false;
			list->Close();
			if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return false;
			fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			// Timing is optional.
			uint64_t frequency = 0;
			D3D12_QUERY_HEAP_DESC const heap{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kSlots * 2};
			D3D12_HEAP_PROPERTIES const readback{D3D12_HEAP_TYPE_READBACK};
			D3D12_RESOURCE_DESC buffer{};
			buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			buffer.Width = kSlots * 2 * sizeof(uint64_t);
			buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
			buffer.SampleDesc.Count = 1;
			buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			if (SUCCEEDED(queue->GetTimestampFrequency(&frequency)) && frequency != 0 &&
			    SUCCEEDED(device->CreateQueryHeap(&heap, IID_PPV_ARGS(&queries))) &&
			    SUCCEEDED(device->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &buffer,
			                                              D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
			                                              IID_PPV_ARGS(&queryReadback))) &&
			    SUCCEEDED(queryReadback->Map(0, nullptr, reinterpret_cast<void **>(&queryValues)))) {
				tickMilliseconds = 1000.0 / static_cast<double>(frequency);
			} else {
				queries.Reset();
			}
			return fenceEvent != nullptr;
		}

		void WaitFor(uint64_t value) {
			if (fence->GetCompletedValue() >= value) return;
			fence->SetEventOnCompletion(value, fenceEvent);
			WaitForSingleObject(fenceEvent, INFINITE);
		}

		bool Output(uint32_t width, uint32_t height) {
			if (output && outputWidth == width && outputHeight == height) return true;
			WaitFor(fenceValue);
			output.Reset();
			D3D12_HEAP_PROPERTIES const heap{D3D12_HEAP_TYPE_DEFAULT};
			D3D12_RESOURCE_DESC description{};
			description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			description.Width = width;
			description.Height = height;
			description.DepthOrArraySize = description.MipLevels = 1;
			description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			description.SampleDesc.Count = 1;
			description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
			if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
			                                           D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
			                                           IID_PPV_ARGS(&output)))) return false;
			outputWidth = width;
			outputHeight = height;
			return true;
		}

		void SetConstants(UpscalerInputs const &inputs) {
			using namespace DirectX;
			D3D12_RESOURCE_DESC const motion = inputs.motion->GetDesc();
			XMMATRIX const projection = ToDirect3D(inputs.projection);
			XMMATRIX const view = LoadMatrix(inputs.view);
			XMMATRIX const previous = XMMatrixMultiply(LoadMatrix(inputs.previousView), ToDirect3D(inputs.previousProjection));
			XMMATRIX const clip = XMMatrixMultiply(view, projection);
			XMMATRIX const clipToPrevious = XMMatrixMultiply(XMMatrixInverse(nullptr, clip), previous);
			XMMATRIX const viewToWorld = XMMatrixInverse(nullptr, view);
			XMFLOAT4X4 world;
			XMStoreFloat4x4(&world, viewToWorld);

			sl::Constants constants{};
			Store(constants.cameraViewToClip, projection);
			Store(constants.clipToCameraView, XMMatrixInverse(nullptr, projection));
			Store(constants.clipToLensClip, XMMatrixIdentity());
			Store(constants.clipToPrevClip, clipToPrevious);
			Store(constants.prevClipToClip, XMMatrixInverse(nullptr, clipToPrevious));
			// Streamline wants the shift of what is drawn, the other way from where the samples lie (measured: the
			// sign this way round gives 6 dB more against the full frame in performance mode).
			constants.jitterOffset = {-inputs.jitter[0], -inputs.jitter[1]};
			constants.mvecScale = {1.0f / static_cast<float>(motion.Width), 1.0f / static_cast<float>(motion.Height)};
			constants.cameraPinholeOffset = {0.0f, 0.0f};
			// The view looks down its negative z axis, as OpenGL's does.
			constants.cameraRight = {world._11, world._12, world._13};
			constants.cameraUp = {world._21, world._22, world._23};
			constants.cameraFwd = {-world._31, -world._32, -world._33};
			constants.cameraPos = {world._41, world._42, world._43};
			// An orthographic projection's depth is linear: z = p10 x view z + p14.
			float const p10 = inputs.projection[10], p14 = inputs.projection[14];
			constants.cameraNear = std::fabs((p14 + 1.0f) / p10);
			constants.cameraFar = std::fabs((p14 - 1.0f) / p10);
			// An orthographic camera has no field of view, but Streamline wants one.
			constants.cameraFOV = 1.0f;
			constants.cameraAspectRatio = static_cast<float>(inputs.renderWidth) / static_cast<float>(inputs.renderHeight);
			constants.depthInverted = sl::Boolean::eFalse;
			constants.cameraMotionIncluded = sl::Boolean::eTrue;
			constants.motionVectors3D = sl::Boolean::eFalse;
			constants.reset = inputs.reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
			constants.orthographicProjection = sl::Boolean::eTrue;
			constants.motionVectorsDilated = sl::Boolean::eFalse;
			constants.motionVectorsJittered = sl::Boolean::eFalse;
			Check(api.setConstants(constants, *token, viewport), "slSetConstants");
		}
	};

	Dlss::Dlss() : impl(std::make_unique<Impl>()) {}
	Dlss::~Dlss() = default;

	std::unique_ptr<Dlss> Dlss::Start(DlssMode mode, bool frameGeneration) {
		if (mode == DlssMode::Off && !frameGeneration) return nullptr;
		std::unique_ptr<Dlss> dlss(new Dlss);
		dlss->impl->mode = mode;
		dlss->impl->frameGeneration = frameGeneration;
		if (!dlss->impl->Load()) return nullptr;
		return dlss;
	}

	bool Dlss::SetDevice(ID3D12Device *device, ID3D12CommandQueue *queue) {
		Impl &d = *impl;
		if (!Check(api.setD3DDevice(device), "slSetD3DDevice")) return false;
		d.device = device;
		d.queue = queue;
		LUID const luid = device->GetAdapterLuid();
		if (d.mode != DlssMode::Off && !d.Supported(sl::kFeatureDLSS, luid)) return false;
		if (d.frameGeneration && !d.Supported(sl::kFeatureDLSS_G, luid)) return false;
		if (d.frameGeneration) {
			sl::ReflexOptions reflex{};
			reflex.mode = sl::ReflexMode::eLowLatency;
			Check(slReflexSetOptions(reflex), "slReflexSetOptions");
		}
		return d.CreateGpu();
	}

	DlssMode Dlss::Mode() const { return impl->mode; }
	bool Dlss::FrameGeneration() const { return impl->frameGeneration; }

	void Dlss::RenderSize(uint32_t outputWidth, uint32_t outputHeight, uint32_t &renderWidth, uint32_t &renderHeight) {
		Impl &d = *impl;
		renderWidth = outputWidth;
		renderHeight = outputHeight;
		if (d.mode == DlssMode::Off || d.mode == DlssMode::Dlaa) {
			d.jitterPhases = 8;
			return;
		}
		sl::DLSSOptions options{};
		options.mode = ToSl(d.mode);
		options.outputWidth = outputWidth;
		options.outputHeight = outputHeight;
		sl::DLSSOptimalSettings settings{};
		if (Check(slDLSSGetOptimalSettings(options, settings), "slDLSSGetOptimalSettings") &&
		    settings.optimalRenderWidth != 0 && settings.optimalRenderHeight != 0) {
			renderWidth = settings.optimalRenderWidth;
			renderHeight = settings.optimalRenderHeight;
		}
		// NVIDIA's advice: 8 phases times the scale's area.
		float const scale = static_cast<float>(outputWidth) / static_cast<float>(renderWidth);
		d.jitterPhases = static_cast<uint32_t>(std::ceil(8.0f * scale * scale));
	}

	void Dlss::NextJitter(float &x, float &y) {
		Impl &d = *impl;
		uint32_t const index = d.jitterIndex++ % d.jitterPhases + 1;
		x = Halton(index, 2) - 0.5f;
		y = Halton(index, 3) - 0.5f;
	}

	void Dlss::BeginFrame(uint64_t frame) {
		Impl &d = *impl;
		d.frame = frame;
		uint32_t const index = static_cast<uint32_t>(frame);
		Check(api.getNewFrameToken(d.token, &index), "slGetNewFrameToken");
		if (!d.frameGeneration) return;
		if (d.frameLimit != 0) slReflexSleep(*d.token);
		slPCLSetMarker(sl::PCLMarker::eSimulationStart, *d.token);
		slPCLSetMarker(sl::PCLMarker::eSimulationEnd, *d.token);
		slPCLSetMarker(sl::PCLMarker::eRenderSubmitStart, *d.token);
	}

	ID3D12Resource *Dlss::Evaluate(UpscalerInputs const &inputs,
	                               std::function<void(ID3D12GraphicsCommandList *)> const &prepare) {
		Impl &d = *impl;
		if (d.token == nullptr) return inputs.color;
		bool const upscale = d.mode != DlssMode::Off;
		if (upscale && !d.Output(inputs.outputWidth, inputs.outputHeight)) return inputs.color;

		d.slot = (d.slot + 1) % kSlots;
		d.WaitFor(d.slotFence[d.slot]);
		if (d.slotTimed[d.slot]) {
			d.gpuMilliseconds += static_cast<double>(d.queryValues[d.slot * 2 + 1] - d.queryValues[d.slot * 2]) *
			                     d.tickMilliseconds;
			++d.gpuFrames;
			d.slotTimed[d.slot] = false;
		}
		d.allocators[d.slot]->Reset();
		d.list->Reset(d.allocators[d.slot].Get(), nullptr);
		auto *const commands = reinterpret_cast<sl::CommandBuffer *>(d.list.Get());
		if (prepare) prepare(d.list.Get());

		d.SetConstants(inputs);
		sl::Resource color(sl::ResourceType::eTex2d, inputs.color, D3D12_RESOURCE_STATE_RENDER_TARGET);
		sl::Resource depth(sl::ResourceType::eTex2d, inputs.depth, inputs.depthState);
		sl::Resource motion(sl::ResourceType::eTex2d, inputs.motion, D3D12_RESOURCE_STATE_RENDER_TARGET);
		sl::Resource hudless(sl::ResourceType::eTex2d, inputs.hudless, D3D12_RESOURCE_STATE_RENDER_TARGET);
		sl::Resource output(sl::ResourceType::eTex2d, d.output.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
		sl::Extent const render{0, 0, inputs.renderWidth, inputs.renderHeight};
		// The driver's queue waits for each present before it draws again, so the inputs hold until then.
		std::vector<sl::ResourceTag> tags{
			{&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &render},
			{&motion, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent},
		};
		if (upscale) {
			tags.emplace_back(&color, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilPresent, &render);
			tags.emplace_back(&output, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilPresent);
		}
		if (inputs.hudless != nullptr) {
			tags.emplace_back(&hudless, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent);
		}
		Check(api.setTagForFrame(*d.token, d.viewport, tags.data(), static_cast<uint32_t>(tags.size()), commands),
		      "slSetTagForFrame");

		if (upscale) {
			sl::DLSSOptions options{};
			options.mode = ToSl(d.mode);
			options.outputWidth = inputs.outputWidth;
			options.outputHeight = inputs.outputHeight;
			// SC4 draws in 8 bits per channel, already tone mapped.
			options.colorBuffersHDR = sl::Boolean::eFalse;
			options.useAutoExposure = sl::Boolean::eFalse;
			if (!d.optionsSet || options.mode != d.options.mode || options.outputWidth != d.options.outputWidth ||
			    options.outputHeight != d.options.outputHeight) {
				d.optionsSet = Check(slDLSSSetOptions(d.viewport, options), "slDLSSSetOptions");
				d.options = options;
			}
			if (d.queries) d.list->EndQuery(d.queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, d.slot * 2);
			sl::BaseStructure const *evaluateInputs[]{&d.viewport};
			Check(api.evaluateFeature(sl::kFeatureDLSS, *d.token, evaluateInputs, 1, commands), "slEvaluateFeature");
			if (d.queries) {
				d.list->EndQuery(d.queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, d.slot * 2 + 1);
				d.list->ResolveQueryData(d.queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, d.slot * 2, 2, d.queryReadback.Get(),
				                         d.slot * 2 * sizeof(uint64_t));
				d.slotTimed[d.slot] = true;
			}
		}
		d.list->Close();
		ID3D12CommandList *const lists[]{d.list.Get()};
		d.queue->ExecuteCommandLists(1, lists);
		d.queue->Signal(d.fence.Get(), ++d.fenceValue);
		d.slotFence[d.slot] = d.fenceValue;
		if (d.frameGeneration) slPCLSetMarker(sl::PCLMarker::eRenderSubmitEnd, *d.token);
		return upscale ? d.output.Get() : inputs.color;
	}

	void Dlss::TakeGpuTime(double &milliseconds, uint32_t &frames) {
		milliseconds = impl->gpuMilliseconds;
		frames = impl->gpuFrames;
		impl->gpuMilliseconds = 0.0;
		impl->gpuFrames = 0;
	}

	ID3D12CommandQueue *Dlss::PresentQueue() {
		Impl &d = *impl;
		if (d.presentQueue) return d.presentQueue.Get();
		d.proxyDevice = d.device;
		d.proxyDevice->AddRef();
		if (!UpgradeInterface(d.proxyDevice, "slUpgradeInterface (device)")) return nullptr;
		D3D12_COMMAND_QUEUE_DESC description{};
		description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		if (FAILED(d.proxyDevice->CreateCommandQueue(&description, IID_PPV_ARGS(&d.presentQueue)))) return nullptr;
		return d.presentQueue.Get();
	}

	ID3D12CommandQueue *Dlss::NativePresentQueue() {
		Impl &d = *impl;
		if (!d.nativePresentQueue) {
			ID3D12CommandQueue *const proxy = PresentQueue();
			if (proxy == nullptr) return nullptr;
			void *native = nullptr;
			if (!Check(api.getNativeInterface(proxy, &native), "slGetNativeInterface")) return nullptr;
			d.nativePresentQueue.Attach(static_cast<ID3D12CommandQueue *>(native));
		}
		return d.nativePresentQueue.Get();
	}

	bool Dlss::Upgrade(IDXGISwapChain1 *&swapChain) {
		return UpgradeInterface(swapChain, "slUpgradeInterface (swap chain)");
	}

	bool Dlss::Upgrade(IDXGIFactory2 *&factory) { return UpgradeInterface(factory, "slUpgradeInterface (factory)"); }

	void Dlss::PresentStart() {
		if (impl->frameGeneration && impl->token) slPCLSetMarker(sl::PCLMarker::ePresentStart, *impl->token);
	}

	void Dlss::PresentEnd() {
		if (impl->frameGeneration && impl->token) slPCLSetMarker(sl::PCLMarker::ePresentEnd, *impl->token);
	}

	void Dlss::EnableFrameGeneration(bool on) {
		if (!impl->frameGeneration) return;
		sl::DLSSGOptions options{};
		options.mode = on ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
		options.numFramesToGenerate = 1;
		Check(slDLSSGSetOptions(impl->viewport, options), "slDLSSGSetOptions");
	}

	void Dlss::FrameGenerationState(uint32_t &presented, uint32_t &status) {
		presented = status = 0;
		if (!impl->frameGeneration) return;
		sl::DLSSGState state{};
		if (slDLSSGGetState(impl->viewport, state, nullptr) != sl::Result::eOk) return;
		presented = state.numFramesActuallyPresented;
		status = static_cast<uint32_t>(state.status);
	}

	void Dlss::LimitFrameRate(uint32_t framesPerSecond) {
		Impl &d = *impl;
		if (!d.frameGeneration) return;
		d.frameLimit = framesPerSecond;
		sl::ReflexOptions reflex{};
		reflex.mode = sl::ReflexMode::eLowLatency;
		reflex.frameLimitUs = framesPerSecond != 0 ? 1000000 / framesPerSecond : 0;
		Check(slReflexSetOptions(reflex), "slReflexSetOptions");
	}
}
