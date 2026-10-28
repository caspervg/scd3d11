/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

#include "Textures.h"

#include "FSHReader.h"

namespace nSCAssets {
	bool DecodeTexture(std::span<uint8_t const> bytes, Texture &texture, uint32_t element, std::string *error) {
		texture = {};
		auto parsed = FSH::Reader::Parse(bytes);
		if (!parsed) {
			if (error) *error = parsed.error().message;
			return false;
		}
		if (element >= parsed->entries.size() || parsed->entries[element].bitmaps.empty()) {
			if (error) *error = "no such image";
			return false;
		}
		FSH::Entry const &entry = parsed->entries[element];
		texture.code = entry.formatCode;
		switch (entry.formatCode) {
		case FSH::kCodeDXT1: texture.format = TextureFormat::Bc1; break;
		case FSH::kCodeDXT3: texture.format = TextureFormat::Bc2; break;
		case FSH::kCodeDXT5: texture.format = TextureFormat::Bc3; break;
		default: texture.format = TextureFormat::Rgba8; break;
		}
		for (FSH::Bitmap const &bitmap : entry.bitmaps) {
			TextureLevel level{bitmap.width, bitmap.height, {}};
			if (texture.format != TextureFormat::Rgba8) {
				level.data = bitmap.data;
			} else if (!FSH::Reader::ConvertToRGBA8(bitmap, level.data)) {
				if (error) *error = "unsupported bitmap code";
				return false;
			}
			texture.levels.push_back(std::move(level));
		}
		return true;
	}

	bool LoadModelTexture(AssetStore const &store, uint32_t instance, uint32_t modelGroup, Texture &texture, Tgi *found) {
		std::vector<uint8_t> bytes;
		for (Tgi const tgi : {Tgi{kFshType, kModelTextureGroup, instance}, Tgi{kFshType, modelGroup, instance},
		                      Tgi{kFshType, kModelTextureGroup, 0x07F94200}}) {
			if (store.Read(tgi, bytes) && DecodeTexture(bytes, texture)) {
				if (found) *found = tgi;
				return true;
			}
		}
		return false;
	}

	bool TextureToRgba8(Texture const &texture, size_t level, std::vector<uint8_t> &rgba) {
		if (level >= texture.levels.size()) return false;
		TextureLevel const &l = texture.levels[level];
		if (texture.format == TextureFormat::Rgba8) {
			rgba = l.data;
			return true;
		}
		FSH::Bitmap bitmap;
		bitmap.code = texture.code;
		bitmap.width = static_cast<uint16_t>(l.width);
		bitmap.height = static_cast<uint16_t>(l.height);
		bitmap.data = l.data;
		return FSH::Reader::ConvertToRGBA8(bitmap, rgba);
	}
}
