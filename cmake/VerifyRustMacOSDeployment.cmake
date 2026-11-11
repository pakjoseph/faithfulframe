if(NOT DEFINED FF_ARCHIVE OR NOT EXISTS "${FF_ARCHIVE}")
	message(FATAL_ERROR "FaithfulFrame Rust archive is missing: ${FF_ARCHIVE}")
endif()
if(NOT DEFINED FF_DEPLOYMENT_TARGET OR NOT FF_DEPLOYMENT_TARGET)
	message(FATAL_ERROR "The macOS deployment target was not provided to the Rust archive check")
endif()
if(NOT DEFINED FF_OTOOL OR NOT EXISTS "${FF_OTOOL}")
	message(FATAL_ERROR "otool is required to validate the macOS Rust archive")
endif()

execute_process(
	COMMAND "${FF_OTOOL}" -l "${FF_ARCHIVE}"
	RESULT_VARIABLE _otool_result
	OUTPUT_VARIABLE _otool_output
	ERROR_VARIABLE _otool_error)
if(NOT _otool_result EQUAL 0)
	message(FATAL_ERROR "Could not inspect FaithfulFrame Rust archive: ${_otool_error}")
endif()

string(REGEX MATCHALL "minos[ \t]+[0-9]+(\\.[0-9]+)*" _minimum_versions "${_otool_output}")
if(NOT _minimum_versions)
	message(FATAL_ERROR "No macOS minimum-version load commands found in ${FF_ARCHIVE}")
endif()

foreach(_entry IN LISTS _minimum_versions)
	string(REGEX REPLACE "^minos[ \t]+" "" _minimum_version "${_entry}")
	if(_minimum_version VERSION_GREATER FF_DEPLOYMENT_TARGET)
		message(FATAL_ERROR
			"FaithfulFrame Rust archive contains an object targeting macOS ${_minimum_version}, "
			"above the plugin deployment target ${FF_DEPLOYMENT_TARGET}. Use the repository-pinned "
			"Rustup toolchain or raise CMAKE_OSX_DEPLOYMENT_TARGET before packaging.")
	endif()
endforeach()

message(STATUS "FaithfulFrame Rust archive supports deployment target ${FF_DEPLOYMENT_TARGET}")
