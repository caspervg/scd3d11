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

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (NOT CMAKE_HOST_WIN32 AND NOT CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "SCD3D11 targets SimCity 4, a 32-bit Windows application, and must be cross-compiled.")
endif ()

if (NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "You're using a compiler that generates 64-bit code. SCD3D11 must be 32-bit.")
endif ()

# gzcom-dll and sc4-dll-utilities (with the WIL headers it needs), wired up as in
# sc4-dll-template. They are git submodules.
set(GZCOM_DIR "${PROJECT_SOURCE_DIR}/vendor/gzcom-dll/gzcom-dll")
set(SC4_DLL_UTILITIES_DIR "${PROJECT_SOURCE_DIR}/vendor/sc4-dll-utilities/sc4-dll-utilities")
set(WIL_INCLUDE_DIR "${PROJECT_SOURCE_DIR}/vendor/wil/include")
foreach (required "${GZCOM_DIR}/include" "${SC4_DLL_UTILITIES_DIR}/include" "${WIL_INCLUDE_DIR}/wil")
    if (NOT EXISTS "${required}")
        message(FATAL_ERROR "Missing ${required}. Initialize submodules with: git submodule update --init")
    endif ()
endforeach ()

file(GLOB GZCOM_SOURCES CONFIGURE_DEPENDS "${GZCOM_DIR}/src/*.cpp")
list(REMOVE_ITEM GZCOM_SOURCES
        "${GZCOM_DIR}/src/main.cpp"
        "${GZCOM_DIR}/src/cGZExampleDllDirector.cpp"
        "${GZCOM_DIR}/src/cGZExtraCheatsDllDirector.cpp"
        "${GZCOM_DIR}/src/cGZExtraExtraCheatsDllDirector.cpp"
        "${GZCOM_DIR}/src/cGZCityHallUpgradeDllDirector.cpp")
add_library(gzcom2 STATIC ${GZCOM_SOURCES})
target_include_directories(gzcom2 PUBLIC "${GZCOM_DIR}/include")

# Its Logger is left out: SCD3D11 logs from before SC4's application exists and from its watchdog
# thread, which Diagnostics.cpp handles.
file(GLOB SC4_DLL_UTILITIES_SOURCES CONFIGURE_DEPENDS "${SC4_DLL_UTILITIES_DIR}/src/*.cpp")
list(REMOVE_ITEM SC4_DLL_UTILITIES_SOURCES "${SC4_DLL_UTILITIES_DIR}/src/Logger.cpp")
add_library(sc4_dll_utilities STATIC ${SC4_DLL_UTILITIES_SOURCES})
target_include_directories(sc4_dll_utilities PUBLIC "${SC4_DLL_UTILITIES_DIR}/include" "${WIL_INCLUDE_DIR}")
target_compile_definitions(sc4_dll_utilities PUBLIC NOMINMAX)
target_link_libraries(sc4_dll_utilities PUBLIC gzcom2 version)

# Settings shared by the driver and its tests, kept on one INTERFACE target so the two cannot
# drift apart. These apply to every configuration on purpose: a MinSizeRel or RelWithDebInfo
# build needs /utf-8 and the warning level just as much as Debug and Release do. Optimisation
# flags are deliberately absent, since CMake's own per-configuration defaults supply them.
add_library(scd3d11_common INTERFACE)

target_include_directories(scd3d11_common INTERFACE
        "${PROJECT_SOURCE_DIR}/scd3d11"
        "${PROJECT_SOURCE_DIR}/scd3d11/ext"
        "${PROJECT_SOURCE_DIR}/vendor/xxhash"
)
target_link_libraries(scd3d11_common INTERFACE sc4_dll_utilities)

# Limited to C++ sources so none of these reach the resource compiler.
target_compile_options(scd3d11_common INTERFACE
        $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/W3;/utf-8;/MP>
        $<$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>:-Wall;-fpermissive>
        $<$<AND:$<COMPILE_LANG_AND_ID:CXX,MSVC>,$<CONFIG:Release,RelWithDebInfo>>:/Gw;/Zc:threadSafeInit->
        $<$<AND:$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>,$<CONFIG:Release,RelWithDebInfo>>:-flto>
)

# RelWithDebInfo is the profiling build, so it must generate the same code as Release plus a PDB.
# CMake's MSVC defaults differ in two ways that would skew a profile: /Ob1 inlines only functions
# marked inline, and /INCREMENTAL (implied by /DEBUG, as is dropping /OPT:REF and /OPT:ICF) pads
# and keeps every function.
if (MSVC)
    foreach (language C CXX)
        string(REPLACE "/Ob1" "/Ob2" CMAKE_${language}_FLAGS_RELWITHDEBINFO "${CMAKE_${language}_FLAGS_RELWITHDEBINFO}")
    endforeach ()
    foreach (kind SHARED EXE)
        string(REGEX REPLACE "/INCREMENTAL( |$)" "/INCREMENTAL:NO\\1"
                CMAKE_${kind}_LINKER_FLAGS_RELWITHDEBINFO "${CMAKE_${kind}_LINKER_FLAGS_RELWITHDEBINFO}")
        string(APPEND CMAKE_${kind}_LINKER_FLAGS_RELWITHDEBINFO " /OPT:REF /OPT:ICF")
    endforeach ()
endif ()

target_compile_definitions(scd3d11_common INTERFACE
        $<$<AND:$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>,$<CONFIG:Debug>>:__FUNCSIG__=__PRETTY_FUNCTION__>
)

# Everything but the .def, so the driver integration test can compile the same code.
set(SCD3D11_DRIVER_SOURCES
        scd3d11/cGDriver.cpp
        scd3d11/cGDriver_Geometry.cpp
        scd3d11/cGDriver_LiveShadows.cpp
        scd3d11/cGDriversCOMDirector.cpp
        scd3d11/LiveShadowHooks.cpp
        scd3d11/ShadowRegistry.cpp
        scd3d11/TerrainShadowMap.cpp
        scd3d11/TerrainShadows.cpp
        scd3d11/ThumbnailFocusGuard.cpp
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
        scd3d11/Doctor.cpp
        scd3d11/DoctorScan.cpp
        scd3d11/SCD3D11Service.cpp
        scd3d11/Settings.cpp
        scd3d11/TextureUploadUtils.cpp
        scd3d11/VideoModeUtils.cpp
        scd3d11/VertexFormatUtils.cpp
        scd3d11/ViewportCull.cpp
        scd3d11/ext/cGDriver_BufferRegions.cpp
        scd3d11/ext/cGDriver_Lighting.cpp
        scd3d11/ext/cGDriver_Snapshot.cpp
        scd3d11/ext/cGDriver_VertexBuffers.cpp
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

# Release builds also write SCD3D11.pdb, which is published with each release so crash reports can be
# matched to functions. /DEBUG turns off /OPT:REF and /OPT:ICF unless they are asked for again, and /FS
# lets parallel compiles share the PDB.
target_compile_options(SCD3D11 PRIVATE $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>>:/Zi;/FS>)
target_link_options(SCD3D11 PRIVATE $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>>:/DEBUG;/OPT:REF;/OPT:ICF>)

# SC4 loads the driver as SCD3D11.dll, so suppress CMake's "lib" prefix.
set_target_properties(SCD3D11 PROPERTIES PREFIX "")

set(SCD3D11_DRIVER_INCLUDES "${PROJECT_SOURCE_DIR}/vendor/reshade/include")
target_include_directories(SCD3D11 PRIVATE ${SCD3D11_DRIVER_INCLUDES})

# Unsuffixed names so the same list resolves under both MSVC and the MinGW cross-toolchain.
target_link_libraries(SCD3D11 PRIVATE scd3d11_common d3d11 d3dcompiler dxgi version wininet advapi32)

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

    scd3d11_add_test(GeometryCacheTest
            tests/GeometryCacheTest.cpp
            scd3d11/D3D11Conversions.cpp
            scd3d11/VertexFormatUtils.cpp)
    target_link_libraries(GeometryCacheTest PRIVATE d3d11)

    scd3d11_add_test(D3D11ShaderCompileTest tests/D3D11ShaderCompileTest.cpp)
    target_compile_definitions(D3D11ShaderCompileTest PRIVATE
            SC4D3D11_GEOMETRY_SOURCE="${PROJECT_SOURCE_DIR}/scd3d11/cGDriver_Geometry.cpp")
    target_link_libraries(D3D11ShaderCompileTest PRIVATE d3dcompiler)

    scd3d11_add_test(ReShadeShaderCompileTest tests/D3D11ShaderCompileTest.cpp)
    target_compile_definitions(ReShadeShaderCompileTest PRIVATE
            SC4D3D11_GEOMETRY_SOURCE="${PROJECT_SOURCE_DIR}/scd3d11/cGDriver_ReShade.cpp")
    target_link_libraries(ReShadeShaderCompileTest PRIVATE d3dcompiler)

    scd3d11_add_test(LiveShadowShaderCompileTest tests/D3D11ShaderCompileTest.cpp)
    target_compile_definitions(LiveShadowShaderCompileTest PRIVATE
            SC4D3D11_GEOMETRY_SOURCE="${PROJECT_SOURCE_DIR}/scd3d11/cGDriver_LiveShadows.cpp"
            SC4D3D11_SHADER_VARIABLE="kLiveShadowShader"
            SC4D3D11_SHADER_ENTRIES="CasterVS=vs_4_0,CasterPS=ps_4_0,FullscreenVS=vs_4_0,CompositePS=ps_4_0,RegistryVS=vs_4_0,RegistryPS=ps_4_0")
    target_link_libraries(LiveShadowShaderCompileTest PRIVATE d3dcompiler)

    scd3d11_add_test(ShadowRegistryTest
            tests/ShadowRegistryTest.cpp
            scd3d11/ShadowRegistry.cpp
            scd3d11/LiveShadowHooks.cpp
            scd3d11/Diagnostics.cpp
            scd3d11/Settings.cpp)
    target_link_libraries(ShadowRegistryTest PRIVATE version)

    scd3d11_add_test(SettingsTest
            tests/SettingsTest.cpp
            scd3d11/Settings.cpp
            scd3d11/Diagnostics.cpp)

    scd3d11_add_test(DoctorTest
            tests/DoctorTest.cpp
            scd3d11/Doctor.cpp)

    scd3d11_add_test(TerrainShadowMapTest
            tests/TerrainShadowMapTest.cpp
            scd3d11/TerrainShadowMap.cpp)

    scd3d11_add_test(D3D11DepthRegionCopyTest tests/D3D11DepthRegionCopyTest.cpp)
    target_link_libraries(D3D11DepthRegionCopyTest PRIVATE d3d11)

    scd3d11_add_test(VideoModeUtilsTest
            tests/VideoModeUtilsTest.cpp
            scd3d11/VideoModeUtils.cpp)

    scd3d11_add_test(ViewportCullTest
            tests/ViewportCullTest.cpp
            scd3d11/ViewportCull.cpp)

    scd3d11_add_test(TextureUploadUtilsTest
            tests/TextureUploadUtilsTest.cpp
            scd3d11/TextureUploadUtils.cpp)

    scd3d11_add_test(SCD3D11ServiceTest
            tests/SCD3D11ServiceTest.cpp
            scd3d11/SCD3D11Service.cpp
            scd3d11/Diagnostics.cpp)

    # Drives a real cGDriver on a hidden window. Built from source instead of reusing the DLL objects:
    # the test must not see NDEBUG, and cGDriver's layout depends on it.
    scd3d11_add_test(cGDriverTest tests/cGDriverTest.cpp ${SCD3D11_DRIVER_SOURCES})
    target_include_directories(cGDriverTest PRIVATE ${SCD3D11_DRIVER_INCLUDES})
    # Enables the fault injection points the tests use.
    target_compile_definitions(cGDriverTest PRIVATE SCD3D11_TESTING)
    target_link_libraries(cGDriverTest PRIVATE d3d11 d3dcompiler dxgi version wininet advapi32)
    # No usable D3D11 device skips instead of failing.
    set_tests_properties(cGDriverTest PROPERTIES SKIP_RETURN_CODE 77)
endif ()
