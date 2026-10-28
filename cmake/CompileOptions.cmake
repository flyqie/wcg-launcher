# Compiler and linker settings, in one place because every target takes the same ones.
# Nothing CMake's own Debug and Release defaults already give is repeated here.

# Everything both targets share. Call it on every target in the project.
function(launcher_configure_target target)
	target_compile_options(${target} PRIVATE
		/W3             # WarningLevel Level3
		/permissive-    # ConformanceMode
		/sdl            # SDLCheck
		$<$<COMPILE_LANGUAGE:CXX>:/EHsc>)

	# CharacterSet Unicode and SubSystem Console, which the Windows headers read.
	target_compile_definitions(${target} PRIVATE UNICODE _UNICODE _CONSOLE)

	# /GL and /LTCG, Release only.
	set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)

	# Release too, so a crash dump from a container still resolves.
	target_link_options(${target} PRIVATE $<$<CONFIG:Release>:/DEBUG>)
endfunction()

# Output/x64/<Configuration>/, which is the layout Docs/BUILD.md documents. The
# generator expression stops a multi-config generator from appending a second
# configuration directory of its own.
function(launcher_set_output_dir target)
	set_target_properties(${target} PROPERTIES
		RUNTIME_OUTPUT_DIRECTORY "${LAUNCHER_OUTPUT_DIR}/x64/$<CONFIG>"
		PDB_OUTPUT_DIRECTORY     "${LAUNCHER_OUTPUT_DIR}/x64/$<CONFIG>")
endfunction()
