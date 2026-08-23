/*
 *  SCGL - a free graphics driver for SimCity 4's SimGL interface
 *
 *  Direct3D 9 fixed-function state manager.
 */

#include <algorithm>
#include <cassert>
#include <cstring>
#include "GLStateManager.h"
#include "VertexFormatUtils.h"

static D3DPRIMITIVETYPE d3dPrimitiveMap[8] = {
	D3DPT_TRIANGLELIST,
	D3DPT_TRIANGLESTRIP,
	D3DPT_TRIANGLEFAN,
	D3DPT_POINTLIST,
	D3DPT_LINELIST,
	D3DPT_LINESTRIP,
	D3DPT_TRIANGLELIST,
	D3DPT_TRIANGLELIST,
};

static D3DCMPFUNC d3dFuncMap[8] = {
	D3DCMP_NEVER,
	D3DCMP_LESS,
	D3DCMP_EQUAL,
	D3DCMP_LESSEQUAL,
	D3DCMP_GREATER,
	D3DCMP_NOTEQUAL,
	D3DCMP_GREATEREQUAL,
	D3DCMP_ALWAYS,
};

static D3DBLEND d3dBlendMap[11] = {
	D3DBLEND_ZERO,
	D3DBLEND_ONE,
	D3DBLEND_SRCCOLOR,
	D3DBLEND_INVSRCCOLOR,
	D3DBLEND_SRCALPHA,
	D3DBLEND_INVSRCALPHA,
	D3DBLEND_DESTALPHA,
	D3DBLEND_INVDESTALPHA,
	D3DBLEND_DESTCOLOR,
	D3DBLEND_INVDESTCOLOR,
	D3DBLEND_SRCALPHASAT,
};

static uint32_t PrimitiveCount(uint32_t gdMode, int32_t vertexOrIndexCount)
{
	switch (gdMode) {
	case 0: return vertexOrIndexCount / 3;
	case 1: return vertexOrIndexCount > 2 ? vertexOrIndexCount - 2 : 0;
	case 2: return vertexOrIndexCount > 2 ? vertexOrIndexCount - 2 : 0;
	case 3: return vertexOrIndexCount;
	case 4: return vertexOrIndexCount / 2;
	case 5: return vertexOrIndexCount > 1 ? vertexOrIndexCount - 1 : 0;
	case 6: return (vertexOrIndexCount / 4) * 2;
	case 7: return vertexOrIndexCount >= 4 ? ((vertexOrIndexCount - 2) / 2) * 2 : 0;
	default: return 0;
	}
}

static D3DMATRIX ToD3DMatrix(float const* m)
{
	D3DMATRIX matrix{};
	std::memcpy(&matrix, m, sizeof(matrix));
	return matrix;
}

static D3DTextureHandle* TextureFromId(uint32_t textureId)
{
	return reinterpret_cast<D3DTextureHandle*>(static_cast<uintptr_t>(textureId));
}

static uint32_t AlignOffset(uint32_t value, uint32_t alignment)
{
	if (alignment == 0) {
		return value;
	}

	return ((value + alignment - 1) / alignment) * alignment;
}

static uint32_t MaxU32(uint32_t lhs, uint32_t rhs)
{
	return lhs > rhs ? lhs : rhs;
}

static uint32_t MinU32(uint32_t lhs, uint32_t rhs)
{
	return lhs < rhs ? lhs : rhs;
}

static float Clamp01(float value)
{
	if (value < 0.0f) {
		return 0.0f;
	}

	if (value > 1.0f) {
		return 1.0f;
	}

	return value;
}

GLStateManager::GLStateManager() :
	device(nullptr),
	streamingVertexBuffer(nullptr),
	streamingIndexBuffer(nullptr),
	streamingVertexOffset(0),
	streamingIndexOffset(0),
	currentFVF(0),
	currentStream(nullptr),
	currentStreamOffset(0),
	currentStreamStride(0),
	currentIndices(nullptr),
	interleavedFormat(0),
	interleavedStride(0),
	interleavedPointer(nullptr),
	activeMatrixMode(0),
	activeTextureUnit(0),
	textureHandles{},
	textureEnabled{ true, false },
	textureCoordSource{},
	enabledCapabilities{},
	ambientLightEnabled(false),
	diffuseLightEnabled(false),
	ambientLightParams{ 0.2f, 0.2f, 0.2f, 1.0f },
	diffuseLightParams{ 1.0f, 1.0f, 1.0f, 1.0f },
	textureEnvColor{ 0.0f, 0.0f, 0.0f, 0.0f },
	isIdentityMatrix{ true, true },
	convertedIndices(),
	renderStateCache{},
	renderStateKnown{},
	textureStageStateCache{},
	textureStageStateKnown{},
	samplerStateCache{},
	samplerStateKnown{},
	boundTextureIds{},
	counters{}
{
}

GLStateManager::~GLStateManager()
{
	ReleaseStreamingBuffers();
}

void GLStateManager::SetDevice(IDirect3DDevice9* newDevice)
{
	ReleaseStreamingBuffers();
	device = newDevice;
	InvalidateDeviceState();
	ResetStateCache();
}

void GLStateManager::ReleaseStreamingBuffers()
{
	if (streamingIndexBuffer != nullptr) {
		streamingIndexBuffer->Release();
		streamingIndexBuffer = nullptr;
	}

	if (streamingVertexBuffer != nullptr) {
		streamingVertexBuffer->Release();
		streamingVertexBuffer = nullptr;
	}

	streamingVertexOffset = 0;
	streamingIndexOffset = 0;
	currentStream = nullptr;
	currentIndices = nullptr;
}

void GLStateManager::InvalidateDeviceState()
{
	currentFVF = 0;
	currentStream = nullptr;
	currentStreamOffset = 0;
	currentStreamStride = 0;
	currentIndices = nullptr;
	std::memset(renderStateKnown, 0, sizeof(renderStateKnown));
	std::memset(textureStageStateKnown, 0, sizeof(textureStageStateKnown));
	std::memset(samplerStateKnown, 0, sizeof(samplerStateKnown));
	std::memset(boundTextureIds, 0xff, sizeof(boundTextureIds));
}

void GLStateManager::ResetStateCache()
{
	interleavedFormat = 0;
	interleavedStride = 0;
	interleavedPointer = nullptr;
	activeMatrixMode = 0;
	activeTextureUnit = 0;
	textureHandles[0] = 0;
	textureHandles[1] = 0;
	textureEnabled[0] = true;
	textureEnabled[1] = false;
	textureCoordSource[0] = 0;
	textureCoordSource[1] = 0;
	std::memset(enabledCapabilities, 0, sizeof(enabledCapabilities));
	isIdentityMatrix[0] = true;
	isIdentityMatrix[1] = true;

	if (device != nullptr) {
		SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
		SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
		SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
		SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
		SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
		SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
		SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
		SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
	}
}

void GLStateManager::SetRenderState(D3DRENDERSTATETYPE state, DWORD value)
{
	uint32_t index = static_cast<uint32_t>(state);
	if (device == nullptr || index >= RenderStateCount) {
		return;
	}

	if (!renderStateKnown[index] || renderStateCache[index] != value) {
		device->SetRenderState(state, value);
		renderStateCache[index] = value;
		renderStateKnown[index] = true;
		counters.renderStateChanges++;
	}
}

void GLStateManager::SetTextureStageState(uint32_t stage, D3DTEXTURESTAGESTATETYPE state, DWORD value)
{
	uint32_t index = static_cast<uint32_t>(state);
	if (device == nullptr || stage >= MAX_TEXTURE_UNITS || index >= TextureStageStateCount) {
		return;
	}

	if (!textureStageStateKnown[stage][index] || textureStageStateCache[stage][index] != value) {
		device->SetTextureStageState(stage, state, value);
		textureStageStateCache[stage][index] = value;
		textureStageStateKnown[stage][index] = true;
		counters.textureStageStateChanges++;
	}
}

DWORD GLStateManager::GetTextureStageState(uint32_t stage, D3DTEXTURESTAGESTATETYPE state)
{
	uint32_t index = static_cast<uint32_t>(state);
	if (device == nullptr || stage >= MAX_TEXTURE_UNITS || index >= TextureStageStateCount) {
		return 0;
	}

	if (!textureStageStateKnown[stage][index]) {
		DWORD value = 0;
		if (SUCCEEDED(device->GetTextureStageState(stage, state, &value))) {
			textureStageStateCache[stage][index] = value;
			textureStageStateKnown[stage][index] = true;
		}
	}

	return textureStageStateCache[stage][index];
}

void GLStateManager::SetSamplerState(uint32_t stage, D3DSAMPLERSTATETYPE state, DWORD value)
{
	uint32_t index = static_cast<uint32_t>(state);
	if (device == nullptr || stage >= MAX_TEXTURE_UNITS || index >= SamplerStateCount) {
		return;
	}

	if (!samplerStateKnown[stage][index] || samplerStateCache[stage][index] != value) {
		device->SetSamplerState(stage, state, value);
		samplerStateCache[stage][index] = value;
		samplerStateKnown[stage][index] = true;
		counters.samplerStateChanges++;
	}
}

void GLStateManager::SetTransform(D3DTRANSFORMSTATETYPE state, D3DMATRIX const& matrix)
{
	if (device != nullptr) {
		device->SetTransform(state, &matrix);
	}
}

void GLStateManager::SetTextureNow(uint32_t stage, uint32_t textureId)
{
	if (device == nullptr || stage >= MAX_TEXTURE_UNITS || boundTextureIds[stage] == textureId) {
		return;
	}

	D3DTextureHandle* handle = TextureFromId(textureId);
	device->SetTexture(stage, handle != nullptr ? handle->texture : nullptr);
	boundTextureIds[stage] = textureId;
	counters.textureBinds++;
}

DWORD GLStateManager::CurrentFVF() const
{
	DWORD fvf = D3DFVF_XYZ;

	if (RZVertexFormatNumElements(interleavedFormat, kGDElementType_Normal) != 0) {
		fvf |= D3DFVF_NORMAL;
	}

	if (RZVertexFormatNumElements(interleavedFormat, kGDElementType_Color) != 0) {
		fvf |= D3DFVF_DIFFUSE;
	}

	uint32_t texCoordCount = RZVertexFormatNumElements(interleavedFormat, kGDElementType_TexCoord);
	if (texCoordCount > 0) {
		fvf |= texCoordCount << D3DFVF_TEXCOUNT_SHIFT;
	}

	return fvf;
}

void GLStateManager::ApplyVertexFormat()
{
	if (device == nullptr) {
		return;
	}

	DWORD fvf = CurrentFVF();
	if (currentFVF != fvf) {
		device->SetFVF(fvf);
		currentFVF = fvf;
		counters.fvfChanges++;
	}
}

void GLStateManager::ApplyTextureStages()
{
	for (uint32_t i = 0; i < MAX_TEXTURE_UNITS; i++) {
		SetTextureNow(i, textureEnabled[i] ? textureHandles[i] : 0);

		if (textureEnabled[i]) {
			SetTextureStageState(i, D3DTSS_TEXCOORDINDEX, textureCoordSource[i]);
		}
		else {
			SetTextureStageState(i, D3DTSS_COLOROP, D3DTOP_DISABLE);
			SetTextureStageState(i, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
		}
	}
}

bool GLStateManager::EnsureStreamingBuffers()
{
	if (device == nullptr) {
		return false;
	}

	if (streamingVertexBuffer == nullptr) {
		HRESULT hr = device->CreateVertexBuffer(
			StreamingVertexBufferSize,
			D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
			0,
			D3DPOOL_DEFAULT,
			&streamingVertexBuffer,
			nullptr);

		if (FAILED(hr)) {
			return false;
		}
	}

	if (streamingIndexBuffer == nullptr) {
		HRESULT hr = device->CreateIndexBuffer(
			StreamingIndexBufferSize,
			D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
			D3DFMT_INDEX32,
			D3DPOOL_DEFAULT,
			&streamingIndexBuffer,
			nullptr);

		if (FAILED(hr)) {
			streamingVertexBuffer->Release();
			streamingVertexBuffer = nullptr;
			return false;
		}
	}

	return true;
}

bool GLStateManager::StreamVertices(void const* source, uint32_t vertexCount, uint32_t stride, uint32_t& startVertex)
{
	if (source == nullptr || vertexCount == 0 || stride == 0 || !EnsureStreamingBuffers()) {
		return false;
	}

	uint64_t byteCount64 = static_cast<uint64_t>(vertexCount) * stride;
	if (byteCount64 > StreamingVertexBufferSize) {
		return false;
	}

	uint32_t byteCount = static_cast<uint32_t>(byteCount64);
	uint32_t alignedOffset = AlignOffset(streamingVertexOffset, stride);
	DWORD lockFlags = D3DLOCK_NOOVERWRITE;

	if (alignedOffset + byteCount > StreamingVertexBufferSize) {
		alignedOffset = 0;
		lockFlags = D3DLOCK_DISCARD;
	}

	void* dest = nullptr;
	if (FAILED(streamingVertexBuffer->Lock(alignedOffset, byteCount, &dest, lockFlags))) {
		return false;
	}

	std::memcpy(dest, source, byteCount);
	streamingVertexBuffer->Unlock();

	streamingVertexOffset = alignedOffset + byteCount;
	startVertex = alignedOffset / stride;
	counters.verticesStreamed += vertexCount;

	if (currentStream != streamingVertexBuffer || currentStreamOffset != 0 || currentStreamStride != stride) {
		device->SetStreamSource(0, streamingVertexBuffer, 0, stride);
		currentStream = streamingVertexBuffer;
		currentStreamOffset = 0;
		currentStreamStride = stride;
		counters.streamChanges++;
	}

	return true;
}

bool GLStateManager::StreamIndices(void const* source, uint32_t indexCount, D3DFORMAT format, uint32_t& startIndex)
{
	if (source == nullptr || indexCount == 0 || !EnsureStreamingBuffers()) {
		return false;
	}

	uint32_t sourceIndexSize = format == D3DFMT_INDEX16 ? sizeof(uint16_t) : sizeof(uint32_t);
	uint64_t byteCount64 = static_cast<uint64_t>(indexCount) * sizeof(uint32_t);
	if (byteCount64 > StreamingIndexBufferSize) {
		return false;
	}

	uint32_t byteCount = static_cast<uint32_t>(byteCount64);
	uint32_t alignedOffset = AlignOffset(streamingIndexOffset, 4);
	DWORD lockFlags = D3DLOCK_NOOVERWRITE;

	if (alignedOffset + byteCount > StreamingIndexBufferSize) {
		alignedOffset = 0;
		lockFlags = D3DLOCK_DISCARD;
	}

	void* dest = nullptr;
	if (FAILED(streamingIndexBuffer->Lock(alignedOffset, byteCount, &dest, lockFlags))) {
		return false;
	}

	if (sourceIndexSize == sizeof(uint32_t)) {
		std::memcpy(dest, source, byteCount);
	}
	else {
		uint16_t const* src = reinterpret_cast<uint16_t const*>(source);
		uint32_t* dst = reinterpret_cast<uint32_t*>(dest);
		for (uint32_t i = 0; i < indexCount; i++) {
			dst[i] = src[i];
		}
	}

	streamingIndexBuffer->Unlock();

	streamingIndexOffset = alignedOffset + byteCount;
	startIndex = alignedOffset / sizeof(uint32_t);
	counters.indicesStreamed += indexCount;

	if (currentIndices != streamingIndexBuffer) {
		device->SetIndices(streamingIndexBuffer);
		currentIndices = streamingIndexBuffer;
		counters.indexBufferChanges++;
	}

	return true;
}

bool GLStateManager::StreamConvertedArrayIndices(uint32_t gdMode, int32_t count, uint32_t& indexCount, uint32_t& startIndex)
{
	convertedIndices.clear();

	if (gdMode == 6) {
		convertedIndices.reserve((count / 4) * 6);
		for (uint32_t i = 0; i + 3 < static_cast<uint32_t>(count); i += 4) {
			convertedIndices.push_back(i);
			convertedIndices.push_back(i + 1);
			convertedIndices.push_back(i + 2);
			convertedIndices.push_back(i);
			convertedIndices.push_back(i + 2);
			convertedIndices.push_back(i + 3);
		}
	}
	else {
		convertedIndices.reserve(((count - 2) / 2) * 6);
		for (uint32_t i = 0; i + 3 < static_cast<uint32_t>(count); i += 2) {
			convertedIndices.push_back(i);
			convertedIndices.push_back(i + 1);
			convertedIndices.push_back(i + 2);
			convertedIndices.push_back(i + 1);
			convertedIndices.push_back(i + 3);
			convertedIndices.push_back(i + 2);
		}
	}

	indexCount = static_cast<uint32_t>(convertedIndices.size());
	return StreamIndices(convertedIndices.data(), indexCount, D3DFMT_INDEX32, startIndex);
}

void GLStateManager::DrawArrays(uint32_t gdMode, int32_t first, int32_t count)
{
	SIZE_CHECK(gdMode, d3dPrimitiveMap);
	if (device == nullptr || interleavedPointer == nullptr || count <= 0 || interleavedStride == 0) {
		return;
	}

	ApplyVertexFormat();
	ApplyTextureStages();

	uint32_t primitiveCount = PrimitiveCount(gdMode, count);
	if (primitiveCount == 0) {
		return;
	}

	uint8_t const* vertices = reinterpret_cast<uint8_t const*>(interleavedPointer) + (static_cast<uint32_t>(first) * interleavedStride);
	uint32_t startVertex = 0;

	if (!StreamVertices(vertices, count, interleavedStride, startVertex)) {
		counters.fallbackUPDraws++;
		if (gdMode == 6 || gdMode == 7) {
			uint32_t indexCount = 0;
			uint32_t ignoredStart = 0;
			StreamConvertedArrayIndices(gdMode, count, indexCount, ignoredStart);
			if (convertedIndices.empty()) {
				return;
			}

			device->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, count, primitiveCount, convertedIndices.data(), D3DFMT_INDEX32, vertices, interleavedStride);
			return;
		}

		device->DrawPrimitiveUP(d3dPrimitiveMap[gdMode], primitiveCount, vertices, interleavedStride);
		counters.drawCalls++;
		counters.primitives += primitiveCount;
		return;
	}

	if (gdMode == 6 || gdMode == 7) {
		uint32_t indexCount = 0;
		uint32_t startIndex = 0;
		if (!StreamConvertedArrayIndices(gdMode, count, indexCount, startIndex)) {
			return;
		}

		device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, startVertex, 0, count, startIndex, primitiveCount);
	}
	else {
		device->DrawPrimitive(d3dPrimitiveMap[gdMode], startVertex, primitiveCount);
	}

	counters.drawCalls++;
	counters.primitives += primitiveCount;
}

void GLStateManager::DrawIndexedConvertedQuads(uint32_t gdType, void const* indices, int32_t count)
{
	convertedIndices.clear();
	uint32_t maxIndex = 0;
	uint32_t minIndex = UINT32_MAX;

	if (gdType == 3) {
		uint16_t const* src = reinterpret_cast<uint16_t const*>(indices);
		convertedIndices.reserve((count / 4) * 6);
		for (int32_t i = 0; i + 3 < count; i += 4) {
			maxIndex = MaxU32(maxIndex, static_cast<uint32_t>(src[i]));
			maxIndex = MaxU32(maxIndex, static_cast<uint32_t>(src[i + 1]));
			maxIndex = MaxU32(maxIndex, static_cast<uint32_t>(src[i + 2]));
			maxIndex = MaxU32(maxIndex, static_cast<uint32_t>(src[i + 3]));
			minIndex = MinU32(minIndex, static_cast<uint32_t>(src[i]));
			minIndex = MinU32(minIndex, static_cast<uint32_t>(src[i + 1]));
			minIndex = MinU32(minIndex, static_cast<uint32_t>(src[i + 2]));
			minIndex = MinU32(minIndex, static_cast<uint32_t>(src[i + 3]));
		}

		for (int32_t i = 0; i + 3 < count; i += 4) {
			convertedIndices.push_back(src[i] - minIndex);
			convertedIndices.push_back(src[i + 1] - minIndex);
			convertedIndices.push_back(src[i + 2] - minIndex);
			convertedIndices.push_back(src[i] - minIndex);
			convertedIndices.push_back(src[i + 2] - minIndex);
			convertedIndices.push_back(src[i + 3] - minIndex);
		}
	}
	else if (gdType == 5) {
		uint32_t const* src = reinterpret_cast<uint32_t const*>(indices);
		convertedIndices.reserve((count / 4) * 6);
		for (int32_t i = 0; i + 3 < count; i += 4) {
			maxIndex = MaxU32(maxIndex, src[i]);
			maxIndex = MaxU32(maxIndex, src[i + 1]);
			maxIndex = MaxU32(maxIndex, src[i + 2]);
			maxIndex = MaxU32(maxIndex, src[i + 3]);
			minIndex = MinU32(minIndex, src[i]);
			minIndex = MinU32(minIndex, src[i + 1]);
			minIndex = MinU32(minIndex, src[i + 2]);
			minIndex = MinU32(minIndex, src[i + 3]);
		}

		for (int32_t i = 0; i + 3 < count; i += 4) {
			convertedIndices.push_back(src[i] - minIndex);
			convertedIndices.push_back(src[i + 1] - minIndex);
			convertedIndices.push_back(src[i + 2] - minIndex);
			convertedIndices.push_back(src[i] - minIndex);
			convertedIndices.push_back(src[i + 2] - minIndex);
			convertedIndices.push_back(src[i + 3] - minIndex);
		}
	}
	else {
		return;
	}

	uint32_t startVertex = 0;
	uint32_t startIndex = 0;
	uint32_t vertexCount = maxIndex - minIndex + 1;
	uint32_t primitiveCount = (count / 4) * 2;
	void const* vertexStart = reinterpret_cast<uint8_t const*>(interleavedPointer) + (minIndex * interleavedStride);
	if (!StreamVertices(vertexStart, vertexCount, interleavedStride, startVertex) ||
		!StreamIndices(convertedIndices.data(), static_cast<uint32_t>(convertedIndices.size()), D3DFMT_INDEX32, startIndex)) {
		counters.fallbackUPDraws++;
		device->DrawIndexedPrimitiveUP(
			D3DPT_TRIANGLELIST,
			0,
			vertexCount,
			primitiveCount,
			convertedIndices.data(),
			D3DFMT_INDEX32,
			vertexStart,
			interleavedStride);
	}
	else {
		device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, startVertex, 0, vertexCount, startIndex, primitiveCount);
	}

	counters.drawCalls++;
	counters.primitives += primitiveCount;
}

void GLStateManager::DrawElements(uint32_t gdMode, int32_t count, uint32_t gdType, void const* indices)
{
	SIZE_CHECK(gdMode, d3dPrimitiveMap);
	if (device == nullptr || interleavedPointer == nullptr || indices == nullptr || count <= 0 || interleavedStride == 0) {
		return;
	}

	ApplyVertexFormat();
	ApplyTextureStages();

	uint32_t primitiveCount = PrimitiveCount(gdMode, count);
	if (primitiveCount == 0) {
		return;
	}

	if (gdMode == 6) {
		DrawIndexedConvertedQuads(gdType, indices, count);
		return;
	}

	uint32_t maxIndex = 0;
	uint32_t minIndex = UINT32_MAX;
	if (gdType == 3) {
		uint16_t const* src = reinterpret_cast<uint16_t const*>(indices);
		for (int32_t i = 0; i < count; i++) {
			maxIndex = MaxU32(maxIndex, static_cast<uint32_t>(src[i]));
			minIndex = MinU32(minIndex, static_cast<uint32_t>(src[i]));
		}
	}
	else if (gdType == 5) {
		uint32_t const* src = reinterpret_cast<uint32_t const*>(indices);
		for (int32_t i = 0; i < count; i++) {
			maxIndex = MaxU32(maxIndex, src[i]);
			minIndex = MinU32(minIndex, src[i]);
		}
	}
	else {
		UNEXPECTED();
		return;
	}

	uint32_t vertexCount = maxIndex - minIndex + 1;
	uint32_t startVertex = 0;
	uint32_t startIndex = 0;
	void const* vertexStart = reinterpret_cast<uint8_t const*>(interleavedPointer) + (minIndex * interleavedStride);

	convertedIndices.clear();
	convertedIndices.reserve(count);
	if (gdType == 3) {
		uint16_t const* src = reinterpret_cast<uint16_t const*>(indices);
		for (int32_t i = 0; i < count; i++) {
			convertedIndices.push_back(static_cast<uint32_t>(src[i]) - minIndex);
		}
	}
	else {
		uint32_t const* src = reinterpret_cast<uint32_t const*>(indices);
		for (int32_t i = 0; i < count; i++) {
			convertedIndices.push_back(src[i] - minIndex);
		}
	}

	if (!StreamVertices(vertexStart, vertexCount, interleavedStride, startVertex) ||
		!StreamIndices(convertedIndices.data(), static_cast<uint32_t>(convertedIndices.size()), D3DFMT_INDEX32, startIndex)) {
		counters.fallbackUPDraws++;
		device->DrawIndexedPrimitiveUP(
			d3dPrimitiveMap[gdMode],
			0,
			vertexCount,
			primitiveCount,
			convertedIndices.data(),
			D3DFMT_INDEX32,
			vertexStart,
			interleavedStride);
	}
	else {
		device->DrawIndexedPrimitive(d3dPrimitiveMap[gdMode], startVertex, 0, vertexCount, startIndex, primitiveCount);
	}

	counters.drawCalls++;
	counters.primitives += primitiveCount;
}

void GLStateManager::InterleavedArrays(uint32_t format, int32_t stride, void const* pointer)
{
	interleavedFormat = format;
	interleavedStride = stride;
	interleavedPointer = pointer;
}

void GLStateManager::ColorMask(bool flag)
{
	SetRenderState(D3DRS_COLORWRITEENABLE, flag ? (D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA) : 0);
}

void GLStateManager::DepthFunc(uint32_t gdFunc)
{
	SIZE_CHECK(gdFunc, d3dFuncMap);
	SetRenderState(D3DRS_ZFUNC, d3dFuncMap[gdFunc]);
}

void GLStateManager::DepthMask(bool flag)
{
	SetRenderState(D3DRS_ZWRITEENABLE, flag);
}

void GLStateManager::StencilFunc(uint32_t gdFunc, int32_t ref, uint32_t mask)
{
	SIZE_CHECK(gdFunc, d3dFuncMap);
	SetRenderState(D3DRS_STENCILFUNC, d3dFuncMap[gdFunc]);
	SetRenderState(D3DRS_STENCILREF, ref);
	SetRenderState(D3DRS_STENCILMASK, mask);
}

void GLStateManager::StencilMask(uint32_t mask)
{
	SetRenderState(D3DRS_STENCILWRITEMASK, mask);
}

void GLStateManager::StencilOp(uint32_t fail, uint32_t zfail, uint32_t zpass)
{
	static D3DSTENCILOP d3dStencilMap[] = { D3DSTENCILOP_KEEP, D3DSTENCILOP_REPLACE, D3DSTENCILOP_INCRSAT, D3DSTENCILOP_DECRSAT, D3DSTENCILOP_INVERT };
	SIZE_CHECK(fail, d3dStencilMap);
	SIZE_CHECK(zfail, d3dStencilMap);
	SIZE_CHECK(zpass, d3dStencilMap);

	SetRenderState(D3DRS_STENCILFAIL, d3dStencilMap[fail]);
	SetRenderState(D3DRS_STENCILZFAIL, d3dStencilMap[zfail]);
	SetRenderState(D3DRS_STENCILPASS, d3dStencilMap[zpass]);
}

void GLStateManager::BlendFunc(uint32_t sfactor, uint32_t dfactor)
{
	SIZE_CHECK(sfactor, d3dBlendMap);
	SIZE_CHECK(dfactor, d3dBlendMap);

	SetRenderState(D3DRS_SRCBLEND, d3dBlendMap[sfactor]);
	SetRenderState(D3DRS_DESTBLEND, d3dBlendMap[dfactor]);
}

void GLStateManager::AlphaFunc(uint32_t func, float ref)
{
	SIZE_CHECK(func, d3dFuncMap);
	SetRenderState(D3DRS_ALPHAFUNC, d3dFuncMap[func]);
	SetRenderState(D3DRS_ALPHAREF, static_cast<DWORD>(Clamp01(ref) * 255.0f));
}

void GLStateManager::ShadeModel(uint32_t mode)
{
	static D3DSHADEMODE shadeModelMap[] = { D3DSHADE_FLAT, D3DSHADE_GOURAUD };
	SIZE_CHECK(mode, shadeModelMap);
	SetRenderState(D3DRS_SHADEMODE, shadeModelMap[mode]);
}

void GLStateManager::ColorMultiplier(float r, float g, float b)
{
	ambientLightParams[0] = r;
	ambientLightParams[1] = g;
	ambientLightParams[2] = b;
	SetRenderState(D3DRS_AMBIENT, D3DCOLOR_COLORVALUE(r, g, b, ambientLightParams[3]));
}

void GLStateManager::AlphaMultiplier(float a)
{
	diffuseLightParams[3] = a;
}

void GLStateManager::EnableVertexColors(bool ambient, bool diffuse)
{
	ambientLightEnabled = ambient;
	diffuseLightEnabled = diffuse;
	SetRenderState(D3DRS_COLORVERTEX, ambient || diffuse);
	SetRenderState(D3DRS_AMBIENTMATERIALSOURCE, ambient ? D3DMCS_COLOR1 : D3DMCS_MATERIAL);
	SetRenderState(D3DRS_DIFFUSEMATERIALSOURCE, diffuse ? D3DMCS_COLOR1 : D3DMCS_MATERIAL);
}

void GLStateManager::MatrixMode(uint32_t mode)
{
	SIZE_CHECK(mode, isIdentityMatrix);
	activeMatrixMode = mode;
}

void GLStateManager::LoadMatrix(float const* m)
{
	if (m == nullptr) {
		return;
	}

	D3DMATRIX matrix = ToD3DMatrix(m);
	SetTransform(activeMatrixMode == 0 ? D3DTS_VIEW : D3DTS_PROJECTION, matrix);
	isIdentityMatrix[activeMatrixMode] = false;
}

void GLStateManager::LoadIdentity(void)
{
	if (isIdentityMatrix[activeMatrixMode]) {
		return;
	}

	D3DMATRIX identity{};
	identity._11 = 1.0f;
	identity._22 = 1.0f;
	identity._33 = 1.0f;
	identity._44 = 1.0f;
	SetTransform(activeMatrixMode == 0 ? D3DTS_VIEW : D3DTS_PROJECTION, identity);
	isIdentityMatrix[activeMatrixMode] = true;
}

void GLStateManager::Enable(uint32_t gdCap)
{
	SIZE_CHECK(gdCap, enabledCapabilities);
	enabledCapabilities[gdCap] = true;

	switch (gdCap) {
	case kGDCapability_AlphaTest: SetRenderState(D3DRS_ALPHATESTENABLE, TRUE); break;
	case kGDCapability_DepthTest: SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE); break;
	case kGDCapability_StencilTest: SetRenderState(D3DRS_STENCILENABLE, TRUE); break;
	case kGDCapability_CullFace: SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW); break;
	case kGDCapability_Blend: SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE); break;
	case kGDCapability_Texture2D:
		textureEnabled[activeTextureUnit] = true;
		SetTextureStageState(activeTextureUnit, D3DTSS_COLOROP, D3DTOP_MODULATE);
		SetTextureStageState(activeTextureUnit, D3DTSS_COLORARG1, D3DTA_TEXTURE);
		SetTextureStageState(activeTextureUnit, D3DTSS_COLORARG2, activeTextureUnit == 0 ? D3DTA_DIFFUSE : D3DTA_CURRENT);
		SetTextureStageState(activeTextureUnit, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
		SetTextureStageState(activeTextureUnit, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
		SetTextureStageState(activeTextureUnit, D3DTSS_ALPHAARG2, activeTextureUnit == 0 ? D3DTA_DIFFUSE : D3DTA_CURRENT);
		break;
	case kGDCapability_Fog: SetRenderState(D3DRS_FOGENABLE, TRUE); break;
	default: break;
	}
}

void GLStateManager::Disable(uint32_t gdCap)
{
	SIZE_CHECK(gdCap, enabledCapabilities);
	enabledCapabilities[gdCap] = false;

	switch (gdCap) {
	case kGDCapability_AlphaTest: SetRenderState(D3DRS_ALPHATESTENABLE, FALSE); break;
	case kGDCapability_DepthTest: SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE); break;
	case kGDCapability_StencilTest: SetRenderState(D3DRS_STENCILENABLE, FALSE); break;
	case kGDCapability_CullFace: SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE); break;
	case kGDCapability_Blend: SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE); break;
	case kGDCapability_Texture2D: textureEnabled[activeTextureUnit] = false; break;
	case kGDCapability_Fog: SetRenderState(D3DRS_FOGENABLE, FALSE); break;
	default: break;
	}
}

bool GLStateManager::IsEnabled(uint32_t gdCap)
{
	SIZE_CHECK_RETVAL(gdCap, enabledCapabilities, false);
	return enabledCapabilities[gdCap];
}

void GLStateManager::TexEnv(uint32_t, uint32_t pname, int32_t gdParam)
{
	if (pname == kGDTextureEnvParamType_Mode) {
		static D3DTEXTUREOP envOpMap[] = { D3DTOP_SELECTARG1, D3DTOP_MODULATE, D3DTOP_SELECTARG1, D3DTOP_BLENDTEXTUREALPHA, D3DTOP_MODULATE, D3DTOP_MODULATE };
		SIZE_CHECK(gdParam, envOpMap);

		SetTextureStageState(activeTextureUnit, D3DTSS_COLOROP, envOpMap[gdParam]);
		SetTextureStageState(activeTextureUnit, D3DTSS_ALPHAOP, envOpMap[gdParam]);
	}
}

void GLStateManager::TexEnv(uint32_t, uint32_t pname, float const* params)
{
	if (params == nullptr || pname != kGDTextureEnvParamType_Color) {
		return;
	}

	std::memcpy(textureEnvColor, params, sizeof(textureEnvColor));
	SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_COLORVALUE(params[0], params[1], params[2], params[3]));
}

void GLStateManager::TexParameter(uint32_t, uint32_t pname, int32_t param)
{
	static D3DSAMPLERSTATETYPE samplerNameMap[] = { D3DSAMP_MAGFILTER, D3DSAMP_MINFILTER, D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV };
	static DWORD samplerParamMap[] = { D3DTEXF_POINT, D3DTEXF_LINEAR, D3DTADDRESS_CLAMP, D3DTADDRESS_WRAP, D3DTEXF_POINT, D3DTEXF_LINEAR, D3DTEXF_POINT, D3DTEXF_LINEAR };
	SIZE_CHECK(pname, samplerNameMap);
	SIZE_CHECK(param, samplerParamMap);

	for (uint32_t i = 0; i < MAX_TEXTURE_UNITS; i++) {
		SetSamplerState(i, samplerNameMap[pname], samplerParamMap[param]);
		if (pname == 1 && param >= 4) {
			SetSamplerState(i, D3DSAMP_MIPFILTER, param == 4 || param == 6 ? D3DTEXF_POINT : D3DTEXF_LINEAR);
		}
	}
}

void GLStateManager::TexStage(uint32_t texUnit)
{
	if (texUnit < MAX_TEXTURE_UNITS) {
		activeTextureUnit = texUnit;
	}
}

void GLStateManager::TexStageCoord(uint32_t gdTexCoordSource)
{
	textureCoordSource[activeTextureUnit] = gdTexCoordSource;
}

void GLStateManager::TexStageMatrix(float const* matrix, uint32_t, uint32_t, uint32_t gdTexMatFlags)
{
	if (matrix == nullptr) {
		D3DMATRIX identity{};
		identity._11 = 1.0f;
		identity._22 = 1.0f;
		identity._33 = 1.0f;
		identity._44 = 1.0f;
		SetTransform(static_cast<D3DTRANSFORMSTATETYPE>(D3DTS_TEXTURE0 + activeTextureUnit), identity);
		SetTextureStageState(activeTextureUnit, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
		return;
	}

	D3DMATRIX d3dMatrix = ToD3DMatrix(matrix);
	SetTransform(static_cast<D3DTRANSFORMSTATETYPE>(D3DTS_TEXTURE0 + activeTextureUnit), d3dMatrix);
	SetTextureStageState(activeTextureUnit, D3DTSS_TEXTURETRANSFORMFLAGS, gdTexMatFlags & 3);
}

void GLStateManager::BindTexture(uint32_t textureId)
{
	SetTexture(textureId, activeTextureUnit);
}

void GLStateManager::SetTexture(uint32_t textureId, uint32_t texUnit)
{
	if (texUnit < MAX_TEXTURE_UNITS) {
		textureHandles[texUnit] = textureId;
	}
}

void GLStateManager::SetTextureImmediately(uint32_t textureId)
{
	SetTexture(textureId, activeTextureUnit);
	boundTextureIds[activeTextureUnit] = UINT32_MAX;
	SetTextureNow(activeTextureUnit, textureId);
}

intptr_t GLStateManager::GetTexture(uint32_t texUnit)
{
	if (texUnit >= MAX_TEXTURE_UNITS) {
		return 0;
	}

	return textureHandles[texUnit];
}

uint32_t GLStateManager::GetActiveTextureUnit() const
{
	return activeTextureUnit;
}
