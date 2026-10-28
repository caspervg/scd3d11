/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

// FSH textures ready to upload. DBPFKit parses the FSH records; DXT stays as BC blocks, everything else becomes
// RGBA8. This header has no DBPFKit types, so code that includes it can stay C++20.

#pragma once

#include "AssetStore.h"

#include <span>
#include <string>
#include <vector>

namespace nSCAssets {
	constexpr uint32_t kFshType = 0x7AB50E44;
	// The group Maxis model textures live in.
	constexpr uint32_t kModelTextureGroup = 0x1ABE787D;

	enum class TextureFormat : uint8_t {
		Bc1,   // FSH 0x60, DXT1
		Bc2,   // FSH 0x61, DXT3
		Bc3,   // FSH 0x62, DXT5
		Rgba8, // everything else, red in the first byte
	};

	struct TextureLevel {
		uint32_t width = 0, height = 0;
		std::vector<uint8_t> data;
	};

	struct Texture {
		TextureFormat format = TextureFormat::Rgba8;
		// The FSH bitmap code of the image.
		uint8_t code = 0;
		// Level 0 first, down to 1x1 for the game's own textures. A DXT level below 4x4 holds one whole block.
		std::vector<TextureLevel> levels;
	};

	// One image (`element`) of an FSH record with every level the game uploads for it, read the way the game
	// reads them (see Textures.cpp).
	bool DecodeTexture(std::span<uint8_t const> bytes, Texture &texture, uint32_t element = 0, std::string *error = nullptr);

	// The texture an S3D material names, found the way the game looks it up (OpenSC4 graphics-formats §4.1):
	// (FSH, 0x1ABE787D, instance) first, then (FSH, the model's group, instance). The game falls back to a
	// placeholder, instance 0x07F94200 in 0x1ABE787D, when neither exists.
	bool LoadModelTexture(AssetStore const &store, uint32_t instance, uint32_t modelGroup, Texture &texture, Tgi *found = nullptr);

	// Decodes one level to RGBA8 rows, for previews and tests.
	bool TextureToRgba8(Texture const &texture, size_t level, std::vector<uint8_t> &rgba);
}
