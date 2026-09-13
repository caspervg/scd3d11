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

project(SCD3D11 LANGUAGES C CXX RC)

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

# Limited to C++ sources so none of these reach the resource compiler.
target_compile_options(scd3d11_common INTERFACE
        $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/W3;/utf-8;/MP>
        $<$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>:-Wall;-fpermissive>
        $<$<AND:$<COMPILE_LANG_AND_ID:CXX,MSVC>,$<CONFIG:Release>>:/Gw;/Zc:threadSafeInit->
        $<$<AND:$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>,$<CONFIG:Release>>:-flto>
)

target_compile_definitions(scd3d11_common INTERFACE
        $<$<AND:$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>,$<CONFIG:Debug>>:__FUNCSIG__=__PRETTY_FUNCTION__>
)

# Everything but the .def, so the driver integration test can compile the same code.
set(SCD3D11_DRIVER_SOURCES
        scd3d11/cGDriver.cpp
        scd3d11/cGDriver_Geometry.cpp
        scd3d11/cGDriversCOMDirector.cpp
        scd3d11/CpuScheduling.cpp
        scd3d11/ParallelRenderCull.cpp
        scd3d11/SimTickBudget.cpp
        scd3d11/cGDriver_Info.cpp
        scd3d11/cGDriver_Init.cpp
        scd3d11/cGDriver_State.cpp
        scd3d11/cGDriver_Textures.cpp
        scd3d11/cGDriver_Unknown.cpp
        scd3d11/cGDriver_Blit.cpp
        scd3d11/cGDriver_ReShade.cpp
        scd3d11/cGDriver_Viewport.cpp
        scd3d11/D3D11Conversions.cpp
        scd3d11/Diagnostics.cpp
        scd3d11/SCD3D11Service.cpp
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

add_library(SCD3D11 SHARED scd3d11/SCD3D11.def ${SCD3D11_DRIVER_SOURCES})

# Stamped into the DLL's version resource. CI passes the release tag, e.g. v1.2.0 or v1.2.0-rc1.
# Not SCD3D11_VERSION: project() already defines that as an empty variable, which would shadow the cache entry.
set(SCD3D11_VERSION_TAG "0.0.0-dev" CACHE STRING "Version stamped into SCD3D11.dll ([v]MAJOR.MINOR.PATCH[-suffix])")
if (NOT SCD3D11_VERSION_TAG MATCHES "^v?([0-9]+)\\.([0-9]+)\\.([0-9]+)")
    message(FATAL_ERROR "SCD3D11_VERSION_TAG must start with MAJOR.MINOR.PATCH, got '${SCD3D11_VERSION_TAG}'")
endif ()
set(SCD3D11_VERSION_MAJOR ${CMAKE_MATCH_1})
set(SCD3D11_VERSION_MINOR ${CMAKE_MATCH_2})
set(SCD3D11_VERSION_PATCH ${CMAKE_MATCH_3})
string(REGEX REPLACE "^v" "" SCD3D11_VERSION_STRING "${SCD3D11_VERSION_TAG}")
configure_file(scd3d11/SCD3D11.rc.in "${PROJECT_BINARY_DIR}/SCD3D11.rc" @ONLY)
target_sources(SCD3D11 PRIVATE "${PROJECT_BINARY_DIR}/SCD3D11.rc")

# SC4 loads the driver as SCD3D11.dll, so suppress CMake's "lib" prefix.
set_target_properties(SCD3D11 PROPERTIES PREFIX "")

set(SCD3D11_DRIVER_INCLUDES
        "${PROJECT_SOURCE_DIR}/vendor/framework/include"
        "${PROJECT_SOURCE_DIR}/vendor/reshade/include")
target_include_directories(SCD3D11 PRIVATE ${SCD3D11_DRIVER_INCLUDES})

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

    scd3d11_add_test(ReShadeShaderCompileTest tests/D3D11ShaderCompileTest.cpp)
    target_compile_definitions(ReShadeShaderCompileTest PRIVATE
            SC4D3D11_GEOMETRY_SOURCE="${PROJECT_SOURCE_DIR}/scd3d11/cGDriver_ReShade.cpp")
    target_link_libraries(ReShadeShaderCompileTest PRIVATE d3dcompiler)

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

    scd3d11_add_test(CpuSchedulingTest
            tests/CpuSchedulingTest.cpp
            scd3d11/CpuScheduling.cpp
            scd3d11/Diagnostics.cpp)

    # Drives a real cGDriver on a hidden window. Built from source instead of reusing the DLL objects:
    # the test must not see NDEBUG, and cGDriver's layout depends on it.
    scd3d11_add_test(cGDriverTest tests/cGDriverTest.cpp ${SCD3D11_DRIVER_SOURCES})
    target_include_directories(cGDriverTest PRIVATE ${SCD3D11_DRIVER_INCLUDES})
    target_link_libraries(cGDriverTest PRIVATE d3d11 d3dcompiler dxgi version)
    # No usable D3D11 device skips instead of failing.
    set_tests_properties(cGDriverTest PROPERTIES SKIP_RETURN_CODE 77)
endif ()
