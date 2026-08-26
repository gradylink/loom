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
    DOWNLOAD_ONLY TRUE
)
add_library(lyra INTERFACE)
target_include_directories(lyra INTERFACE ${lyra_SOURCE_DIR}/include)

CPMAddPackage(
  NAME ryml
  GITHUB_REPOSITORY biojppm/rapidyaml
  VERSION 0.15.2
  OPTIONS "RYML_INSTALL OFF"
)

CPMAddPackage(
  NAME nlohmann_json
  GITHUB_REPOSITORY nlohmann/json
  VERSION 3.11.3
  OPTIONS "JSON_BuildTests OFF"
)

file(GLOB_RECURSE LOOM_SOURCES src/*.cpp)
add_executable(loom ${LOOM_SOURCES})
target_link_libraries(loom PRIVATE lyra ryml nlohmann_json::nlohmann_json)

install(TARGETS loom RUNTIME DESTINATION bin)

add_custom_target(loom-tests
  COMMAND ${CMAKE_COMMAND} -E env LOOM_EXEC=$<TARGET_FILE:loom> ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_all.sh
  DEPENDS loom
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  COMMENT "Running Loom tests"
)
