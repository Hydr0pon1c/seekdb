# Local correctness tests for the competition SQL extensions.
file(GLOB_RECURSE sql_test_sources CONFIGURE_DEPENDS sql/*.cpp)
add_executable(sql_tests EXCLUDE_FROM_ALL ${sql_test_sources})
target_link_libraries(sql_tests PRIVATE oceanbase gtest gtest_main)
set_target_properties(sql_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/sql")
set(SEEKDB_PRETEST_MODULES sql PARENT_SCOPE)
set(SEEKDB_PRETEST_SQL_TARGET sql_tests PARENT_SCOPE)
add_custom_target(pretest DEPENDS sql_tests)
add_test(NAME sql_tests_shard_0 COMMAND sql_tests)
set_tests_properties(sql_tests_shard_0 PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/run_tests.sh"
  CONTENT "#!/bin/bash\ncd \"${CMAKE_BINARY_DIR}\" || exit 1\nexec ctest \"$@\"\n"
  FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE)
