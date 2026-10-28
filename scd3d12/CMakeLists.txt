# SCD3D12Server: the 64-bit Direct3D 12 renderer SCD3D11 hands SimCity 4's drawing to.
# Configured from the top-level CMakeLists.txt with a 64-bit compiler (preset server-release-x64).

set(SCD3D11_SOURCE "${PROJECT_SOURCE_DIR}/scd3d11")

add_executable(SCD3D12Server
        src/ServerMain.cpp
        src/Serve.cpp
        "${SCD3D11_SOURCE}/wire/SharedRing.cpp"
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
target_link_libraries(SCD3D12Server PRIVATE d3d12 dxgi dxguid d3dcompiler dcomp windowscodecs ole32)
target_link_options(SCD3D12Server PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/ENTRY:wmainCRTStartup>)
set_target_properties(SCD3D12Server PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# DBPFKit reads the formats. It builds as C++23 in its own directory, and only the targets that include its
# headers need C++23 too. EXCLUDE_FROM_ALL keeps its tests and Catch2 out of the normal build.
add_subdirectory("${PROJECT_SOURCE_DIR}/vendor/DBPFKit" "${CMAKE_BINARY_DIR}/DBPFKit" EXCLUDE_FROM_ALL)

# Reads SimCity 4's own data: every package in the game's load order, and models and textures by TGI.
add_library(scd3d12_assets STATIC
        src/assets/Dbpf.cpp
        src/assets/AssetStore.cpp
        src/assets/Models.cpp
        src/assets/Textures.cpp
)
target_include_directories(scd3d12_assets PUBLIC src/assets)
target_link_libraries(scd3d12_assets PRIVATE DBPFKitLib)
target_compile_features(scd3d12_assets PRIVATE cxx_std_23)
target_compile_definitions(scd3d12_assets PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(scd3d12_assets PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/W3;/utf-8;/MP>)

