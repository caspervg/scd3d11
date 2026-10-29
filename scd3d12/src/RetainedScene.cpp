/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

// SC4's camera, as the stream shows it (matrices are OpenGL's, column major):
//
//   projection  orthographic: x scale 2 / (window width x metres per pixel), y scale the same for the height,
//               z scale and offset for the depth range, and an offset of less than a pixel in x and y
//   view        world to view: a turn about world up (yaw), then a tilt about the screen's x axis (pitch), then
//               a translation. World up never leans sideways on screen, so view[4] is 0
//   modelview   view x world for a model, where world = rotation x (scale x vertex) + translation
//
// Every frame SC4 loads the full window's projection and the view before restoring its backing store, and again
// before the dynamic passes. A partial redraw fits the projection to its viewport but keeps the view.

#include "RetainedScene.h"

#include "Dlss.h"
#include "wire/ImageFile.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace nSCD3D12 {
	namespace {
		constexpr uint32_t kSlots = 4;
		constexpr DXGI_FORMAT kMotionFormat = DXGI_FORMAT_R16G16_FLOAT;
		// The stage 1 coordinate source of the game's flora tint: positions in view space.
		constexpr uint32_t kViewPositions = 16;

		// Column major, as OpenGL: m[column * 4 + row].
		void Multiply(float const *a, float const *b, float *out) {
			float r[16];
			for (int c = 0; c < 4; ++c) {
				for (int row = 0; row < 4; ++row) {
					r[c * 4 + row] = a[row] * b[c * 4] + a[4 + row] * b[c * 4 + 1] + a[8 + row] * b[c * 4 + 2] +
					                 a[12 + row] * b[c * 4 + 3];
				}
			}
			std::memcpy(out, r, sizeof(r));
		}

		// The inverse of a general 4x4 matrix, in double precision. False when it has none.
		bool Invert(float const *m, float *out) {
			double a[4][8];
			for (int row = 0; row < 4; ++row) {
				for (int c = 0; c < 4; ++c) {
					a[row][c] = m[c * 4 + row];
					a[row][c + 4] = row == c ? 1.0 : 0.0;
				}
			}
			for (int c = 0; c < 4; ++c) {
				int pivot = c;
				for (int row = c + 1; row < 4; ++row) {
					if (std::fabs(a[row][c]) > std::fabs(a[pivot][c])) pivot = row;
				}
				if (std::fabs(a[pivot][c]) < 1e-30) return false;
				std::swap(a[c], a[pivot]);
				double const scale = 1.0 / a[c][c];
				for (double &value: a[c]) value *= scale;
				for (int row = 0; row < 4; ++row) {
					if (row == c) continue;
					double const factor = a[row][c];
					for (int k = 0; k < 8; ++k) a[row][k] -= factor * a[c][k];
				}
			}
			for (int row = 0; row < 4; ++row) {
				for (int c = 0; c < 4; ++c) out[c * 4 + row] = static_cast<float>(a[row][c + 4]);
			}
			return true;
		}

		// The world transform of an instance: world = rotation (row major) x (scale x vertex) + translation, each
		// part only when the instance has it.
		void WorldMatrix(nSCDWire::ModelInstance const &instance, float *m) {
			float const scale = (instance.parts & 2) ? instance.scale : 1.0f;
			std::memset(m, 0, 16 * sizeof(float));
			for (int row = 0; row < 3; ++row) {
				for (int c = 0; c < 3; ++c) {
					float const r = (instance.parts & 4) ? instance.rotation[row * 3 + c] : (row == c ? 1.0f : 0.0f);
					m[c * 4 + row] = r * scale;
				}
				m[12 + row] = (instance.parts & 1) ? instance.translation[row] : 0.0f;
			}
			m[15] = 1.0f;
		}

		struct Key {
			uint64_t occupant;
			uint32_t group, instance;
			bool operator==(Key const &) const = default;
		};

		struct KeyHash {
			size_t operator()(Key const &key) const noexcept {
				uint64_t h = key.occupant * 0x9E3779B97F4A7C15ull;
				h ^= (static_cast<uint64_t>(key.group) << 32 | key.instance) + 0x632BE59BD9B4E019ull + (h << 6) + (h >> 2);
				return static_cast<size_t>(h);
			}
		};

		struct MeshDraw {
			D3D12_VERTEX_BUFFER_VIEW vertices;
			D3D12_INDEX_BUFFER_VIEW indices;
			uint32_t count;
			int32_t polygonOffset;
			bool fog, stencil;
			cGDriver12::DrawState state;
		};

		struct Box {
			float min[3], max[3];
		};

		struct Instance {
			Key key{};
			// Where the model lies in the world.
			Box box{};
			bool alive = true;
			// Its occupant was removed or moved. SC4 keeps showing it until it redraws over it.
			bool orphan = false;
			// The frame SC4 last drew it in, and the camera it drew it with (0 while that is this frame's).
			uint64_t frame = 0;
			uint32_t camera = 0;
			float world[16]{};
			float previousWorld[16]{};
			// SC4's own modelview, used as it is while the camera has not changed since.
			float modelView[16]{};
			// The view SC4 drew it with, which texture coordinates made from view positions depend on.
			float view[16]{};
			bool viewPositions = false;
			std::vector<MeshDraw> meshes;
		};

		struct Camera {
			bool known = false;
			float projection[16]{};
			float view[16]{};
			int32_t width = 0, height = 0;
		};

		char const kMotionShaders[] = R"(
cbuffer Motion : register(b0)
{
	// Full screen: this frame's clip space to world, and the previous frame's world to clip space.
	// Instances: this frame's modelview, the previous frame's projection x view x world, this frame's projection.
	float4x4 current;
	float4x4 previous;
	float4x4 projection;
	float4 size; // width, height, 1 / width, 1 / height
};

Texture2D<float> depth : register(t0);

// Where a clip space position lands in pixels, y down.
float2 Pixel(float4 clip)
{
	float2 ndc = clip.xy / clip.w;
	return float2(ndc.x + 1.0f, 1.0f - ndc.y) * 0.5f * size.xy;
}

float4 FullScreenVS(uint id : SV_VertexID) : SV_POSITION
{
	float2 uv = float2((id << 1) & 2, id & 2);
	return float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
}

// Motion of whatever stands still: the pixel back to the world through its depth, then into the previous frame.
float2 ReprojectPS(float4 position : SV_POSITION) : SV_TARGET
{
	float z = depth.Load(int3(position.xy, 0));
	float2 ndc = float2(position.x * size.z * 2.0f - 1.0f, 1.0f - position.y * size.w * 2.0f);
	float4 world = mul(current, float4(ndc, z * 2.0f - 1.0f, 1.0f));
	return Pixel(mul(previous, world / world.w)) - position.xy;
}

struct InstanceOutput
{
	float4 position : SV_POSITION;
	float4 previous : TEXCOORD0;
};

// The same two steps as the driver's vertex shader, so the depth matches what the instance wrote.
InstanceOutput InstanceVS(float3 position : POSITION)
{
	InstanceOutput output;
	float4 view = mul(current, float4(position, 1.0f));
	output.position = mul(projection, view);
	output.position.z = (output.position.z + output.position.w) * 0.5f - 1.0e-5f * output.position.w;
	output.previous = mul(previous, float4(position, 1.0f));
	return output;
}

float2 InstancePS(InstanceOutput input) : SV_TARGET
{
	return Pixel(input.previous) - input.position.xy;
}
)";

		struct MotionConstants {
			float current[16];
			float previous[16];
			float projection[16];
			float size[4];
		};
		static_assert(sizeof(MotionConstants) == 52 * 4);

		struct Totals {
			uint64_t composites = 0, skipped = 0, instancesDrawn = 0, meshesDrawn = 0, culled = 0;
			uint64_t captured = 0, meshesCaptured = 0;
			uint64_t removedOccupant = 0, removedView = 0, removedRedrawn = 0;
			uint64_t movedInstances = 0, otherCamera = 0;
			double worstModelViewError = 0.0;
			std::vector<float> cpuMilliseconds, gpuMilliseconds;
		};

		// Timing per zoom, keyed by millimetres per pixel.
		struct Zoom {
			uint64_t frames = 0, instances = 0, meshes = 0;
			std::vector<float> cpuMilliseconds, gpuMilliseconds;
		};
	}

	struct RetainedScene::Impl {
		cGDriver12 &driver;
		SceneLane &lane;
		std::vector<Instance> instances;
		std::unordered_map<Key, uint32_t, KeyHash> byKey;
		std::unordered_map<uint64_t, std::vector<uint32_t> > byOccupant;
		uint32_t dead = 0;
		// The instance being captured, and the drawable and mesh it came from.
		uint32_t capturing = UINT32_MAX;
		uint64_t capturingDrawable = 0;
		uint32_t lastMesh = 0;
		std::vector<uint32_t> capturedThisFrame;
		// SC4's static redraws this frame: viewports as SimGL gives them, x0, y0, x1, y1 from the bottom left.
		std::vector<std::array<int32_t, 4> > redraws;
		int32_t lastPass = -1;
		uint64_t frame = 0;
		bool composited = false;
		Camera camera, previousCamera;
		uint32_t cameraId = 0;
		// For the upscaler: the jitter instances are drawn with, the camera motion was measured from and the
		// frame that was last drawn with a city camera.
		float jitter[2]{}, drawnJitter[2]{};
		Camera motionFrom;
		bool reset = true;
		uint64_t cameraFrame = UINT64_MAX;
		Totals totals;
		std::map<int32_t, Zoom> zooms;
		int32_t slotZoom[kSlots]{};

		// Motion vectors and timing.
		bool gpuReady = false;
		bool keepMotion = false, motionKept = false;
		ComPtr<ID3D12RootSignature> rootSignature;
		ComPtr<ID3D12PipelineState> reprojectPipeline, instancePipeline;
		ComPtr<ID3D12DescriptorHeap> rtvHeap, viewHeap;
		uint32_t viewIncrement = 0;
		ComPtr<ID3D12Resource> motion, readback;
		int32_t motionWidth = 0, motionHeight = 0;
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
		ComPtr<ID3D12QueryHeap> queries;
		ComPtr<ID3D12Resource> queryReadback;
		ComPtr<ID3D12Fence> fence;
		HANDLE fenceEvent = nullptr;
		uint64_t fenceValue = 0;
		uint64_t slotFence[kSlots]{};
		bool slotTimed[kSlots]{};
		double tickMilliseconds = 0.0;

		Impl(cGDriver12 &driver, SceneLane &lane) : driver(driver), lane(lane) { gpuReady = CreateGpu(); }

		~Impl() {
			if (fence) {
				WaitFor(fenceValue);
				for (uint32_t slot = 0; slot < kSlots; ++slot) ReadTimes(slot);
			}
			if (fenceEvent != nullptr) CloseHandle(fenceEvent);
		}

		bool CreateGpu() {
			ID3D12Device *const device = driver.Device();
			if (device == nullptr) return false;
			D3D12_DESCRIPTOR_RANGE const range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
			D3D12_ROOT_PARAMETER parameters[2]{};
			parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
			parameters[0].Constants = {0, 0, sizeof(MotionConstants) / 4};
			parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
			parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			parameters[1].DescriptorTable = {1, &range};
			parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
			D3D12_ROOT_SIGNATURE_DESC const signature{
				2, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
			};
			ComPtr<ID3DBlob> serialized, errors;
			if (FAILED(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)) ||
			    FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
			                                       IID_PPV_ARGS(&rootSignature)))) return false;

			auto const compile = [](char const *entry, char const *target, ComPtr<ID3DBlob> &code) {
				ComPtr<ID3DBlob> messages;
				HRESULT const result = D3DCompile(kMotionShaders, sizeof(kMotionShaders) - 1, "RetainedScene", nullptr,
				                                  nullptr, entry, target, D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &messages);
				if (FAILED(result) && messages) std::fprintf(stderr, "%s\n", static_cast<char const *>(messages->GetBufferPointer()));
				return SUCCEEDED(result);
			};
			ComPtr<ID3DBlob> fullScreenVS, reprojectPS, instanceVS, instancePS;
			if (!compile("FullScreenVS", "vs_5_0", fullScreenVS) || !compile("ReprojectPS", "ps_5_0", reprojectPS) ||
			    !compile("InstanceVS", "vs_5_0", instanceVS) || !compile("InstancePS", "ps_5_0", instancePS)) return false;

			D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
			description.pRootSignature = rootSignature.Get();
			description.VS = {fullScreenVS->GetBufferPointer(), fullScreenVS->GetBufferSize()};
			description.PS = {reprojectPS->GetBufferPointer(), reprojectPS->GetBufferSize()};
			description.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
			description.SampleMask = UINT_MAX;
			description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
			description.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
			description.RasterizerState.DepthClipEnable = TRUE;
			description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			description.NumRenderTargets = 1;
			description.RTVFormats[0] = kMotionFormat;
			description.SampleDesc.Count = 1;
			if (FAILED(device->CreateGraphicsPipelineState(&description, IID_PPV_ARGS(&reprojectPipeline)))) return false;

			D3D12_INPUT_ELEMENT_DESC const input{
				"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0
			};
			description.VS = {instanceVS->GetBufferPointer(), instanceVS->GetBufferSize()};
			description.PS = {instancePS->GetBufferPointer(), instancePS->GetBufferSize()};
			description.InputLayout = {&input, 1};
			description.DepthStencilState.DepthEnable = TRUE;
			description.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
			description.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
			description.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
			if (FAILED(device->CreateGraphicsPipelineState(&description, IID_PPV_ARGS(&instancePipeline)))) return false;

			D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
			if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&rtvHeap)))) return false;
			heap = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSlots, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
			if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&viewHeap)))) return false;
			viewIncrement = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

			D3D12_QUERY_HEAP_DESC const queryHeap{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kSlots * 2};
			if (FAILED(device->CreateQueryHeap(&queryHeap, IID_PPV_ARGS(&queries)))) return false;
			if (!CreateBuffer(D3D12_HEAP_TYPE_READBACK, kSlots * 2 * sizeof(uint64_t), queryReadback)) return false;
			uint64_t frequency = 0;
			if (FAILED(driver.Queue()->GetTimestampFrequency(&frequency)) || frequency == 0) return false;
			tickMilliseconds = 1000.0 / static_cast<double>(frequency);
			if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return false;
			fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			return fenceEvent != nullptr;
		}

		bool CreateBuffer(D3D12_HEAP_TYPE type, uint64_t bytes, ComPtr<ID3D12Resource> &buffer) {
			D3D12_HEAP_PROPERTIES const heap{type};
			D3D12_RESOURCE_DESC description{};
			description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			description.Width = bytes;
			description.Height = description.DepthOrArraySize = description.MipLevels = 1;
			description.SampleDesc.Count = 1;
			description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			return SUCCEEDED(driver.Device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
			                                                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
			                                                         IID_PPV_ARGS(&buffer)));
		}

		// The motion target and its readback buffer, at the window's size.
		bool MotionTarget(int32_t width, int32_t height) {
			if (motion && motionWidth == width && motionHeight == height) return true;
			WaitFor(fenceValue);
			motion.Reset();
			readback.Reset();
			ID3D12Device *const device = driver.Device();
			D3D12_HEAP_PROPERTIES const heap{D3D12_HEAP_TYPE_DEFAULT};
			D3D12_RESOURCE_DESC description{};
			description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			description.Width = static_cast<UINT64>(width);
			description.Height = static_cast<UINT>(height);
			description.DepthOrArraySize = description.MipLevels = 1;
			description.Format = kMotionFormat;
			description.SampleDesc.Count = 1;
			description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
			D3D12_CLEAR_VALUE const clear{kMotionFormat, {0, 0, 0, 0}};
			if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
			                                           D3D12_RESOURCE_STATE_RENDER_TARGET, &clear, IID_PPV_ARGS(&motion)))) {
				return false;
			}
			device->CreateRenderTargetView(motion.Get(), nullptr, rtvHeap->GetCPUDescriptorHandleForHeapStart());
			UINT64 bytes = 0;
			device->GetCopyableFootprints(&description, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
			if (!CreateBuffer(D3D12_HEAP_TYPE_READBACK, bytes, readback)) return false;
			motionWidth = width;
			motionHeight = height;
			return true;
		}

		void WaitFor(uint64_t value) {
			if (!fence || fence->GetCompletedValue() >= value) return;
			fence->SetEventOnCompletion(value, fenceEvent);
			WaitForSingleObject(fenceEvent, INFINITE);
		}

		void ReadTimes(uint32_t slot) {
			if (!slotTimed[slot]) return;
			slotTimed[slot] = false;
			D3D12_RANGE const range{slot * 16, slot * 16 + 16};
			uint64_t *ticks = nullptr;
			if (FAILED(queryReadback->Map(0, &range, reinterpret_cast<void **>(&ticks)))) return;
			uint64_t const begin = ticks[slot * 2], end = ticks[slot * 2 + 1];
			D3D12_RANGE const none{0, 0};
			queryReadback->Unmap(0, &none);
			if (end <= begin) return;
			float const milliseconds = static_cast<float>((end - begin) * tickMilliseconds);
			totals.gpuMilliseconds.push_back(milliseconds);
			zooms[slotZoom[slot]].gpuMilliseconds.push_back(milliseconds);
		}

		void Barrier(ID3D12Resource *resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
			D3D12_RESOURCE_BARRIER barrier{};
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrier.Transition.pResource = resource;
			barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			barrier.Transition.StateBefore = before;
			barrier.Transition.StateAfter = after;
			driver.CommandList()->ResourceBarrier(1, &barrier);
		}

		// Instances

		void Kill(uint32_t index) {
			Instance &instance = instances[index];
			if (!instance.alive) return;
			instance.alive = false;
			instance.meshes = {};
			byKey.erase(instance.key);
			auto const found = byOccupant.find(instance.key.occupant);
			if (found != byOccupant.end()) {
				std::erase(found->second, index);
				if (found->second.empty()) byOccupant.erase(found);
			}
			++dead;
		}

		void Orphan(uint64_t occupant) {
			auto const found = byOccupant.find(occupant);
			if (found == byOccupant.end()) return;
			for (uint32_t index: found->second) instances[index].orphan = true;
		}

		// Drops dead instances, keeping the others in order.
		void Compact() {
			if (dead == 0) return;
			std::vector<Instance> kept;
			kept.reserve(instances.size() - dead);
			std::vector<uint32_t> remap(instances.size(), UINT32_MAX);
			for (uint32_t i = 0; i < instances.size(); ++i) {
				if (!instances[i].alive) continue;
				remap[i] = static_cast<uint32_t>(kept.size());
				kept.push_back(std::move(instances[i]));
			}
			instances = std::move(kept);
			for (auto &[key, index]: byKey) index = remap[index];
			for (auto &[occupant, list]: byOccupant) {
				for (uint32_t &index: list) index = remap[index];
			}
			for (uint32_t &index: capturedThisFrame) index = remap[index];
			capturedThisFrame.erase(std::remove(capturedThisFrame.begin(), capturedThisFrame.end(), UINT32_MAX),
			                        capturedThisFrame.end());
			if (capturing != UINT32_MAX) capturing = remap[capturing];
			dead = 0;
		}

		// The rectangle a world box covers on screen, in pixels from the top left, with the camera's
		// projection x view. False when it lies off screen.
		static bool ScreenRect(float const *pv, Box const &box, int32_t width, int32_t height, float *rect) {
			float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
			for (int corner = 0; corner < 8; ++corner) {
				float const x = (corner & 1) ? box.max[0] : box.min[0];
				float const y = (corner & 2) ? box.max[1] : box.min[1];
				float const z = (corner & 4) ? box.max[2] : box.min[2];
				float const cx = pv[0] * x + pv[4] * y + pv[8] * z + pv[12];
				float const cy = pv[1] * x + pv[5] * y + pv[9] * z + pv[13];
				x0 = (std::min)(x0, cx);
				x1 = (std::max)(x1, cx);
				y0 = (std::min)(y0, cy);
				y1 = (std::max)(y1, cy);
			}
			rect[0] = (x0 + 1.0f) * 0.5f * static_cast<float>(width);
			rect[2] = (x1 + 1.0f) * 0.5f * static_cast<float>(width);
			rect[1] = (1.0f - y1) * 0.5f * static_cast<float>(height);
			rect[3] = (1.0f - y0) * 0.5f * static_cast<float>(height);
			return x1 >= -1.0f && x0 <= 1.0f && y1 >= -1.0f && y0 <= 1.0f;
		}

		// The frame's camera, as SC4 left it for the dynamic passes. False when what is loaded is not a city
		// camera over the whole window.
		bool ReadCamera(cGDriver12::Transform const &transform, Camera &out) {
			float const *const p = transform.projection;
			float const *const v = transform.modelView;
			int32_t viewport[4];
			driver.GetViewport(viewport);
			if (viewport[0] != 0 || viewport[1] != 0 || viewport[2] <= 0 || viewport[3] <= 0) return false;
			if (p[3] != 0.0f || p[7] != 0.0f || p[11] != 0.0f || p[15] != 1.0f || p[0] == 0.0f || p[5] == 0.0f) return false;
			// World up stays upright on screen, and the turn is a rotation.
			float const up = std::sqrt(v[4] * v[4] + v[5] * v[5] + v[6] * v[6]);
			if (std::fabs(v[4]) > 1e-4f || std::fabs(up - 1.0f) > 1e-3f || v[15] != 1.0f) return false;
			out.known = true;
			std::memcpy(out.projection, p, sizeof(out.projection));
			std::memcpy(out.view, v, sizeof(out.view));
			out.width = viewport[2];
			out.height = viewport[3];
			return true;
		}

		static bool SameView(Camera const &a, Camera const &b) {
			if (!a.known || !b.known || a.width != b.width || a.height != b.height) return false;
			for (int i: {0, 5, 10}) {
				if (a.projection[i] != b.projection[i]) return false;
			}
			for (int c = 0; c < 3; ++c) {
				for (int row = 0; row < 3; ++row) {
					if (std::fabs(a.view[c * 4 + row] - b.view[c * 4 + row]) > 1e-5f) return false;
				}
			}
			return true;
		}

		static void Describe(Camera const &camera) {
			float const *const v = camera.view;
			// Row 0 of the view's turn is the screen's x axis in the world, row 2 points back at the camera.
			double const yaw = std::atan2(-v[8], v[0]) * 180.0 / 3.14159265358979;
			double const pitch = std::asin(std::clamp(static_cast<double>(v[6]), -1.0, 1.0)) * 180.0 / 3.14159265358979;
			double const metres = 2.0 / (camera.projection[0] * camera.width);
			std::printf("retained: camera yaw %.2f, pitch %.2f, %.4f metres per pixel (%.3f by %.3f), window %dx%d\n",
			            yaw, pitch, metres, 2.0 / camera.projection[0], 2.0 / camera.projection[5], camera.width,
			            camera.height);
		}

		void Composite() {
			composited = true;
			auto const start = std::chrono::steady_clock::now();
			cGDriver12::Transform game{};
			driver.SaveTransform(game);
			Camera now;
			if (!ReadCamera(game, now)) {
				++totals.skipped;
				return;
			}
			ID3D12GraphicsCommandList *const list = driver.CommandList();
			uint32_t const slot = static_cast<uint32_t>(frame % kSlots);
			if (gpuReady) {
				WaitFor(slotFence[slot]);
				ReadTimes(slot);
				list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2);
			}

			// A new zoom or rotation shows other models: only what SC4 drew this frame stays.
			if (!SameView(now, camera)) {
				Describe(now);
				for (uint32_t i = 0; i < instances.size(); ++i) {
					if (instances[i].alive && instances[i].frame != frame) {
						Kill(i);
						++totals.removedView;
					}
				}
				previousCamera = {};
			}
			if (!camera.known || std::memcmp(now.view, camera.view, sizeof(now.view)) != 0 ||
			    std::memcmp(now.projection, camera.projection, sizeof(now.projection)) != 0) {
				++cameraId;
			}
			camera = now;
			float pv[16];
			Multiply(now.projection, now.view, pv);
			float inverseView[16];
			bool const inverted = Invert(now.view, inverseView);

			// What SC4 drew this frame normally has this frame's camera, and then its own modelview is used as it
			// is. How closely view x world matches it shows how well the camera is recovered.
			for (uint32_t index: capturedThisFrame) {
				Instance &instance = instances[index];
				if (!instance.alive) continue;
				float mine[16];
				Multiply(now.view, instance.world, mine);
				double worst = 0.0;
				for (int i = 0; i < 16; ++i) {
					worst = (std::max)(worst, std::fabs(static_cast<double>(mine[i]) - instance.modelView[i]));
				}
				// Right after a city loads SC4 can redraw parts of the view with an older camera than the one it
				// shows the frame with. Such a modelview is no use for this camera.
				if (worst < 0.01) {
					instance.camera = cameraId;
					std::memcpy(instance.view, now.view, sizeof(instance.view));
					totals.worstModelViewError = (std::max)(totals.worstModelViewError, worst);
				} else {
					float inverseWorld[16];
					if (Invert(instance.world, inverseWorld)) Multiply(instance.modelView, inverseWorld, instance.view);
					++totals.otherCamera;
				}
			}

			// A rectangle SC4 redrew without an instance that lies wholly inside it means the instance is gone. An
			// orphan goes as soon as SC4 redraws any part of it.
			for (auto const &redraw: redraws) {
				float const left = static_cast<float>(redraw[0]), right = static_cast<float>(redraw[2]);
				float const top = static_cast<float>(now.height - redraw[3]);
				float const bottom = static_cast<float>(now.height - redraw[1]);
				for (uint32_t i = 0; i < instances.size(); ++i) {
					Instance const &instance = instances[i];
					if (!instance.alive || instance.frame == frame) continue;
					float rect[4];
					if (!ScreenRect(pv, instance.box, now.width, now.height, rect)) continue;
					if (instance.orphan ? rect[0] < right && rect[2] > left && rect[1] < bottom && rect[3] > top
					                    : rect[0] >= left && rect[2] <= right && rect[1] >= top && rect[3] <= bottom) {
						++(instance.orphan ? totals.removedOccupant : totals.removedRedrawn);
						Kill(i);
					}
				}
			}
			if (dead > 4096 && dead > instances.size() / 2) Compact();

			// Draw every instance on screen, in the order SC4 first drew them.
			cGDriver12::DrawState saved{};
			driver.SaveDrawState(saved);
			bool const fog = driver.IsEnabled(kGDCapability_Fog);
			bool const stencil = driver.IsEnabled(kGDCapability_StencilTest);
			int32_t const offset = driver.GetPolygonOffset();
			bool fogNow = fog, stencilNow = stencil;
			int32_t offsetNow = offset;
			// The jitter moves what is drawn the other way.
			if (jitter[0] != 0.0f || jitter[1] != 0.0f) {
				float projection[16];
				std::memcpy(projection, now.projection, sizeof(projection));
				projection[12] -= 2.0f * jitter[0] / static_cast<float>(now.width);
				projection[13] += 2.0f * jitter[1] / static_cast<float>(now.height);
				driver.MatrixMode(1);
				driver.LoadMatrix(projection);
			}
			std::memcpy(drawnJitter, jitter, sizeof(jitter));
			driver.MatrixMode(0);
			// Bracketed as the static grid, so the driver counts the draws as the city.
			nSCDWire::DrawOwner owner{};
			owner.kind = nSCDWire::DrawOwnerKind::Grid;
			owner.pass = 1;
			driver.OwnerBegin(owner);
			uint64_t drawn = 0, meshes = 0;
			for (Instance &instance: instances) {
				if (!instance.alive) continue;
				float rect[4];
				if (!ScreenRect(pv, instance.box, now.width, now.height, rect)) {
					++totals.culled;
					continue;
				}
				bool const sameCamera = instance.camera == cameraId;
				if (sameCamera) {
					driver.LoadMatrix(instance.modelView);
				} else {
					float modelView[16];
					Multiply(now.view, instance.world, modelView);
					driver.LoadMatrix(modelView);
				}
				// Coordinates from view positions stay where SC4 put them: back to the view it drew with.
				float viewChange[16];
				bool const moveCoordinates = !sameCamera && instance.viewPositions && inverted && instance.view[15] == 1.0f;
				if (moveCoordinates) Multiply(instance.view, inverseView, viewChange);
				for (MeshDraw const &mesh: instance.meshes) {
					if (moveCoordinates && mesh.state.stages[1].enabled &&
					    mesh.state.stages[1].coordinates == kViewPositions) {
						cGDriver12::DrawState state = mesh.state;
						Multiply(mesh.state.stages[1].matrix, viewChange, state.stages[1].matrix);
						driver.RestoreDrawState(state);
					} else {
						driver.RestoreDrawState(mesh.state);
					}
					if (mesh.fog != fogNow) {
						fogNow = mesh.fog;
						if (fogNow) driver.Enable(kGDCapability_Fog);
						else driver.Disable(kGDCapability_Fog);
					}
					if (mesh.stencil != stencilNow) {
						stencilNow = mesh.stencil;
						if (stencilNow) driver.Enable(kGDCapability_StencilTest);
						else driver.Disable(kGDCapability_StencilTest);
					}
					if (mesh.polygonOffset != offsetNow) driver.PolygonOffset(offsetNow = mesh.polygonOffset);
					driver.DrawTriangles(mesh.vertices, mesh.indices, mesh.count, 0x80004001);
					++meshes;
				}
				++drawn;
			}
			driver.OwnerEnd();
			driver.RestoreDrawState(saved);
			if (fogNow != fog) {
				if (fog) driver.Enable(kGDCapability_Fog);
				else driver.Disable(kGDCapability_Fog);
			}
			if (stencilNow != stencil) {
				if (stencil) driver.Enable(kGDCapability_StencilTest);
				else driver.Disable(kGDCapability_StencilTest);
			}
			if (offsetNow != offset) driver.PolygonOffset(offset);
			driver.RestoreTransform(game);
			int32_t const zoom = static_cast<int32_t>(std::lround(2000.0 / (now.projection[0] * now.width)));
			totals.instancesDrawn += drawn;
			totals.meshesDrawn += meshes;
			++totals.composites;

			if (gpuReady) {
				Motion(pv);
				list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2 + 1);
				list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2, 2, queryReadback.Get(),
				                       slot * 16);
				slotTimed[slot] = true;
				slotZoom[slot] = zoom;
				driver.Rebind();
			}
			for (Instance &instance: instances) {
				if (instance.alive) std::memcpy(instance.previousWorld, instance.world, sizeof(instance.world));
			}
			reset = !previousCamera.known;
			motionFrom = previousCamera.known ? previousCamera : camera;
			cameraFrame = frame;
			previousCamera = camera;
			float const milliseconds = static_cast<float>(
				std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
			totals.cpuMilliseconds.push_back(milliseconds);
			Zoom &z = zooms[zoom];
			++z.frames;
			z.instances += drawn;
			z.meshes += meshes;
			z.cpuMilliseconds.push_back(milliseconds);
		}

		// The motion vector target: the previous position of every pixel minus its position now, in pixels.
		void Motion(float const *pv) {
			if (!MotionTarget(camera.width, camera.height)) return;
			ID3D12GraphicsCommandList *const list = driver.CommandList();
			ID3D12Resource *const depth = driver.DepthBuffer();
			MotionConstants constants{};
			constants.size[0] = static_cast<float>(camera.width);
			constants.size[1] = static_cast<float>(camera.height);
			constants.size[2] = 1.0f / constants.size[0];
			constants.size[3] = 1.0f / constants.size[1];
			// Without a previous frame nothing moved.
			Camera const &before = previousCamera.known ? previousCamera : camera;
			float previousPv[16];
			Multiply(before.projection, before.view, previousPv);
			if (!Invert(pv, constants.current)) return;
			std::memcpy(constants.previous, previousPv, sizeof(previousPv));

			uint32_t const slot = static_cast<uint32_t>(frame % kSlots);
			D3D12_SHADER_RESOURCE_VIEW_DESC view{};
			view.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
			view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			view.Texture2D.MipLevels = 1;
			D3D12_CPU_DESCRIPTOR_HANDLE cpu = viewHeap->GetCPUDescriptorHandleForHeapStart();
			cpu.ptr += slot * viewIncrement;
			driver.Device()->CreateShaderResourceView(depth, &view, cpu);
			D3D12_GPU_DESCRIPTOR_HANDLE gpu = viewHeap->GetGPUDescriptorHandleForHeapStart();
			gpu.ptr += slot * viewIncrement;

			D3D12_VIEWPORT const full{0.0f, 0.0f, constants.size[0], constants.size[1], 0.0f, 1.0f};
			D3D12_RECT const all{0, 0, camera.width, camera.height};
			ID3D12DescriptorHeap *const heaps[]{viewHeap.Get()};
			list->SetDescriptorHeaps(1, heaps);
			list->SetGraphicsRootSignature(rootSignature.Get());
			list->RSSetViewports(1, &full);
			list->RSSetScissorRects(1, &all);
			D3D12_CPU_DESCRIPTOR_HANDLE const target = rtvHeap->GetCPUDescriptorHandleForHeapStart();
			list->OMSetRenderTargets(1, &target, FALSE, nullptr);
			Barrier(depth, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			list->SetGraphicsRoot32BitConstants(0, sizeof(constants) / 4, &constants, 0);
			list->SetGraphicsRootDescriptorTable(1, gpu);
			list->SetPipelineState(reprojectPipeline.Get());
			list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			list->DrawInstanced(3, 1, 0, 0);
			Barrier(depth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);

			// Instances that moved since the previous frame carry their own motion.
			D3D12_CPU_DESCRIPTOR_HANDLE const depthView = driver.DepthView();
			bool bound = false;
			std::memcpy(constants.projection, camera.projection, sizeof(constants.projection));
			for (Instance const &instance: instances) {
				if (!instance.alive || std::memcmp(instance.world, instance.previousWorld, sizeof(instance.world)) == 0) {
					continue;
				}
				if (!bound) {
					list->OMSetRenderTargets(1, &target, FALSE, &depthView);
					list->SetPipelineState(instancePipeline.Get());
					bound = true;
				}
				++totals.movedInstances;
				if (instance.camera == cameraId) std::memcpy(constants.current, instance.modelView, sizeof(constants.current));
				else Multiply(camera.view, instance.world, constants.current);
				Multiply(previousPv, instance.previousWorld, constants.previous);
				list->SetGraphicsRoot32BitConstants(0, sizeof(constants) / 4, &constants, 0);
				for (MeshDraw const &mesh: instance.meshes) {
					list->IASetVertexBuffers(0, 1, &mesh.vertices);
					list->IASetIndexBuffer(&mesh.indices);
					list->DrawIndexedInstanced(mesh.count, 1, 0, 0, 0);
				}
			}

			if (keepMotion) {
				Barrier(motion.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
				D3D12_TEXTURE_COPY_LOCATION const to{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {footprint}};
				D3D12_TEXTURE_COPY_LOCATION from{motion.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
				from.SubresourceIndex = 0;
				list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
				Barrier(motion.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
				motionKept = true;
			}
		}

		// Writes the kept motion vectors as an image: the direction as the hue, the length as the brightness, up
		// to the longest vector in the frame. Prints the average.
		void SaveMotion(std::wstring const &path) {
			WaitFor(fenceValue);
			uint8_t *mapped = nullptr;
			D3D12_RANGE const range{0, footprint.Footprint.RowPitch * static_cast<size_t>(motionHeight)};
			if (FAILED(readback->Map(0, &range, reinterpret_cast<void **>(&mapped)))) return;
			auto const half = [](uint16_t h) {
				uint32_t const sign = (h & 0x8000u) << 16, exponent = (h >> 10) & 0x1F, mantissa = h & 0x3FF;
				uint32_t bits;
				if (exponent == 0) {
					float const value = std::ldexp(static_cast<float>(mantissa), -24);
					return sign ? -value : value;
				}
				if (exponent == 31) bits = sign | 0x7F800000u | mantissa << 13;
				else bits = sign | (exponent + 112) << 23 | mantissa << 13;
				float value;
				std::memcpy(&value, &bits, sizeof(value));
				return value;
			};
			size_t const pixels = static_cast<size_t>(motionWidth) * motionHeight;
			std::vector<float> vectors(pixels * 2);
			double sumX = 0.0, sumY = 0.0, longest = 0.0;
			for (int32_t y = 0; y < motionHeight; ++y) {
				auto const *row = reinterpret_cast<uint16_t const *>(mapped + footprint.Footprint.RowPitch * y);
				for (int32_t x = 0; x < motionWidth; ++x) {
					float const dx = half(row[x * 2]), dy = half(row[x * 2 + 1]);
					size_t const i = static_cast<size_t>(y) * motionWidth + x;
					vectors[i * 2] = dx;
					vectors[i * 2 + 1] = dy;
					sumX += dx;
					sumY += dy;
					longest = (std::max)(longest, std::sqrt(static_cast<double>(dx) * dx + static_cast<double>(dy) * dy));
				}
			}
			D3D12_RANGE const none{0, 0};
			readback->Unmap(0, &none);
			std::vector<uint8_t> rgba(pixels * 4);
			for (size_t i = 0; i < pixels; ++i) {
				double const dx = vectors[i * 2], dy = vectors[i * 2 + 1];
				double const value = longest > 0.0 ? std::sqrt(dx * dx + dy * dy) / longest : 0.0;
				double const hue = (std::atan2(dy, dx) / (2.0 * 3.14159265358979) + 1.0) * 6.0;
				double const f = hue - std::floor(hue);
				int const sector = static_cast<int>(std::floor(hue)) % 6;
				double const v = value, p = 0.0, q = v * (1.0 - f), t = v * f;
				double r = v, g = t, b = p;
				switch (sector) {
					case 0: r = v, g = t, b = p; break;
					case 1: r = q, g = v, b = p; break;
					case 2: r = p, g = v, b = t; break;
					case 3: r = p, g = q, b = v; break;
					case 4: r = t, g = p, b = v; break;
					default: r = v, g = p, b = q; break;
				}
				rgba[i * 4] = static_cast<uint8_t>(std::clamp(r, 0.0, 1.0) * 255.0);
				rgba[i * 4 + 1] = static_cast<uint8_t>(std::clamp(g, 0.0, 1.0) * 255.0);
				rgba[i * 4 + 2] = static_cast<uint8_t>(std::clamp(b, 0.0, 1.0) * 255.0);
				rgba[i * 4 + 3] = 255;
			}
			nSCDWire::SavePng(path.c_str(), static_cast<uint32_t>(motionWidth), static_cast<uint32_t>(motionHeight),
			                  rgba.data());
			// The vectors themselves next to the image, as x, y float pairs row by row.
			std::wstring raw = path;
			raw.replace(raw.size() - 4, 4, L".rg32f");
			std::FILE *file = nullptr;
			if (_wfopen_s(&file, raw.c_str(), L"wb") == 0 && file != nullptr) {
				std::fwrite(vectors.data(), sizeof(float), vectors.size(), file);
				std::fclose(file);
			}
			std::printf("retained: motion of frame %llu: average %.3f, %.3f pixels, longest %.3f\n",
			            static_cast<unsigned long long>(frame), sumX / static_cast<double>(pixels),
			            sumY / static_cast<double>(pixels), longest);
		}
	};

	RetainedScene::RetainedScene(cGDriver12 &driver, SceneLane &lane) : impl(std::make_unique<Impl>(driver, lane)) {
		lane.SetSink(this);
		if (!impl->gpuReady) std::puts("retained: no motion vectors or timing, their GPU objects failed");
	}

	RetainedScene::~RetainedScene() { impl->lane.SetSink(nullptr); }

	void RetainedScene::OwnerBegin(nSCDWire::DrawOwner const &owner) {
		Impl &d = *impl;
		if (owner.pass <= 2) {
			// A static redraw starts over at pass 0 (or 1 when it has no pass 0 views).
			if (static_cast<int32_t>(owner.pass) < d.lastPass || d.lastPass < 0 || d.lastPass > 2) {
				std::array<int32_t, 4> viewport;
				d.driver.GetViewport(viewport.data());
				d.redraws.push_back(viewport);
			}
			d.lastPass = owner.pass;
			return;
		}
		d.lastPass = owner.pass;
		if (!d.composited && !d.instances.empty()) d.Composite();
	}

	void RetainedScene::Occupant(nSCDWire::OccupantEvent const &event) {
		Impl &d = *impl;
		switch (event.kind) {
			case nSCDWire::OccupantEventKind::Insert:
				break;
			case nSCDWire::OccupantEventKind::Move:
			case nSCDWire::OccupantEventKind::Remove:
				d.Orphan(event.id);
				break;
		}
	}

	void RetainedScene::VideoMode() {
		Impl &d = *impl;
		for (uint32_t i = 0; i < d.instances.size(); ++i) d.Kill(i);
		d.Compact();
		d.camera = d.previousCamera = {};
	}

	void RetainedScene::KeepMotion() { impl->keepMotion = true; }

	void RetainedScene::SetJitter(float x, float y) {
		impl->jitter[0] = x;
		impl->jitter[1] = y;
	}

	bool RetainedScene::UpscalerCamera(UpscalerInputs &inputs) const {
		Impl const &d = *impl;
		if (d.cameraFrame != d.frame || !d.motion) return false;
		inputs.motion = d.motion.Get();
		inputs.jitter[0] = d.drawnJitter[0];
		inputs.jitter[1] = d.drawnJitter[1];
		std::memcpy(inputs.projection, d.camera.projection, sizeof(inputs.projection));
		std::memcpy(inputs.view, d.camera.view, sizeof(inputs.view));
		std::memcpy(inputs.previousProjection, d.motionFrom.projection, sizeof(inputs.previousProjection));
		std::memcpy(inputs.previousView, d.motionFrom.view, sizeof(inputs.previousView));
		inputs.reset = d.reset;
		return true;
	}

	void RetainedScene::EndFrame(std::wstring const *motionPath) {
		Impl &d = *impl;
		if (d.gpuReady) {
			d.driver.Queue()->Signal(d.fence.Get(), ++d.fenceValue);
			d.slotFence[d.frame % kSlots] = d.fenceValue;
		}
		if (d.motionKept && motionPath != nullptr) d.SaveMotion(*motionPath);
		d.keepMotion = d.motionKept = false;
		d.capturedThisFrame.clear();
		d.redraws.clear();
		d.capturing = UINT32_MAX;
		d.lastPass = -1;
		d.composited = false;
		++d.frame;
	}

	bool RetainedScene::TakeMesh(nSCDWire::DrawOwner const &owner, nSCDWire::ModelInstance const &model,
	                             float const *bounds, uint32_t mesh, D3D12_VERTEX_BUFFER_VIEW const &vertices,
	                             D3D12_INDEX_BUFFER_VIEW const &indices, uint32_t count) {
		Impl &d = *impl;
		if (owner.pass != 1 || owner.occupant == 0) return false;
		// A new bracket, or the same drawable drawn again, starts the instance's meshes over.
		if (d.capturing == UINT32_MAX || owner.drawable != d.capturingDrawable || mesh <= d.lastMesh) {
			Key const key{owner.occupant, model.group, model.instance};
			auto const found = d.byKey.find(key);
			bool const fresh = found == d.byKey.end();
			uint32_t index;
			if (!fresh) {
				index = found->second;
			} else {
				index = static_cast<uint32_t>(d.instances.size());
				d.instances.emplace_back().key = key;
				d.byKey.emplace(key, index);
				d.byOccupant[key.occupant].push_back(index);
			}
			Instance &instance = d.instances[index];
			WorldMatrix(model, instance.world);
			if (fresh) std::memcpy(instance.previousWorld, instance.world, sizeof(instance.world));
			Box &box = instance.box;
			for (int axis = 0; axis < 3; ++axis) box.min[axis] = 1e30f, box.max[axis] = -1e30f;
			for (int corner = 0; corner < 8; ++corner) {
				float const x = bounds[(corner & 1) ? 3 : 0], y = bounds[(corner & 2) ? 4 : 1], z = bounds[(corner & 4) ? 5 : 2];
				float const *const w = instance.world;
				for (int axis = 0; axis < 3; ++axis) {
					float const value = w[axis] * x + w[4 + axis] * y + w[8 + axis] * z + w[12 + axis];
					box.min[axis] = (std::min)(box.min[axis], value);
					box.max[axis] = (std::max)(box.max[axis], value);
				}
			}
			cGDriver12::Transform transform{};
			d.driver.SaveTransform(transform);
			std::memcpy(instance.modelView, transform.modelView, sizeof(instance.modelView));
			instance.frame = d.frame;
			instance.camera = 0;
			instance.orphan = false;
			instance.viewPositions = false;
			instance.meshes.clear();
			d.capturedThisFrame.push_back(index);
			d.capturing = index;
			d.capturingDrawable = owner.drawable;
			++d.totals.captured;
		}
		d.lastMesh = mesh;
		MeshDraw &draw = d.instances[d.capturing].meshes.emplace_back();
		draw.vertices = vertices;
		draw.indices = indices;
		draw.count = count;
		d.driver.SaveDrawState(draw.state);
		draw.state.vertexPointer = nullptr;
		if (draw.state.stages[1].enabled && draw.state.stages[1].coordinates == kViewPositions) {
			d.instances[d.capturing].viewPositions = true;
		}
		draw.polygonOffset = d.driver.GetPolygonOffset();
		draw.fog = d.driver.IsEnabled(kGDCapability_Fog);
		draw.stencil = d.driver.IsEnabled(kGDCapability_StencilTest);
		++d.totals.meshesCaptured;
		return true;
	}

	void RetainedScene::PrintTotals() const {
		Impl const &d = *impl;
		Totals const &t = d.totals;
		auto const u = [](uint64_t value) { return static_cast<unsigned long long>(value); };
		uint64_t alive = 0;
		for (Instance const &instance: d.instances) alive += instance.alive;
		std::printf("retained: %llu instances kept, %llu captures (%llu meshes), removed: %llu after their occupant "
		            "went, %llu by a new view, %llu redrawn without them\n",
		            u(alive), u(t.captured), u(t.meshesCaptured), u(t.removedOccupant), u(t.removedView),
		            u(t.removedRedrawn));
		std::printf("retained: %llu frames composited (%llu skipped), %.1f instances and %.1f meshes drawn per frame, "
		            "%.1f culled, %llu moved instance draws\n",
		            u(t.composites), u(t.skipped),
		            t.composites ? static_cast<double>(t.instancesDrawn) / t.composites : 0.0,
		            t.composites ? static_cast<double>(t.meshesDrawn) / t.composites : 0.0,
		            t.composites ? static_cast<double>(t.culled) / t.composites : 0.0, u(t.movedInstances));
		std::printf("retained: largest difference between SC4's modelview and view x world: %.3g, %llu drawn with "
		            "another camera\n", t.worstModelViewError, u(t.otherCamera));
		auto const report = [](char const *indent, char const *name, std::vector<float> values) {
			if (values.empty()) return;
			std::sort(values.begin(), values.end());
			double total = 0.0;
			for (float value: values) total += value;
			std::printf("retained: %s%s per composite: %.3f ms average, median %.3f, 95th percentile %.3f, slowest %.3f\n",
			            indent, name, total / static_cast<double>(values.size()), values[values.size() / 2],
			            values[values.size() * 95 / 100], values.back());
		};
		report("", "CPU", t.cpuMilliseconds);
		report("", "GPU", t.gpuMilliseconds);
		for (auto const &[millimetres, z]: d.zooms) {
			std::printf("retained:   at %.4f metres per pixel: %llu frames, %.0f instances and %.0f meshes drawn per frame\n",
			            millimetres / 1000.0, u(z.frames), static_cast<double>(z.instances) / z.frames,
			            static_cast<double>(z.meshes) / z.frames);
			report("    ", "CPU", z.cpuMilliseconds);
			report("    ", "GPU", z.gpuMilliseconds);
		}
	}
}
