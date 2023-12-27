function(CompileShaders)
	set(options "")
	set(single_value_args target config out folder additional_options)
	set(multi_value_args include_directories source)
	cmake_parse_arguments(args "${options}" "${single_value_args}" "${multi_value_args}" ${ARGN})

	if (NOT args_target)
		message(FATAL_ERROR "target required")
	endif()

	if (NOT args_config)
		message(FATAL_ERROR "config required")
	endif()

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

	set(compiler_command ShaderMake
		--compiler ${DXC_PATH}
		${include}
		--config ${args_config}
		--out ${args_out}
		--header --binaryBlob
		--platform DXIL
		${useAPI})
	separate_arguments(args_additional_options NATIVE_COMMAND ${args_additional_options})
	list(APPEND compiler_command ${args_additional_options})

	foreach (compiled_shader ${args_source})
		cmake_path(RELATIVE_PATH compiled_shader)
		cmake_path(REMOVE_EXTENSION compiled_shader)
		set(compiled_shader "${args_out}/${compiled_shader}.dxil.h")
		if (NOT ${compiled_shader} IN_LIST compiled_shaders)
			list(APPEND compiled_shaders ${compiled_shader})
		endif()
	endforeach()

	add_custom_target(${args_target} COMMAND ${compiler_command} DEPENDS ShaderMake BYPRODUCTS ${compiled_shaders})

	if(args_folder)
		set_target_properties(${args_target} PROPERTIES FOLDER ${args_folder})
	endif()
endfunction()
