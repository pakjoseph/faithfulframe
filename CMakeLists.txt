cmake_minimum_required(VERSION 3.12)
include(cmake/FaithfulFrameMacOSDefaults.cmake)

project(face-tracker VERSION 0.9.1)
set(PLUGIN_URL "https://github.com/pakjoseph/faithfulframe")
set(PLUGIN_AUTHOR "FaithfulFrame contributors")
set(MACOS_BUNDLEID "net.nagater.obs-face-tracker")
set(MACOS_PACKAGE_UUID   "A2EB2F73-9828-40C4-A307-F47282BB8D3F")
set(MACOS_INSTALLER_UUID "652CF739-5A6F-4DAC-9611-B068FAC15BF3")
set(WIN_INSTALLER_UUID   "723828DB-04B8-41F7-9F7A-AF692B2CDF3D")
set(LINUX_PACKAGE_MAINTAINER "FaithfulFrame contributors <joepak@bu.edu>")

option(WITH_PTZ_TCP "Enable to connect PTZ camera through TCP socket" ON)
option(WITH_DLIB_SUBMODULE "Link dlib from submodule" ON)
option(ENABLE_MONITOR_USER "Enable monitor source for user" OFF)
option(ENABLE_DEBUG_DATA "Enable property to save error and control data" OFF)
option(WITH_DOCK "Enable dock" ON)
option(ENABLE_DATAGEN "Enable generating data" OFF)
option(ENABLE_RUST_CORE_TESTS "Enable pure FaithfulFrame Rust core tests" OFF)
option(ENABLE_RUST_CORE_BRIDGE "Build the Rust core bridge and its smoke test" ON)

set(CMAKE_PREFIX_PATH "${QTDIR}")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Establish platform defaults before configuring the Rust static library so
# its deployment target matches the OBS plugin target below.
include(cmake/ObsPluginHelpers.cmake)

find_package(libobs REQUIRED)
find_package(obs-frontend-api REQUIRED)

set(FAITHFULFRAME_OBS_RUNTIME_DIRECTORY)
foreach(_obs_location_property
		IMPORTED_LOCATION
		IMPORTED_LOCATION_NOCONFIG
		IMPORTED_LOCATION_RELEASE
		IMPORTED_LOCATION_RELWITHDEBINFO
		IMPORTED_LOCATION_MINSIZEREL
		IMPORTED_LOCATION_DEBUG)
	get_target_property(_obs_library_location OBS::libobs ${_obs_location_property})
	if (_obs_library_location AND EXISTS "${_obs_library_location}")
		get_filename_component(FAITHFULFRAME_OBS_RUNTIME_DIRECTORY "${_obs_library_location}" DIRECTORY)
		break()
	endif()
endforeach()
set(FAITHFULFRAME_SESSION_REPLAY_TEST_ENV)
set(FAITHFULFRAME_SESSION_REPLAY_TEST_PATH)
if (APPLE)
	set(_obs_runtime_candidates)
	foreach(_framework_path IN LISTS CMAKE_FRAMEWORK_PATH)
		get_filename_component(_framework_parent "${_framework_path}" DIRECTORY)
		list(APPEND _obs_runtime_candidates "${_framework_parent}/lib")
	endforeach()
	if (FAITHFULFRAME_OBS_RUNTIME_DIRECTORY)
		set(_obs_ancestor "${FAITHFULFRAME_OBS_RUNTIME_DIRECTORY}")
		foreach(_depth RANGE 0 8)
			list(APPEND _obs_runtime_candidates "${_obs_ancestor}/lib")
			get_filename_component(_obs_ancestor "${_obs_ancestor}" DIRECTORY)
		endforeach()
	endif()
	foreach(_candidate_directory IN LISTS _obs_runtime_candidates)
		if (EXISTS "${_candidate_directory}/libavcodec.dylib")
			set(FAITHFULFRAME_OBS_RUNTIME_DIRECTORY "${_candidate_directory}")
			break()
		endif()
	endforeach()
	if (FAITHFULFRAME_OBS_RUNTIME_DIRECTORY)
		set(_obs_runtime_path "${FAITHFULFRAME_OBS_RUNTIME_DIRECTORY}")
		if (DEFINED ENV{DYLD_LIBRARY_PATH} AND NOT "$ENV{DYLD_LIBRARY_PATH}" STREQUAL "")
			string(APPEND _obs_runtime_path ":$ENV{DYLD_LIBRARY_PATH}")
		endif()
		list(APPEND FAITHFULFRAME_SESSION_REPLAY_TEST_ENV "DYLD_LIBRARY_PATH=${_obs_runtime_path}")
	endif()
elseif (WIN32)
	if (FAITHFULFRAME_OBS_RUNTIME_DIRECTORY)
		list(APPEND FAITHFULFRAME_SESSION_REPLAY_TEST_PATH
			"${FAITHFULFRAME_OBS_RUNTIME_DIRECTORY}")
		get_filename_component(_obs_runtime_parent
			"${FAITHFULFRAME_OBS_RUNTIME_DIRECTORY}" DIRECTORY)
		list(APPEND FAITHFULFRAME_SESSION_REPLAY_TEST_PATH "${_obs_runtime_parent}")
	endif()
endif()

if (ENABLE_RUST_CORE_TESTS)
	find_program(CARGO_EXECUTABLE cargo REQUIRED)
	add_custom_target(faithfulframe-rust-core-test
		COMMAND ${CARGO_EXECUTABLE} test --locked --manifest-path
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
	set(FAITHFULFRAME_RUST_TARGET_ARGS)
	set(FAITHFULFRAME_RUST_ARTIFACT_DIR ${FAITHFULFRAME_RUST_TARGET_DIR})
	if (APPLE)
		if(CMAKE_OSX_ARCHITECTURES STREQUAL "arm64")
			set(FAITHFULFRAME_RUST_TARGET_TRIPLE aarch64-apple-darwin)
		elseif(CMAKE_OSX_ARCHITECTURES STREQUAL "x86_64")
			set(FAITHFULFRAME_RUST_TARGET_TRIPLE x86_64-apple-darwin)
		else()
			message(FATAL_ERROR
				"FaithfulFrame's Rust bridge requires one macOS architecture (arm64 or x86_64); "
				"CMAKE_OSX_ARCHITECTURES='${CMAKE_OSX_ARCHITECTURES}' is unsupported. Build each architecture separately.")
		endif()
		list(APPEND FAITHFULFRAME_RUST_TARGET_ARGS --target ${FAITHFULFRAME_RUST_TARGET_TRIPLE})
		set(FAITHFULFRAME_RUST_ARTIFACT_DIR
			${FAITHFULFRAME_RUST_TARGET_DIR}/${FAITHFULFRAME_RUST_TARGET_TRIPLE})
		execute_process(
			COMMAND "${RUSTUP_EXECUTABLE}" target list --installed --toolchain ${FAITHFULFRAME_RUST_TOOLCHAIN}
			WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
			OUTPUT_VARIABLE FAITHFULFRAME_RUST_INSTALLED_TARGETS
			OUTPUT_STRIP_TRAILING_WHITESPACE
			RESULT_VARIABLE FAITHFULFRAME_RUST_TARGET_LIST_RESULT)
		if(NOT FAITHFULFRAME_RUST_TARGET_LIST_RESULT EQUAL 0 OR
		   NOT FAITHFULFRAME_RUST_INSTALLED_TARGETS MATCHES "(^|\\n)${FAITHFULFRAME_RUST_TARGET_TRIPLE}($|\\n)")
			message(FATAL_ERROR
				"Rust target ${FAITHFULFRAME_RUST_TARGET_TRIPLE} is required by CMAKE_OSX_ARCHITECTURES='${CMAKE_OSX_ARCHITECTURES}' "
				"but is not installed for toolchain ${FAITHFULFRAME_RUST_TOOLCHAIN}. Install it with: "
				"rustup target add --toolchain ${FAITHFULFRAME_RUST_TOOLCHAIN} ${FAITHFULFRAME_RUST_TARGET_TRIPLE}; "
				"for native Apple Silicon development, configure with -DCMAKE_OSX_ARCHITECTURES=arm64")
		endif()
	endif()
	if (WIN32)
		set(FAITHFULFRAME_FFI_LIBRARY ${FAITHFULFRAME_RUST_ARTIFACT_DIR}/release/faithfulframe_ffi.lib)
	else()
		set(FAITHFULFRAME_FFI_LIBRARY ${FAITHFULFRAME_RUST_ARTIFACT_DIR}/release/libfaithfulframe_ffi.a)
	endif()
	set(FAITHFULFRAME_CARGO_BUILD
		${CMAKE_COMMAND} -E env "RUSTC=${FAITHFULFRAME_RUSTC_EXECUTABLE}"
		${CARGO_EXECUTABLE} build --locked --release --manifest-path ${FAITHFULFRAME_FFI_MANIFEST}
		--target-dir ${FAITHFULFRAME_RUST_TARGET_DIR} ${FAITHFULFRAME_RUST_TARGET_ARGS})
	if (APPLE AND CMAKE_OSX_DEPLOYMENT_TARGET)
		find_program(OTOOL_EXECUTABLE otool)
		if (NOT OTOOL_EXECUTABLE)
			message(FATAL_ERROR "otool is required to validate the macOS Rust archive deployment target")
		endif()
		find_program(LIPO_EXECUTABLE lipo)
		if (NOT LIPO_EXECUTABLE)
			message(FATAL_ERROR "lipo is required to validate the macOS Rust archive architecture")
		endif()
		set(FAITHFULFRAME_CARGO_BUILD
			${CMAKE_COMMAND} -E env MACOSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}
			${FAITHFULFRAME_CARGO_BUILD})
		set(FAITHFULFRAME_VERIFY_RUST_DEPLOYMENT
			COMMAND ${CMAKE_COMMAND}
				-DFF_ARCHIVE=${FAITHFULFRAME_FFI_LIBRARY}
				-DFF_ARCHITECTURE=${CMAKE_OSX_ARCHITECTURES}
				-DFF_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}
				-DFF_LIPO=${LIPO_EXECUTABLE}
				-DFF_OTOOL=${OTOOL_EXECUTABLE}
				-P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyRustMacOSDeployment.cmake)
	endif()
	add_custom_command(OUTPUT ${FAITHFULFRAME_FFI_LIBRARY}
		COMMAND ${FAITHFULFRAME_CARGO_BUILD}
		${FAITHFULFRAME_VERIFY_RUST_DEPLOYMENT}
		DEPENDS ${FAITHFULFRAME_FFI_MANIFEST}
			${CMAKE_CURRENT_SOURCE_DIR}/rust/faithfulframe-core/Cargo.toml
			${CMAKE_CURRENT_SOURCE_DIR}/rust-toolchain.toml
			${CMAKE_CURRENT_SOURCE_DIR}/cmake/ExtractMacOSMinimumVersions.cmake
			${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyRustMacOSDeployment.cmake
			${CMAKE_CURRENT_SOURCE_DIR}/rust/faithfulframe-ffi/Cargo.lock
			${CMAKE_CURRENT_SOURCE_DIR}/rust/faithfulframe-ffi/src/lib.rs
			${CMAKE_CURRENT_SOURCE_DIR}/rust/faithfulframe-core/src/framing.rs
			${CMAKE_CURRENT_SOURCE_DIR}/rust/faithfulframe-core/src/lib.rs
			${CARGO_EXECUTABLE}
		COMMENT "Building FaithfulFrame Rust C ABI library"
		# Keep rustup's cargo shim in the directory containing rust-toolchain.toml.
		WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
		VERBATIM)
	add_custom_target(faithfulframe-rust-ffi-build DEPENDS ${FAITHFULFRAME_FFI_LIBRARY})
	add_library(faithfulframe-rust-ffi STATIC IMPORTED GLOBAL)
	set_target_properties(faithfulframe-rust-ffi PROPERTIES IMPORTED_LOCATION ${FAITHFULFRAME_FFI_LIBRARY})
	if (WIN32)
		target_link_libraries(faithfulframe-rust-ffi INTERFACE ws2_32 ntdll userenv)
	endif()
	add_executable(faithfulframe-rust-bridge-smoke
		tests/faithfulframe-core-bridge-smoke.cpp src/faithfulframe-core-bridge.cpp)
	add_executable(faithfulframe-source-registration-smoke
		tests/faithfulframe-source-registration-smoke.c)
	target_include_directories(faithfulframe-source-registration-smoke PRIVATE src)
	add_executable(faithfulframe-session-executor-smoke
		tests/faithfulframe-session-executor-smoke.cpp src/faithfulframe-core-bridge.cpp)
	target_compile_definitions(faithfulframe-session-executor-smoke PRIVATE
		FAITHFULFRAME_TEST_REAPER_JOIN_HOOK=1 FAITHFULFRAME_TEST_REAPER_ENQUEUE_HOOK=1
		FAITHFULFRAME_TEST_REAPER_QUEUE_LOCK_HOOK=1 FAITHFULFRAME_TEST_REAPER_WAIT_HOOK=1
		FAITHFULFRAME_TEST_REAPER_SHUTDOWN_HOOK=1
		FAITHFULFRAME_TEST_SESSION_WORK_HOOK=1)
	add_executable(faithfulframe-detector-worker-smoke
		tests/faithfulframe-detector-worker-smoke.cpp src/face-detector-base.cpp)
	add_executable(faithfulframe-tracker-worker-smoke
		tests/faithfulframe-tracker-worker-smoke.cpp src/face-tracker-base.cpp)
	add_executable(faithfulframe-worker-log-gate-smoke tests/faithfulframe-worker-log-gate-smoke.cpp)
	target_include_directories(faithfulframe-worker-log-gate-smoke PRIVATE src)
	add_executable(faithfulframe-performance-log-smoke tests/faithfulframe-performance-log-smoke.cpp)
	target_include_directories(faithfulframe-performance-log-smoke PRIVATE ui)
	if (WITH_DOCK)
		add_executable(faithfulframe-performance-spool-smoke tests/faithfulframe-performance-spool-smoke.cpp)
		target_include_directories(faithfulframe-performance-spool-smoke PRIVATE ui)
	endif()
	add_executable(faithfulframe-texture-object-smoke
		tests/faithfulframe-texture-object-smoke.cpp src/texture-object.cpp)
	add_executable(faithfulframe-preview-geometry-smoke tests/faithfulframe-preview-geometry-smoke.cpp)
	add_executable(faithfulframe-crop-selector-smoke tests/faithfulframe-crop-selector-smoke.cpp)
	target_include_directories(faithfulframe-crop-selector-smoke PRIVATE src rust/faithfulframe-ffi/include)
	add_executable(faithfulframe-active-slot-smoke tests/faithfulframe-active-slot-smoke.cpp)
	target_include_directories(faithfulframe-active-slot-smoke PRIVATE src)
	target_link_libraries(faithfulframe-active-slot-smoke PRIVATE Threads::Threads)
	add_executable(faithfulframe-candidate-selection-smoke tests/faithfulframe-candidate-selection-smoke.cpp)
	add_executable(faithfulframe-candidate-handle-smoke tests/faithfulframe-candidate-handle-smoke.cpp)
	add_executable(faithfulframe-source-identity-smoke tests/faithfulframe-source-identity-smoke.cpp)
	target_include_directories(faithfulframe-source-identity-smoke PRIVATE src)
	add_executable(faithfulframe-single-adapter-lease-smoke
		tests/faithfulframe-single-adapter-lease-smoke.cpp)
	target_include_directories(faithfulframe-single-adapter-lease-smoke PRIVATE src)
	target_link_libraries(faithfulframe-single-adapter-lease-smoke PRIVATE Threads::Threads)
	add_executable(faithfulframe-deferred-pending-smoke tests/faithfulframe-deferred-pending-smoke.cpp)
	target_include_directories(faithfulframe-deferred-pending-smoke PRIVATE src)
	target_link_libraries(faithfulframe-deferred-pending-smoke PRIVATE Threads::Threads)
	add_executable(faithfulframe-tracker-association-smoke tests/faithfulframe-tracker-association-smoke.cpp)
	add_executable(faithfulframe-tracker-worker-budget-smoke
		tests/faithfulframe-tracker-worker-budget-smoke.cpp)
	target_include_directories(faithfulframe-tracker-worker-budget-smoke PRIVATE src)
	add_library(faithfulframe-offline-frame-sequence STATIC
		tests/faithfulframe-offline-frame-sequence.cpp)
	target_include_directories(faithfulframe-offline-frame-sequence PUBLIC tests)
	add_executable(faithfulframe-offline-frame-sequence-smoke
		tests/faithfulframe-offline-frame-sequence-smoke.cpp)
	target_link_libraries(faithfulframe-offline-frame-sequence-smoke PRIVATE faithfulframe-offline-frame-sequence)
	add_executable(faithfulframe-offline-replay
		tests/faithfulframe-offline-replay.cpp
		tests/faithfulframe-offline-detector-worker.cpp
		src/face-detector-dlib-hog.cpp
		src/texture-object.cpp)
	add_executable(faithfulframe-offline-session-replay
		tests/faithfulframe-offline-session-replay.cpp
		tests/faithfulframe-offline-detector-worker.cpp
		src/face-tracker-manager.cpp
		src/face-tracker-base.cpp
		src/face-tracker-dlib.cpp
		src/face-detector-dlib-hog.cpp
		src/face-detector-dlib-cnn.cpp
		src/texture-object.cpp)
	add_dependencies(faithfulframe-offline-session-replay faithfulframe-rust-ffi-build)
	target_include_directories(faithfulframe-offline-session-replay PRIVATE
		${CMAKE_CURRENT_BINARY_DIR} src tests rust/faithfulframe-ffi/include
		"$<TARGET_PROPERTY:OBS::libobs,INTERFACE_INCLUDE_DIRECTORIES>")
	target_link_libraries(faithfulframe-offline-session-replay PRIVATE
		faithfulframe-offline-frame-sequence faithfulframe-rust-ffi dlib Threads::Threads OBS::libobs)
	if (OS_WINDOWS)
		target_link_libraries(faithfulframe-offline-session-replay PRIVATE OBS::w32-pthreads)
		add_custom_command(TARGET faithfulframe-offline-session-replay POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy_if_different
				"$<TARGET_FILE:OBS::w32-pthreads>"
				"$<TARGET_FILE_DIR:faithfulframe-offline-session-replay>"
			VERBATIM)
	endif()
	if (OS_WINDOWS)
		target_link_libraries(faithfulframe-offline-replay PRIVATE Psapi)
	endif()
	add_executable(faithfulframe-dlib-tracker-lifetime-smoke
		tests/faithfulframe-dlib-tracker-lifetime-smoke.cpp
		src/face-tracker-base.cpp
		src/face-tracker-dlib.cpp
		src/texture-object.cpp)
	target_compile_definitions(faithfulframe-dlib-tracker-lifetime-smoke PRIVATE
		FAITHFULFRAME_TEST_TRACKER_TEARDOWN_HOOK=1)
	add_dependencies(faithfulframe-rust-bridge-smoke faithfulframe-rust-ffi-build)
	add_dependencies(faithfulframe-session-executor-smoke faithfulframe-rust-ffi-build)
	target_include_directories(faithfulframe-rust-bridge-smoke PRIVATE src rust/faithfulframe-ffi/include)
	target_include_directories(faithfulframe-session-executor-smoke PRIVATE src rust/faithfulframe-ffi/include)
	target_include_directories(faithfulframe-detector-worker-smoke PRIVATE ${CMAKE_CURRENT_BINARY_DIR} src)
	target_include_directories(faithfulframe-tracker-worker-smoke PRIVATE ${CMAKE_CURRENT_BINARY_DIR} src)
	target_include_directories(faithfulframe-texture-object-smoke PRIVATE ${CMAKE_CURRENT_BINARY_DIR} src)
	target_compile_definitions(faithfulframe-detector-worker-smoke PRIVATE
		FAITHFULFRAME_DETECTOR_WORKER_STANDALONE=1)
	target_include_directories(faithfulframe-preview-geometry-smoke PRIVATE src rust/faithfulframe-ffi/include)
	target_include_directories(faithfulframe-candidate-selection-smoke PRIVATE src rust/faithfulframe-ffi/include)
	target_include_directories(faithfulframe-candidate-handle-smoke PRIVATE src)
	target_include_directories(faithfulframe-tracker-association-smoke PRIVATE src)
	target_include_directories(faithfulframe-offline-replay PRIVATE
		${CMAKE_CURRENT_BINARY_DIR} src tests
		"$<TARGET_PROPERTY:OBS::libobs,INTERFACE_INCLUDE_DIRECTORIES>")
	target_include_directories(faithfulframe-dlib-tracker-lifetime-smoke PRIVATE
		${CMAKE_CURRENT_BINARY_DIR} src
		"$<TARGET_PROPERTY:OBS::libobs,INTERFACE_INCLUDE_DIRECTORIES>")
	target_link_libraries(faithfulframe-rust-bridge-smoke PRIVATE faithfulframe-rust-ffi)
	target_link_libraries(faithfulframe-rust-bridge-smoke PRIVATE Threads::Threads)
	target_link_libraries(faithfulframe-session-executor-smoke PRIVATE faithfulframe-rust-ffi Threads::Threads)
	target_link_libraries(faithfulframe-detector-worker-smoke PRIVATE Threads::Threads)
	target_link_libraries(faithfulframe-tracker-worker-smoke PRIVATE Threads::Threads)
	target_link_libraries(faithfulframe-texture-object-smoke PRIVATE dlib Threads::Threads)
	target_link_libraries(faithfulframe-offline-replay PRIVATE
		faithfulframe-offline-frame-sequence dlib Threads::Threads)
	target_link_libraries(faithfulframe-dlib-tracker-lifetime-smoke PRIVATE dlib Threads::Threads)
	if (OS_WINDOWS)
		target_link_libraries(faithfulframe-dlib-tracker-lifetime-smoke PRIVATE OBS::w32-pthreads)
		add_custom_command(TARGET faithfulframe-dlib-tracker-lifetime-smoke POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy_if_different
				"$<TARGET_FILE:OBS::w32-pthreads>"
				"$<TARGET_FILE_DIR:faithfulframe-dlib-tracker-lifetime-smoke>"
			VERBATIM)
	endif()
	enable_testing()
	add_test(NAME faithfulframe-preview-geometry-smoke COMMAND faithfulframe-preview-geometry-smoke)
	add_test(NAME faithfulframe-crop-selector-smoke COMMAND faithfulframe-crop-selector-smoke)
	add_test(NAME faithfulframe-active-slot-smoke COMMAND faithfulframe-active-slot-smoke)
	add_test(NAME faithfulframe-candidate-selection-smoke COMMAND faithfulframe-candidate-selection-smoke)
	add_test(NAME faithfulframe-candidate-handle-smoke COMMAND faithfulframe-candidate-handle-smoke)
	add_test(NAME faithfulframe-source-identity-smoke COMMAND faithfulframe-source-identity-smoke)
	add_test(NAME faithfulframe-single-adapter-lease-smoke COMMAND faithfulframe-single-adapter-lease-smoke)
	add_test(NAME faithfulframe-deferred-pending-smoke COMMAND faithfulframe-deferred-pending-smoke)
	add_test(NAME faithfulframe-rust-bridge-smoke COMMAND faithfulframe-rust-bridge-smoke)
	add_test(NAME faithfulframe-session-executor-smoke COMMAND faithfulframe-session-executor-smoke)
	add_test(NAME faithfulframe-source-registration-smoke COMMAND faithfulframe-source-registration-smoke)
	add_test(NAME faithfulframe-macos-minimum-version-parser
		COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/VerifyMacOSMinimumVersions.cmake)
	if(APPLE)
		add_test(NAME faithfulframe-macos-deployment-default
			COMMAND ${CMAKE_COMMAND}
				-DFAITHFULFRAME_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
				-P ${CMAKE_CURRENT_SOURCE_DIR}/tests/VerifyMacOSDeploymentDefault.cmake)
	endif()
	add_test(NAME faithfulframe-detector-worker-smoke COMMAND faithfulframe-detector-worker-smoke)
	add_test(NAME faithfulframe-tracker-worker-smoke COMMAND faithfulframe-tracker-worker-smoke)
	add_test(NAME faithfulframe-worker-log-gate-smoke COMMAND faithfulframe-worker-log-gate-smoke)
	add_test(NAME faithfulframe-performance-log-smoke COMMAND faithfulframe-performance-log-smoke)
	add_test(NAME faithfulframe-texture-object-smoke COMMAND faithfulframe-texture-object-smoke)
	add_test(NAME faithfulframe-tracker-association-smoke COMMAND faithfulframe-tracker-association-smoke)
	add_test(NAME faithfulframe-tracker-worker-budget-smoke COMMAND faithfulframe-tracker-worker-budget-smoke)
	add_test(NAME faithfulframe-offline-frame-sequence-smoke
		COMMAND faithfulframe-offline-frame-sequence-smoke)
	find_package(Python3 3.8 COMPONENTS Interpreter QUIET)
	if (Python3_Interpreter_FOUND)
		add_test(NAME faithfulframe-video-replay-wrapper
			COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_faithfulframe_video_replay.py)
	endif()
	add_test(NAME faithfulframe-offline-replay-help COMMAND faithfulframe-offline-replay --help)
	add_test(NAME faithfulframe-offline-session-replay-help
		COMMAND faithfulframe-offline-session-replay --help)
	add_test(NAME faithfulframe-dlib-tracker-lifetime-smoke
		COMMAND faithfulframe-dlib-tracker-lifetime-smoke)
	set(FAITHFULFRAME_DLIB_HOG_SMOKE_MODEL
		"${CMAKE_CURRENT_SOURCE_DIR}/data/dlib_hog_model/frontal_face_detector.dat"
		CACHE FILEPATH "Optional dlib HOG model used for the offline detector smoke test")
	if (EXISTS "${FAITHFULFRAME_DLIB_HOG_SMOKE_MODEL}")
		add_test(NAME faithfulframe-offline-replay-detector-smoke
			COMMAND faithfulframe-offline-replay
				--model "${FAITHFULFRAME_DLIB_HOG_SMOKE_MODEL}"
				--manifest "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/offline-smoke.tsv")
		if (Python3_Interpreter_FOUND)
			add_test(NAME faithfulframe-offline-replay-stream-smoke
				COMMAND ${Python3_EXECUTABLE}
					${CMAKE_CURRENT_SOURCE_DIR}/tests/test_faithfulframe_offline_replay_stream.py
					--binary $<TARGET_FILE:faithfulframe-offline-replay>
					--model "${FAITHFULFRAME_DLIB_HOG_SMOKE_MODEL}")
		endif()
		add_test(NAME faithfulframe-offline-session-replay-policy-smoke
			COMMAND faithfulframe-offline-session-replay
				--model "${FAITHFULFRAME_DLIB_HOG_SMOKE_MODEL}"
				--manifest "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/offline-smoke.tsv")
		set_tests_properties(faithfulframe-offline-session-replay-policy-smoke PROPERTIES
			TIMEOUT 30
			PASS_REGULAR_EXPRESSION "\"kind\":\"summary\".*\"state\":\"manual\"")
		if (Python3_Interpreter_FOUND)
			add_test(NAME faithfulframe-offline-session-replay-geometry-integration
				COMMAND ${Python3_EXECUTABLE}
					${CMAKE_CURRENT_SOURCE_DIR}/tests/test_faithfulframe_offline_session_replay.py
					--binary $<TARGET_FILE:faithfulframe-offline-session-replay>
					--model "${FAITHFULFRAME_DLIB_HOG_SMOKE_MODEL}")
			set_tests_properties(faithfulframe-offline-session-replay-geometry-integration
				PROPERTIES TIMEOUT 30)
		endif()
		set_tests_properties(faithfulframe-offline-replay-detector-smoke PROPERTIES
			TIMEOUT 30
			PASS_REGULAR_EXPRESSION
				"process_cpu_us=[0-9]+ process_cpu_percent=[0-9.]+ resident_bytes=[0-9]+ peak_resident_bytes=[0-9]+")
		if (Python3_Interpreter_FOUND)
			set_tests_properties(faithfulframe-offline-replay-stream-smoke PROPERTIES TIMEOUT 30)
		endif()
	endif()
	set(FAITHFULFRAME_SESSION_REPLAY_TESTS faithfulframe-offline-session-replay-help)
	if (EXISTS "${FAITHFULFRAME_DLIB_HOG_SMOKE_MODEL}")
		list(APPEND FAITHFULFRAME_SESSION_REPLAY_TESTS faithfulframe-offline-session-replay-policy-smoke)
		if (Python3_Interpreter_FOUND)
			list(APPEND FAITHFULFRAME_SESSION_REPLAY_TESTS
				faithfulframe-offline-session-replay-geometry-integration)
		endif()
	endif()
	if (APPLE AND FAITHFULFRAME_SESSION_REPLAY_TEST_ENV)
		set_tests_properties(${FAITHFULFRAME_SESSION_REPLAY_TESTS} PROPERTIES
			ENVIRONMENT "${FAITHFULFRAME_SESSION_REPLAY_TEST_ENV}")
	elseif (WIN32)
		if (CMAKE_VERSION VERSION_GREATER_EQUAL 3.22)
			set(_faithfulframe_windows_runtime_dirs)
			if (FAITHFULFRAME_OBS_RUNTIME_DIRECTORY)
				list(APPEND _faithfulframe_windows_runtime_dirs
					"${FAITHFULFRAME_OBS_RUNTIME_DIRECTORY}")
				get_filename_component(_obs_runtime_parent
					"${FAITHFULFRAME_OBS_RUNTIME_DIRECTORY}" DIRECTORY)
				list(APPEND _faithfulframe_windows_runtime_dirs "${_obs_runtime_parent}")
			endif()
			foreach(_prefix IN LISTS CMAKE_PREFIX_PATH)
				list(APPEND _faithfulframe_windows_runtime_dirs
					"${_prefix}"
					"${_prefix}/lib/64bit"
					"${_prefix}/lib"
					"${_prefix}/bin/64bit"
					"${_prefix}/bin")
				get_filename_component(_prefix_parent "${_prefix}" DIRECTORY)
				list(APPEND _faithfulframe_windows_runtime_dirs
					"${_prefix_parent}/obs-studio/bin/64bit"
					"${_prefix_parent}/obs-studio/bin")
			endforeach()
			set(_faithfulframe_windows_path_modifications)
			foreach(_runtime_directory IN LISTS _faithfulframe_windows_runtime_dirs)
				if (IS_DIRECTORY "${_runtime_directory}")
					list(APPEND _faithfulframe_windows_path_modifications
						"PATH=path_list_prepend:${_runtime_directory}")
				endif()
			endforeach()
			set_tests_properties(${FAITHFULFRAME_SESSION_REPLAY_TESTS} PROPERTIES
				ENVIRONMENT_MODIFICATION "${_faithfulframe_windows_path_modifications}")
		else()
			string(REPLACE ";" "\\;" _faithfulframe_runtime_path
				"${FAITHFULFRAME_SESSION_REPLAY_TEST_PATH}")
			string(REPLACE ";" "\\;" _faithfulframe_existing_path "$ENV{PATH}")
			set_tests_properties(${FAITHFULFRAME_SESSION_REPLAY_TESTS} PROPERTIES
				ENVIRONMENT "PATH=${_faithfulframe_runtime_path}\\;${_faithfulframe_existing_path}")
		endif()
	endif()
	set_tests_properties(faithfulframe-detector-worker-smoke faithfulframe-tracker-worker-smoke
		faithfulframe-worker-log-gate-smoke
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
	target_include_directories(faithfulframe-tracker-worker-smoke PRIVATE
		"$<TARGET_PROPERTY:OBS::libobs,INTERFACE_INCLUDE_DIRECTORIES>")
	if (OS_WINDOWS)
		target_link_libraries(faithfulframe-tracker-worker-smoke PRIVATE OBS::w32-pthreads)
		add_custom_command(TARGET faithfulframe-tracker-worker-smoke POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy_if_different
				"$<TARGET_FILE:OBS::w32-pthreads>"
				"$<TARGET_FILE_DIR:faithfulframe-tracker-worker-smoke>"
			VERBATIM)
	endif()
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
	if (TARGET faithfulframe-performance-spool-smoke)
		target_link_libraries(faithfulframe-performance-spool-smoke PRIVATE Qt::Core)
		if (WIN32)
			set(_qt_core_dir_variable "Qt${QT_VERSION}Core_DIR")
			get_filename_component(_qt_core_config_dir "${${_qt_core_dir_variable}}" ABSOLUTE)
			get_filename_component(_qt_core_runtime_dir "${_qt_core_config_dir}/../../../bin" ABSOLUTE)
			add_test(NAME faithfulframe-performance-spool-smoke
				COMMAND ${CMAKE_COMMAND} -E env
					"PATH=${_qt_core_runtime_dir}$<SEMICOLON>$ENV{PATH}"
					"$<TARGET_FILE:faithfulframe-performance-spool-smoke>")
		else()
			add_test(NAME faithfulframe-performance-spool-smoke COMMAND faithfulframe-performance-spool-smoke)
		endif()
	endif()
	set(plugin_additional_incs ${plugin_additional_incs} src)
	set(plugin_additional_libs ${plugin_additional_libs} Qt::Gui Qt::Widgets)
	# Only for Wayland with Qt before 6.9, where it comes with Qt Gui
	if (OS_LINUX AND Qt${QT_VERSION}Core_VERSION VERSION_LESS 6.9)
		find_package(Qt6 QUIET COMPONENTS GuiPrivate)
		set(plugin_additional_libs ${plugin_additional_libs} Qt::GuiPrivate)
	endif()
endif()

if(APPLE)
	include(cmake/VerifyMacOSLinkTargets.cmake)
	set(_faithfulframe_macos_link_targets OBS::libobs OBS::obs-frontend-api)
	if(WITH_DOCK)
		list(APPEND _faithfulframe_macos_link_targets
			Qt${QT_VERSION}::Core
			Qt${QT_VERSION}::Gui
			Qt${QT_VERSION}::Widgets)
	endif()
	faithfulframe_verify_macos_link_targets(${_faithfulframe_macos_link_targets})
	unset(_faithfulframe_macos_link_targets)
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
	# GCC 16 diagnoses an unused variable in dlib's point_transforms.h as an
	# error when compiling the dlib-backed translation units below. Keep the
	# warning visible, but do not let this external-header warning break Linux
	# package builds; all other warnings remain errors.
	if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "16.0")
		set_source_files_properties(
			src/face-detector-dlib-hog.cpp
			src/face-detector-dlib-cnn.cpp
			src/face-tracker-dlib.cpp
			PROPERTIES COMPILE_OPTIONS "-Wno-error=unused-but-set-variable"
		)
	endif()

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
