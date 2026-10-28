# SCD3D12Server: the 64-bit Direct3D 12 renderer SCD3D11 hands SimCity 4's drawing to.
# Configured from the top-level CMakeLists.txt with a 64-bit compiler (preset server-release-x64).

set(SCD3D11_SOURCE "${PROJECT_SOURCE_DIR}/scd3d11")

add_executable(SCD3D12Server
        src/ServerMain.cpp
        src/cGDriver12.cpp
        "${SCD3D11_SOURCE}/wire/ImageFile.cpp"
        "${SCD3D11_SOURCE}/wire/WireReplay.cpp"
        "${SCD3D11_SOURCE}/wire/WireStream.cpp"
        "${SCD3D11_SOURCE}/D3D11Conversions.cpp"
        "${SCD3D11_SOURCE}/TextureUploadUtils.cpp"
        "${SCD3D11_SOURCE}/VertexFormatUtils.cpp"
)
target_include_directories(SCD3D12Server PRIVATE
        src
        "${SCD3D11_SOURCE}"
        "${SCD3D11_SOURCE}/ext"
        "${GZCOM_DIR}/include"
        "${PROJECT_SOURCE_DIR}/vendor/xxhash"
)
target_compile_definitions(SCD3D12Server PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(SCD3D12Server PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/W3;/utf-8;/MP>)
target_link_libraries(SCD3D12Server PRIVATE d3d12 dxgi dxguid d3dcompiler windowscodecs ole32)
target_link_options(SCD3D12Server PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/ENTRY:wmainCRTStartup>)
set_target_properties(SCD3D12Server PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
