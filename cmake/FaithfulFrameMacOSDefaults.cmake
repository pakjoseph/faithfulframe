# CMake uses these values while detecting the Apple toolchain. Set their cache
# defaults before project() enables C/C++, or compiler checks can target the
# host SDK minimum even when plugin and Rust archive targets use macOS 13.
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR
	(NOT CMAKE_SYSTEM_NAME AND CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin"))
	if(NOT CMAKE_OSX_ARCHITECTURES)
		execute_process(
			COMMAND sysctl -n hw.optional.arm64
			OUTPUT_VARIABLE _faithfulframe_is_apple_silicon
			OUTPUT_STRIP_TRAILING_WHITESPACE
			RESULT_VARIABLE _faithfulframe_sysctl_result)
		if(_faithfulframe_sysctl_result EQUAL 0 AND _faithfulframe_is_apple_silicon STREQUAL "1")
			set(_faithfulframe_default_macos_arch "arm64")
		else()
			execute_process(
				COMMAND uname -m
				OUTPUT_VARIABLE _faithfulframe_host_arch
				OUTPUT_STRIP_TRAILING_WHITESPACE
				RESULT_VARIABLE _faithfulframe_uname_result)
			if(_faithfulframe_uname_result EQUAL 0 AND
			   _faithfulframe_host_arch MATCHES "^(arm64|aarch64)$")
				set(_faithfulframe_default_macos_arch "arm64")
			else()
				set(_faithfulframe_default_macos_arch "x86_64")
			endif()
		endif()
		# An explicitly supplied architecture remains authoritative. An empty
		# cache value means native host architecture, which must be materialized
		# before project() so CMake and Cargo agree on Apple Silicon builds.
		set(CMAKE_OSX_ARCHITECTURES "${_faithfulframe_default_macos_arch}" CACHE STRING
			"OBS build architectures for macOS" FORCE)
		unset(_faithfulframe_default_macos_arch)
		unset(_faithfulframe_host_arch)
		unset(_faithfulframe_uname_result)
		unset(_faithfulframe_is_apple_silicon)
		unset(_faithfulframe_sysctl_result)
	endif()
	# Do not inherit a workstation's MACOSX_DEPLOYMENT_TARGET: doing so can
	# silently raise the minimum supported macOS version for local release builds.
	# A deliberate product-floor change remains available through -D at configure time.
	set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING
		"OBS deployment target for macOS - 13.0+ required")
	if(NOT CMAKE_OSX_DEPLOYMENT_TARGET MATCHES "^[0-9]+(\\.[0-9]+)*$" OR
	   CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS "13.0")
		message(FATAL_ERROR "FaithfulFrame requires a macOS deployment target of 13.0 or later")
	endif()
endif()
