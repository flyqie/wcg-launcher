
function(launcher_configure_target target)
	target_compile_options(${target} PRIVATE
		/W4
		/WX
		/permissive-
		/sdl
		$<$<COMPILE_LANGUAGE:CXX>:/EHsc>)

	target_compile_definitions(${target} PRIVATE UNICODE _UNICODE _CONSOLE)

	set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)

	target_link_options(${target} PRIVATE $<$<CONFIG:Release>:/DEBUG>)
endfunction()

function(launcher_set_output_dir target)
	set_target_properties(${target} PROPERTIES
		RUNTIME_OUTPUT_DIRECTORY "${LAUNCHER_OUTPUT_DIR}/x64/$<CONFIG>"
		PDB_OUTPUT_DIRECTORY     "${LAUNCHER_OUTPUT_DIR}/x64/$<CONFIG>")
endfunction()
