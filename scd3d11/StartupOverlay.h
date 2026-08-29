#pragma once

#include <cstdint>
#include <d3d11.h>
#include <wrl/client.h>

namespace nSCD3D11 {

	class StartupOverlay final {
	public:
		HRESULT Initialize(ID3D11Device *device, uint32_t width, uint32_t height);
		void Shutdown(void);
		void SetActive(bool active);
		bool IsActive(void) const;
		void Draw(ID3D11DeviceContext *context);

	private:
		HRESULT CreatePipeline(ID3D11Device *device);
		HRESULT CreateNoticeTexture(ID3D11Device *device, uint32_t width, uint32_t height);
		void RefreshNoticeTexture(ID3D11DeviceContext *context);

		bool active = false;
		Microsoft::WRL::ComPtr<ID3D11VertexShader> vertexShader;
		Microsoft::WRL::ComPtr<ID3D11PixelShader> pixelShader;
		Microsoft::WRL::ComPtr<ID3D11InputLayout> inputLayout;
		Microsoft::WRL::ComPtr<ID3D11Buffer> vertexBuffer;
		Microsoft::WRL::ComPtr<ID3D11Texture2D> noticeTexture;
		Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> noticeView;
		Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler;
		Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthStencilState;
		Microsoft::WRL::ComPtr<ID3D11BlendState> blendState;
		Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizerState;
		uint32_t width = 0;
		uint32_t height = 0;
		uint32_t renderedRevision = 0;
	};

}
