cmake_minimum_required(VERSION 3.12)

if(NOT FAITHFULFRAME_SOURCE_DIR)
	message(FATAL_ERROR "FAITHFULFRAME_SOURCE_DIR must identify the FaithfulFrame checkout")
endif()

include("${FAITHFULFRAME_SOURCE_DIR}/cmake/FaithfulFrameMacOSDefaults.cmake")
project(FaithfulFrameMacOSDeploymentDefaultProbe LANGUAGES CXX)
add_executable(faithfulframe-macos-deployment-probe main.cpp)
