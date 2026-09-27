#include <d3dcompiler.h>

#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>

// Extracts an embedded HLSL raw string from a driver source and compiles it. By default the
// variable is kShaderSource with VSMain/PSMain entry points; a target may name another variable
// and list its entry points as "entry=profile,entry=profile".
#ifndef SC4D3D11_SHADER_VARIABLE
#define SC4D3D11_SHADER_VARIABLE "kShaderSource"
#endif

namespace
{
	bool Compile(std::string const& source, char const* entryPoint, char const* target,
	             D3D_SHADER_MACRO const* macros = nullptr) {
		ID3DBlob* bytecode = nullptr;
		ID3DBlob* messages = nullptr;
		HRESULT const result = D3DCompile(
			source.data(), source.size(), "SC4D3D11", macros, nullptr, entryPoint, target,
			D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS |
			D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION, 0, &bytecode, &messages);
		if (messages) {
			std::cerr.write(static_cast<char const*>(messages->GetBufferPointer()), messages->GetBufferSize());
			messages->Release();
		}
		if (bytecode) bytecode->Release();
		if (FAILED(result)) std::cerr << "failed to compile " << entryPoint << " (" << target << ")\n";
		return SUCCEEDED(result);
	}
}

int main() {
	std::ifstream file(SC4D3D11_GEOMETRY_SOURCE, std::ios::binary);
	std::string const cpp((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	std::string const marker = std::string("char const ") + SC4D3D11_SHADER_VARIABLE + "[] = R\"(";
	std::string::size_type const begin = cpp.find(marker);
	std::string::size_type const end = begin == std::string::npos ? begin : cpp.find(")\";", begin + marker.size());
	if (!file || begin == std::string::npos || end == std::string::npos) {
		std::cerr << "could not extract " << SC4D3D11_SHADER_VARIABLE << " from " << SC4D3D11_GEOMETRY_SOURCE << '\n';
		return 1;
	}

	std::string const shader = cpp.substr(begin + marker.size(), end - begin - marker.size());
#ifdef SC4D3D11_SHADER_ENTRIES
	std::istringstream entries(SC4D3D11_SHADER_ENTRIES);
	std::string entry;
	bool compiled = false;
	while (std::getline(entries, entry, ',')) {
		std::string::size_type const separator = entry.find('=');
		if (separator == std::string::npos) return 1;
		if (!Compile(shader, entry.substr(0, separator).c_str(), entry.substr(separator + 1).c_str())) return 1;
		compiled = true;
	}
	return compiled ? 0 : 1;
#else
	D3D_SHADER_MACRO const flat[] = {{"SCD3D11_INTERPOLATION", "nointerpolation"}, {nullptr, nullptr}};
	return Compile(shader, "VSMain", "vs_4_0") && Compile(shader, "PSMain", "ps_4_0") &&
	       Compile(shader, "PSMain", "ps_4_0", flat) ? 0 : 1;
#endif
}
