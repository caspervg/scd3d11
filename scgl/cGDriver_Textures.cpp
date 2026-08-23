/*
 *  SCGL - a free graphics driver for SimCity 4's SimGL interface
 */

#include <algorithm>
#include <cstring>
#include "cGDriver.h"
#include "cGDCombiner.h"

namespace
{
	static D3DFORMAT internalFormatMap[8] = {
		D3DFMT_R5G6B5,
		D3DFMT_X8R8G8B8,
		D3DFMT_A4R4G4B4,
		D3DFMT_A1R5G5B5,
		D3DFMT_A8R8G8B8,
		D3DFMT_DXT1,
		D3DFMT_DXT3,
		D3DFMT_DXT5,
	};

	static uint32_t BytesPerSourcePixel(uint32_t gdTexFormat)
	{
		static uint32_t bppMap[] = { 3, 4, 3, 4, 1, 1, 2, 0, 0, 0, 0 };
		if (gdTexFormat >= sizeof(bppMap) / sizeof(bppMap[0])) {
			return 0;
		}

		return bppMap[gdTexFormat];
	}

	static uint32_t CompressedImageSize(uint32_t gdTexFormat, uint32_t width, uint32_t height)
	{
		uint32_t blockBytes = (gdTexFormat == 7 || gdTexFormat == 8) ? 8 : 16;
		return ((width + 3) / 4) * ((height + 3) / 4) * blockBytes;
	}

	static D3DTextureHandle* TextureFromId(uint32_t textureId)
	{
		return reinterpret_cast<D3DTextureHandle*>(static_cast<uintptr_t>(textureId));
	}

	static uint32_t TextureIdFromHandle(D3DTextureHandle* handle)
	{
		return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(handle));
	}

	static uint32_t ReadSourcePixel(uint8_t const* src, uint32_t gdTexFormat)
	{
		switch (gdTexFormat) {
		case 0: return 0xff000000 | (src[0] << 16) | (src[1] << 8) | src[2];
		case 1: return (src[3] << 24) | (src[0] << 16) | (src[1] << 8) | src[2];
		case 2: return 0xff000000 | (src[2] << 16) | (src[1] << 8) | src[0];
		case 3: return (src[3] << 24) | (src[2] << 16) | (src[1] << 8) | src[0];
		case 4: return src[0] << 24;
		case 5: return 0xff000000 | (src[0] << 16) | (src[0] << 8) | src[0];
		case 6: return (src[1] << 24) | (src[0] << 16) | (src[0] << 8) | src[0];
		default: return 0xffffffff;
		}
	}
}

namespace nSCGL
{
	void cGDriver::GenTextures(int32_t n, uint32_t* textures) {
		if (textures == nullptr) {
			return;
		}

		for (int32_t i = 0; i < n; i++) {
			textures[i] = TextureIdFromHandle(new D3DTextureHandle());
		}
	}

	void cGDriver::DeleteTextures(int32_t n, uint32_t const* textures) {
		if (textures == nullptr) {
			return;
		}

		for (int32_t i = 0; i < n; i++) {
			for (uint32_t stage = 0; stage < MAX_TEXTURE_UNITS; stage++) {
				if (state.GetTexture(stage) == textures[i]) {
					state.SetTexture(0, stage);
				}
			}

			ReleaseTexture(textures[i]);
		}
	}

	bool cGDriver::IsTexture(uint32_t texture) {
		D3DTextureHandle* handle = TextureFromId(texture);
		return handle != nullptr && handle->texture != nullptr;
	}

	void cGDriver::PrioritizeTextures(int32_t, uint32_t const*, float const*) {
	}

	bool cGDriver::AreTexturesResident(int32_t n, uint32_t const*, bool* residences) {
		if (residences != nullptr) {
			for (int32_t i = 0; i < n; i++) {
				residences[i] = true;
			}
		}

		return true;
	}

	void cGDriver::BindTexture(uint32_t, uint32_t texture) {
		state.BindTexture(texture);
	}

	void cGDriver::TexImage2D(uint32_t, int32_t level, int32_t gdInternalTexFormat, int32_t width, int32_t height, int32_t, uint32_t gdTexFormat, uint32_t gdType, void const* pixels) {
		SIZE_CHECK(gdInternalTexFormat, internalFormatMap);
		uint32_t texture = static_cast<uint32_t>(state.GetTexture(state.GetActiveTextureUnit()));
		D3DTextureHandle* handle = TextureFromId(texture);
		if (handle == nullptr || d3dDevice == nullptr || width <= 0 || height <= 0 || level < 0) {
			return;
		}

		if (handle->texture == nullptr) {
			handle->format = internalFormatMap[gdInternalTexFormat];
			handle->width = width;
			handle->height = height;
			handle->levels = 1;
			d3dDevice->CreateTexture(width, height, 1, 0, handle->format, D3DPOOL_MANAGED, &handle->texture, nullptr);
		}

		LoadTextureLevel(texture, level, 0, 0, width, height, gdTexFormat, gdType, 0, pixels);
	}

	void cGDriver::PixelStore(uint32_t, int32_t) {
	}

	void cGDriver::TexEnv(uint32_t target, uint32_t pname, int32_t gdParam) {
		state.TexEnv(target, pname, gdParam);
	}

	void cGDriver::TexEnv(uint32_t target, uint32_t pname, float const* params) {
		state.TexEnv(target, pname, params);
	}

	void cGDriver::TexParameter(uint32_t target, uint32_t pname, int32_t param) {
		state.TexParameter(target, pname, param);
	}

	void cGDriver::TexStage(uint32_t texUnit) {
		if (texUnit < MAX_TEXTURE_UNITS) {
			state.TexStage(texUnit);
			return;
		}

		SetLastError(DriverError::INVALID_VALUE);
	}

	void cGDriver::TexStageCoord(uint32_t gdTexCoordSource) {
		state.TexStageCoord(gdTexCoordSource);
	}

	void cGDriver::TexStageMatrix(float const* matrix, uint32_t unknown0, uint32_t unknown1, uint32_t gdTexMatFlags) {
		state.TexStageMatrix(matrix, unknown0, unknown1, gdTexMatFlags);
	}

	void cGDriver::TexStageCombine(eGDTextureStageCombineParamType gdParamType, eGDTextureStageCombineModeParam gdParam) {
		static D3DTEXTURESTAGESTATETYPE pnameMap[] = { D3DTSS_COLOROP, D3DTSS_ALPHAOP };
		static D3DTEXTUREOP paramMap[] = { D3DTOP_SELECTARG1, D3DTOP_MODULATE, D3DTOP_ADD, D3DTOP_ADDSIGNED, D3DTOP_BLENDCURRENTALPHA, D3DTOP_DOTPRODUCT3 };
		SIZE_CHECK(static_cast<int>(gdParamType), pnameMap);
		SIZE_CHECK(static_cast<int>(gdParam), paramMap);

		if (d3dDevice != nullptr) {
			state.SetTextureStageState(state.GetActiveTextureUnit(), pnameMap[static_cast<int>(gdParamType)], paramMap[static_cast<int>(gdParam)]);
		}
	}

	void cGDriver::TexStageCombine(eGDTextureStageCombineSourceParamType gdParamType, eGDTextureStageCombineSourceParam gdParam) {
		static D3DTEXTURESTAGESTATETYPE pnameMap[] = { D3DTSS_COLORARG1, D3DTSS_COLORARG2, D3DTSS_COLORARG0, D3DTSS_COLORARG0, D3DTSS_ALPHAARG1, D3DTSS_ALPHAARG2, D3DTSS_ALPHAARG0, D3DTSS_ALPHAARG0 };
		static DWORD paramMap[] = { D3DTA_TEXTURE, D3DTA_CURRENT, D3DTA_TFACTOR, D3DTA_DIFFUSE };
		SIZE_CHECK(static_cast<int>(gdParamType), pnameMap);
		SIZE_CHECK(static_cast<int>(gdParam), paramMap);

		if (d3dDevice != nullptr) {
			state.SetTextureStageState(state.GetActiveTextureUnit(), pnameMap[static_cast<int>(gdParamType)], paramMap[static_cast<int>(gdParam)]);
		}
	}

	void cGDriver::TexStageCombine(eGDTextureStageCombineOperandType gdParamType, eGDBlend gdBlend) {
		static D3DTEXTURESTAGESTATETYPE pnameMap[] = { D3DTSS_COLORARG1, D3DTSS_COLORARG2, D3DTSS_COLORARG0, D3DTSS_COLORARG0, D3DTSS_ALPHAARG1, D3DTSS_ALPHAARG2, D3DTSS_ALPHAARG0, D3DTSS_ALPHAARG0 };
		SIZE_CHECK(static_cast<int>(gdParamType), pnameMap);

		if (d3dDevice != nullptr) {
			DWORD currentValue = state.GetTextureStageState(state.GetActiveTextureUnit(), pnameMap[static_cast<int>(gdParamType)]);
			currentValue &= ~(D3DTA_COMPLEMENT | D3DTA_ALPHAREPLICATE);
			if (gdBlend == eGDBlend::OneMinusSrcColor || gdBlend == eGDBlend::OneMinusSrcAlpha) {
				currentValue |= D3DTA_COMPLEMENT;
			}

			if (gdBlend == eGDBlend::SrcAlpha || gdBlend == eGDBlend::OneMinusSrcAlpha) {
				currentValue |= D3DTA_ALPHAREPLICATE;
			}

			state.SetTextureStageState(state.GetActiveTextureUnit(), pnameMap[static_cast<int>(gdParamType)], currentValue);
		}
	}

	void cGDriver::TexStageCombine(eGDTextureStageCombineScaleParamType gdPname, eGDTextureStageCombineScaleParam gdParam) {
		static D3DTEXTURESTAGESTATETYPE pnameMap[] = { D3DTSS_COLOROP, D3DTSS_ALPHAOP };
		static D3DTEXTUREOP modulateOpMap[] = { D3DTOP_MODULATE, D3DTOP_MODULATE2X, D3DTOP_MODULATE4X };
		SIZE_CHECK(static_cast<int>(gdPname), pnameMap);
		SIZE_CHECK(static_cast<int>(gdParam), modulateOpMap);

		DWORD currentOp = state.GetTextureStageState(state.GetActiveTextureUnit(), pnameMap[static_cast<int>(gdPname)]);
		if (currentOp == D3DTOP_MODULATE || currentOp == D3DTOP_MODULATE2X || currentOp == D3DTOP_MODULATE4X) {
			state.SetTextureStageState(state.GetActiveTextureUnit(), pnameMap[static_cast<int>(gdPname)], modulateOpMap[static_cast<int>(gdParam)]);
		}
	}

	void cGDriver::SetTexture(uint32_t textureId, uint32_t texUnit) {
		state.SetTexture(textureId, texUnit);
	}

	intptr_t cGDriver::GetTexture(uint32_t texUnit) {
		return state.GetTexture(texUnit);
	}

	intptr_t cGDriver::CreateTexture(uint32_t texformat, uint32_t width, uint32_t height, uint32_t levels, uint32_t) {
		SIZE_CHECK_RETVAL(texformat, internalFormatMap, 0);
		if (d3dDevice == nullptr) {
			return 0;
		}

		D3DTextureHandle* handle = new D3DTextureHandle();
		handle->format = internalFormatMap[texformat];
		handle->width = width;
		handle->height = height;
		handle->levels = levels == 0 ? 1 : levels;

		if (FAILED(d3dDevice->CreateTexture(width, height, handle->levels, 0, handle->format, D3DPOOL_MANAGED, &handle->texture, nullptr))) {
			delete handle;
			return 0;
		}

		return TextureIdFromHandle(handle);
	}

	void cGDriver::LoadTextureLevel(uint32_t texture, int32_t level, int32_t xoffset, int32_t yoffset, int32_t width, int32_t height, uint32_t gdTexFormat, uint32_t, uint32_t rowLength, void const* pixels) {
		D3DTextureHandle* handle = TextureFromId(texture);
		if (handle == nullptr || handle->texture == nullptr || pixels == nullptr || level < 0 || width <= 0 || height <= 0) {
			return;
		}

		D3DLOCKED_RECT locked{};
		RECT rect{ xoffset, yoffset, xoffset + width, yoffset + height };
		if (FAILED(handle->texture->LockRect(level, &locked, &rect, 0))) {
			return;
		}

		if (handle->format == D3DFMT_DXT1 || handle->format == D3DFMT_DXT3 || handle->format == D3DFMT_DXT5) {
			uint32_t size = CompressedImageSize(gdTexFormat, width, height);
			memcpy(locked.pBits, pixels, size);
		}
		else {
			uint32_t sourceBpp = BytesPerSourcePixel(gdTexFormat);
			if (sourceBpp == 0) {
				handle->texture->UnlockRect(level);
				return;
			}

			uint32_t sourcePitch = (rowLength == 0 ? width : rowLength) * sourceBpp;
			uint8_t const* sourceRow = reinterpret_cast<uint8_t const*>(pixels);
			uint8_t* destRow = reinterpret_cast<uint8_t*>(locked.pBits);

			if (handle->format == D3DFMT_A8R8G8B8 && gdTexFormat == 3) {
				uint32_t copyBytes = width * 4;
				for (int32_t y = 0; y < height; y++) {
					memcpy(destRow, sourceRow, copyBytes);
					sourceRow += sourcePitch;
					destRow += locked.Pitch;
				}
			}
			else {
				for (int32_t y = 0; y < height; y++) {
					uint32_t* dest = reinterpret_cast<uint32_t*>(destRow);
					uint8_t const* source = sourceRow;
					for (int32_t x = 0; x < width; x++) {
						dest[x] = ReadSourcePixel(source, gdTexFormat);
						source += sourceBpp;
					}

					sourceRow += sourcePitch;
					destRow += locked.Pitch;
				}
			}
		}

		handle->texture->UnlockRect(level);
	}

	void cGDriver::SetCombiner(cGDCombiner const& combiner, uint32_t texUnit) {
		TexStage(texUnit);
		TexEnv(0, kGDTextureEnvParamType_Mode, kGDTextureEnvParam_Combine);
		TexStageCombine(eGDTextureStageCombineParamType::RGB, static_cast<eGDTextureStageCombineModeParam>(combiner.RGBCombineMode));
		TexStageCombine(eGDTextureStageCombineParamType::Alpha, static_cast<eGDTextureStageCombineModeParam>(combiner.AlphaCombineMode));
	}
}
