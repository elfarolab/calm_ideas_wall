cmake_minimum_required(VERSION 3.16)

project(calm_ideas_wall CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(APP_NAME calm_ideas_wall)
set(LIBS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/libs)

if(NOT EXISTS ${LIBS_DIR}/rapidjson/include/rapidjson/document.h)
  message(FATAL_ERROR "RapidJSON not found in ${LIBS_DIR}/rapidjson/include/rapidjson/document.h")
endif()

if(NOT EXISTS ${LIBS_DIR}/spdlog/include/spdlog/spdlog.h)
  message(FATAL_ERROR "spdlog not found in ${LIBS_DIR}/spdlog/include/spdlog/spdlog.h")
endif()

if(NOT EXISTS ${LIBS_DIR}/httplib/include/httplib.h AND NOT EXISTS ${LIBS_DIR}/httplib/include/httplib.hpp)
  message(FATAL_ERROR "cpp-httplib not found in ${LIBS_DIR}/httplib/include")
endif()


# -----------------------------------------------------------------------------
# Local header-only dependencies
# -----------------------------------------------------------------------------

# cpp-httplib
add_library(httplib_iface INTERFACE)
target_include_directories(httplib_iface INTERFACE ${LIBS_DIR}/httplib/include)
target_compile_definitions(httplib_iface INTERFACE CPPHTTPLIB_THREAD_POOL_COUNT=8)

# RapidJSON
add_library(rapidjson_iface INTERFACE)
target_include_directories(rapidjson_iface INTERFACE ${LIBS_DIR}/rapidjson/include)

# spdlog header-only
add_library(spdlog_iface INTERFACE)
target_include_directories(spdlog_iface INTERFACE ${LIBS_DIR}/spdlog/include)
target_compile_definitions(spdlog_iface INTERFACE SPDLOG_HEADER_ONLY)

# -----------------------------------------------------------------------------
# Executable
# -----------------------------------------------------------------------------

add_executable(${APP_NAME} main.cpp)

target_include_directories(${APP_NAME} PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(${APP_NAME} PRIVATE
  httplib_iface
  rapidjson_iface
  spdlog_iface
)

# Use a different macro name so it does not collide with C++ variable names.
target_compile_definitions(${APP_NAME} PRIVATE
  BASE_DIR_STR="${CMAKE_CURRENT_SOURCE_DIR}"
)

# On some older GCC versions you need this for std::filesystem
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.1)
  target_link_libraries(${APP_NAME} PRIVATE stdc++fs)
endif()

set_target_properties(${APP_NAME} PROPERTIES
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
)

