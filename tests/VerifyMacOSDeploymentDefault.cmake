include("${CMAKE_CURRENT_LIST_DIR}/../cmake/ExtractMacOSMinimumVersions.cmake")

if(NOT APPLE)
	message(FATAL_ERROR "This deployment-default probe is intended to run on macOS")
endif()
if(NOT FAITHFULFRAME_SOURCE_DIR)
	message(FATAL_ERROR "FAITHFULFRAME_SOURCE_DIR must identify the FaithfulFrame checkout")
endif()

find_program(_faithfulframe_otool otool)
if(NOT _faithfulframe_otool)
	message(FATAL_ERROR "otool is required for the macOS deployment-default probe")
endif()

set(_probe_build "${CMAKE_CURRENT_BINARY_DIR}/faithfulframe-macos-deployment-default")
file(REMOVE_RECURSE "${_probe_build}")

execute_process(
	COMMAND "${CMAKE_COMMAND}" -E env MACOSX_DEPLOYMENT_TARGET=15.0 "${CMAKE_COMMAND}"
		-S "${CMAKE_CURRENT_LIST_DIR}/macos-deployment-default"
		-B "${_probe_build}"
		-DFAITHFULFRAME_SOURCE_DIR=${FAITHFULFRAME_SOURCE_DIR}
	RESULT_VARIABLE _configure_result
	OUTPUT_VARIABLE _configure_output
	ERROR_VARIABLE _configure_error)
if(NOT _configure_result EQUAL 0)
	message(FATAL_ERROR "Isolated deployment-target configure failed:\n${_configure_output}\n${_configure_error}")
endif()

file(STRINGS "${_probe_build}/CMakeCache.txt" _deployment_target_line
	REGEX "^CMAKE_OSX_DEPLOYMENT_TARGET:STRING=")
if(NOT _deployment_target_line STREQUAL "CMAKE_OSX_DEPLOYMENT_TARGET:STRING=13.0")
	message(FATAL_ERROR "Expected a default macOS 13.0 target despite the 15.0 environment override; got '${_deployment_target_line}'")
endif()

execute_process(
	COMMAND "${CMAKE_COMMAND}" --build "${_probe_build}" --config Release
	RESULT_VARIABLE _build_result
	OUTPUT_VARIABLE _build_output
	ERROR_VARIABLE _build_error)
if(NOT _build_result EQUAL 0)
	message(FATAL_ERROR "Isolated deployment-target build failed:\n${_build_output}\n${_build_error}")
endif()

set(_probe_executable "${_probe_build}/faithfulframe-macos-deployment-probe")
execute_process(
	COMMAND "${_faithfulframe_otool}" -arch all -l "${_probe_executable}"
	RESULT_VARIABLE _otool_result
	OUTPUT_VARIABLE _load_commands
	ERROR_VARIABLE _otool_error)
if(NOT _otool_result EQUAL 0)
	message(FATAL_ERROR "Could not inspect the isolated probe binary: ${_otool_error}")
endif()

faithfulframe_extract_macos_minimum_versions(_minimum_versions "${_load_commands}")
if(NOT _minimum_versions)
	message(FATAL_ERROR "The isolated probe binary has no macOS minimum-version load command")
endif()
foreach(_minimum_version IN LISTS _minimum_versions)
	if(_minimum_version VERSION_GREATER "13.0")
		message(FATAL_ERROR "The 15.0 environment override leaked into the binary minimum (${_minimum_version})")
	endif()
endforeach()

message(STATUS "Verified the repository default overrides MACOSX_DEPLOYMENT_TARGET=15.0 with macOS 13.0")
