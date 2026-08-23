# Can only cross-compile on non-Windows for obvious reasons, so enforce.
if(CMAKE_HOST_UNIX)
	set(CMAKE_SYSTEM_NAME "Windows")
	set(CMAKE_SYSTEM_VERSION "10.0")
	set(CMAKE_FIND_ROOT_PATH /usr/i686-w64-mingw32)

	# Use host binaries but target libraries and headers.
	set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)

	set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
	set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
endif()

set(CMAKE_SYSTEM_PROCESSOR "x86")

cmake_minimum_required(VERSION 3.22)
project(SCGL LANGUAGES C CXX)

if(NOT CMAKE_HOST_WIN32 AND NOT CMAKE_CROSSCOMPILING)
	message(FATAL_ERROR "SCGL targets SimCity 4, a 32-bit Windows application, and must be cross-compiled.")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
	message(FATAL_ERROR "You're using a compiler that generates 64-bit code. SCGL must be 32-bit.")
endif()

add_library(SCGL SHARED
            scgl/cGDriver.cpp
            scgl/cGDriver_Geometry.cpp
            scgl/cGDriversCOMDirector.cpp
            scgl/cGDriver_Info.cpp
            scgl/cGDriver_Init.cpp
            scgl/cGDriver_State.cpp
            scgl/cGDriver_Textures.cpp
            scgl/cGDriver_Unknown.cpp
            scgl/cGDriver_Viewport.cpp
            scgl/D3D11Conversions.cpp
            scgl/Diagnostics.cpp
            scgl/VideoModeUtils.cpp
            scgl/ext/cGDriver_BufferRegions.cpp
            scgl/ext/cGDriver_Lighting.cpp
            scgl/ext/cGDriver_Snapshot.cpp
            scgl/ext/cGDriver_VertexBuffers.cpp
            scgl/VertexFormatUtils.cpp
            vendor/framework/src/cRZCOMDllDirector.cpp
            vendor/framework/src/cRZRefCount.cpp
)

set_target_properties(SCGL PROPERTIES PREFIX "")

target_include_directories(SCGL PRIVATE "${CMAKE_SOURCE_DIR}/vendor/framework/include;${CMAKE_SOURCE_DIR}/scgl;${CMAKE_SOURCE_DIR}/scgl/ext")

# Flags for debug builds.
target_compile_options(SCGL PRIVATE
                       $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Debug>>:/W3 /utf-8 /MP>
                       $<$<AND:$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:GNU>>,$<CONFIG:Debug>>:-Wall -fpermissive>
)

# For release
target_compile_options(SCGL PRIVATE
                       $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>>:/W3 /utf-8 /Gw /Zc:threadSafeInit- /O2>
                       $<$<AND:$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:GNU>>,$<CONFIG:Release>>:-Wall -fpermissive -O3 -flto>
)

# Definitions
target_compile_definitions(SCGL PRIVATE
                           $<$<AND:$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>,$<CONFIG:Debug>>:__FUNCSIG__=__PRETTY_FUNCTION__>
)

# Should come with MinGW
target_link_libraries(SCGL d3d11.lib d3dcompiler.lib dxgi.lib)

include(CTest)
if(BUILD_TESTING)
	add_executable(D3D11ConversionsTest tests/D3D11ConversionsTest.cpp scgl/D3D11Conversions.cpp scgl/VertexFormatUtils.cpp)
	target_include_directories(D3D11ConversionsTest PRIVATE "${CMAKE_SOURCE_DIR}/scgl")
	add_test(NAME D3D11ConversionsTest COMMAND D3D11ConversionsTest)

	add_executable(D3D11ShaderCompileTest tests/D3D11ShaderCompileTest.cpp)
	target_compile_definitions(D3D11ShaderCompileTest PRIVATE SC4D3D11_GEOMETRY_SOURCE="${CMAKE_SOURCE_DIR}/scgl/cGDriver_Geometry.cpp")
	target_link_libraries(D3D11ShaderCompileTest d3dcompiler.lib)
	add_test(NAME D3D11ShaderCompileTest COMMAND D3D11ShaderCompileTest)

	add_executable(D3D11DepthRegionCopyTest tests/D3D11DepthRegionCopyTest.cpp)
	target_link_libraries(D3D11DepthRegionCopyTest d3d11.lib)
	add_test(NAME D3D11DepthRegionCopyTest COMMAND D3D11DepthRegionCopyTest)

	add_executable(VideoModeUtilsTest tests/VideoModeUtilsTest.cpp scgl/VideoModeUtils.cpp)
	target_include_directories(VideoModeUtilsTest PRIVATE "${CMAKE_SOURCE_DIR}/scgl")
	add_test(NAME VideoModeUtilsTest COMMAND VideoModeUtilsTest)
endif()
