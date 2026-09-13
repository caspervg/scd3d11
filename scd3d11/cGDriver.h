/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2025  Nelson Gomez (nsgomez) <nelson@ngomez.me>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, see <https://www.gnu.org/licenses/>.
 */

#pragma once
#include <d3d11.h>
#include <dxgi.h>
#include <string>
#include <unordered_map>
#include <vector>
#include <wrl/client.h>
#include <cRZRefCount.h>
#include "cIGZGDriver.h"
#include "D3D11Conversions.h"
#include "sGDMode.h"
#include "ext/cIGZGBufferRegionExtension.h"
#include "ext/cIGZGDriverLightingExtension.h"
#include "ext/cIGZGDriverVertexBufferExtension.h"
#include "ext/cIGZGSnapshotExtension.h"

namespace nSCD3D11 {
	constexpr size_t MAX_BUFFER_REGIONS = sizeof(uint8_t) * 8U;
	constexpr size_t MAX_TEXTURE_UNITS = 2;
	constexpr size_t GEOMETRY_CACHE_SEGMENTS = 8;
	constexpr size_t CONSTANT_BUFFER_COUNT = 8;

	class cGDriver final :
			public cIGZGDriver,
			public cIGZGBufferRegionExtension,
			public cIGZGDriverLightingExtension,
			public cIGZGDriverVertexBufferExtension,
			public cIGZGSnapshotExtension,
			public cRZRefCount {
	private:
		enum class DriverError {
			OK = 0,
			OUT_OF_RANGE = 2,
			NOT_SUPPORTED = 3,
			CREATE_CONTEXT_FAIL = 6,
			INVALID_ENUM = 0x500,
			INVALID_VALUE = 0x501,

			FORCE_DWORD = 0x7FFFFFFF
		};

		enum SGLMatrixMode {
			MODEL_VIEW = 0,
			PROJECTION,
			TEXTURE,
			COLOR,

			NUM_MATRIX_MODES = COLOR + 1,
		};

		enum class PresentationMode : uint8_t {
			Windowed,
			ExclusiveFullscreen,
			BorderlessFullscreen,
		};

	private:
		struct BufferRegionResource {
			Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
			int32_t type = 0;
		};

		unsigned int refCount;
		DriverError lastError;

#ifndef NDEBUG
		uint32_t dbgLastError;
#endif

		std::vector<sGDMode> videoModes;
		bool initialized;
		int videoModeCount;
		int currentVideoMode;
		std::string driverInfo;

		int windowWidth, windowHeight;
		int viewportX, viewportY, viewportWidth, viewportHeight;

		// We're not expecting to use a lot of buffer regions simultaneously, so we'll use an
		// 8-bit mask to indicate which regions are allocated and free.
		uint8_t bufferRegionFlags;
		BufferRegionResource bufferRegions[MAX_BUFFER_REGIONS];

	private:
		struct {
			// OpenGL
			bool bgraColor;
			bool stencilBuffer;
			bool multitexture;
			bool textureEnvCombine;
			bool fogCoord;
			bool textureCompression;
			bool nvTextureEnvCombine4;
			bool debugOutput;
			bool noError;

			// WGL
			bool bufferRegion;
			bool createContext;
			bool createContextNoError;
			bool createContextProfile;
			bool multisample;
			bool pixelFormat;
			bool swapControl;
		} supportedExtensions;

	private:
		struct TextureResource {
			Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
			Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
			DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
			uint32_t width = 0;
			uint32_t height = 0;
			uint32_t levels = 0;
			uint32_t uploadedMipLevels = 0;
			uint32_t internalFormat = 0;
		};

		struct TextureStageState {
			Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler;
			uint8_t magFilter = 1;
			uint8_t minFilter = 6;
			uint8_t wrapU = 3;
			uint8_t wrapV = 3;
			uint8_t environmentMode = 1;
			uint8_t rgbMode = 1;
			uint8_t alphaMode = 1;
			uint8_t rgbScale = 0;
			uint8_t alphaScale = 0;
			uint8_t rgbParameters[3]{0x00, 0x01, 0x02};
			uint8_t alphaParameters[3]{0x00, 0x01, 0x02};
			uint32_t coordinateSource = 0;
			float environmentColor[4]{};
			float matrix[16]{};
		};

		struct GeometryCacheEntry {
			uint8_t segment;
			uint32_t offset;
		};

		struct GeometryCacheSegment {
			Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
			uint32_t capacity = 0;
			uint32_t cursor = 0;
			std::vector<uint64_t> keys;
		};

		void *windowHandle;
		void *windowProcedure;
		bool showDriverWindow;
		bool recoveringDevice;
		Microsoft::WRL::ComPtr<ID3D11Device> d3dDevice;
		Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3dContext;
		Microsoft::WRL::ComPtr<IDXGISwapChain> swapChain;
		PresentationMode presentationMode;
		UINT swapChainFlags;
		// SC4 redraws only dirty rectangles and expects the back buffer to survive Present. DISCARD
		// swap chains give no such guarantee (exclusive fullscreen really flips), so the game renders
		// into this persistent texture, which is copied to swapChainBuffer right before each Present.
		Microsoft::WRL::ComPtr<ID3D11Texture2D> backBufferTexture;
		Microsoft::WRL::ComPtr<ID3D11Texture2D> swapChainBuffer;
		Microsoft::WRL::ComPtr<ID3D11RenderTargetView> renderTargetView;
		// sRGB view of backBufferTexture, for ReShade techniques that write with SRGBWriteEnable.
		Microsoft::WRL::ComPtr<ID3D11RenderTargetView> renderTargetViewSrgb;
		Microsoft::WRL::ComPtr<ID3D11Texture2D> depthStencilTexture;
		Microsoft::WRL::ComPtr<ID3D11DepthStencilView> depthStencilView;
		Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> depthShaderView;
		DXGI_FORMAT depthStencilFormat;
		// Scene depth re-encoded for ReShade's DEPTH semantic (see cGDriver_ReShade.cpp).
		struct SceneDepthPipeline {
			Microsoft::WRL::ComPtr<ID3D11VertexShader> vertexShader;
			Microsoft::WRL::ComPtr<ID3D11PixelShader> pixelShader;
			Microsoft::WRL::ComPtr<ID3D11Buffer> constants;
			Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
			Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target;
			Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
			float encodedFarPlane = 0.0f;
		} sceneDepth;
		// Effects rendered into the persistent back buffer and not yet overwritten by a full clear.
		bool reshadeEffectsInBackBuffer = false;
		bool reshadeEffectsThisFrame = false;
		// Plain (non-depth-stencil-bound) copy of the depth buffer; partial CopySubresourceRegion
		// is illegal on D3D11_BIND_DEPTH_STENCIL resources, so depth region blits bounce through this.
		Microsoft::WRL::ComPtr<ID3D11Texture2D> depthRegionScratch;
		bool depthRegionScratchValid;
		Microsoft::WRL::ComPtr<ID3D11SamplerState> defaultSampler;
		Microsoft::WRL::ComPtr<ID3D11VertexShader> vertexShader;
		Microsoft::WRL::ComPtr<ID3D11PixelShader> pixelShader;
		Microsoft::WRL::ComPtr<ID3D11PixelShader> flatPixelShader;
		ID3D11PixelShader *appliedPixelShader;
		Microsoft::WRL::ComPtr<ID3D11InputLayout> inputLayout;
		Microsoft::WRL::ComPtr<ID3D11Buffer> transformBuffers[CONSTANT_BUFFER_COUNT];
		uint8_t activeTransformBuffer;
		ID3D11Buffer *appliedTransformBuffer;
		Microsoft::WRL::ComPtr<ID3D11Buffer> dynamicVertexBuffer;
		Microsoft::WRL::ComPtr<ID3D11Buffer> dynamicIndexBuffer;
		GeometryCacheSegment vertexBufferSegments[GEOMETRY_CACHE_SEGMENTS];
		GeometryCacheSegment indexBufferSegments[GEOMETRY_CACHE_SEGMENTS];
		uint8_t activeVertexBufferSegment;
		uint8_t activeIndexBufferSegment;
		uint32_t dynamicVertexBufferOffset;
		uint32_t dynamicIndexBufferOffset;
		ID3D11Buffer *appliedVertexBuffer;
		uint32_t appliedVertexBufferOffset;
		std::unordered_map<uint64_t, GeometryCacheEntry> vertexBufferCache;
		std::unordered_map<uint64_t, GeometryCacheEntry> indexBufferCache;
		uint64_t vertexBufferCacheHits;
		uint64_t vertexBufferCacheMisses;
		uint64_t indexBufferCacheHits;
		uint64_t indexBufferCacheMisses;
		bool geometryPipelineBound;
		bool textureBindingsValid;
		D3D11_PRIMITIVE_TOPOLOGY appliedTopology;
		ID3D11ShaderResourceView *appliedTextureViews[2];
		ID3D11SamplerState *appliedSamplers[2];
		uint32_t interleavedFormat;
		uint32_t interleavedStride;
		uint8_t const *interleavedPointer;
		uint8_t activeMatrixMode;
		float matrices[2][16];
		std::vector<D3D11Vertex> vertexScratch;
		std::vector<uint32_t> sourceIndexScratch;
		std::vector<uint32_t> drawIndexScratch;
		std::vector<uint8_t> textureUploadScratch;
		std::vector<uint8_t> constantBufferCache;
		std::vector<uint8_t> extensionVertexData;
		uint32_t extensionVertexCursor;
		uint32_t extensionVertexStart;
		uint64_t extensionVertexGeneration;
		bool extensionVerticesLocked;
		std::unordered_map<uint32_t, TextureResource> textures;
		std::unordered_map<uint32_t, Microsoft::WRL::ComPtr<ID3D11SamplerState> > samplerStates;
		uint32_t nextTextureId;
		uint32_t boundTextures[2];
		uint8_t activeTextureStage;
		bool textureStageEnabled[2];
		TextureStageState textureStages[2];
		uint32_t pixelStoreRowLength;
		struct BlitPipeline {
			Microsoft::WRL::ComPtr<ID3D11VertexShader> vertexShader;
			Microsoft::WRL::ComPtr<ID3D11PixelShader> pixelShader;
			Microsoft::WRL::ComPtr<ID3D11InputLayout> inputLayout;
			Microsoft::WRL::ComPtr<ID3D11Buffer> vertexBuffer;
			Microsoft::WRL::ComPtr<ID3D11Buffer> constants;
			Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler;
			Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthState;
			Microsoft::WRL::ComPtr<ID3D11BlendState> opaqueBlend;
			Microsoft::WRL::ComPtr<ID3D11BlendState> alphaBlend;
			Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer;
			Microsoft::WRL::ComPtr<ID3D11RasterizerState> scissorRasterizer;
			Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
			Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> textureView;
			uint32_t textureWidth = 0;
			uint32_t textureHeight = 0;
			std::vector<uint8_t> scratch;
		} blit;
		// Toggled by Punt(0x6C4236E7): SC4 brackets a plain StretchBlt with it when the source
		// carries per-pixel alpha that should blend.
		bool blitUsesSourceAlpha;
		bool enabledCapabilities[kGDNumCapabilities];
		bool colorWriteEnabled;
		uint8_t depthFunction;
		bool depthWriteEnabled;
		uint8_t stencilFunction;
		int32_t stencilReference;
		uint8_t stencilReadMask;
		uint8_t stencilWriteMask;
		uint8_t stencilFailOperation;
		uint8_t stencilDepthFailOperation;
		uint8_t stencilPassOperation;
		uint8_t sourceBlend;
		uint8_t destinationBlend;
		uint8_t alphaFunction;
		float alphaReference;
		uint8_t shadeModel;
		float colorMultipliers[4];
		uint8_t fogMode;
		uint8_t fogSource;
		float fogColor[4];
		float fogDensity;
		float fogStart;
		float fogEnd;
		bool ambientVertexColors;
		bool diffuseVertexColors;
		int32_t polygonOffset;
		bool scissorEnabled;
		bool lightingEnabled;
		bool lightsEnabled[8];
		float globalAmbient[4];
		float lightAmbient[8][4];
		float lightDiffuse[8][4];
		float lightSpecular[8][4];
		float lightPosition[8][4];
		float materialAmbient[4];
		float materialDiffuse[4];
		float materialSpecular[4];
		float materialEmission[4];
		float materialShininess;
		std::unordered_map<uint64_t, Microsoft::WRL::ComPtr<ID3D11DepthStencilState> > depthStencilStates;
		std::unordered_map<uint64_t, Microsoft::WRL::ComPtr<ID3D11BlendState> > blendStates;
		std::unordered_map<uint64_t, Microsoft::WRL::ComPtr<ID3D11RasterizerState> > rasterizerStates;
		uint64_t appliedDepthStateKey;
		uint64_t appliedBlendStateKey;
		uint64_t appliedRasterizerStateKey;
		int32_t appliedStencilReference;
		D3D_FEATURE_LEVEL featureLevel;
		uint32_t deviceGeneration;
		float clearColor[4];
		float clearDepth;
		uint8_t clearStencil;

	private:
		void SetLastError(DriverError err);

		static LRESULT CALLBACK DriverWindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

		static Microsoft::WRL::ComPtr<IDXGIAdapter> SelectAdapter(void);

		void DestroyD3D11Context(bool preserveResources = false);

		HRESULT CreateBackBufferTargets(uint32_t width, uint32_t height);

		HRESULT ResizeBackBufferIfNeeded();

		bool RecoverD3D11Device();

		HRESULT CreateGeometryPipeline();

		bool UseCachedBuffer(
			GeometryCacheSegment *segments,
			std::unordered_map<uint64_t, GeometryCacheEntry> &cache,
			uint64_t key,
			Microsoft::WRL::ComPtr<ID3D11Buffer> &buffer,
			uint32_t &offset,
			uint32_t bindFlags);

		bool UploadCachedBuffer(
			GeometryCacheSegment *segments,
			uint8_t &activeSegment,
			std::unordered_map<uint64_t, GeometryCacheEntry> &cache,
			uint64_t key,
			uint32_t requiredSize,
			uint32_t bindFlags,
			void const *data,
			Microsoft::WRL::ComPtr<ID3D11Buffer> &buffer,
			uint32_t &offset);

		bool UploadVertices(uint32_t first, uint32_t count);

		bool UploadIndices(std::vector<uint32_t> const &indices);

		bool BindGeometryPipeline(uint32_t primitive);

		bool UploadExtensionVertices(uint32_t byteSize);

		bool ApplyRenderStates();

		void InvalidateD3D11StateCache();

		enum class BlitAlpha { Opaque, Source, Constant, SourceModulated };

		// Shared body of the BitBlt/StretchBlt family: SC4 hands over a CPU pixel rectangle that is
		// converted, uploaded and drawn as a screen-space quad (top-left origin, window pixels).
		void DrawBlit(int32_t destX, int32_t destY, int32_t destWidth, int32_t destHeight,
		              int32_t sourceWidth, int32_t sourceHeight, uint32_t format, uint32_t type,
		              void const *pixels, bool colorKeyed, void const *colorKey, BlitAlpha alpha, uint32_t alphaValue);

		HRESULT CreateBlitPipeline();

		HRESULT CreateTextureResource(
			TextureResource &resource,
			uint32_t internalFormat,
			uint32_t width,
			uint32_t height,
			uint32_t levels);

		HRESULT RecreateTextureResources();

		HRESULT EnsureSampler(TextureStageState &stage);

		HRESULT EnsureDepthRegionScratch(void);

		int FindFreeBufferRegionIndex(void);

		HRESULT CreateBufferRegionResource(uint32_t index, int32_t type);

		HRESULT RecreateBufferRegions();

		int InitializeVideoModeVector(void);

		void InstallReShadeAddon(void);

		void UninstallReShadeAddon(void);

		HRESULT UpdateSceneDepth(void);

		void FinishReShadeFrame(void);

	public:
		// Called at the end of cSC43DRender::Draw: the city view is complete and no UI is drawn yet.
		void RenderSceneEffects(void);

		cGDriver();

		virtual ~cGDriver() override;

		// Override SC4's native DirectX driver by presenting its GZCLSID with a
		// higher version number to GZCOM.
		static const uint32_t kSCD3D11GDriverGZCLSID = 0x0badb6906;

		static bool FactoryFunctionPtr2(uint32_t riid, void **ppvObj) {
			cGDriver *pDriver = new cGDriver();
			bool bSucceeded = pDriver->QueryInterface(riid, ppvObj);

			if (!bSucceeded || *ppvObj == NULL) {
				bSucceeded = false;

				delete pDriver;
				pDriver = NULL;
			}

			return bSucceeded;
		}

	public:
		virtual bool QueryInterface(uint32_t riid, void **ppvObj) override;

		virtual uint32_t AddRef(void) override;

		virtual uint32_t Release(void) override;

		virtual bool FinalRelease(void) override;

	public:
		virtual void DrawArrays(uint32_t gdPrimType, int32_t, int32_t) override;

		virtual void DrawElements(uint32_t gdPrimType, int32_t count, uint32_t gdType, void const *indices) override;

		virtual void InterleavedArrays(uint32_t gdVertexFormat, int32_t, void const *) override;

		virtual uint32_t MakeVertexFormat(uint32_t, intptr_t gdElementTypePtr) override;

		virtual uint32_t MakeVertexFormat(uint32_t gdVertexFormat) override;

		virtual uint32_t VertexFormatStride(uint32_t gdVertexFormat) override;

		virtual uint32_t VertexFormatElementOffset(uint32_t gdVertexFormat, uint32_t gdElementType, uint32_t) override;

		virtual uint32_t VertexFormatNumElements(uint32_t gdVertexFormat, uint32_t gdElementType) override;

		virtual void Clear(uint32_t) override;

		virtual void ClearColor(float, float, float, float) override;

		virtual void ClearDepth(double) override;

		virtual void ClearStencil(int32_t) override;

		virtual void ColorMask(bool) override;

		virtual void DepthFunc(uint32_t gdTestFunc) override;

		virtual void DepthMask(bool) override;

		virtual void StencilFunc(uint32_t gdTestFunc, int32_t, uint32_t) override;

		virtual void StencilMask(uint32_t) override;

		virtual void StencilOp(uint32_t gdStencilOp, uint32_t gdStencilOp2, uint32_t gdStencilOp3) override;

		virtual void BlendFunc(uint32_t gdBlendFunc, uint32_t gdBlend) override;

		virtual void AlphaFunc(uint32_t gdTestFunc, float) override;

		virtual void ShadeModel(uint32_t gdShade) override;

		virtual void BindTexture(uint32_t gdTextureTarget, uint32_t) override;

		virtual void TexImage2D(uint32_t gdTextureTarget, int32_t, int32_t gdInternalTexFormat, int32_t, int32_t,
		                        int32_t, uint32_t gdTexFormat, uint32_t gdType, void const *) override;

		virtual void PixelStore(uint32_t gdParameter, int32_t) override;

		virtual void TexEnv(uint32_t gdTextureEnvTarget, uint32_t gdTextureEnvParamType,
		                    int32_t gdTextureEnvModeParam) override;

		virtual void TexEnv(uint32_t gdTextureEnvTarget, uint32_t gdTextureEnvParamType, float const *) override;

		virtual void TexParameter(uint32_t gdTextureTarget, uint32_t gdTextureParamType,
		                          int32_t gdTextureParam) override;

		virtual void Fog(uint32_t gdFogParamType, uint32_t gdFogParam) override;

		virtual void Fog(uint32_t gdFogParamType, float const *) override;

		virtual void ColorMultiplier(float r, float g, float b) override;

		virtual void AlphaMultiplier(float a) override;

		virtual void EnableVertexColors(bool, bool) override;

		virtual void GenTextures(int32_t, uint32_t *) override;

		virtual void DeleteTextures(int32_t, uint32_t const *) override;

		virtual bool IsTexture(uint32_t) override;

		virtual void PrioritizeTextures(int32_t, uint32_t const *, float const *) override;

		virtual bool AreTexturesResident(int32_t, uint32_t const *, bool *) override;

		virtual void MatrixMode(uint32_t gdMatrixTarget) override;

		virtual void LoadMatrix(float const *) override;

		virtual void LoadIdentity(void) override;

		virtual void Flush(void) override;

		virtual void Enable(uint32_t gdDriverState) override;

		virtual void Disable(uint32_t gdDriverState) override;

		virtual bool IsEnabled(uint32_t gdDriverState) override;

		virtual void GetBoolean(uint32_t gdParameter, bool *) override;

		virtual void GetInteger(uint32_t gdParameter, int32_t *) override;

		virtual void GetFloat(uint32_t gdParameter, float *) override;

		virtual uint32_t GetError(void) override;

		virtual void TexStage(uint32_t) override;

		virtual void TexStageCoord(uint32_t gdTexCoordSource) override;

		virtual void TexStageMatrix(float const *, uint32_t, uint32_t, uint32_t gdTexMatFlags) override;

		virtual void TexStageCombine(eGDTextureStageCombineScaleParamType gdParamType,
		                             eGDTextureStageCombineScaleParam gdParam) override;

		virtual void TexStageCombine(eGDTextureStageCombineOperandType gdParamType, eGDBlend gdBlend) override;

		virtual void TexStageCombine(eGDTextureStageCombineSourceParamType gdParamType,
		                             eGDTextureStageCombineSourceParam gdParam) override;

		virtual void TexStageCombine(eGDTextureStageCombineParamType gdParamType,
		                             eGDTextureStageCombineModeParam gdParam) override;

		virtual void SetTexture(uint32_t, uint32_t) override;

		virtual intptr_t GetTexture(uint32_t) override;

		virtual intptr_t CreateTexture(uint32_t gdInternalTexFormat, uint32_t, uint32_t, uint32_t,
		                               uint32_t gdTexHintFlags) override;

		virtual void LoadTextureLevel(uint32_t, int32_t, int32_t, int32_t, int32_t, int32_t, uint32_t gdTexFormat,
		                              uint32_t gdType, uint32_t, void const *) override;

		virtual void SetCombiner(cGDCombiner const &combiner, uint32_t) override;

		virtual uint32_t CountVideoModes(void) const override;

		virtual void GetVideoModeInfo(uint32_t dwIndex, sGDMode &gdMode) override;

		virtual void GetVideoModeInfo(sGDMode &gdMode) override;

		virtual void SetVideoMode(int32_t newModeIndex, void *, bool, bool) override;

		virtual void PolygonOffset(int32_t) override;

		virtual void BitBlt(int32_t, int32_t, int32_t, int32_t, uint32_t gdTexFormat, uint32_t gdType, void const *,
		                    bool, void const *) override;

		virtual void StretchBlt(int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, uint32_t gdTexFormat,
		                        uint32_t gdType, void const *, bool, void const *) override;

		virtual void BitBltAlpha(int32_t, int32_t, int32_t, int32_t, uint32_t gdTexFormat, uint32_t gdType,
		                         void const *, bool, void const *, uint32_t) override;

		virtual void StretchBltAlpha(int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, uint32_t gdTexFormat,
		                             uint32_t gdType, void const *, bool, void const *, uint32_t) override;

		virtual void BitBltAlphaModulate(int32_t, int32_t, int32_t, int32_t, uint32_t gdTexFormat, uint32_t gdType, void const *,
		                                 bool, void const *, uint32_t) override;

		virtual void StretchBltAlphaModulate(int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, uint32_t gdTexFormat,
		                                     uint32_t gdType, void const *, bool, void const *, uint32_t) override;

		virtual void SetViewport(int32_t x, int32_t y, int32_t width, int32_t height) override;

		virtual void SetViewport(void) override;

		virtual void GetViewport(int32_t dimensions[4]) override;

		virtual char const *GetDriverInfo(void) const override;

		virtual uint32_t GetGZCLSID(void) const override;

		virtual bool Init(void) override;

		virtual bool Shutdown(void) override;

		virtual bool IsDeviceReady(void) override;

		virtual bool Punt(uint32_t, void *) override;

	public:
		virtual char const *GetVertexBufferName(uint32_t gdVertexFormat) override;

		virtual uint32_t VertexBufferType(uint32_t) override;

		virtual uint32_t MaxVertices(uint32_t) override;

		virtual uint32_t GetVertices(int32_t, uint32_t) override;

		virtual uint32_t ContinueVertices(uint32_t, uint32_t) override;

		virtual void ReleaseVertices(uint32_t) override;

		virtual void DrawPrims(uint32_t, uint32_t gdPrimType, void *, uint32_t) override;

		virtual void DrawPrimsIndexed(uint32_t, uint32_t gdPrimType, uint32_t, uint16_t *) override;

		virtual void Reset(void) override;

	public:
		virtual bool BufferRegionEnabled(void) override;

		virtual uint32_t NewBufferRegion(int32_t gdBufferRegionType) override;

		virtual bool DeleteBufferRegion(int32_t bufferRegion) override;

		virtual bool ReadBufferRegion(uint32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t) override;

		virtual bool DrawBufferRegion(uint32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t) override;

		virtual bool IsBufferRegion(uint32_t bufferRegion) override;

		virtual bool CanDoPartialRegionWrites(void) override;

		virtual bool CanDoOffsetReads(void) override;

		virtual bool DeleteAllBufferRegions(void) override;

		virtual cIGZBuffer *CopyColorBuffer(int32_t, int32_t, int32_t, int32_t, cIGZBuffer *) override;

	public:
		virtual void EnableLighting(bool) override;

		virtual void EnableLight(uint32_t, bool) override;

		virtual void LightModelAmbient(float, float, float, float) override;

		virtual void LightColor(uint32_t, uint32_t, float const *) override;

		virtual void LightColor(uint32_t, float const *, float const *, float const *) override;

		virtual void LightPosition(uint32_t, float const *) override;

		virtual void LightDirection(uint32_t, float const *) override;

		virtual void MaterialColor(uint32_t, float const *) override;

		virtual void MaterialColor(float const *, float const *, float const *, float const *, float) override;
	};
}
