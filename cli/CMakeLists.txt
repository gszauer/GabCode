file(GLOB CLI_SOURCES *.cpp)

add_executable(gabcode ${CLI_SOURCES})
set_target_properties(gabcode PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}
)

target_link_libraries(gabcode PRIVATE gabcore_static)

find_package(CURL REQUIRED)
target_link_libraries(gabcode PRIVATE CURL::libcurl)

find_package(Threads REQUIRED)
target_link_libraries(gabcode PRIVATE Threads::Threads)
