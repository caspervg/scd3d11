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
#include "cGDCombiner.h"
#include "Diagnostics.h"
#include "TextureUploadUtils.h"

#include <cfloat>
#include <cstring>
#include <vector>

namespace nSCD3D11 {
    namespace {
        bool IsCompressed(DXGI_FORMAT format) {
            return format == DXGI_FORMAT_BC1_UNORM || format == DXGI_FORMAT_BC2_UNORM || format ==
                   DXGI_FORMAT_BC3_UNORM;
        }

        uint16_t Pack16BitPixel(DXGI_FORMAT format, uint8_t const rgba[4]) {
            switch (format) {
                case DXGI_FORMAT_B5G6R5_UNORM:
                    return static_cast<uint16_t>(((rgba[0] >> 3) << 11) | ((rgba[1] >> 2) << 5) | (rgba[2] >> 3));
                case DXGI_FORMAT_B5G5R5A1_UNORM:
                    return static_cast<uint16_t>(((rgba[3] >= 128) ? 0x8000 : 0) |
                                                 ((rgba[0] >> 3) << 10) | ((rgba[1] >> 3) << 5) | (rgba[2] >> 3));
                case DXGI_FORMAT_B4G4R4A4_UNORM:
                    return static_cast<uint16_t>(((rgba[3] >> 4) << 12) |
                                                 ((rgba[0] >> 4) << 8) | ((rgba[1] >> 4) << 4) | (rgba[2] >> 4));
                default:
                    return 0;
            }
        }
    }

    HRESULT cGDriver::CreateTextureResource(
        TextureResource &resource,
        uint32_t internalFormat,
        uint32_t width,
        uint32_t height,
        uint32_t levels) {
        DXGI_FORMAT const format = D3D11TextureFormat(internalFormat);
        if (!d3dDevice || format == DXGI_FORMAT_UNKNOWN || width == 0 || height == 0 ||
            width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION) {
            return E_INVALIDARG;
        }
        if (levels == 0) levels = 1;

        uint32_t const maximumLevels = D3D11MipLevelCount(width, height);
        if (levels > maximumLevels) {
            return E_INVALIDARG;
        }

        D3D11_TEXTURE2D_DESC description{};
        description.Width = width;
        description.Height = height;
        description.MipLevels = levels;
        description.ArraySize = 1;
        description.Format = format;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        HRESULT result = d3dDevice->CreateTexture2D(&description, nullptr, &texture);
        if (FAILED(result)) {
            LogHRESULT(LogCategory::Resource, "ID3D11Device::CreateTexture2D(texture)", result);
            return result;
        }

        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
        result = d3dDevice->CreateShaderResourceView(texture.Get(), nullptr, &view);
        if (FAILED(result)) {
            LogHRESULT(LogCategory::Resource, "ID3D11Device::CreateShaderResourceView", result);
            return result;
        }

        resource.texture = texture;
        resource.view = view;
        resource.format = format;
		resource.width = width;
		resource.height = height;
		resource.levels = levels;
		resource.uploadedMipLevels = 0;
		resource.internalFormat = internalFormat;
        RecordEncountered(ObservedCategory::TextureFormat,
                          (static_cast<uint64_t>(internalFormat) << 32) | static_cast<uint32_t>(format));
        return S_OK;
    }

	// No CPU copy of texture contents is kept: in a 32-bit process shadowing every texture doubled
	// the texture address-space footprint and caused out-of-memory crashes on heavily modded games.
	// After device loss textures come back with their names and sizes but blank contents until the
	// game re-uploads them.
	HRESULT cGDriver::RecreateTextureResources() {
		uint32_t recreated = 0;
		for (auto &entry: textures) {
			TextureResource &resource = entry.second;
			if (resource.width == 0) continue;
			HRESULT const result = CreateTextureResource(
				resource, resource.internalFormat, resource.width, resource.height, resource.levels);
			if (FAILED(result)) return result;
			++recreated;
		}
		Log(LogCategory::Resource, "device loss: recreated %u textures without contents", recreated);
		return S_OK;
	}

    HRESULT cGDriver::EnsureSampler(TextureStageState &stage) {
        if (stage.sampler) {
            return S_OK;
        }

        D3D11_FILTER_TYPE const minimum =
                (stage.minFilter == 1 || stage.minFilter == 5 || stage.minFilter == 7)
                    ? D3D11_FILTER_TYPE_LINEAR
                    : D3D11_FILTER_TYPE_POINT;
        D3D11_FILTER_TYPE const magnification =
                stage.magFilter == 1 ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT;
        D3D11_FILTER_TYPE const mip =
                (stage.minFilter == 6 || stage.minFilter == 7)
                    ? D3D11_FILTER_TYPE_LINEAR
                    : D3D11_FILTER_TYPE_POINT;

        D3D11_SAMPLER_DESC description{};
        description.Filter = static_cast<D3D11_FILTER>(
            D3D11_ENCODE_BASIC_FILTER(minimum, magnification, mip, D3D11_FILTER_REDUCTION_TYPE_STANDARD));
        description.AddressU = stage.wrapU == 2 ? D3D11_TEXTURE_ADDRESS_CLAMP : D3D11_TEXTURE_ADDRESS_WRAP;
        description.AddressV = stage.wrapV == 2 ? D3D11_TEXTURE_ADDRESS_CLAMP : D3D11_TEXTURE_ADDRESS_WRAP;
        description.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        description.ComparisonFunc = D3D11_COMPARISON_NEVER;
        description.MaxLOD = FLT_MAX;

        uint32_t const key = stage.magFilter |
                              (static_cast<uint32_t>(stage.minFilter) << 8) |
                              (static_cast<uint32_t>(stage.wrapU) << 16) |
                              (static_cast<uint32_t>(stage.wrapV) << 24);
        auto const cached = samplerStates.find(key);
        if (cached != samplerStates.end()) {
            stage.sampler = cached->second;
            return S_OK;
        }

        HRESULT const result = d3dDevice->CreateSamplerState(&description, &stage.sampler);
        if (FAILED(result)) {
            LogHRESULT(LogCategory::Resource, "ID3D11Device::CreateSamplerState", result);
        } else {
            samplerStates.emplace(key, stage.sampler);
        }
        return result;
    }

    void cGDriver::GenTextures(int32_t count, uint32_t *textureIds) {
        if (count < 0 || (count > 0 && textureIds == nullptr)) {
            SetLastError(DriverError::INVALID_VALUE);
            return;
        }
        for (int32_t i = 0; i < count; ++i) {
            while (nextTextureId == 0 || textures.find(nextTextureId) != textures.end()) ++nextTextureId;
            textureIds[i] = nextTextureId;
            textures.emplace(nextTextureId++, TextureResource{});
        }
    }

    void cGDriver::DeleteTextures(int32_t count, uint32_t const *textureIds) {
        if (count < 0 || (count > 0 && textureIds == nullptr)) {
            SetLastError(DriverError::INVALID_VALUE);
            return;
        }
        for (int32_t i = 0; i < count; ++i) {
            for (uint32_t &bound: boundTextures) if (bound == textureIds[i]) bound = 0;
            textures.erase(textureIds[i]);
        }
    }

    bool cGDriver::IsTexture(uint32_t texture) {
        auto const iterator = textures.find(texture);
        return iterator != textures.end() && iterator->second.texture;
    }

    void cGDriver::PrioritizeTextures(int32_t, uint32_t const *, float const *) {
        // D3D11 residency is managed by the runtime.
    }

    bool cGDriver::AreTexturesResident(int32_t count, uint32_t const *textureIds, bool *residences) {
        if (count < 0 || (count > 0 && (textureIds == nullptr || residences == nullptr))) {
            SetLastError(DriverError::INVALID_VALUE);
            return false;
        }
        bool allResident = true;
        for (int32_t i = 0; i < count; ++i) {
            residences[i] = IsTexture(textureIds[i]);
            allResident &= residences[i];
        }
        return allResident;
    }

    void cGDriver::BindTexture(uint32_t target, uint32_t texture) {
        if (target != 0 || (texture != 0 && textures.find(texture) == textures.end())) {
            SetLastError(DriverError::INVALID_VALUE);
            Log(LogCategory::Unsupported, "BindTexture target %u texture %u", target, texture);
            return;
        }
        boundTextures[activeTextureStage] = texture;
    }

    void cGDriver::TexImage2D(
        uint32_t target, int32_t level, int32_t internalFormat, int32_t width, int32_t height,
        int32_t border, uint32_t format, uint32_t type, void const *pixels) {
        uint32_t const texture = boundTextures[activeTextureStage];
        auto iterator = textures.find(texture);
        if (target != 0 || level < 0 || border != 0 || width <= 0 || height <= 0 || iterator == textures.end()) {
            SetLastError(DriverError::INVALID_VALUE);
            return;
        }
        TextureResource &resource = iterator->second;
        DXGI_FORMAT const requestedFormat = D3D11TextureFormat(static_cast<uint32_t>(internalFormat));
        if (level == 0) {
            if (!resource.texture || resource.width != static_cast<uint32_t>(width) ||
                resource.height != static_cast<uint32_t>(height) || resource.format != requestedFormat) {
                HRESULT const result = CreateTextureResource(
                    resource, static_cast<uint32_t>(internalFormat),
                    static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1);
                if (FAILED(result)) {
                    SetLastError(DriverError::CREATE_CONTEXT_FAIL);
                    return;
                }
            }
        } else {
            uint32_t const maximumLevels = resource.texture
                                               ? D3D11MipLevelCount(resource.width, resource.height)
                                               : 0;
            if (!d3dContext || requestedFormat != resource.format ||
                static_cast<uint32_t>(level) >= maximumLevels ||
                static_cast<uint32_t>(width) != D3D11MipDimension(resource.width, static_cast<uint32_t>(level)) ||
                static_cast<uint32_t>(height) != D3D11MipDimension(resource.height, static_cast<uint32_t>(level))) {
                SetLastError(DriverError::INVALID_VALUE);
                return;
            }
			if (static_cast<uint32_t>(level) >= resource.levels) {
				Microsoft::WRL::ComPtr<ID3D11Texture2D> const oldTexture = resource.texture;
				uint32_t const oldLevels = resource.levels;
				uint32_t const oldUploadedMipLevels = resource.uploadedMipLevels;
                HRESULT const result = CreateTextureResource(
                    resource, static_cast<uint32_t>(internalFormat),
                    resource.width, resource.height, maximumLevels);
                if (FAILED(result)) {
                    SetLastError(DriverError::CREATE_CONTEXT_FAIL);
                    return;
                }
				for (uint32_t oldLevel = 0; oldLevel < oldLevels; ++oldLevel) {
                    d3dContext->CopySubresourceRegion(
                        resource.texture.Get(), oldLevel, 0, 0, 0, oldTexture.Get(), oldLevel, nullptr);
				}
				resource.uploadedMipLevels = oldUploadedMipLevels;
            }
        }
        if (pixels) {
            LoadTextureLevel(texture, level, 0, 0, width, height, format, type, pixelStoreRowLength, pixels);
        }
    }

    void cGDriver::PixelStore(uint32_t parameter, int32_t value) {
        if (parameter != 0 || value < 0) {
            SetLastError(DriverError::INVALID_VALUE);
            return;
        }
        pixelStoreRowLength = static_cast<uint32_t>(value);
    }

    void cGDriver::TexEnv(uint32_t target, uint32_t parameter, int32_t value) {
        constantsDirty = true;
        if (target != 0 || parameter != 0 || value < 0 || value > 5) {
            SetLastError(DriverError::INVALID_VALUE);
            return;
        }
        textureStages[activeTextureStage].environmentMode = static_cast<uint8_t>(value);
        if (value == 5) Log(LogCategory::Unsupported, "NV combine4 texture environment requested");
    }

    void cGDriver::TexEnv(uint32_t target, uint32_t parameter, float const *value) {
        constantsDirty = true;
        if (target != 0 || parameter != 1 || value == nullptr) {
            SetLastError(DriverError::INVALID_VALUE);
            return;
        }
        memcpy(textureStages[activeTextureStage].environmentColor, value,
               sizeof(textureStages[activeTextureStage].environmentColor));
    }

    void cGDriver::TexParameter(uint32_t target, uint32_t parameter, int32_t value) {
        if (target != 0 || parameter >= 4 || value < 0 || value >= 8) {
            SetLastError(DriverError::INVALID_VALUE);
            return;
        }

        TextureStageState &stage = textureStages[activeTextureStage];
        switch (parameter) {
            case 0: stage.magFilter = static_cast<uint8_t>(value);
                break;
            case 1: stage.minFilter = static_cast<uint8_t>(value);
                break;
            case 2: stage.wrapU = static_cast<uint8_t>(value);
                break;
            case 3: stage.wrapV = static_cast<uint8_t>(value);
                break;
        }
        stage.sampler.Reset();
    }

    void cGDriver::TexStage(uint32_t stage) {
        if (stage >= 2) {
            SetLastError(DriverError::INVALID_VALUE);
            return;
        }
        activeTextureStage = static_cast<uint8_t>(stage);
    }

    void cGDriver::TexStageCoord(uint32_t source) {
        constantsDirty = true;
        textureStages[activeTextureStage].coordinateSource = source;
        if ((source & 0xfffffff8) != 0 && (source & 0xfffffff8) != 0x10) {
            Log(LogCategory::Unsupported, "texture coordinate source 0x%08X requested", source);
        }
    }

    void cGDriver::TexStageMatrix(float const *matrix, uint32_t rows, uint32_t columns, uint32_t flags) {
        constantsDirty = true;
        float *destination = textureStages[activeTextureStage].matrix;
        memset(destination, 0, sizeof(textureStages[activeTextureStage].matrix));
        if (matrix == nullptr) {
            destination[0] = destination[5] = destination[10] = destination[15] = 1.0f;
            return;
        }
        memcpy(destination, matrix, sizeof(textureStages[activeTextureStage].matrix));
        if ((flags & 3) == 1 && rows == 4 && columns == 2) {
            destination[2] = destination[6] = destination[14] = 0.0f;
            destination[10] = destination[15] = 1.0f;
            destination[3] = destination[7] = destination[11] = 0.0f;
        } else if ((flags & 1) != 0 && !(rows <= 3 || (columns <= 3 && (flags & 2) != 0))) {
            Log(LogCategory::Unsupported, "texture matrix rows %u columns %u flags 0x%X", rows, columns, flags);
        }
    }

    void cGDriver::TexStageCombine(eGDTextureStageCombineParamType parameter, eGDTextureStageCombineModeParam value) {
        constantsDirty = true;
        uint32_t const parameterIndex = static_cast<uint32_t>(parameter);
        uint32_t const mode = static_cast<uint32_t>(value);
        if (parameterIndex >= 2 || mode >= 6) {
            SetLastError(DriverError::INVALID_ENUM);
            return;
        }
        if (parameterIndex == 0) textureStages[activeTextureStage].rgbMode = static_cast<uint8_t>(mode);
        else textureStages[activeTextureStage].alphaMode = static_cast<uint8_t>(mode);
    }

    void cGDriver::TexStageCombine(eGDTextureStageCombineSourceParamType parameter,
                                   eGDTextureStageCombineSourceParam value) {
        constantsDirty = true;
        uint32_t const parameterIndex = static_cast<uint32_t>(parameter);
        uint32_t const source = static_cast<uint32_t>(value);
        if (parameterIndex >= 8 || source >= 4) {
            SetLastError(DriverError::INVALID_ENUM);
            return;
        }
        uint8_t *parameters = parameterIndex < 4
                                  ? textureStages[activeTextureStage].rgbParameters
                                  : textureStages[activeTextureStage].alphaParameters;
        uint32_t const index = parameterIndex & 3;
        if (index >= 3) {
            Log(LogCategory::Unsupported, "fourth texture-combiner source requested");
            return;
        }
        parameters[index] = static_cast<uint8_t>((parameters[index] & 0xf0) | source);
    }

    void cGDriver::TexStageCombine(eGDTextureStageCombineOperandType parameter, eGDBlend value) {
        constantsDirty = true;
        uint32_t const parameterIndex = static_cast<uint32_t>(parameter);
        uint32_t const blend = static_cast<uint32_t>(value);
        if (parameterIndex >= 8 || blend < 2 || blend > 5) {
            SetLastError(DriverError::INVALID_ENUM);
            return;
        }
        uint8_t *parameters = parameterIndex < 4
                                  ? textureStages[activeTextureStage].rgbParameters
                                  : textureStages[activeTextureStage].alphaParameters;
        uint32_t const index = parameterIndex & 3;
        if (index >= 3) {
            Log(LogCategory::Unsupported, "fourth texture-combiner operand requested");
            return;
        }
        parameters[index] = static_cast<uint8_t>((parameters[index] & 0x0f) | ((blend - 2) << 4));
    }

    void cGDriver::TexStageCombine(eGDTextureStageCombineScaleParamType parameter,
                                   eGDTextureStageCombineScaleParam value) {
        constantsDirty = true;
        uint32_t const parameterIndex = static_cast<uint32_t>(parameter);
        uint32_t const scale = static_cast<uint32_t>(value);
        if (parameterIndex >= 2 || scale >= 3) {
            SetLastError(DriverError::INVALID_ENUM);
            return;
        }
        if (parameterIndex == 0) textureStages[activeTextureStage].rgbScale = static_cast<uint8_t>(scale);
        else textureStages[activeTextureStage].alphaScale = static_cast<uint8_t>(scale);
    }

    void cGDriver::SetTexture(uint32_t texture, uint32_t stage) {
        if (stage >= 2 || (texture != 0 && textures.find(texture) == textures.end())) {
            SetLastError(DriverError::INVALID_VALUE);
            return;
        }
        boundTextures[stage] = texture;
    }

    intptr_t cGDriver::GetTexture(uint32_t stage) {
        if (stage >= 2) {
            SetLastError(DriverError::INVALID_VALUE);
            return 0;
        }
        return boundTextures[stage];
    }

    intptr_t cGDriver::CreateTexture(
        uint32_t internalFormat, uint32_t width, uint32_t height, uint32_t levels, uint32_t) {
        uint32_t texture = 0;
        GenTextures(1, &texture);
        auto iterator = textures.find(texture);
        HRESULT const result = CreateTextureResource(iterator->second, internalFormat, width, height, levels);
        if (FAILED(result)) {
            textures.erase(iterator);
            SetLastError(DriverError::CREATE_CONTEXT_FAIL);
            return 0;
        }
        boundTextures[activeTextureStage] = texture;
        return texture;
    }

    void cGDriver::LoadTextureLevel(
        uint32_t texture, int32_t level, int32_t xOffset, int32_t yOffset, int32_t width, int32_t height,
        uint32_t sourceFormat, uint32_t sourceType, uint32_t rowLength, void const *pixels) {
        auto iterator = textures.find(texture);
        if (!d3dContext || iterator == textures.end() || !iterator->second.texture || pixels == nullptr ||
            level < 0 || width <= 0 || height <= 0 || xOffset < 0 || yOffset < 0 ||
            static_cast<uint32_t>(level) >= iterator->second.levels) {
            SetLastError(DriverError::INVALID_VALUE);
            return;
        }

        TextureResource &resource = iterator->second;
        RecordEncountered(ObservedCategory::TextureFormat,
                          (static_cast<uint64_t>(sourceFormat) << 32) | sourceType);
        uint32_t const mipWidth = D3D11MipDimension(resource.width, static_cast<uint32_t>(level));
        uint32_t const mipHeight = D3D11MipDimension(resource.height, static_cast<uint32_t>(level));
        if (!RangeFits(static_cast<uint32_t>(xOffset), static_cast<uint32_t>(width), mipWidth) ||
            !RangeFits(static_cast<uint32_t>(yOffset), static_cast<uint32_t>(height), mipHeight)) {
            SetLastError(DriverError::INVALID_VALUE);
            return;
        }

        D3D11_BOX box{
            static_cast<UINT>(xOffset), static_cast<UINT>(yOffset), 0,
            static_cast<UINT>(xOffset + width), static_cast<UINT>(yOffset + height), 1
        };
        uint32_t pitch = 0;
        void const *upload = pixels;
        std::vector<uint8_t> &converted = textureUploadScratch;
        converted.clear();

        if (IsCompressed(resource.format)) {
            DXGI_FORMAT expected = DXGI_FORMAT_UNKNOWN;
            if (sourceFormat == 7 || sourceFormat == 8) expected = DXGI_FORMAT_BC1_UNORM;
            if (sourceFormat == 9) expected = DXGI_FORMAT_BC2_UNORM;
            if (sourceFormat == 10) expected = DXGI_FORMAT_BC3_UNORM;
            uint32_t const sourceWidth = rowLength ? rowLength : static_cast<uint32_t>(width);
            if (sourceWidth < static_cast<uint32_t>(width) || expected != resource.format ||
                !IsValidBlockCompressedUpdate(
	                mipWidth, mipHeight, static_cast<uint32_t>(xOffset), static_cast<uint32_t>(yOffset),
	                static_cast<uint32_t>(width), static_cast<uint32_t>(height))) {
                Log(LogCategory::Unsupported, "compressed texture upload mismatch: source %u destination %u",
                    sourceFormat, resource.format);
                SetLastError(DriverError::NOT_SUPPORTED);
                return;
            }
            uint64_t const blockBytes = D3D11TextureRowPitch(resource.format, 1);
            uint64_t const sourcePitch = (static_cast<uint64_t>(sourceWidth) + 3) / 4 * blockBytes;
            if (sourcePitch > UINT32_MAX ||
                !SourceSpanFits(pixels, (static_cast<uint64_t>(height) + 3) / 4, sourcePitch,
                                D3D11TextureRowPitch(resource.format, static_cast<uint32_t>(width)))) {
                SetLastError(DriverError::INVALID_VALUE);
                return;
            }
            // D3D11 wants block-compressed update boxes expressed in whole blocks. The 2x2 and
            // 1x1 tail mips of a BC chain still occupy one full block, so round the right and
            // bottom edges up instead of passing the logical mip size.
            box.right = (box.right + 3) & ~3u;
            box.bottom = (box.bottom + 3) & ~3u;
            pitch = static_cast<uint32_t>(sourcePitch);
        } else {
			uint32_t const sourcePixelBytes = TextureSourcePixelBytes(sourceFormat, sourceType);
            // Type 13 is GL_UNSIGNED_SHORT_4_4_4_4_REV per the original driver's typeMap:
            // with format 3 (BGRA) that's B in the low nibble — exactly DXGI B4G4R4A4 layout.
			bool const packedBgra4444 = sourceFormat == 3 && sourceType == 13 &&
			                                resource.format == DXGI_FORMAT_B4G4R4A4_UNORM;
			if (sourcePixelBytes == 0) {
                Log(LogCategory::Unsupported, "texture upload format %u type %u is not implemented", sourceFormat,
                    sourceType);
                SetLastError(DriverError::NOT_SUPPORTED);
                return;
            }

            uint32_t const sourceWidth = rowLength ? rowLength : static_cast<uint32_t>(width);
			if (sourceWidth < static_cast<uint32_t>(width)) {
				SetLastError(DriverError::INVALID_VALUE);
				return;
			}
			uint64_t const sourcePitch64 = static_cast<uint64_t>(sourceWidth) * sourcePixelBytes;
			if (sourcePitch64 > UINT32_MAX ||
			    !SourceSpanFits(pixels, static_cast<uint32_t>(height), sourcePitch64,
			                    static_cast<uint64_t>(width) * sourcePixelBytes)) {
				SetLastError(DriverError::INVALID_VALUE);
				return;
			}
			uint32_t const sourcePitch = static_cast<uint32_t>(sourcePitch64);
            pitch = D3D11TextureRowPitch(resource.format, static_cast<uint32_t>(width));
            uint8_t const *sourceRows = static_cast<uint8_t const *>(pixels);
			if ((sourceFormat == 3 && sourceType == 1 && resource.format == DXGI_FORMAT_B8G8R8A8_UNORM) ||
			    packedBgra4444) {
				// Texels already match the texture. UpdateSubresource steps rows by SrcRowPitch, so padded
				// rows (a row length wider than the upload) go straight through as well.
				upload = pixels;
				pitch = sourcePitch;
			} else {
                converted.resize(static_cast<size_t>(pitch) * height);
                for (int32_t y = 0; y < height; ++y) {
                    uint8_t const *source = sourceRows + static_cast<size_t>(y) * sourcePitch;
                    uint8_t *destination = converted.data() + static_cast<size_t>(y) * pitch;
					for (int32_t x = 0; x < width; ++x, source += sourcePixelBytes) {
                        uint8_t rgba[4];
						if (!ConvertTextureSourcePixel(sourceFormat, sourceType, source, rgba)) {
							SetLastError(DriverError::NOT_SUPPORTED);
							return;
						}
                        if (resource.format == DXGI_FORMAT_B8G8R8A8_UNORM) {
                            destination[x * 4 + 0] = rgba[2];
                            destination[x * 4 + 1] = rgba[1];
                            destination[x * 4 + 2] = rgba[0];
                            destination[x * 4 + 3] = rgba[3];
                        } else {
                            reinterpret_cast<uint16_t *>(destination)[x] = Pack16BitPixel(resource.format, rgba);
                        }
                    }
                }
                upload = converted.data();
            }
        }

		d3dContext->UpdateSubresource(
			resource.texture.Get(), D3D11CalcSubresource(level, 0, resource.levels),
			&box, upload, pitch, 0);
		// Don't pin a one-off huge conversion buffer for the rest of the session.
		if (converted.capacity() > 16u * 1024u * 1024u) std::vector<uint8_t>().swap(converted);
		resource.uploadedMipLevels |= 1u << static_cast<uint32_t>(level);
	}

    void cGDriver::SetCombiner(cGDCombiner const &combiner, uint32_t stage) {
        constantsDirty = true;
        if (stage >= 2) {
            SetLastError(DriverError::OUT_OF_RANGE);
            return;
        }
        TextureStageState &destination = textureStages[stage];
        destination.environmentMode = 4;
        destination.rgbMode = combiner.RGBCombineMode;
        destination.alphaMode = combiner.AlphaCombineMode;
        destination.rgbScale = combiner.RGBScale;
        destination.alphaScale = combiner.AlphaScale;
        for (uint32_t index = 0; index < 3; ++index) {
            destination.rgbParameters[index] = static_cast<uint8_t>(
                (combiner.RGBParams[index].OperandType << 4) | combiner.RGBParams[index].SourceType);
            destination.alphaParameters[index] = static_cast<uint8_t>(
                (combiner.AlphaParams[index].OperandType << 4) | combiner.AlphaParams[index].SourceType);
        }
    }
}
