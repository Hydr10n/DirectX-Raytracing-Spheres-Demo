cmake_minimum_required(VERSION 3.28)

set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/Bin/$<CONFIG>")

set(project "RaytracingSpheres")

project(${project})

set(DXC_PATH "$ENV{VCPKG_ROOT}/packages/directx-dxc_x64-windows/tools/directx-dxc/" CACHE STRING "")

set(AMD_library_path "${CMAKE_CURRENT_SOURCE_DIR}/External/AMD/")

add_subdirectory("${AMD_library_path}MemoryAllocator/D3D12")

set(Intel_library_path "${CMAKE_CURRENT_SOURCE_DIR}/External/Intel/")

set(NVIDIA_library_path "${CMAKE_CURRENT_SOURCE_DIR}/External/NVIDIA/")

add_subdirectory("${NVIDIA_library_path}ShaderMake")

option(NRD_EMBEDS_DXBC_SHADERS "" OFF)
option(NRD_EMBEDS_SPIRV_SHADERS "" OFF)
add_subdirectory("${NVIDIA_library_path}NRD")

option(NRI_ENABLE_D3D11_SUPPORT "" OFF)
option(NRI_ENABLE_VK_SUPPORT "" OFF)
add_subdirectory("${NVIDIA_library_path}NRI")

add_subdirectory("${NVIDIA_library_path}RTXDI/rtxdi-sdk")

option(RTXMU_WITH_VULKAN "" OFF)
add_subdirectory("${NVIDIA_library_path}RTXMU")

option(STREAMLINE_FEATURE_DLSS_FG "" ON)
option(STREAMLINE_FEATURE_DLSS_SR "" ON)
option(STREAMLINE_FEATURE_NIS "" ON)
option(STREAMLINE_FEATURE_REFLEX "" ON)
add_subdirectory("${NVIDIA_library_path}Streamline")

file(GLOB modules "Source/*.ixx")
file(GLOB source "Source/*.cpp")
add_executable(${project} WIN32
	${source}
	"Source/App.rc"
	"Source/App.manifest"
	"${NVIDIA_library_path}NRD/Integration/NRDIntegration.hpp")
target_sources(${project} PRIVATE FILE_SET cxx_modules TYPE CXX_MODULES FILES ${modules})

set_target_properties(${project} PROPERTIES CXX_STANDARD 23)
set_target_properties(${project} PROPERTIES CXX_STANDARD_REQUIRED ON)

target_compile_definitions(${project} PRIVATE NOMINMAX)

set(AMD_CXX_include_directories "MemoryAllocator/D3D12/include")
list(TRANSFORM AMD_CXX_include_directories PREPEND ${AMD_library_path})

set(Intel_CXX_include_directories "XeSS/inc")
list(TRANSFORM Intel_CXX_include_directories PREPEND ${Intel_library_path})

set(NVIDIA_CXX_include_directories
	"MathLib"
	"NRD/Include" "NRD/Integration"
	"NRI/Include"
	"NVAPI"
	"RTXDI/rtxdi-sdk/include"
	"RTXMU/include"
	"Streamline/include")
list(TRANSFORM NVIDIA_CXX_include_directories PREPEND ${NVIDIA_library_path})

target_include_directories(${project} PRIVATE
	${AMD_CXX_include_directories}
	${Intel_CXX_include_directories}
	${NVIDIA_CXX_include_directories}
	${CMAKE_CURRENT_BINARY_DIR})

set(NVIDIA_shader_include_directories
	"MathLib"
	"NRD/Shaders/Include"
	"NVAPI"
	"RTXDI/rtxdi-sdk/include")
list(TRANSFORM NVIDIA_shader_include_directories PREPEND ${NVIDIA_library_path})

file(GLOB shaders CONFIGURE_DEPENDS "Shaders/*.hlsl")
include(ShaderCompilation.cmake)
CompileShaders(target ${project}_Shaders
	config ${CMAKE_CURRENT_SOURCE_DIR}/Shaders.cfg
	out ${CMAKE_CURRENT_BINARY_DIR}
	include_directories ${NVIDIA_shader_include_directories}
	source ${shaders}
	folder ${project}
	additional_options "--shaderModel 6_6 --hlsl2021")
add_dependencies(${project} ${project}_Shaders)

target_link_libraries(${project} PRIVATE
	D3D12MemoryAllocator
	NRD
	NRI
	rtxdi-sdk
	rtxmu
	streamline
	"${Intel_library_path}XeSS/lib/libxess.lib"
	"${NVIDIA_library_path}NVAPI/amd64/nvapi64.lib")

set(packages
	directxtex
	directxtk12
	imgui
	nlohmann_json
	unofficial-omniverse-physx-sdk)
foreach(package ${packages})
	find_package(${package} CONFIG REQUIRED)
endforeach()
target_link_libraries(${project} PRIVATE
	Microsoft::DirectXTex
	Microsoft::DirectXTK12
	imgui::imgui
	nlohmann_json::nlohmann_json
	unofficial::omniverse-physx-sdk::sdk)

add_custom_command(TARGET ${project} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
	"${Intel_library_path}XeSS/bin/libxess.dll"
	"${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")

add_custom_command(TARGET ${project} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
	"${CMAKE_CURRENT_SOURCE_DIR}/Assets"
	"${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Assets")
