function(CompileShaders)
	set(options "")
	set(singleValueArgs target config out folder extraOptions)
	set(multiValueArgs include_directories source)
	cmake_parse_arguments(args "${options}" "${singleValueArgs}" "${multiValueArgs}" ${ARGN})

	if (NOT args_target)
		message(FATAL_ERROR "target required")
	endif()

	if (NOT args_config)
		message(FATAL_ERROR "config required")
	endif()

	add_custom_target(${args_target} DEPENDS ShaderMake SOURCES ${args_source})

	set(include "")
	foreach(include_directory ${args_include_directories})
		list(APPEND include "--include")
		list(APPEND include ${include_directory})
	endforeach()

	if (NOT DXC_PATH)
		message(FATAL_ERROR "DXC_PATH required")
	endif()

	if (WIN32)
		set(useAPI "--useAPI")
	else()
		set(useAPI "")
	endif()

	set(compilerCommand ShaderMake
		--compiler "${DXC_PATH}"
		${include}
		--config ${args_config}
		--out ${args_out}
		--header --binaryBlob
		--platform DXIL
		${useAPI})
	separate_arguments(args_extraOptions NATIVE_COMMAND ${args_extraOptions})
	list(APPEND compilerCommand ${args_extraOptions})
	add_custom_command(TARGET ${args_target} PRE_BUILD COMMAND ${compilerCommand})

	if(args_folder)
		set_target_properties(${args_target} PROPERTIES FOLDER ${args_folder})
	endif()
endfunction()
