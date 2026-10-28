/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

#include "ImageFile.h"

#include <utility>
#include <wincodec.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace nSCDWire {
	bool SavePng(wchar_t const *path, uint32_t width, uint32_t height, uint8_t const *rgba) {
		// WIC's PNG encoder takes BGRA.
		std::vector<uint8_t> opaque(rgba, rgba + static_cast<size_t>(width) * height * 4);
		for (size_t i = 0; i < opaque.size(); i += 4) {
			std::swap(opaque[i], opaque[i + 2]);
			opaque[i + 3] = 255;
		}

		ComPtr<IWICImagingFactory> factory;
		ComPtr<IWICStream> stream;
		ComPtr<IWICBitmapEncoder> encoder;
		ComPtr<IWICBitmapFrameEncode> frame;
		WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
		HRESULT result = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
		                                  IID_PPV_ARGS(&factory));
		if (SUCCEEDED(result)) result = factory->CreateStream(&stream);
		if (SUCCEEDED(result)) result = stream->InitializeFromFilename(path, GENERIC_WRITE);
		if (SUCCEEDED(result)) result = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
		if (SUCCEEDED(result)) result = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
		if (SUCCEEDED(result)) result = encoder->CreateNewFrame(&frame, nullptr);
		if (SUCCEEDED(result)) result = frame->Initialize(nullptr);
		if (SUCCEEDED(result)) result = frame->SetSize(width, height);
		if (SUCCEEDED(result)) result = frame->SetPixelFormat(&format);
		if (SUCCEEDED(result) && format != GUID_WICPixelFormat32bppBGRA) result = E_FAIL;
		if (SUCCEEDED(result)) {
			result = frame->WritePixels(height, width * 4, static_cast<UINT>(opaque.size()), opaque.data());
		}
		if (SUCCEEDED(result)) result = frame->Commit();
		if (SUCCEEDED(result)) result = encoder->Commit();
		return SUCCEEDED(result);
	}

	bool LoadPng(wchar_t const *path, uint32_t &width, uint32_t &height, std::vector<uint8_t> &rgba) {
		ComPtr<IWICImagingFactory> factory;
		ComPtr<IWICBitmapDecoder> decoder;
		ComPtr<IWICBitmapFrameDecode> frame;
		ComPtr<IWICFormatConverter> converter;
		HRESULT result = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
		                                  IID_PPV_ARGS(&factory));
		if (SUCCEEDED(result)) {
			result = factory->CreateDecoderFromFilename(path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
			                                            &decoder);
		}
		if (SUCCEEDED(result)) result = decoder->GetFrame(0, &frame);
		if (SUCCEEDED(result)) result = factory->CreateFormatConverter(&converter);
		if (SUCCEEDED(result)) {
			result = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr,
			                               0.0, WICBitmapPaletteTypeCustom);
		}
		if (SUCCEEDED(result)) result = converter->GetSize(&width, &height);
		if (SUCCEEDED(result)) {
			rgba.resize(static_cast<size_t>(width) * height * 4);
			result = converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(rgba.size()), rgba.data());
		}
		for (size_t i = 0; SUCCEEDED(result) && i < rgba.size(); i += 4) std::swap(rgba[i], rgba[i + 2]);
		return SUCCEEDED(result);
	}
}
