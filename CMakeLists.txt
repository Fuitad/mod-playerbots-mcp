if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.20)
  project(mod_playerbots_mcp LANGUAGES CXX)

  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
  include(CTest)

  add_executable(playerbots_mcp_tests
    tests/cpp/PlayerbotMCPStandaloneTest.cpp)
  target_include_directories(playerbots_mcp_tests PRIVATE src)
  target_compile_options(playerbots_mcp_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME playerbots_mcp_tests COMMAND playerbots_mcp_tests)
else()
  if(NOT EXISTS "${CMAKE_SOURCE_DIR}/modules/mod-playerbots/src/Bot/PlayerbotAI.h")
    message(FATAL_ERROR "mod-playerbots-mcp requires mod-playerbots")
  endif()
  if(NOT EXISTS "${CMAKE_SOURCE_DIR}/modules/mod-playerbots-telemetry/src/Bot/Telemetry/PlayerbotInspector.h")
    message(FATAL_ERROR "mod-playerbots-mcp requires mod-playerbots-telemetry")
  endif()
  if(NOT EXISTS "${CMAKE_SOURCE_DIR}/modules/mod-playerbots-recovery/src/Bot/Recovery/PlayerbotRecovery.h")
    message(FATAL_ERROR "mod-playerbots-mcp requires mod-playerbots-recovery")
  endif()

  AC_ADD_SCRIPT("${CMAKE_CURRENT_LIST_DIR}/src")

  if(BUILD_TESTING)
    set_property(GLOBAL APPEND PROPERTY ACORE_MODULE_TEST_SOURCES
      "${CMAKE_CURRENT_LIST_DIR}/tests/cpp/PlayerbotVerificationOperationTest.cpp"
      "${CMAKE_CURRENT_LIST_DIR}/tests/cpp/PlayerbotVerificationProtocolTest.cpp")
    set_property(GLOBAL APPEND PROPERTY ACORE_MODULE_TEST_INCLUDES
      "${CMAKE_CURRENT_LIST_DIR}/src")
  endif()
endif()
