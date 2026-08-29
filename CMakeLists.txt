cmake_minimum_required(VERSION 3.22)

# Toolchain selection has to happen before project(), which is what probes the compiler.
# Cross-compiling is only meaningful from a non-Windows host.
if (CMAKE_HOST_UNIX)
    set(CMAKE_SYSTEM_NAME "Windows")
    set(CMAKE_SYSTEM_VERSION "10.0")
    set(CMAKE_FIND_ROOT_PATH /usr/i686-w64-mingw32)

    # Use host binaries but target libraries and headers.
    set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
    set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
    set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
endif ()

set(CMAKE_SYSTEM_PROCESSOR "x86")

project(SCD3D11 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (NOT CMAKE_HOST_WIN32 AND NOT CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "SCD3D11 targets SimCity 4, a 32-bit Windows application, and must be cross-compiled.")
endif ()

if (NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "You're using a compiler that generates 64-bit code. SCD3D11 must be 32-bit.")
endif ()

# Settings shared by the driver and its tests, kept on one INTERFACE target so the two cannot
# drift apart. These apply to every configuration on purpose: a MinSizeRel or RelWithDebInfo
# build needs /utf-8 and the warning level just as much as Debug and Release do. Optimisation
# flags are deliberately absent, since CMake's own per-configuration defaults supply them.
add_library(scd3d11_common INTERFACE)

target_include_directories(scd3d11_common INTERFACE
        "${PROJECT_SOURCE_DIR}/scd3d11"
        "${PROJECT_SOURCE_DIR}/scd3d11/ext"
)

target_compile_options(scd3d11_common INTERFACE
        $<$<CXX_COMPILER_ID:MSVC>:/W3;/utf-8;/MP>
        $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:GNU>>:-Wall;-fpermissive>
        $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>>:/Gw;/Zc:threadSafeInit->
        $<$<AND:$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:GNU>>,$<CONFIG:Release>>:-flto>
)

target_compile_definitions(scd3d11_common INTERFACE
        $<$<AND:$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>,$<CONFIG:Debug>>:__FUNCSIG__=__PRETTY_FUNCTION__>
)

add_library(SCD3D11 SHARED
        scd3d11/SCD3D11.def
        scd3d11/cGDriver.cpp
        scd3d11/cGDriver_Geometry.cpp
        scd3d11/cGDriversCOMDirector.cpp
        scd3d11/ParallelRenderCull.cpp
        scd3d11/cGDriver_Info.cpp
        scd3d11/cGDriver_Init.cpp
        scd3d11/cGDriver_State.cpp
        scd3d11/cGDriver_Textures.cpp
        scd3d11/cGDriver_Unknown.cpp
        scd3d11/cGDriver_Viewport.cpp
        scd3d11/D3D11Conversions.cpp
        scd3d11/Diagnostics.cpp
        scd3d11/SCD3D11Service.cpp
        scd3d11/StartupOverlay.cpp
        scd3d11/StartupProgress.cpp
        scd3d11/StartupResourceLoadPatches.cpp
        scd3d11/TextureUploadUtils.cpp
        scd3d11/VideoModeUtils.cpp
        scd3d11/VertexFormatUtils.cpp
        scd3d11/ext/cGDriver_BufferRegions.cpp
        scd3d11/ext/cGDriver_Lighting.cpp
        scd3d11/ext/cGDriver_Snapshot.cpp
        scd3d11/ext/cGDriver_VertexBuffers.cpp
        vendor/framework/src/cRZCOMDllDirector.cpp
        vendor/framework/src/cRZRefCount.cpp
)

# SC4 loads the driver as SCD3D11.dll, so suppress CMake's "lib" prefix.
set_target_properties(SCD3D11 PROPERTIES PREFIX "")

target_include_directories(SCD3D11 PRIVATE "${PROJECT_SOURCE_DIR}/vendor/framework/include")

# Unsuffixed names so the same list resolves under both MSVC and the MinGW cross-toolchain.
target_link_libraries(SCD3D11 PRIVATE scd3d11_common d3d11 d3dcompiler dxgi version)

include(CTest)
if (BUILD_TESTING)
    # The tests are driven by assert(), so NDEBUG must never reach them.
    function(scd3d11_add_test name)
        add_executable(${name} ${ARGN})
        target_link_libraries(${name} PRIVATE scd3d11_common)
        target_compile_options(${name} PRIVATE
                $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG>
                $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-UNDEBUG>)
        add_test(NAME ${name} COMMAND ${name})
    endfunction()

    scd3d11_add_test(D3D11ConversionsTest
            tests/D3D11ConversionsTest.cpp
            scd3d11/D3D11Conversions.cpp
            scd3d11/VertexFormatUtils.cpp)
    target_link_libraries(D3D11ConversionsTest PRIVATE d3d11)

    scd3d11_add_test(D3D11ShaderCompileTest tests/D3D11ShaderCompileTest.cpp)
    target_compile_definitions(D3D11ShaderCompileTest PRIVATE
            SC4D3D11_GEOMETRY_SOURCE="${PROJECT_SOURCE_DIR}/scd3d11/cGDriver_Geometry.cpp")
    target_link_libraries(D3D11ShaderCompileTest PRIVATE d3dcompiler)

    scd3d11_add_test(D3D11DepthRegionCopyTest tests/D3D11DepthRegionCopyTest.cpp)
    target_link_libraries(D3D11DepthRegionCopyTest PRIVATE d3d11)

    scd3d11_add_test(VideoModeUtilsTest
            tests/VideoModeUtilsTest.cpp
            scd3d11/VideoModeUtils.cpp)

    scd3d11_add_test(TextureUploadUtilsTest
            tests/TextureUploadUtilsTest.cpp
            scd3d11/TextureUploadUtils.cpp)

    scd3d11_add_test(SCD3D11ServiceTest
            tests/SCD3D11ServiceTest.cpp
            scd3d11/SCD3D11Service.cpp
            scd3d11/Diagnostics.cpp)

    scd3d11_add_test(StartupProgressTest
            tests/StartupProgressTest.cpp
            scd3d11/StartupProgress.cpp)
endif ()
