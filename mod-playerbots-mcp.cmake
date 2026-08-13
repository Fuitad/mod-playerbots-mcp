# Included by AzerothCore's module configuration to register MCP integration tests.

if(BUILD_TESTING)
  set_property(GLOBAL APPEND PROPERTY ACORE_MODULE_TEST_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/tests/cpp/PlayerbotVerificationOperationTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/cpp/PlayerbotVerificationProtocolTest.cpp")
  set_property(GLOBAL APPEND PROPERTY ACORE_MODULE_TEST_INCLUDES
    "${CMAKE_CURRENT_LIST_DIR}/src")
endif()
