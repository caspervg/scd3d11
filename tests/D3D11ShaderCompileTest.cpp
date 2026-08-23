#include <d3dcompiler.h>

#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace
{
	bool Compile(std::string const& source, char const* entryPoint, char const* target) {
		ID3DBlob* bytecode = nullptr;
		ID3DBlob* messages = nullptr;
		HRESULT const result = D3DCompile(
			source.data(), source.size(), "SC4D3D11", nullptr, nullptr, entryPoint, target,
			D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS |
			D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION, 0, &bytecode, &messages);
		if (messages) {
			std::cerr.write(static_cast<char const*>(messages->GetBufferPointer()), messages->GetBufferSize());
			messages->Release();
		}
		if (bytecode) bytecode->Release();
		return SUCCEEDED(result);
	}
}

int main() {
	std::ifstream file(SC4D3D11_GEOMETRY_SOURCE, std::ios::binary);
	std::string const cpp((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	std::string const marker = "char const kShaderSource[] = R\"(";
	std::string::size_type const begin = cpp.find(marker);
	std::string::size_type const end = begin == std::string::npos ? begin : cpp.find(")\";", begin + marker.size());
	if (!file || begin == std::string::npos || end == std::string::npos) {
		std::cerr << "could not extract kShaderSource from " << SC4D3D11_GEOMETRY_SOURCE << '\n';
		return 1;
	}

	std::string const shader = cpp.substr(begin + marker.size(), end - begin - marker.size());
	return Compile(shader, "VSMain", "vs_4_0") && Compile(shader, "PSMain", "ps_4_0") ? 0 : 1;
}
