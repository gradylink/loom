cmake_minimum_required(VERSION 3.10)
project(loom CXX C)

set(CMAKE_CXX_STANDARD 20)

include(${CMAKE_CURRENT_SOURCE_DIR}/cl-bootstrap.cmake)
cl_repo()

file(GLOB_RECURSE LOOM_SOURCES src/*.cpp)
add_executable(loom ${LOOM_SOURCES})
cl_add_dep(loom lyra)
cl_add_dep(loom nlohmann_json)
cl_add_dep(loom rapidyaml)

install(TARGETS loom RUNTIME DESTINATION bin)

add_custom_target(loom-tests
  COMMAND ${CMAKE_COMMAND} -E env LOOM_EXEC=$<TARGET_FILE:loom> ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_all.sh
  DEPENDS loom
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  COMMENT "Running Loom tests"
)
