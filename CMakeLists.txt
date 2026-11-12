cmake_minimum_required(VERSION 3.12)

# CMake uses these values while detecting the Apple toolchain. Set their cache
# defaults before project() enables C/C++, or compiler checks can target the
# host SDK minimum even when plugin and Rust archive targets use macOS 13.
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR
	(NOT CMAKE_SYSTEM_NAME AND CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin"))
	set(CMAKE_OSX_ARCHITECTURES "x86_64" CACHE STRING "OBS build architectures for macOS")
	if(DEFINED ENV{MACOSX_DEPLOYMENT_TARGET} AND NOT "$ENV{MACOSX_DEPLOYMENT_TARGET}" STREQUAL "")
		set(_faithfulframe_default_macos_target "$ENV{MACOSX_DEPLOYMENT_TARGET}")
	else()
		set(_faithfulframe_default_macos_target "13.0")
	endif()
	set(CMAKE_OSX_DEPLOYMENT_TARGET "${_faithfulframe_default_macos_target}" CACHE STRING
		"OBS deployment target for macOS - 13.0+ required")
	unset(_faithfulframe_default_macos_target)
	if(NOT CMAKE_OSX_DEPLOYMENT_TARGET MATCHES "^[0-9]+(\\.[0-9]+)*$" OR
	   CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS "13.0")
		message(FATAL_ERROR "FaithfulFrame requires a macOS deployment target of 13.0 or later")
	endif()
endif()

project(face-tracker VERSION 0.9.1)
set(PLUGIN_URL "https://obsproject.com/forum/resources/face-tracker.1294/")
set(PLUGIN_AUTHOR "Norihiro Kamae")
set(MACOS_BUNDLEID "net.nagater.obs-face-tracker")
set(MACOS_PACKAGE_UUID   "A2EB2F73-9828-40C4-A307-F47282BB8D3F")
set(MACOS_INSTALLER_UUID "652CF739-5A6F-4DAC-9611-B068FAC15BF3")
set(WIN_INSTALLER_UUID   "723828DB-04B8-41F7-9F7A-AF692B2CDF3D")
set(LINUX_MAINTAINER_EMAIL "norihiro@nagater.net")

option(WITH_PTZ_TCP "Enable to connect PTZ camera through TCP socket" ON)
option(WITH_DLIB_SUBMODULE "Link dlib from submodule" ON)
option(ENABLE_MONITOR_USER "Enable monitor source for user" OFF)
option(ENABLE_DEBUG_DATA "Enable property to save error and control data" OFF)
option(WITH_DOCK "Enable dock" ON)
option(ENABLE_DATAGEN "Enable generating data" OFF)
option(ENABLE_RUST_CORE_TESTS "Enable pure FaithfulFrame Rust core tests" OFF)
option(ENABLE_RUST_CORE_BRIDGE "Link the Rust core bridge and build its smoke test" OFF)

set(CMAKE_PREFIX_PATH "${QTDIR}")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Establish platform defaults before configuring the Rust static library so
# its deployment target matches the OBS plugin target below.
include(cmake/ObsPluginHelpers.cmake)

if (ENABLE_RUST_CORE_TESTS)
	find_program(CARGO_EXECUTABLE cargo REQUIRED)
	add_custom_target(faithfulframe-rust-core-test
		COMMAND ${CARGO_EXECUTABLE} test --manifest-path
			${CMAKE_CURRENT_SOURCE_DIR}/rust/faithfulframe-core/Cargo.toml
		WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/rust/faithfulframe-core
		COMMENT "Running deterministic FaithfulFrame Rust core tests"
		VERBATIM
	)
endif()

if (ENABLE_RUST_CORE_BRIDGE)
	set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
		"${CMAKE_CURRENT_SOURCE_DIR}/rust-toolchain.toml")
	find_program(CARGO_EXECUTABLE cargo REQUIRED)
	get_filename_component(FAITHFULFRAME_CARGO_BIN_DIR "${CARGO_EXECUTABLE}" DIRECTORY)
	find_program(RUSTUP_EXECUTABLE rustup HINTS "${FAITHFULFRAME_CARGO_BIN_DIR}")
	if (NOT RUSTUP_EXECUTABLE)
		message(FATAL_ERROR "rustup is required to resolve the repository-pinned Rust compiler")
	endif()
	execute_process(
		COMMAND "${RUSTUP_EXECUTABLE}" which rustc
		WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
		OUTPUT_VARIABLE FAITHFULFRAME_RUSTC_EXECUTABLE
		OUTPUT_STRIP_TRAILING_WHITESPACE
		RESULT_VARIABLE FAITHFULFRAME_RUSTUP_RESULT)
	if (NOT FAITHFULFRAME_RUSTUP_RESULT EQUAL 0 OR NOT EXISTS "${FAITHFULFRAME_RUSTC_EXECUTABLE}")
		message(FATAL_ERROR "Could not resolve rustc from rust-toolchain.toml via rustup")
	endif()
	find_package(Threads REQUIRED)
	set(FAITHFULFRAME_FFI_MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/rust/faithfulframe-ffi/Cargo.toml)
	file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/rust-toolchain.toml" FAITHFULFRAME_RUST_TOOLCHAIN_LINE
		REGEX "^channel[ \t]*=")
	string(REGEX REPLACE ".*\"([^\"]+)\".*" "\\1" FAITHFULFRAME_RUST_TOOLCHAIN
		"${FAITHFULFRAME_RUST_TOOLCHAIN_LINE}")
	if (NOT FAITHFULFRAME_RUST_TOOLCHAIN)
		message(FATAL_ERROR "Could not read the Rust toolchain channel from rust-toolchain.toml")
	endif()
	set(FAITHFULFRAME_RUST_TARGET_DIR
		${CMAKE_CURRENT_BINARY_DIR}/faithfulframe-rust-target-rust-${FAITHFULFRAME_RUST_TOOLCHAIN})
	if (APPLE AND CMAKE_OSX_DEPLOYMENT_TARGET)
		set(FAITHFULFRAME_RUST_TARGET_DIR
			${FAITHFULFRAME_RUST_TARGET_DIR}-macos-${CMAKE_OSX_DEPLOYMENT_TARGET})
	endif()
	if (WIN32)
		set(FAITHFULFRAME_FFI_LIBRARY ${FAITHFULFRAME_RUST_TARGET_DIR}/release/faithfulframe_ffi.lib)
	else()
		set(FAITHFULFRAME_FFI_LIBRARY ${FAITHFULFRAME_RUST_TARGET_DIR}/release/libfaithfulframe_ffi.a)
	endif()
	set(FAITHFULFRAME_CARGO_BUILD
		${CMAKE_COMMAND} -E env "RUSTC=${FAITHFULFRAME_RUSTC_EXECUTABLE}"
		${CARGO_EXECUTABLE} build --release --manifest-path ${FAITHFULFRAME_FFI_MANIFEST}
		--target-dir ${FAITHFULFRAME_RUST_TARGET_DIR})
	if (APPLE AND CMAKE_OSX_DEPLOYMENT_TARGET)
		find_program(OTOOL_EXECUTABLE otool)
		if (NOT OTOOL_EXECUTABLE)
			message(FATAL_ERROR "otool is required to validate the macOS Rust archive deployment target")
		endif()
		set(FAITHFULFRAME_CARGO_BUILD
			${CMAKE_COMMAND} -E env MACOSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}
			${FAITHFULFRAME_CARGO_BUILD})
		set(FAITHFULFRAME_VERIFY_RUST_DEPLOYMENT
			COMMAND ${CMAKE_COMMAND}
				-DFF_ARCHIVE=${FAITHFULFRAME_FFI_LIBRARY}
				-DFF_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}
				-DFF_OTOOL=${OTOOL_EXECUTABLE}
				-P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyRustMacOSDeployment.cmake)
	endif()
	add_custom_command(OUTPUT ${FAITHFULFRAME_FFI_LIBRARY}
		COMMAND ${FAITHFULFRAME_CARGO_BUILD}
		${FAITHFULFRAME_VERIFY_RUST_DEPLOYMENT}
		DEPENDS ${FAITHFULFRAME_FFI_MANIFEST}
			${CMAKE_CURRENT_SOURCE_DIR}/rust-toolchain.toml
			${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyRustMacOSDeployment.cmake
			${CMAKE_CURRENT_SOURCE_DIR}/rust/faithfulframe-ffi/Cargo.lock
			${CMAKE_CURRENT_SOURCE_DIR}/rust/faithfulframe-ffi/src/lib.rs
			${CMAKE_CURRENT_SOURCE_DIR}/rust/faithfulframe-core/src/framing.rs
			${CMAKE_CURRENT_SOURCE_DIR}/rust/faithfulframe-core/src/lib.rs
			${CARGO_EXECUTABLE}
		COMMENT "Building FaithfulFrame Rust C ABI library"
		VERBATIM)
	add_custom_target(faithfulframe-rust-ffi-build DEPENDS ${FAITHFULFRAME_FFI_LIBRARY})
	add_library(faithfulframe-rust-ffi STATIC IMPORTED GLOBAL)
	set_target_properties(faithfulframe-rust-ffi PROPERTIES IMPORTED_LOCATION ${FAITHFULFRAME_FFI_LIBRARY})
	if (WIN32)
		target_link_libraries(faithfulframe-rust-ffi INTERFACE ws2_32 ntdll userenv)
	endif()
	add_executable(faithfulframe-rust-bridge-smoke
		tests/faithfulframe-core-bridge-smoke.cpp src/faithfulframe-core-bridge.cpp)
	add_executable(faithfulframe-session-executor-smoke
		tests/faithfulframe-session-executor-smoke.cpp src/faithfulframe-core-bridge.cpp)
	add_executable(faithfulframe-detector-worker-smoke
		tests/faithfulframe-detector-worker-smoke.cpp src/face-detector-base.cpp)
	add_executable(faithfulframe-worker-log-gate-smoke tests/faithfulframe-worker-log-gate-smoke.cpp)
	add_executable(faithfulframe-texture-object-smoke
		tests/faithfulframe-texture-object-smoke.cpp src/texture-object.cpp)
	add_executable(faithfulframe-preview-geometry-smoke tests/faithfulframe-preview-geometry-smoke.cpp)
	add_executable(faithfulframe-candidate-selection-smoke tests/faithfulframe-candidate-selection-smoke.cpp)
	add_executable(faithfulframe-tracker-association-smoke tests/faithfulframe-tracker-association-smoke.cpp)
	add_executable(faithfulframe-tracker-worker-budget-smoke
		tests/faithfulframe-tracker-worker-budget-smoke.cpp)
	target_include_directories(faithfulframe-tracker-worker-budget-smoke PRIVATE src)
	add_dependencies(faithfulframe-rust-bridge-smoke faithfulframe-rust-ffi-build)
	add_dependencies(faithfulframe-session-executor-smoke faithfulframe-rust-ffi-build)
	target_include_directories(faithfulframe-rust-bridge-smoke PRIVATE src rust/faithfulframe-ffi/include)
	target_include_directories(faithfulframe-session-executor-smoke PRIVATE src rust/faithfulframe-ffi/include)
	target_include_directories(faithfulframe-detector-worker-smoke PRIVATE ${CMAKE_CURRENT_BINARY_DIR} src)
	target_include_directories(faithfulframe-texture-object-smoke PRIVATE ${CMAKE_CURRENT_BINARY_DIR} src)
	target_compile_definitions(faithfulframe-detector-worker-smoke PRIVATE
		FAITHFULFRAME_DETECTOR_WORKER_STANDALONE=1)
	target_include_directories(faithfulframe-preview-geometry-smoke PRIVATE src rust/faithfulframe-ffi/include)
	target_include_directories(faithfulframe-candidate-selection-smoke PRIVATE src rust/faithfulframe-ffi/include)
	target_include_directories(faithfulframe-tracker-association-smoke PRIVATE src)
	target_link_libraries(faithfulframe-rust-bridge-smoke PRIVATE faithfulframe-rust-ffi)
	target_link_libraries(faithfulframe-rust-bridge-smoke PRIVATE Threads::Threads)
	target_link_libraries(faithfulframe-session-executor-smoke PRIVATE faithfulframe-rust-ffi Threads::Threads)
	target_link_libraries(faithfulframe-detector-worker-smoke PRIVATE Threads::Threads)
	target_link_libraries(faithfulframe-texture-object-smoke PRIVATE dlib Threads::Threads)
	enable_testing()
	add_test(NAME faithfulframe-preview-geometry-smoke COMMAND faithfulframe-preview-geometry-smoke)
	add_test(NAME faithfulframe-candidate-selection-smoke COMMAND faithfulframe-candidate-selection-smoke)
	add_test(NAME faithfulframe-rust-bridge-smoke COMMAND faithfulframe-rust-bridge-smoke)
	add_test(NAME faithfulframe-session-executor-smoke COMMAND faithfulframe-session-executor-smoke)
	add_test(NAME faithfulframe-detector-worker-smoke COMMAND faithfulframe-detector-worker-smoke)
	add_test(NAME faithfulframe-worker-log-gate-smoke COMMAND faithfulframe-worker-log-gate-smoke)
	add_test(NAME faithfulframe-texture-object-smoke COMMAND faithfulframe-texture-object-smoke)
	add_test(NAME faithfulframe-tracker-association-smoke COMMAND faithfulframe-tracker-association-smoke)
	add_test(NAME faithfulframe-tracker-worker-budget-smoke COMMAND faithfulframe-tracker-worker-budget-smoke)
	set_tests_properties(faithfulframe-detector-worker-smoke faithfulframe-worker-log-gate-smoke
		faithfulframe-texture-object-smoke PROPERTIES TIMEOUT 10)
endif()

set(plugin_additional_libs)
set(plugin_additional_incs)

if (WITH_PTZ_TCP)
	set(CMAKE_POSITION_INDEPENDENT_CODE true)
	set(WITH_VISCA_SERIAL OFF)
	add_definitions("-DVISCA_API= ")
	add_subdirectory(libvisca/visca)
	set(plugin_additional_libs ${plugin_additional_libs} visca)
	set(plugin_additional_incs ${plugin_additional_incs} libvisca/visca)
endif()

find_package(libobs REQUIRED)
find_package(obs-frontend-api REQUIRED)

if (WITH_DLIB_SUBMODULE)
	set(CMAKE_POSITION_INDEPENDENT_CODE True)
	set(DLIB_NO_GUI_SUPPORT ON)
	set(DLIB_PNG_SUPPORT OFF)
	set(DLIB_GIF_SUPPORT OFF)
	set(DLIB_USE_FFMPEG OFF)
	set(DLIB_JPEG_SUPPORT OFF)
	set(DLIB_LINK_WITH_SQLITE3 OFF)
	set(DLIB_WEBP_SUPPORT OFF)
	set(DLIB_JXL_SUPPORT OFF)
	set(DLIB_TEST_COMPILE_ALL_SOURCE_CPP OFF)
	add_subdirectory(dlib)
else()
	find_package(dlib REQUIRED)
	add_library(dlib ALIAS dlib::dlib)
endif()

if (ENABLE_RUST_CORE_BRIDGE)
	target_include_directories(faithfulframe-texture-object-smoke PRIVATE
		"$<TARGET_PROPERTY:OBS::libobs,INTERFACE_INCLUDE_DIRECTORIES>")
endif()

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)

configure_file(
	src/plugin-macros.h.in
	plugin-macros.generated.h
)

set(PLUGIN_SOURCES
	src/module-main.c
	src/face-tracker.cpp
	src/source_list.cc
	src/face-tracker-preset.cpp
	src/face-tracker-manager.cpp
	src/face-tracker-ptz.cpp
	src/face-tracker-monitor.cpp
	src/face-detector-base.cpp
	src/face-detector-dlib-hog.cpp
	src/face-detector-dlib-cnn.cpp
	src/face-tracker-base.cpp
	src/face-tracker-dlib.cpp
	src/texture-object.cpp
	src/helper.cpp
	src/ptz-backend.cpp
	src/obsptz-backend.cpp
	src/dummy-backend.cpp
)

if (ENABLE_RUST_CORE_BRIDGE)
	list(APPEND PLUGIN_SOURCES src/faithfulframe-core-bridge.cpp)
endif()

if (WITH_PTZ_TCP)
	set(PLUGIN_SOURCES ${PLUGIN_SOURCES} src/libvisca-thread.cpp)
endif()
if (WITH_DOCK)
	set(PLUGIN_SOURCES
		${PLUGIN_SOURCES}
		ui/face-tracker-dock.cpp
		ui/face-tracker-widget.cpp
		ui/sanctuary-style.cpp
	)
endif()

add_library(${PROJECT_NAME} MODULE ${PLUGIN_SOURCES})

if (WITH_DOCK)
	set(QT_NO_CREATE_VERSIONLESS_TARGETS OFF)
	find_package(Qt${QT_VERSION} COMPONENTS Widgets Core Gui)
	set(plugin_additional_incs ${plugin_additional_incs} src)
	set(plugin_additional_libs ${plugin_additional_libs} Qt::Gui Qt::Widgets)
	# Only for Wayland with Qt before 6.9, where it comes with Qt Gui
	if (OS_LINUX AND Qt${QT_VERSION}Core_VERSION VERSION_LESS 6.9)
		find_package(Qt6 QUIET COMPONENTS GuiPrivate)
		set(plugin_additional_libs ${plugin_additional_libs} Qt::GuiPrivate)
	endif()
endif()

include_directories(
	${CMAKE_CURRENT_SOURCE_DIR}/dlib
	${plugin_additional_incs}
)

target_link_libraries(${PROJECT_NAME}
	OBS::libobs
	OBS::obs-frontend-api
	dlib
	${plugin_additional_libs}
)

if (ENABLE_RUST_CORE_BRIDGE)
	add_dependencies(${PROJECT_NAME} faithfulframe-rust-ffi-build)
	target_compile_definitions(${PROJECT_NAME} PRIVATE ENABLE_RUST_CORE_BRIDGE=1)
	target_include_directories(${PROJECT_NAME} PRIVATE src rust/faithfulframe-ffi/include)
	target_link_libraries(${PROJECT_NAME} faithfulframe-rust-ffi)
	if (UNIX AND NOT APPLE)
		find_package(Threads REQUIRED)
		target_link_libraries(${PROJECT_NAME} Threads::Threads ${CMAKE_DL_LIBS} m)
	elseif (OS_WINDOWS)
		# Rust's Windows standard library uses native NT file APIs.
		target_link_libraries(${PROJECT_NAME} ntdll)
	endif()
endif()

target_include_directories(${PROJECT_NAME}
	PRIVATE
	${CMAKE_CURRENT_BINARY_DIR}
)

if(OS_WINDOWS)
	# Enable Multicore Builds and disable FH4 (to not depend on VCRUNTIME140_1.DLL when building with VS2019)
	if (MSVC)
		add_definitions(/MP /d2FH4-)
		add_definitions("-D_USE_MATH_DEFINES")
		add_definitions("-D_CRT_SECURE_NO_WARNINGS") # to avoid a warning for `fopen`
		add_definitions("-D_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR") # TODO: Remove once requiring OBS 30.2 or later.
	endif()

	target_link_libraries(${PROJECT_NAME} OBS::w32-pthreads)

	set(LICENSE_DESTINATION "${PROJECT_NAME}/data")
endif()

if(OS_LINUX)
	target_compile_options(${PROJECT_NAME}
		PRIVATE
		-Wall
		-Wextra
		-Wunused-function
		-Werror
		-Wno-error=deprecated-declarations # TODO: Replace obs_frontend_add_dock with the new API.
	)
	target_link_options(${PROJECT_NAME} PRIVATE -Wl,-z,defs)

	set(LICENSE_DESTINATION ${CMAKE_INSTALL_DOCDIR})

	if(ENABLE_COVERAGE)
		target_compile_options(${PROJECT_NAME} PRIVATE -coverage -fprofile-update=atomic)
		target_link_options(${PROJECT_NAME} PRIVATE -coverage)
	endif()
endif()

if(OS_MACOS)
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++ -fvisibility=default")

	set(LICENSE_DESTINATION "${PROJECT_NAME}.plugin/Contents/Resources")

	set_target_properties(${PROJECT_NAME} PROPERTIES PREFIX "")
	set(MACOSX_PLUGIN_GUI_IDENTIFIER "${MACOS_BUNDLEID}")
	set(MACOSX_PLUGIN_BUNDLE_VERSION "${PROJECT_VERSION}")
	set(MACOSX_PLUGIN_SHORT_VERSION_STRING "1")

	if(ENABLE_COVERAGE)
		target_compile_options(${PROJECT_NAME} PRIVATE -coverage)
		target_link_options(${PROJECT_NAME} PRIVATE --coverage)
	endif()
endif()

file(GENERATE OUTPUT .gitignore CONTENT "*\n")

setup_plugin_target(${PROJECT_NAME})

option(INSTALL_LICENSE_FILES "Install license files" ON)
if(INSTALL_LICENSE_FILES)
	install(FILES
		LICENSE
		DESTINATION ${LICENSE_DESTINATION}
	)
	install(FILES
		libvisca/COPYING
		RENAME LICENSE-libvisca.txt
		DESTINATION ${LICENSE_DESTINATION}
	)
	if (WITH_DLIB_SUBMODULE)
		install(FILES
			dlib/LICENSE.txt
			RENAME LICENSE-dlib.txt
			DESTINATION ${LICENSE_DESTINATION}
		)
	endif()
endif()

if(${CMAKE_SOURCE_DIR} STREQUAL ${CMAKE_CURRENT_SOURCE_DIR})
	configure_file(
		installer/installer-Windows.iss.in
		installer-Windows.generated.iss
	)

	configure_file(
		ci/ci_includes.sh.in
		ci/ci_includes.generated.sh
	)
	configure_file(
		ci/ci_includes.cmd.in
		ci/ci_includes.generated.cmd
	)

	configure_file(
		installer/installer-macOS.pkgproj.in
		installer-macOS.generated.pkgproj
	)
endif()

if(ENABLE_DATAGEN)
	add_executable(face-detector-dlib-hog-datagen
		src/face-detector-dlib-hog-datagen.cpp
	)
	target_include_directories(face-detector-dlib-hog-datagen PRIVATE
		${CMAKE_CURRENT_BINARY_DIR}
	)
	target_link_libraries(face-detector-dlib-hog-datagen
		dlib
	)
endif()
