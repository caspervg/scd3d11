/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

// A build tool, not part of the driver. It compiles the HLSL that the driver sources hold as raw strings
// and writes the bytecode to a header, so SCD3D11.dll does not need D3DCOMPILER_47.dll on the player's PC.
//
//   ShaderBaker <folder with the driver sources> <header to write> [--debug]

#include <d3dcompiler.h>

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {
	struct Shader {
		char const *name;
		char const *file;
		char const *variable;
		char const *entry;
		char const *profile;
		// Up to two macros, as name and value.
		char const *macros[4];
	};

	constexpr char const kFlat[] = "SCD3D11_INTERPOLATION", kDepth[] = "SCD3D11_DEPTH_SEMANTIC";

	// The main pixel shader has one build for each feature level, for Depth16 and for flat shading.
	constexpr Shader kShaders[] = {
		{"kMainVS", "cGDriver_Geometry.cpp", "kShaderSource", "VSMain", "vs_4_0"},
		{"kMainPS40", "cGDriver_Geometry.cpp", "kShaderSource", "PSMain", "ps_4_0"},
		{"kMainPS40Flat", "cGDriver_Geometry.cpp", "kShaderSource", "PSMain", "ps_4_0", {kFlat, "nointerpolation"}},
		{"kMainPS40Depth", "cGDriver_Geometry.cpp", "kShaderSource", "PSMain", "ps_4_0", {kDepth, "SV_Depth"}},
		{"kMainPS40DepthFlat", "cGDriver_Geometry.cpp", "kShaderSource", "PSMain", "ps_4_0",
		 {kFlat, "nointerpolation", kDepth, "SV_Depth"}},
		{"kMainPS50", "cGDriver_Geometry.cpp", "kShaderSource", "PSMain", "ps_5_0"},
		{"kMainPS50Flat", "cGDriver_Geometry.cpp", "kShaderSource", "PSMain", "ps_5_0", {kFlat, "nointerpolation"}},
		{"kMainPS50Depth", "cGDriver_Geometry.cpp", "kShaderSource", "PSMain", "ps_5_0", {kDepth, "SV_DepthGreaterEqual"}},
		{"kMainPS50DepthFlat", "cGDriver_Geometry.cpp", "kShaderSource", "PSMain", "ps_5_0",
		 {kFlat, "nointerpolation", kDepth, "SV_DepthGreaterEqual"}},
		{"kBlitVS", "cGDriver_Blit.cpp", "kBlitShaderSource", "VSMain", "vs_4_0"},
		{"kBlitPS", "cGDriver_Blit.cpp", "kBlitShaderSource", "PSMain", "ps_4_0"},
		{"kSceneDepthVS", "cGDriver_ReShade.cpp", "kShaderSource", "VSMain", "vs_4_0"},
		{"kSceneDepthPS", "cGDriver_ReShade.cpp", "kShaderSource", "PSMain", "ps_4_0"},
		{"kFrameRateVS", "cGDriver_FrameRate.cpp", "kFrameRateShader", "VSMain", "vs_4_0"},
		{"kFrameRatePS", "cGDriver_FrameRate.cpp", "kFrameRateShader", "PSMain", "ps_4_0"},
		{"kShadowCasterVS", "cGDriver_LiveShadows.cpp", "kLiveShadowShader", "CasterVS", "vs_4_0"},
		{"kShadowCasterPS", "cGDriver_LiveShadows.cpp", "kLiveShadowShader", "CasterPS", "ps_4_0"},
		{"kShadowFullscreenVS", "cGDriver_LiveShadows.cpp", "kLiveShadowShader", "FullscreenVS", "vs_4_0"},
		{"kShadowCompositePS", "cGDriver_LiveShadows.cpp", "kLiveShadowShader", "CompositePS", "ps_4_0"},
		{"kShadowRegistryVS", "cGDriver_LiveShadows.cpp", "kLiveShadowShader", "RegistryVS", "vs_4_0"},
		{"kShadowRegistryPS", "cGDriver_LiveShadows.cpp", "kLiveShadowShader", "RegistryPS", "ps_4_0"},
	};

	// The text between `char const <variable>[] = R"(` and `)";`, the form the shader tests also rely on.
	bool Extract(std::string const &path, char const *variable, std::string &hlsl) {
		std::ifstream file(path, std::ios::binary);
		std::string const cpp((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		std::string const marker = std::string("char const ") + variable + "[] = R\"(";
		std::string::size_type const begin = cpp.find(marker);
		std::string::size_type const end = begin == std::string::npos ? begin : cpp.find(")\";", begin + marker.size());
		if (!file || end == std::string::npos) return false;
		hlsl = cpp.substr(begin + marker.size(), end - begin - marker.size());
		return true;
	}
}

int main(int argc, char **argv) {
	if (argc < 3) {
		fprintf(stderr, "usage: ShaderBaker <source folder> <header> [--debug]\n");
		return 1;
	}
	UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
	if (argc > 3 && std::string(argv[3]) == "--debug") flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;

	std::string header =
		"// Written by ShaderBaker from the HLSL in the driver sources. Do not edit.\n"
		"#pragma once\n\n#include <cstddef>\n\nnamespace nSCD3D11::Shaders {\n"
		"\tstruct Bytecode {\n\t\tunsigned char const *data;\n\t\tsize_t size;\n\t};\n";
	for (Shader const &shader : kShaders) {
		std::string const path = std::string(argv[1]) + "/" + shader.file;
		std::string hlsl;
		if (!Extract(path, shader.variable, hlsl)) {
			fprintf(stderr, "ShaderBaker: could not find %s in %s\n", shader.variable, path.c_str());
			return 1;
		}
		D3D_SHADER_MACRO const macros[] = {
			{shader.macros[0], shader.macros[1]}, {shader.macros[2], shader.macros[3]}, {nullptr, nullptr}};
		ID3DBlob *bytecode = nullptr, *messages = nullptr;
		HRESULT const result = D3DCompile(hlsl.data(), hlsl.size(), shader.file, macros, nullptr, shader.entry,
		                                  shader.profile, flags, 0, &bytecode, &messages);
		if (messages != nullptr) {
			fprintf(stderr, "%s: %.*s\n", shader.name, static_cast<int>(messages->GetBufferSize()),
			        static_cast<char const *>(messages->GetBufferPointer()));
			messages->Release();
		}
		if (FAILED(result)) {
			fprintf(stderr, "ShaderBaker: %s (%s, %s) did not compile\n", shader.name, shader.entry, shader.profile);
			return 1;
		}
		unsigned char const *const bytes = static_cast<unsigned char const *>(bytecode->GetBufferPointer());
		header += std::string("\n\tinline constexpr unsigned char ") + shader.name + "Bytes[] = {";
		char text[8];
		for (size_t index = 0; index < bytecode->GetBufferSize(); ++index) {
			snprintf(text, sizeof(text), "%s%u,", index % 32 == 0 ? "\n\t\t" : "", bytes[index]);
			header += text;
		}
		header += std::string("\n\t};\n\tinline constexpr Bytecode ") + shader.name + "{" + shader.name + "Bytes, sizeof(" +
			shader.name + "Bytes)};\n";
		bytecode->Release();
	}
	header += "}\n";

	std::ofstream output(argv[2], std::ios::binary);
	output << header;
	return output ? 0 : 1;
}
