function(faithfulframe_verify_macos_link_targets)
	if(NOT APPLE)
		return()
	endif()

	find_program(_faithfulframe_otool otool)
	if(NOT _faithfulframe_otool)
		message(FATAL_ERROR "otool is required to validate macOS link dependencies")
	endif()

	set(_faithfulframe_checked_locations)
	foreach(_target ${ARGN})
		if(NOT TARGET "${_target}")
			continue()
		endif()

		get_target_property(_configs "${_target}" IMPORTED_CONFIGURATIONS)
		if(_configs STREQUAL "_configs-NOTFOUND")
			set(_configs)
		endif()
		list(APPEND _configs DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
		list(REMOVE_DUPLICATES _configs)
		set(_locations)
		get_target_property(_location "${_target}" IMPORTED_LOCATION)
		if(_location)
			list(APPEND _locations "${_location}")
		endif()
		foreach(_config IN LISTS _configs)
			string(TOUPPER "${_config}" _config_upper)
			get_target_property(_location "${_target}" "IMPORTED_LOCATION_${_config_upper}")
			if(_location)
				list(APPEND _locations "${_location}")
			endif()
		endforeach()
		list(REMOVE_DUPLICATES _locations)

		foreach(_location IN LISTS _locations)
			if(NOT EXISTS "${_location}" OR _location MATCHES "\\$<")
				continue()
			endif()
			list(FIND _faithfulframe_checked_locations "${_location}" _already_checked)
			if(NOT _already_checked EQUAL -1)
				continue()
			endif()
			list(APPEND _faithfulframe_checked_locations "${_location}")

			execute_process(
				COMMAND "${_faithfulframe_otool}" -l "${_location}"
				RESULT_VARIABLE _otool_result
				OUTPUT_VARIABLE _otool_output
				ERROR_VARIABLE _otool_error)
			if(NOT _otool_result EQUAL 0)
				message(FATAL_ERROR
					"Could not inspect macOS link dependency ${_target} at ${_location}: ${_otool_error}")
			endif()

			string(REGEX MATCHALL "minos[ \t]+[0-9]+(\\.[0-9]+)*" _minimum_versions "${_otool_output}")
			if(NOT _minimum_versions)
				message(FATAL_ERROR
					"Could not find a macOS minimum-version load command in ${_target} at ${_location}")
			endif()
			foreach(_entry IN LISTS _minimum_versions)
				string(REGEX REPLACE "^minos[ \t]+" "" _minimum_version "${_entry}")
				if(_minimum_version VERSION_GREATER CMAKE_OSX_DEPLOYMENT_TARGET)
					message(FATAL_ERROR
						"${_target} at ${_location} requires macOS ${_minimum_version}, but "
						"FaithfulFrame is configured for macOS ${CMAKE_OSX_DEPLOYMENT_TARGET}. "
						"Use a Qt/OBS SDK built for the desired deployment target, or reconfigure with "
						"-DCMAKE_OSX_DEPLOYMENT_TARGET=${_minimum_version}.")
				endif()
			endforeach()
			message(STATUS
				"Verified ${_target} minimum macOS version is compatible with ${CMAKE_OSX_DEPLOYMENT_TARGET}")
		endforeach()
	endforeach()
endfunction()
