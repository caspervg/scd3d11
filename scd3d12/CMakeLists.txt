# SCD3D12Server: the 64-bit Direct3D 12 renderer SCD3D11 hands SimCity 4's drawing to.
# Configured from the top-level CMakeLists.txt with a 64-bit compiler (preset server-release-x64).

set(SCD3D11_SOURCE "${PROJECT_SOURCE_DIR}/scd3d11")

add_executable(SCD3D12Server
        src/ServerMain.cpp
        src/Serve.cpp
        src/SceneLane.cpp
        src/RetainedScene.cpp
        src/Dlss.cpp
        src/DlssTest.cpp
        src/OwnedScene.cpp
        src/WorldLight.cpp
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
target_link_libraries(SCD3D12Server PRIVATE d3d12 dxgi dxguid d3dcompiler dcomp windowscodecs ole32 scd3d12_assets)
target_link_options(SCD3D12Server PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/ENTRY:wmainCRTStartup>)
set_target_properties(SCD3D12Server PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# NVIDIA Streamline for DLSS and frame generation (MIT, the DLSS runtimes under NVIDIA's own licence). Only its
# headers are compiled in: the server loads sl.interposer.dll itself when asked to, so it runs without the DLLs.
# The package is large, so point FETCHCONTENT_SOURCE_DIR_STREAMLINE at an unpacked copy to skip the download.
include(FetchContent)
FetchContent_Declare(streamline
        URL https://github.com/NVIDIA-RTX/Streamline/releases/download/v2.14.16/streamline-sdk-v2.14.16.zip
        URL_HASH SHA256=32041E51DB0CAB709A271B72A352281B690C2ABBD669A827809401A5DD23956B
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        # Only the files are wanted, not the package's own CMakeLists.txt.
        SOURCE_SUBDIR none)
FetchContent_MakeAvailable(streamline)
target_include_directories(SCD3D12Server PRIVATE "${streamline_SOURCE_DIR}/include")
target_link_libraries(SCD3D12Server PRIVATE wintrust crypt32)
# Next to the server, where it looks for them. In the game that is Plugins\SCD3D12\, which SC4 loads no DLLs from.
set(STREAMLINE_RUNTIME
        sl.interposer.dll sl.common.dll sl.dlss.dll sl.dlss_g.dll sl.reflex.dll sl.pcl.dll
        nvngx_dlss.dll nvngx_dlssg.dll nvngx_dlss.license.txt reflex.license.txt)
list(TRANSFORM STREAMLINE_RUNTIME PREPEND "${streamline_SOURCE_DIR}/bin/x64/")
add_custom_command(TARGET SCD3D12Server POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${STREAMLINE_RUNTIME} "$<TARGET_FILE_DIR:SCD3D12Server>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${streamline_SOURCE_DIR}/license.txt"
                "$<TARGET_FILE_DIR:SCD3D12Server>/streamline.license.txt")

# The world light's shaders use ray queries (shader model 6.5), which only DXC compiles. The Windows SDK's dxc.exe
# turns them into headers at build time, so the server needs no compiler next to it. Without DXC the server
# builds without them and keeps the screen-space shadow trace.
set(SCD3D12_PROGRAM_FILES_X86 "ProgramFiles(x86)")
file(GLOB SCD3D12_SDK_DXC "$ENV{${SCD3D12_PROGRAM_FILES_X86}}/Windows Kits/10/bin/10.*/x64/dxc.exe")
list(SORT SCD3D12_SDK_DXC COMPARE NATURAL ORDER DESCENDING)
set(SCD3D12_SDK_BIN)
if (SCD3D12_SDK_DXC)
    list(GET SCD3D12_SDK_DXC 0 SCD3D12_SDK_DXC_NEWEST)
    get_filename_component(SCD3D12_SDK_BIN "${SCD3D12_SDK_DXC_NEWEST}" DIRECTORY)
endif ()
find_program(SCD3D12_DXC dxc HINTS ${SCD3D12_SDK_BIN})
if (SCD3D12_DXC)
    set(SCD3D12_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
    set(SCD3D12_WORLD_LIGHT_HLSL "${CMAKE_CURRENT_SOURCE_DIR}/src/WorldLight.hlsl")
    set(SCD3D12_SHADER_HEADERS)
    foreach (entry FullScreenVS:vs_6_5 TraceSunPS:ps_6_5 EmissiveCS:cs_6_5)
        string(REPLACE ":" ";" parts "${entry}")
        list(GET parts 0 name)
        list(GET parts 1 profile)
        set(header "${SCD3D12_SHADER_DIR}/${name}.h")
        add_custom_command(OUTPUT "${header}"
                COMMAND "${CMAKE_COMMAND}" -E make_directory "${SCD3D12_SHADER_DIR}"
                COMMAND "${SCD3D12_DXC}" -nologo -O3 -T ${profile} -E ${name} -Vn k${name} -Fh "${header}"
                        "${SCD3D12_WORLD_LIGHT_HLSL}"
                DEPENDS "${SCD3D12_WORLD_LIGHT_HLSL}"
                COMMENT "DXC ${name}")
        list(APPEND SCD3D12_SHADER_HEADERS "${header}")
    endforeach ()
    target_sources(SCD3D12Server PRIVATE ${SCD3D12_SHADER_HEADERS})
    target_include_directories(SCD3D12Server PRIVATE "${SCD3D12_SHADER_DIR}")
    target_compile_definitions(SCD3D12Server PRIVATE SCD3D12_RAY_SHADERS)
else ()
    message(STATUS "SCD3D12Server: no dxc.exe, ray traced shadows left out")
endif ()

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

# Indexes folders, dumps entries and exports models and textures, for checking the readers on real data.
add_executable(SCD3D12Assets
        tools/AssetsTool.cpp
        "${SCD3D11_SOURCE}/wire/ImageFile.cpp"
)
target_include_directories(SCD3D12Assets PRIVATE "${SCD3D11_SOURCE}/wire")
target_compile_definitions(SCD3D12Assets PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
target_compile_options(SCD3D12Assets PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/W3;/utf-8;/MP>)
target_link_libraries(SCD3D12Assets PRIVATE scd3d12_assets windowscodecs ole32)
set_target_properties(SCD3D12Assets PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# Checks the asset readers. Pass the game folder (and a Plugins folder) to run it on real data.
add_executable(AssetsTest tests/AssetsTest.cpp)
target_compile_definitions(AssetsTest PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(AssetsTest PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/W3;/utf-8;/MP>)
target_link_libraries(AssetsTest PRIVATE scd3d12_assets)
set_target_properties(AssetsTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
