cmake_minimum_required(VERSION 3.28)

set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/Bin/$<CONFIG>")

set(project "RaytracingSpheres")

project(${project})

set(AMD_library_path "${CMAKE_CURRENT_SOURCE_DIR}/External/AMD/")

set(FFX_FSR2_API_VK OFF CACHE BOOL "")
add_subdirectory("${AMD_library_path}FSR2/src/ffx-fsr2-api")

set(NVIDIA_library_path "${CMAKE_CURRENT_SOURCE_DIR}/External/NVIDIA/")

add_subdirectory("${NVIDIA_library_path}ShaderMake")

set(NRD_EMBEDS_DXBC_SHADERS OFF CACHE BOOL "")
set(NRD_EMBEDS_SPIRV_SHADERS OFF CACHE BOOL "")
file(GLOB NRD_integration_headers "${NVIDIA_library_path}*")
add_subdirectory("${NVIDIA_library_path}NRD")

set(NRI_ENABLE_D3D11_SUPPORT OFF CACHE BOOL "")
set(NRI_ENABLE_VK_SUPPORT OFF CACHE BOOL "")
add_subdirectory("${NVIDIA_library_path}NRI")

add_subdirectory("${NVIDIA_library_path}RTXDI/rtxdi-sdk")

set(RTXMU_WITH_VULKAN OFF CACHE BOOL "")
add_subdirectory("${NVIDIA_library_path}RTXMU")

file(GLOB include "Source/*.h")
file(GLOB modules "Source/*.ixx")
file(GLOB source "Source/*.cpp")
source_group("Include" FILES ${include})
source_group("Source" FILES ${source} ${modules})
add_executable(${project} WIN32 ${include} ${source} "Source/App.rc" "Source/App.manifest" ${NRD_integration_headers})
target_sources(${project} PRIVATE FILE_SET cxx_modules TYPE CXX_MODULES FILES ${modules})

set_target_properties(${project} PROPERTIES CXX_STANDARD 23)
set_target_properties(${project} PROPERTIES CXX_STANDARD_REQUIRED ON)

set_target_properties(${project} PROPERTIES FOLDER ${project})

set(cxx_include_directories
	"MathLib"
	"NRD/Include" "NRD/Integration"
	"NRI/Include"
	"RTXDI/rtxdi-sdk/include"
	"RTXMU/include"
	"Streamline/include")
list(TRANSFORM cxx_include_directories PREPEND ${NVIDIA_library_path})
target_include_directories(${project} PRIVATE ${cxx_include_directories} "${AMD_library_path}FSR2/src/ffx-fsr2-api" ${CMAKE_CURRENT_BINARY_DIR})

include(ShaderCompilation.cmake)
set(shader_include_directories "MathLib" "NRD/Shaders/Include" "RTXDI/rtxdi-sdk/include")
list(TRANSFORM shader_include_directories PREPEND ${NVIDIA_library_path})
file(GLOB shaders "Shaders/*.hlsl")
CompileShaders(target ${project}_Shaders
	config ${CMAKE_CURRENT_SOURCE_DIR}/Shaders.cfg
	out ${CMAKE_CURRENT_BINARY_DIR}
	include_directories ${shader_include_directories}
	source ${shaders}
	folder ${project}
	additional_options "--shaderModel 6_6 --hlsl2021")
add_dependencies(${project} ${project}_Shaders)

target_link_libraries(${project} PRIVATE ffx_fsr2_api_x64 ffx_fsr2_api_dx12_x64 NRD NRI rtxdi-sdk rtxmu "${NVIDIA_library_path}Streamline/lib/x64/sl.interposer.lib")

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

set(DLL
	"nvngx_dlss.dll"
	"sl.common.dll"
	"sl.dlss.dll"
	"sl.interposer.dll"
	"sl.nis.dll")
add_custom_command(TARGET ${project} POST_BUILD COMMAND ${CMAKE_COMMAND} COMMAND_EXPAND_LISTS
	-E copy_if_different
	"$<LIST:TRANSFORM,${DLL},PREPEND,${NVIDIA_library_path}Streamline/bin/x64/$<IF:$<CONFIG:Debug>,development/,>>"
	${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

add_custom_command(TARGET ${project} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different "${CMAKE_CURRENT_SOURCE_DIR}/Assets" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Assets")
