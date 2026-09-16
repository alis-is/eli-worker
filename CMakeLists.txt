project (eli_worker)

# Explicit list: the test-only fixture (eli_worker_test.c) lives here too and
# must never be compiled into the production library.
set(eli_worker_sources eli_worker.c eli_channel.c eli_xfer.c)
add_library(eli_worker STATIC ${eli_worker_sources})
target_include_directories(eli_worker PRIVATE
  ${LUA_INCLUDE} ${C11THREADS_INCLUDE} ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(eli_worker c11threads)
set_target_properties(eli_worker PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
