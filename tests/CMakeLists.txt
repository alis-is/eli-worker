# The Lua worker suite (lib/tests/worker.lua) requires the native fixture via
# the test build's preload registration.
target_sources(eli_worker PRIVATE ../eli_worker_test.c)
target_compile_definitions(eli_worker PRIVATE ELI_WORKER_TESTS)

add_executable(eli_worker_constructor_test
  constructor.c
  ../eli_channel.c
  ../eli_xfer.c
  ../eli_worker_test.c
  ../../lua/onelua.c)
target_compile_definitions(eli_worker_constructor_test PRIVATE MAKE_LIB ELI_WORKER_TESTS)
target_include_directories(eli_worker_constructor_test PRIVATE
  ${LUA_INCLUDE} ${C11THREADS_INCLUDE} ${ELI_EXTRA_UTILS_INCLUDE}
  ${ELI_OS_EXTRA_INCLUDE} ${CMAKE_CURRENT_SOURCE_DIR}/..)
get_target_property(eli_test_libraries eli LINK_LIBRARIES)
target_link_libraries(eli_worker_constructor_test PRIVATE ${eli_test_libraries})
if(UNIX AND NOT APPLE)
  target_link_libraries(eli_worker_constructor_test PRIVATE m dl)
endif()
add_test(NAME eli_worker_constructor COMMAND eli_worker_constructor_test)

foreach(clock native time32)
  add_executable(eli_worker_deadline_${clock}_test deadline.c)
  target_include_directories(eli_worker_deadline_${clock}_test PRIVATE
    ${LUA_INCLUDE} ${C11THREADS_INCLUDE} ${CMAKE_CURRENT_SOURCE_DIR}/..)
  target_link_libraries(eli_worker_deadline_${clock}_test PRIVATE c11threads)
  add_test(NAME eli_worker_deadline_${clock} COMMAND eli_worker_deadline_${clock}_test)
  set_tests_properties(eli_worker_deadline_${clock} PROPERTIES TIMEOUT 5)
endforeach()
target_compile_definitions(eli_worker_deadline_time32_test PRIVATE ELI_TEST_TIME32)
