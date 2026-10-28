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

#include <algorithm>
#include <cstring>

namespace nSCAssets {
	// The levels are read the way the game lays them out (OpenSC4 graphics-formats §2.7 and §2.8, SerialRead
	// 0x00800860), which differs from DBPFKit's reader: there are M + 1 levels (M from the entry header), each
	// starting 16-byte aligned after the previous one, and DXT levels stop shrinking at 4x4 in the file while the
	// texture's own levels go down to 1x1. So the 2x2 and 1x1 levels of a DXT texture hold a whole 4x4 block, and
	// a file whose levels are packed without the padding gives the bytes at the aligned offsets, as in the game.
	bool DecodeTexture(std::span<uint8_t const> bytes, Texture &texture, uint32_t element, std::string *error) {
		texture = {};
		auto const fail = [&](char const *why) {
			if (error) *error = why;
			return false;
		};
		auto const word = [&](size_t at) {
			uint32_t value = 0;
			std::memcpy(&value, bytes.data() + at, sizeof(value));
			return value;
		};
		auto const half = [&](size_t at) { return static_cast<uint16_t>(bytes[at] | bytes[at + 1] << 8); };
		if (bytes.size() < 16) return fail("short record");
		uint32_t const count = word(8);
		if (element >= count || 16 + 8ull * (element + 1) > bytes.size()) return fail("no such image");
		size_t const entry = word(16 + 8 * element + 4);
		if (entry + 16 > bytes.size()) return fail("image header past the record");
		uint8_t const code = bytes[entry];
		uint16_t const width = half(entry + 4), height = half(entry + 6);
		uint32_t const mips = half(entry + 14) >> 12;
		// Twice the bytes per pixel.
		uint32_t pixelBytes2 = 0;
		switch (code) {
		case FSH::kCodeDXT1: pixelBytes2 = 1, texture.format = TextureFormat::Bc1; break;
		case FSH::kCodeDXT3: pixelBytes2 = 2, texture.format = TextureFormat::Bc2; break;
		case FSH::kCodeDXT5: pixelBytes2 = 2, texture.format = TextureFormat::Bc3; break;
		case FSH::kCode32Bit: pixelBytes2 = 8; break;
		case FSH::kCode24Bit: pixelBytes2 = 6; break;
		case FSH::kCode4444:
		case FSH::kCode0565:
		case FSH::kCode1555: pixelBytes2 = 4; break;
		default: return fail("unsupported bitmap code");
		}
		if (height == 0 || (height & (height - 1)) != 0) return fail("height not a power of two");
		texture.code = code;
		uint32_t const minimum = texture.format == TextureFormat::Rgba8 ? 1 : 4;
		uint32_t const textureWidth = std::max<uint32_t>(width, minimum), textureHeight = std::max<uint32_t>(height, minimum);
		uint32_t w = textureWidth, h = textureHeight;
		size_t start = entry + 16;
		for (uint32_t level = 0; level <= mips; ++level) {
			size_t const size = static_cast<size_t>(pixelBytes2) * w * h / 2;
			TextureLevel out{std::max(1u, textureWidth >> level), std::max(1u, textureHeight >> level), {}};
			// The game reads whatever lies there. Past the record, which no shipped file reaches, reads as zero.
			std::vector<uint8_t> stored(size, 0);
			if (start < bytes.size()) std::memcpy(stored.data(), bytes.data() + start, std::min(size, bytes.size() - start));
			if (texture.format != TextureFormat::Rgba8) {
				out.data = std::move(stored);
			} else {
				FSH::Bitmap bitmap;
				bitmap.code = code;
				bitmap.width = static_cast<uint16_t>(w);
				bitmap.height = static_cast<uint16_t>(h);
				bitmap.data = std::move(stored);
				if (!FSH::Reader::ConvertToRGBA8(bitmap, out.data)) return fail("unsupported bitmap code");
			}
			texture.levels.push_back(std::move(out));
			start = (start - entry - 16 + size + 15) / 16 * 16 + entry + 16;
			if (w > minimum) w /= 2;
			if (h > minimum) h /= 2;
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
