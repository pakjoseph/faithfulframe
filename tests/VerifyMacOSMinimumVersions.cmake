include("${CMAKE_CURRENT_LIST_DIR}/../cmake/ExtractMacOSMinimumVersions.cmake")

set(_load_commands [=[
Load command 1
      cmd LC_BUILD_VERSION
  cmdsize 32
 platform MACOS
    minos 13.0
      sdk 26.1
Load command 2
      cmd LC_VERSION_MIN_MACOSX
  cmdsize 16
  version 14.2
      sdk 15.0
Load command 3
      cmd LC_VERSION_MIN_IPHONEOS
  cmdsize 16
  version 99.0
Load command 4
      cmd LC_VERSION_MIN_MACOSX
  cmdsize 16
Load command 5
      cmd LC_VERSION_MIN_IPHONEOS
  cmdsize 16
  version 100.0
]=])

faithfulframe_extract_macos_minimum_versions(_actual "${_load_commands}")
set(_expected "13.0;14.2")
if(NOT "${_actual}" STREQUAL "${_expected}")
	message(FATAL_ERROR "Expected macOS minimum versions '${_expected}', got '${_actual}'")
endif()

faithfulframe_extract_macos_minimum_versions(_missing "sdk 26.1")
if(_missing)
	message(FATAL_ERROR "Unexpected minimum versions found without a load command: '${_missing}'")
endif()
