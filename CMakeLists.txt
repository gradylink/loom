cmake_minimum_required(VERSION 3.10)
project(loom CXX C)

set(CMAKE_CXX_STANDARD 20)

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/CPM.cmake")
	message(STATUS "Downloading CPM.cmake...")
	file(DOWNLOAD "https://github.com/cpm-cmake/CPM.cmake/releases/latest/download/CPM.cmake"
		 "${CMAKE_CURRENT_SOURCE_DIR}/CPM.cmake"
		 STATUS status LOG log_content)
	if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/CPM.cmake")
		message(FATAL_ERROR "Failed to download CPM.cmake: ${status}\n${log_content}")
	endif()
endif()
include("${CMAKE_CURRENT_SOURCE_DIR}/CPM.cmake")

CPMAddPackage(
    NAME lyra
    GITHUB_REPOSITORY bfgroup/Lyra
    VERSION 1.7.0
    GIT_TAG 1.7.0
)

CPMAddPackage(
  NAME ryml
  GITHUB_REPOSITORY biojppm/rapidyaml
  VERSION 0.15.2
  OPTIONS "RYML_INSTALL OFF"
)

find_package(tree-sitter QUIET)
if(NOT tree-sitter_FOUND)
  CPMAddPackage(
        NAME tree-sitter
        GITHUB_REPOSITORY tree-sitter/tree-sitter
        VERSION 0.25.10
    )
endif()

add_executable(loom src/main.cpp src/compiler.cpp tree-sitter-loom/src/parser.c)
target_include_directories(loom PRIVATE tree-sitter-loom/src)
target_link_libraries(loom PRIVATE tree-sitter bfg::lyra ryml)

install(TARGETS loom RUNTIME DESTINATION bin)
