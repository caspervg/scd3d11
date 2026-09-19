/*
 *  SCD3D11 - a free graphics driver for SimCity 4's SimGL interface
 *  Copyright (C) 2025  Nelson Gomez (nsgomez) <nelson@ngomez.me>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

#include "cGDriver.h"
#include "Diagnostics.h"
#include "VertexFormatUtils.h"

#include <cstddef>
#include <cstring>
#include <d3dcompiler.h>
#include <limits>
#include <new>
#include <cmath>

namespace nSCD3D11 {
	// 8 segments each: at most 128 MB vertex + 32 MB index cache (was 512 + 128 MB).
	constexpr uint32_t VERTEX_CACHE_SEGMENT_BYTES = 16u * 1024u * 1024u;
	constexpr uint32_t INDEX_CACHE_SEGMENT_BYTES = 4u * 1024u * 1024u;
	constexpr uint64_t LOW_ADDRESS_SPACE_BYTES = 512ull * 1024u * 1024u;
	// Bounds cache bookkeeping (roughly 100 bytes per entry) when uploads are tiny.
	constexpr size_t MAX_CACHE_ENTRIES_PER_SEGMENT = 32768;

	namespace {
		char const kShaderSource[] = R"(
#ifndef SCD3D11_INTERPOLATION
#define SCD3D11_INTERPOLATION
#endif
struct StageState
{
	uint4 modes;
	uint4 parameters0;
	uint4 parameters1;
};

cbuffer DriverConstants : register(b0)
{
	column_major float4x4 modelView;
	column_major float4x4 projection;
	column_major float4x4 normalMatrix;
	uint flags;
	uint alphaFunction;
	float alphaReference;
	uint enabledLights;
	float4 globalAmbient;
	float4 lightAmbient[8];
	float4 lightDiffuse[8];
	float4 lightSpecular[8];
	float4 lightPosition[8];
	float4 materialAmbient;
	float4 materialDiffuse;
	float4 materialSpecular;
	float4 materialEmission;
	float4 materialParameters;
	column_major float4x4 textureMatrix0;
	column_major float4x4 textureMatrix1;
	StageState stage0;
	StageState stage1;
	float4 fogColor;
	float4 fogParameters;
	float4 textureFactor;
};

Texture2D texture0 : register(t0);
Texture2D texture1 : register(t1);
SamplerState sampler0 : register(s0);
SamplerState sampler1 : register(s1);

struct VSInput
{
	float3 position : POSITION;
	float3 normal : NORMAL;
	float4 color : COLOR0;
	float2 texCoord0 : TEXCOORD0;
	float2 texCoord1 : TEXCOORD1;
};

struct PSInput
{
	float4 position : SV_POSITION;
	SCD3D11_INTERPOLATION float3 normal : NORMAL;
	SCD3D11_INTERPOLATION float4 color : COLOR0;
	SCD3D11_INTERPOLATION float3 viewPosition : TEXCOORD3;
	float2 texCoord0 : TEXCOORD0;
	float2 texCoord1 : TEXCOORD1;
	float fogDistance : TEXCOORD2;
};

PSInput VSMain(VSInput input)
{
	PSInput output;
	float4 viewPosition = mul(modelView, float4(input.position, 1.0f));
	output.position = mul(projection, viewPosition);
	output.position.z = (output.position.z + output.position.w) * 0.5f;
	output.normal = mul((float3x3)normalMatrix, input.normal);
	output.color = input.color;
	output.viewPosition = viewPosition.xyz;
	float4 source0 = (stage0.parameters1.w & 0xfffffff8) == 0x10
		? viewPosition
		: float4(stage0.parameters1.w == 1 ? input.texCoord1 : input.texCoord0, 0.0f, 1.0f);
	float4 source1 = (stage1.parameters1.w & 0xfffffff8) == 0x10
		? viewPosition
		: float4(stage1.parameters1.w == 1 ? input.texCoord1 : input.texCoord0, 0.0f, 1.0f);
	output.texCoord0 = mul(textureMatrix0, source0).xy;
	output.texCoord1 = mul(textureMatrix1, source1).xy;
	output.fogDistance = abs(viewPosition.z);
	return output;
}

float4 CombinerArgument(uint packed, float4 textureColor, float4 previous, float4 constantColor, float4 primary)
{
	uint source = packed & 15;
	uint operand = packed >> 4;
	float4 value = source == 0 ? textureColor : (source == 1 ? previous : (source == 2 ? constantColor : primary));
	return operand == 0 ? value : (operand == 1 ? 1.0f - value :
		(operand == 2 ? value.aaaa : 1.0f - value.aaaa));
}

float3 CombineRGB(StageState state, float4 textureColor, float4 previous, float4 primary)
{
	float4 a = CombinerArgument(state.parameters0.y, textureColor, previous, textureFactor, primary);
	float4 b = CombinerArgument(state.parameters0.z, textureColor, previous, textureFactor, primary);
	float4 c = CombinerArgument(state.parameters0.w, textureColor, previous, textureFactor, primary);
	uint mode = state.modes.y;
	float3 result = mode == 0 ? a.rgb :
		(mode == 1 ? a.rgb * b.rgb :
		(mode == 2 ? a.rgb + b.rgb :
		(mode == 3 ? a.rgb + b.rgb - 0.5f :
		(mode == 4 ? a.rgb * c.rgb + b.rgb * (1.0f - c.rgb) :
		4.0f * dot(a.rgb - 0.5f, b.rgb - 0.5f)))));
	return saturate(result * (1u << state.modes.w));
}

float CombineAlpha(StageState state, float4 textureColor, float4 previous, float4 primary)
{
	float a = CombinerArgument(state.parameters1.x, textureColor, previous, textureFactor, primary).a;
	float b = CombinerArgument(state.parameters1.y, textureColor, previous, textureFactor, primary).a;
	float c = CombinerArgument(state.parameters1.z, textureColor, previous, textureFactor, primary).a;
	uint mode = state.modes.z;
	float result = mode == 0 ? a : (mode == 1 ? a * b :
		(mode == 2 ? a + b : (mode == 3 ? a + b - 0.5f : a * c + b * (1.0f - c))));
	return saturate(result * (1u << state.parameters0.x));
}

float4 ApplyStage(StageState state, float4 textureColor, float4 previous, float4 primary)
{
	uint mode = state.modes.x;
	float4 result = textureColor;
	if (mode == 1) result = previous * textureColor;
	else if (mode == 2) result = float4(lerp(previous.rgb, textureColor.rgb, textureColor.a), previous.a);
	else if (mode == 3) result = float4(lerp(previous.rgb, textureFactor.rgb, textureColor.rgb), previous.a * textureColor.a);
	else if (mode != 0) result = float4(CombineRGB(state, textureColor, previous, primary),
		CombineAlpha(state, textureColor, previous, primary));
	return result;
}

float4 PSMain(PSInput input) : SV_TARGET
{
	float4 primary = input.color;
	if ((flags & 4) != 0)
	{
		// Direct3D 7 fixed-function lighting as SimGLDX7 set it up: specular disabled, a vertex without
		// a normal gets no diffuse term, and the lit color clamps before texturing.
		float4 ambientMaterial = (flags & 32) != 0 ? input.color : materialAmbient;
		float4 diffuseMaterial = (flags & 64) != 0 ? input.color : materialDiffuse;
		float3 normal = (flags & 128) != 0 ? normalize(input.normal) : 0.0f;
		float3 lit = materialEmission.rgb + globalAmbient.rgb * ambientMaterial.rgb;
		[loop] for (uint light = 0; light < 8; ++light)
		{
			if ((enabledLights & (1u << light)) == 0) continue;
			float3 vectorToLight = lightPosition[light].w == 0.0f
				? normalize(lightPosition[light].xyz)
				: normalize(lightPosition[light].xyz / lightPosition[light].w - input.viewPosition);
			lit += lightAmbient[light].rgb * ambientMaterial.rgb +
				lightDiffuse[light].rgb * diffuseMaterial.rgb * saturate(dot(normal, vectorToLight));
		}
		primary = float4(saturate(lit), diffuseMaterial.a);
	}
	float4 color = primary;
	if ((flags & 1) != 0) color = ApplyStage(stage0, texture0.Sample(sampler0, input.texCoord0), color, primary);
	if ((flags & 2) != 0) color = ApplyStage(stage1, texture1.Sample(sampler1, input.texCoord1), color, primary);
	if ((flags & 8) != 0)
	{
		bool alphaPass = alphaFunction == 0 ? false :
			alphaFunction == 1 ? color.a < alphaReference :
			alphaFunction == 2 ? color.a == alphaReference :
			alphaFunction == 3 ? color.a <= alphaReference :
			alphaFunction == 4 ? color.a > alphaReference :
			alphaFunction == 5 ? color.a != alphaReference :
			alphaFunction == 6 ? color.a >= alphaReference : true;
		clip(alphaPass ? 1.0f : -1.0f);
	}
	if ((flags & 16) != 0)
	{
		float distance = input.fogDistance;
		float mode = fogParameters.w;
		float factor = mode < 0.5f ? exp(-fogParameters.x * distance) :
			(mode < 1.5f ? exp(-fogParameters.x * fogParameters.x * distance * distance) :
			saturate((fogParameters.z - distance) / max(fogParameters.z - fogParameters.y, 0.000001f)));
		color.rgb = lerp(fogColor.rgb, color.rgb, saturate(factor));
	}
	return color;
}
)";

		struct DriverConstants {
			float modelView[16];
			float projection[16];
			float normalMatrix[16];
			uint32_t flags;
			uint32_t alphaFunction;
			float alphaReference;
			uint32_t enabledLights;
			float globalAmbient[4];
			float lightAmbient[8][4];
			float lightDiffuse[8][4];
			float lightSpecular[8][4];
			float lightPosition[8][4];
			float materialAmbient[4];
			float materialDiffuse[4];
			float materialSpecular[4];
			float materialEmission[4];
			float materialParameters[4];
			float textureMatrices[2][16];

			struct StageConstants {
				uint32_t modes[4];
				uint32_t parameters0[4];
				uint32_t parameters1[4];
			} stages[2];

			float fogColor[4];
			float fogParameters[4];
			float textureFactor[4];
		};

		void MakeNormalMatrix(float const *m, float *out) {
			float const a00=m[0], a01=m[4], a02=m[8], a10=m[1], a11=m[5], a12=m[9];
			float const a20=m[2], a21=m[6], a22=m[10];
			float const c00=a11*a22-a12*a21, c01=a12*a20-a10*a22, c02=a10*a21-a11*a20;
			float const c10=a02*a21-a01*a22, c11=a00*a22-a02*a20, c12=a01*a20-a00*a21;
			float const c20=a01*a12-a02*a11, c21=a02*a10-a00*a12, c22=a00*a11-a01*a10;
			float const determinant = a00*c00 + a01*c01 + a02*c02;
			memset(out, 0, sizeof(float) * 16);
			if (std::fabs(determinant) < 1.0e-12f) {
				out[0]=out[5]=out[10]=out[15]=1.0f;
				return;
			}
			float const scale=1.0f/determinant;
			out[0]=c00*scale; out[4]=c01*scale; out[8]=c02*scale;
			out[1]=c10*scale; out[5]=c11*scale; out[9]=c12*scale;
			out[2]=c20*scale; out[6]=c21*scale; out[10]=c22*scale; out[15]=1.0f;
		}
	}

	HRESULT cGDriver::CreateGeometryPipeline() {
		if (!d3dDevice) {
			return E_POINTER;
		}

		UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifndef NDEBUG
		compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
		Microsoft::WRL::ComPtr<ID3DBlob> vertexBytecode;
		Microsoft::WRL::ComPtr<ID3DBlob> pixelBytecode;
		Microsoft::WRL::ComPtr<ID3DBlob> flatPixelBytecode;
		Microsoft::WRL::ComPtr<ID3DBlob> messages;

		HRESULT result = D3DCompile(
			kShaderSource, sizeof(kShaderSource) - 1, "SC4D3D11", nullptr, nullptr,
			"VSMain", "vs_4_0", compileFlags, 0, &vertexBytecode, &messages);
		if (messages) {
			Log(LogCategory::Resource, "vertex shader compiler: %s",
			    static_cast<char const *>(messages->GetBufferPointer()));
			messages.Reset();
		}
		if (FAILED(result)) {
			LogHRESULT(LogCategory::Resource, "D3DCompile(VSMain)", result);
			return result;
		}
		D3D_SHADER_MACRO const flatMacros[] = {{"SCD3D11_INTERPOLATION", "nointerpolation"}, {nullptr, nullptr}};
		result = D3DCompile(
			kShaderSource, sizeof(kShaderSource) - 1, "SC4D3D11", flatMacros, nullptr,
			"PSMain", "ps_4_0", compileFlags, 0, &flatPixelBytecode, &messages);
		if (messages) messages.Reset();
		if (FAILED(result)) {
			LogHRESULT(LogCategory::Resource, "D3DCompile(PSMain flat)", result);
			return result;
		}

		result = D3DCompile(
			kShaderSource, sizeof(kShaderSource) - 1, "SC4D3D11", nullptr, nullptr,
			"PSMain", "ps_4_0", compileFlags, 0, &pixelBytecode, &messages);
		if (messages) {
			Log(LogCategory::Resource, "pixel shader compiler: %s",
			    static_cast<char const *>(messages->GetBufferPointer()));
			messages.Reset();
		}
		if (FAILED(result)) {
			LogHRESULT(LogCategory::Resource, "D3DCompile(PSMain)", result);
			return result;
		}

		result = d3dDevice->CreateVertexShader(
			vertexBytecode->GetBufferPointer(), vertexBytecode->GetBufferSize(), nullptr, &vertexShader);
		if (FAILED(result)) {
			LogHRESULT(LogCategory::Resource, "ID3D11Device::CreateVertexShader", result);
			return result;
		}
		result = d3dDevice->CreatePixelShader(
			pixelBytecode->GetBufferPointer(), pixelBytecode->GetBufferSize(), nullptr, &pixelShader);
		if (FAILED(result)) {
			LogHRESULT(LogCategory::Resource, "ID3D11Device::CreatePixelShader", result);
			return result;
		}
		result = d3dDevice->CreatePixelShader(
			flatPixelBytecode->GetBufferPointer(), flatPixelBytecode->GetBufferSize(), nullptr, &flatPixelShader);
		if (FAILED(result)) return result;

		D3D11_INPUT_ELEMENT_DESC const elements[] = {
			{
				"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(D3D11Vertex, position),
				D3D11_INPUT_PER_VERTEX_DATA, 0
			},
			{
				"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(D3D11Vertex, normal), D3D11_INPUT_PER_VERTEX_DATA,
				0
			},
			{"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, offsetof(D3D11Vertex, color), D3D11_INPUT_PER_VERTEX_DATA, 0},
			{
				"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(D3D11Vertex, texCoord[0]),
				D3D11_INPUT_PER_VERTEX_DATA, 0
			},
			{
				"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(D3D11Vertex, texCoord[1]),
				D3D11_INPUT_PER_VERTEX_DATA, 0
			}
		};
		result = d3dDevice->CreateInputLayout(
			elements, static_cast<UINT>(sizeof(elements) / sizeof(elements[0])),
			vertexBytecode->GetBufferPointer(), vertexBytecode->GetBufferSize(), &inputLayout);
		if (FAILED(result)) {
			LogHRESULT(LogCategory::Resource, "ID3D11Device::CreateInputLayout", result);
			return result;
		}

		D3D11_BUFFER_DESC constantDescription{};
		constantDescription.ByteWidth = sizeof(DriverConstants);
		constantDescription.Usage = D3D11_USAGE_DYNAMIC;
		constantDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		constantDescription.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		for (auto &buffer: transformBuffers) {
			result = d3dDevice->CreateBuffer(&constantDescription, nullptr, &buffer);
			if (FAILED(result)) {
				LogHRESULT(LogCategory::Resource, "ID3D11Device::CreateBuffer(constants)", result);
				return result;
			}
		}

		// Bound to empty texture stages so the debug layer never sees a NULL sampler.
		D3D11_SAMPLER_DESC samplerDescription{};
		samplerDescription.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		samplerDescription.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
		samplerDescription.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
		samplerDescription.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
		samplerDescription.ComparisonFunc = D3D11_COMPARISON_NEVER;
		samplerDescription.MaxLOD = FLT_MAX;
		result = d3dDevice->CreateSamplerState(&samplerDescription, &defaultSampler);
		if (FAILED(result)) {
			LogHRESULT(LogCategory::Resource, "ID3D11Device::CreateSamplerState(default)", result);
			return result;
		}

		Log(LogCategory::Resource, "generic geometry pipeline created");
		return S_OK;
	}

	bool cGDriver::UseCachedBuffer(
		GeometryCacheSegment *segments,
		GeometryCache &cache,
		GeometryCacheKey const &key,
		Microsoft::WRL::ComPtr<ID3D11Buffer> &buffer,
		uint32_t &offset,
		uint32_t bindFlags) {
		auto const cached = cache.find(key);
		if (cached == cache.end() || cached->second.segment >= GEOMETRY_CACHE_SEGMENTS ||
		    !segments[cached->second.segment].buffer) {
			return false;
		}
		if (bindFlags == D3D11_BIND_VERTEX_BUFFER) ++vertexBufferCacheHits;
		else ++indexBufferCacheHits;
		buffer = segments[cached->second.segment].buffer;
		offset = cached->second.offset;
		return true;
	}

	bool cGDriver::UploadCachedBuffer(
		GeometryCacheSegment *segments,
		uint8_t &activeSegment,
		GeometryCache &cache,
		GeometryCacheKey const &key,
		uint32_t requiredSize,
		uint32_t bindFlags,
		void const *data,
		Microsoft::WRL::ComPtr<ID3D11Buffer> &buffer,
		uint32_t &offset) {
		if (!d3dDevice || !d3dContext || deviceLost || data == nullptr || requiredSize == 0) {
			return false;
		}

		if (UseCachedBuffer(segments, cache, key, buffer, offset, bindFlags)) return true;
		if (bindFlags == D3D11_BIND_VERTEX_BUFFER) ++vertexBufferCacheMisses;
		else ++indexBufferCacheMisses;

		// Segment count, actual capacities and entries per segment are all bounded, so an oversized upload
		// evicts older segments rather than growing the cache past its budget.
		uint32_t const normalCapacity = bindFlags == D3D11_BIND_VERTEX_BUFFER
			                                ? VERTEX_CACHE_SEGMENT_BYTES
			                                : INDEX_CACHE_SEGMENT_BYTES;
		uint64_t const budget = static_cast<uint64_t>(normalCapacity) * GEOMETRY_CACHE_SEGMENTS;
		if (requiredSize > budget) {
			Log(LogCategory::Unsupported, "%u byte upload exceeds the geometry cache budget", requiredSize);
			return false;
		}

		GeometryCacheSegment *segment = &segments[activeSegment];
		if (!segment->buffer || static_cast<uint64_t>(segment->cursor) + requiredSize > segment->capacity ||
		    segment->keys.size() >= MAX_CACHE_ENTRIES_PER_SEGMENT) {
			if (segment->buffer && segment->cursor != 0) {
				activeSegment = static_cast<uint8_t>((activeSegment + 1) % GEOMETRY_CACHE_SEGMENTS);
				// Low 32-bit address space: recycle the segments we already have instead of growing.
				MEMORYSTATUSEX memory{sizeof(memory)};
				if (!segments[activeSegment].buffer && GlobalMemoryStatusEx(&memory) &&
				    memory.ullAvailVirtual < LOW_ADDRESS_SPACE_BYTES) {
					activeSegment = 0;
				}
				segment = &segments[activeSegment];
				LogTrace(LogCategory::Resource, "%s cache advanced to segment %u",
				    bindFlags == D3D11_BIND_VERTEX_BUFFER ? "vertex" : "index", activeSegment);
			}

			// An oversized segment shrinks back once it is reused for ordinary uploads.
			ClearCacheSegment(segments, cache, activeSegment,
			                  segment->capacity > normalCapacity && requiredSize <= normalCapacity);

			if (!segment->buffer || segment->capacity < requiredSize) {
				uint32_t newCapacity = (std::max)(normalCapacity, requiredSize);
				uint64_t others = 0;
				for (size_t index = 0; index < GEOMETRY_CACHE_SEGMENTS; ++index) others += segments[index].capacity;
				others -= segment->capacity;
				// Oldest first: the segment after the active one is the next to be recycled anyway.
				for (uint8_t step = 1; others + newCapacity > budget && step < GEOMETRY_CACHE_SEGMENTS; ++step) {
					uint8_t const victim = static_cast<uint8_t>((activeSegment + step) % GEOMETRY_CACHE_SEGMENTS);
					others -= segments[victim].capacity;
					ClearCacheSegment(segments, cache, victim, true);
				}
				segment->buffer.Reset();
				segment->capacity = 0;

				// DYNAMIC buffers cost their full size in 32-bit address space (measured 1:1 on NVIDIA),
				// hence the modest segment sizes. DEFAULT + UpdateSubresource avoids that but made each
				// cache miss 3-5x slower, which shows up as stutter while panning large cities.
				auto const allocate = [&](uint32_t capacity) {
					D3D11_BUFFER_DESC description{};
					description.ByteWidth = capacity;
					description.Usage = D3D11_USAGE_DYNAMIC;
					description.BindFlags = bindFlags;
					description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
					HRESULT result = TakeInjectedFault(FAULT_ALLOCATE);
					if (SUCCEEDED(result)) result = d3dDevice->CreateBuffer(&description, nullptr, &segment->buffer);
					return result;
				};
				HRESULT result = allocate(newCapacity);
				if (FAILED(result) && !NoteDeviceLoss(result)) {
					// Out of memory: drop every other cached segment and retry once, sized for this upload only.
					LogHRESULT(LogCategory::Resource, "ID3D11Device::CreateBuffer(dynamic segment); evicting and retrying",
					           result);
					buffer.Reset();
					for (uint8_t index = 0; index < GEOMETRY_CACHE_SEGMENTS; ++index) {
						if (index != activeSegment) ClearCacheSegment(segments, cache, index, true);
					}
					newCapacity = (requiredSize + 3u) & ~3u;
					result = allocate(newCapacity);
				}
				if (FAILED(result)) {
					LogHRESULT(LogCategory::Resource, "ID3D11Device::CreateBuffer(dynamic segment)", result);
					NoteDeviceLoss(result);
					segment->buffer.Reset();
					return false;
				}
				segment->capacity = newCapacity;
				Log(LogCategory::Resource, "%s cache segment %u allocated at %u bytes",
				    bindFlags == D3D11_BIND_VERTEX_BUFFER ? "vertex" : "index", activeSegment, newCapacity);
			}
		}

		D3D11_MAPPED_SUBRESOURCE mapping{};
		HRESULT result = TakeInjectedFault(FAULT_MAP);
		if (SUCCEEDED(result)) result = d3dContext->Map(
			segment->buffer.Get(), 0,
			segment->cursor == 0 ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE, 0, &mapping);
		if (FAILED(result)) {
			LogHRESULT(LogCategory::Resource, "ID3D11DeviceContext::Map", result);
			NoteDeviceLoss(result);
			return false;
		}
		offset = segment->cursor;
		memcpy(static_cast<uint8_t *>(mapping.pData) + offset, data, requiredSize);
		d3dContext->Unmap(segment->buffer.Get(), 0);
		segment->cursor = (segment->cursor + requiredSize + 3u) & ~3u;
		buffer = segment->buffer;
		// Remembering the upload is optional: if the bookkeeping cannot allocate, it still draws.
		bool keyed = false;
		try {
			segment->keys.push_back(key);
			keyed = true;
			cache.insert_or_assign(key, GeometryCacheEntry{activeSegment, offset});
		} catch (std::bad_alloc const &) {
			if (keyed) segment->keys.pop_back();
		}
		return true;
	}

	void cGDriver::ClearCacheSegment(
		GeometryCacheSegment *segments, GeometryCache &cache, uint8_t index, bool release) {
		GeometryCacheSegment &segment = segments[index];
		for (GeometryCacheKey const &oldKey: segment.keys) {
			auto const old = cache.find(oldKey);
			if (old != cache.end() && old->second.segment == index) cache.erase(old);
		}
		segment.cursor = 0;
		if (release) {
			std::vector<GeometryCacheKey>().swap(segment.keys);
			segment.buffer.Reset();
			segment.capacity = 0;
		} else {
			segment.keys.clear();
		}
	}

	bool cGDriver::UploadVertices(uint32_t first, uint32_t count) {
		if (interleavedPointer == nullptr || interleavedStride == 0 || count == 0) {
			Log(LogCategory::Unsupported, "draw without valid interleaved vertex data");
			return false;
		}

		// Validate the whole strided source span, first vertex through the last one's consumed bytes,
		// before anything reads it.
		uint64_t const byteSize = static_cast<uint64_t>(count) * sizeof(D3D11Vertex);
		if (byteSize > (std::numeric_limits<uint32_t>::max)() ||
		    !SourceSpanFits(interleavedPointer, static_cast<uint64_t>(first) + count, interleavedStride,
		                    RZVertexFormatStride(interleavedFormat))) {
			Log(LogCategory::Unsupported, "vertex upload exceeds 32-bit limits");
			return false;
		}

		uint8_t const *source = interleavedPointer + static_cast<size_t>(first) * interleavedStride;
		// InterleavedArrays hands over a bare pointer into game memory with no allocation, modification
		// or release boundary, so pointer and size cannot identify contents; only the terrain vertex
		// buffer, which the driver owns, can use generation keys instead of hashing.
		GeometryCacheKey const key = VertexCacheKey(interleavedFormat, interleavedStride, source, count);
		if (UseCachedBuffer(vertexBufferSegments, vertexBufferCache, key,
		                    dynamicVertexBuffer, dynamicVertexBufferOffset, D3D11_BIND_VERTEX_BUFFER)) return true;

		try {
			if (!ConvertVertices(interleavedFormat, interleavedStride, source, count, vertexScratch)) {
				Log(LogCategory::Unsupported, "unsupported vertex format 0x%08X stride %u", interleavedFormat,
				    interleavedStride);
				return false;
			}
		} catch (std::bad_alloc const &) {
			Log(LogCategory::Resource, "out of memory converting %u vertices", count);
			return false;
		}
		return UploadCachedBuffer(
			vertexBufferSegments, activeVertexBufferSegment, vertexBufferCache,
			key, static_cast<uint32_t>(byteSize), D3D11_BIND_VERTEX_BUFFER, vertexScratch.data(),
			dynamicVertexBuffer,
			dynamicVertexBufferOffset);
	}

	bool cGDriver::UploadIndices(std::vector<uint32_t> const &indices) {
		uint64_t const byteSize = static_cast<uint64_t>(indices.size()) * sizeof(uint32_t);
		if (indices.empty() || byteSize > (std::numeric_limits<uint32_t>::max)()) {
			return false;
		}
		return UploadCachedBuffer(
			indexBufferSegments, activeIndexBufferSegment, indexBufferCache,
			IndexCacheKey(DXGI_FORMAT_R32_UINT, indices.data(), static_cast<uint32_t>(indices.size())),
			static_cast<uint32_t>(byteSize), D3D11_BIND_INDEX_BUFFER, indices.data(),
			dynamicIndexBuffer,
			dynamicIndexBufferOffset);
	}

	bool cGDriver::BindGeometryPipeline(uint32_t primitive) {
		D3D11_PRIMITIVE_TOPOLOGY const topology = D3D11Topology(primitive);
		if (!IsDeviceReady() || deviceLost || !vertexShader || !pixelShader || !flatPixelShader || !inputLayout ||
		    !transformBuffers[activeTransformBuffer] || !dynamicVertexBuffer ||
		    topology == D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED) {
			return false;
		}

		ID3D11ShaderResourceView *textureViews[2]{};
		ID3D11SamplerState *samplers[2]{defaultSampler.Get(), defaultSampler.Get()};
		uint32_t flagInputs = 0;
		for (uint32_t stage = 0; stage < 2; ++stage) {
			if (!textureStageEnabled[stage]) continue;
			auto iterator = textures.find(boundTextures[stage]);
			if (iterator == textures.end() || !iterator->second.view || FAILED(EnsureSampler(textureStages[stage])))
				continue;
			textureViews[stage] = iterator->second.view.Get();
			samplers[stage] = textureStages[stage].sampler.Get();
			flagInputs |= 1u << stage;
		}
		bool const lit = lightingEnabled;
		if (lit) flagInputs |= 4;
		if (lit && RZVertexFormatNumElements(interleavedFormat, kGDElementType_Normal) != 0) flagInputs |= 128;

		// Setters mark the constants dirty; the only other inputs are bound textures and whether the vertex
		// format carries normals, which can change without a setter and are compared here instead.
		if (constantsDirty || flagInputs != constantsFlagInputs || constantBufferCache.size() != sizeof(DriverConstants)) {
			DriverConstants constants{};
			constants.flags = flagInputs;
			memcpy(constants.modelView, matrices[0], sizeof(constants.modelView));
			memcpy(constants.projection, matrices[1], sizeof(constants.projection));
			if (normalMatrixDirty) {
				MakeNormalMatrix(matrices[MODEL_VIEW], normalMatrix);
				normalMatrixDirty = false;
			}
			memcpy(constants.normalMatrix, normalMatrix, sizeof(constants.normalMatrix));
			constants.alphaFunction = alphaFunction;
			constants.alphaReference = alphaReference;
			if (enabledCapabilities[kGDCapability_AlphaTest]) constants.flags |= 8;
			memcpy(constants.globalAmbient, globalAmbient, sizeof(constants.globalAmbient));
			memcpy(constants.lightAmbient, lightAmbient, sizeof(constants.lightAmbient));
			memcpy(constants.lightDiffuse, lightDiffuse, sizeof(constants.lightDiffuse));
			memcpy(constants.lightSpecular, lightSpecular, sizeof(constants.lightSpecular));
			memcpy(constants.lightPosition, lightPosition, sizeof(constants.lightPosition));
			// Snapshot the projection for the ReShade shadow uniforms. Not gated on lighting: SC4 bakes
			// its lighting into vertex colours and leaves fixed-function lighting off for the city view,
			// so waiting for a lit draw would never capture anything. RenderSceneEffects runs at the end
			// of cSC43DRender::Draw, before any UI, so the last projection seen here is the scene's.
			CaptureShadowUniforms();
			memcpy(constants.materialAmbient, materialAmbient, sizeof(constants.materialAmbient));
			memcpy(constants.materialDiffuse, materialDiffuse, sizeof(constants.materialDiffuse));
			memcpy(constants.materialSpecular, materialSpecular, sizeof(constants.materialSpecular));
			memcpy(constants.materialEmission, materialEmission, sizeof(constants.materialEmission));
			constants.materialParameters[0] = materialShininess;
			for (uint32_t stageIndex = 0; stageIndex < 2; ++stageIndex) {
				TextureStageState const &source = textureStages[stageIndex];
				DriverConstants::StageConstants &destination = constants.stages[stageIndex];
				memcpy(constants.textureMatrices[stageIndex], source.matrix, sizeof(source.matrix));
				destination.modes[0] = source.environmentMode;
				destination.modes[1] = source.rgbMode;
				destination.modes[2] = source.alphaMode;
				destination.modes[3] = source.rgbScale;
				destination.parameters0[0] = source.alphaScale;
				for (uint32_t parameter = 0; parameter < 3; ++parameter) {
					destination.parameters0[parameter + 1] = source.rgbParameters[parameter];
					destination.parameters1[parameter] = source.alphaParameters[parameter];
				}
				destination.parameters1[3] = source.coordinateSource;
			}
			if (lit) {
				if (ambientVertexColors) constants.flags |= 32;
				if (diffuseFromVertex) constants.flags |= 64;
				for (uint32_t light = 0; light < 8; ++light)
					if (lightsEnabled[light]) constants.enabledLights |= 1u << light;
			}
			if (enabledCapabilities[kGDCapability_Fog]) constants.flags |= 16;
			memcpy(constants.fogColor, fogColor, sizeof(constants.fogColor));
			constants.fogParameters[0] = fogDensity;
			constants.fogParameters[1] = fogStart;
			constants.fogParameters[2] = fogEnd;
			constants.fogParameters[3] = static_cast<float>(fogMode);
			memcpy(constants.textureFactor, textureFactor, sizeof(constants.textureFactor));

			if (constantBufferCache.size() != sizeof(constants) ||
			    memcmp(constantBufferCache.data(), &constants, sizeof(constants)) != 0) {
				uint8_t const nextBuffer = static_cast<uint8_t>((activeTransformBuffer + 1) % CONSTANT_BUFFER_COUNT);
				D3D11_MAPPED_SUBRESOURCE mapping{};
				HRESULT result = TakeInjectedFault(FAULT_MAP);
				if (SUCCEEDED(result)) result = d3dContext->Map(
					transformBuffers[nextBuffer].Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapping);
				if (FAILED(result)) {
					LogHRESULT(LogCategory::Resource, "ID3D11DeviceContext::Map(constants)", result);
					NoteDeviceLoss(result);
					return false;
				}
				memcpy(mapping.pData, &constants, sizeof(constants));
				d3dContext->Unmap(transformBuffers[nextBuffer].Get(), 0);
				activeTransformBuffer = nextBuffer;
				constantBufferCache.assign(
					reinterpret_cast<uint8_t const *>(&constants),
					reinterpret_cast<uint8_t const *>(&constants) + sizeof(constants));
			}
			constantsDirty = false;
			constantsFlagInputs = flagInputs;
		}
		static bool const gridDebug = std::strstr(GetCommandLineA(), "-GridDebug") != nullptr;
		if (gridDebug && textureStageEnabled[0] &&
		    (textureStages[0].coordinateSource & 0xfffffff8u) == 0x10u) {
			auto const gridTexture = textures.find(boundTextures[0]);
			if (gridTexture != textures.end()) {
				static ULONGLONG lastGridLog = 0;
				ULONGLONG const now = GetTickCount64();
				if (now - lastGridLog >= 1000) {
					TextureResource const &texture = gridTexture->second;
					float const *matrix = textureStages[0].matrix;
					Log(LogCategory::Grid,
					    "texture=%u size=%ux%u mips=%u uploaded=0x%X filter=%u/%u wrap=%u/%u "
					    "vb=%p+%u matrix=[%.4g %.4g %.4g %.4g; %.4g %.4g %.4g %.4g; "
					    "%.4g %.4g %.4g %.4g; %.4g %.4g %.4g %.4g]",
					    boundTextures[0], texture.width, texture.height, texture.levels,
					    texture.uploadedMipLevels, textureStages[0].magFilter, textureStages[0].minFilter,
					    textureStages[0].wrapU, textureStages[0].wrapV, dynamicVertexBuffer.Get(), dynamicVertexBufferOffset,
					    matrix[0], matrix[1], matrix[2], matrix[3], matrix[4], matrix[5], matrix[6], matrix[7],
					    matrix[8], matrix[9], matrix[10], matrix[11], matrix[12], matrix[13], matrix[14], matrix[15]);
					lastGridLog = now;
				}
			}
		}

		UINT const stride = sizeof(D3D11Vertex);
		UINT const offset = dynamicVertexBufferOffset;
		ID3D11Buffer *vertexBuffer = dynamicVertexBuffer.Get();
		ID3D11Buffer *constantBuffer = transformBuffers[activeTransformBuffer].Get();
		ID3D11PixelShader *desiredPixelShader = shadeModel == 0 ? flatPixelShader.Get() : pixelShader.Get();
		if (!geometryPipelineBound) {
			d3dContext->IASetInputLayout(inputLayout.Get());
			d3dContext->VSSetShader(vertexShader.Get(), nullptr, 0);
			geometryPipelineBound = true;
		}
		if (desiredPixelShader != appliedPixelShader) {
			d3dContext->PSSetShader(desiredPixelShader, nullptr, 0);
			appliedPixelShader = desiredPixelShader;
		}
		if (constantBuffer != appliedTransformBuffer) {
			d3dContext->VSSetConstantBuffers(0, 1, &constantBuffer);
			d3dContext->PSSetConstantBuffers(0, 1, &constantBuffer);
			appliedTransformBuffer = constantBuffer;
		}
		if (vertexBuffer != appliedVertexBuffer || offset != appliedVertexBufferOffset) {
			d3dContext->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);
			appliedVertexBuffer = vertexBuffer;
			appliedVertexBufferOffset = offset;
		}
		if (topology != appliedTopology) {
			d3dContext->IASetPrimitiveTopology(topology);
			appliedTopology = topology;
		}
		if (!textureBindingsValid ||
		    textureViews[0] != appliedTextureViews[0] || textureViews[1] != appliedTextureViews[1]) {
			d3dContext->PSSetShaderResources(0, 2, textureViews);
			appliedTextureViews[0] = textureViews[0];
			appliedTextureViews[1] = textureViews[1];
		}
		if (!textureBindingsValid ||
		    samplers[0] != appliedSamplers[0] || samplers[1] != appliedSamplers[1]) {
			d3dContext->PSSetSamplers(0, 2, samplers);
			appliedSamplers[0] = samplers[0];
			appliedSamplers[1] = samplers[1];
		}
		textureBindingsValid = true;
		if (!ApplyRenderStates()) return false;
		if ((enabledCapabilities[kGDCapability_DepthTest] && depthWriteEnabled) ||
		    enabledCapabilities[kGDCapability_StencilTest]) {
			depthRegionScratchValid = false;
		}
		return true;
	}

	void cGDriver::InterleavedArrays(uint32_t format, int32_t stride, void const *pointer) {
		uint32_t const packedStride = RZVertexFormatStride(format);
		if (!IsSupportedVertexFormat(format) || pointer == nullptr || stride < 0 ||
		    (stride != 0 && static_cast<uint32_t>(stride) < packedStride)) {
			Log(LogCategory::Unsupported, "invalid InterleavedArrays format 0x%08X stride %d pointer %p", format,
			    stride, pointer);
			SetLastError(DriverError::INVALID_VALUE);
			return;
		}
		interleavedFormat = format;
		RecordEncountered(ObservedCategory::VertexFormat, format);
		interleavedStride = stride == 0 ? packedStride : static_cast<uint32_t>(stride);
		interleavedPointer = static_cast<uint8_t const *>(pointer);
	}

	void cGDriver::DrawArrays(uint32_t primitive, int32_t first, int32_t count) {
		if (first < 0 || count <= 0 || !UploadVertices(static_cast<uint32_t>(first), static_cast<uint32_t>(count))) {
			return;
		}

		if (D3D11Topology(primitive) != D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED) {
			if (BindGeometryPipeline(primitive)) {
				d3dContext->Draw(static_cast<UINT>(count), 0);
			}
			return;
		}

		bool built = false;
		try {
			built = BuildSequentialIndices(primitive, static_cast<uint32_t>(count), drawIndexScratch);
		} catch (std::bad_alloc const &) {
		}
		if (!built || !UploadIndices(drawIndexScratch) || !BindGeometryPipeline(0)) {
			Log(LogCategory::Unsupported, "unsupported array primitive %u with %d vertices", primitive, count);
			return;
		}
		ID3D11Buffer *indexBuffer = dynamicIndexBuffer.Get();
		d3dContext->IASetIndexBuffer(indexBuffer, DXGI_FORMAT_R32_UINT, dynamicIndexBufferOffset);
		d3dContext->DrawIndexed(static_cast<UINT>(drawIndexScratch.size()), 0, 0);
	}

	void cGDriver::DrawElements(uint32_t primitive, int32_t count, uint32_t type, void const *indices) {
		if (count == 0) return;
		if (count < 0 || indices == nullptr || (type != 3 && type != 5)) {
			Log(LogCategory::Unsupported, "invalid indexed draw type %u count %d pointer %p", type, count, indices);
			return;
		}

		D3D11_PRIMITIVE_TOPOLOGY const topology = D3D11Topology(primitive);
		bool const convertPrimitive = topology == D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
		uint32_t minimumIndex = UINT32_MAX;
		uint32_t maximumIndex = 0;
		// Index keys hash the final upload, so this pass only finds the range and, for primitives
		// D3D11 lacks, gathers the indices to convert. A separate min/max loop vectorizes.
		auto const scan = [&](auto const *source) {
			for (int32_t i = 0; i < count; ++i) {
				minimumIndex = (std::min)(minimumIndex, static_cast<uint32_t>(source[i]));
				maximumIndex = (std::max)(maximumIndex, static_cast<uint32_t>(source[i]));
			}
			if (convertPrimitive) sourceIndexScratch.assign(source, source + count);
		};
		try {
			if (type == 3) scan(static_cast<uint16_t const *>(indices));
			else scan(static_cast<uint32_t const *>(indices));
			if (convertPrimitive && !ConvertPrimitiveIndices(primitive, sourceIndexScratch, drawIndexScratch)) {
				Log(LogCategory::Unsupported, "unsupported indexed primitive %u with %d indices", primitive, count);
				return;
			}
		} catch (std::bad_alloc const &) {
			Log(LogCategory::Resource, "out of memory converting %d indices", count);
			return;
		}
		if (maximumIndex == UINT32_MAX || minimumIndex > INT32_MAX ||
		    !UploadVertices(minimumIndex, maximumIndex - minimumIndex + 1)) {
			return;
		}
		INT const baseVertex = -static_cast<INT>(minimumIndex);
		if (MatchesLiveShadowMesh(minimumIndex, maximumIndex - minimumIndex + 1)) {
			std::vector<uint32_t> liveIndices;
			try {
				if (convertPrimitive) liveIndices = drawIndexScratch;
				else if (type == 3) {
					auto const *source = static_cast<uint16_t const *>(indices);
					liveIndices.assign(source, source + count);
				} else {
					auto const *source = static_cast<uint32_t const *>(indices);
					liveIndices.assign(source, source + count);
				}
				for (uint32_t &index: liveIndices) index -= minimumIndex;
				CaptureLiveShadowDraw(minimumIndex, maximumIndex - minimumIndex + 1, liveIndices,
				                      convertPrimitive ? D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST : topology);
			} catch (std::bad_alloc const &) {
			}
		}

		if (!convertPrimitive) {
			DXGI_FORMAT const indexFormat = type == 3 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
			uint32_t const indexSize = type == 3 ? sizeof(uint16_t) : sizeof(uint32_t);
			uint64_t const indexBytes = static_cast<uint64_t>(count) * indexSize;
			if (indexBytes > UINT32_MAX) return;
			if (!UploadCachedBuffer(
				    indexBufferSegments, activeIndexBufferSegment, indexBufferCache,
				    IndexCacheKey(indexFormat, indices, static_cast<uint32_t>(count)),
				    static_cast<uint32_t>(indexBytes), D3D11_BIND_INDEX_BUFFER, indices,
				    dynamicIndexBuffer, dynamicIndexBufferOffset) ||
			    !BindGeometryPipeline(primitive)) {
				return;
			}
			d3dContext->IASetIndexBuffer(dynamicIndexBuffer.Get(), indexFormat, dynamicIndexBufferOffset);
			d3dContext->DrawIndexed(static_cast<UINT>(count), 0, baseVertex);
			return;
		}

		if (!UploadIndices(drawIndexScratch) || !BindGeometryPipeline(0)) {
			return;
		}
		ID3D11Buffer *indexBuffer = dynamicIndexBuffer.Get();
		d3dContext->IASetIndexBuffer(indexBuffer, DXGI_FORMAT_R32_UINT, dynamicIndexBufferOffset);
		d3dContext->DrawIndexed(static_cast<UINT>(drawIndexScratch.size()), 0, baseVertex);
	}
}
