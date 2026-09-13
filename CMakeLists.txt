project (eli_worker)

file(GLOB eli_worker_sources ./**.c)
add_library(eli_worker STATIC ${eli_worker_sources})
target_include_directories(eli_worker PRIVATE
  ${LUA_INCLUDE} ${C11THREADS_INCLUDE} ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(eli_worker c11threads)
set_target_properties(eli_worker PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
