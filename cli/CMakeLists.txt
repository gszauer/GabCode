file(GLOB CLI_SOURCES *.cpp)

add_executable(gab ${CLI_SOURCES})
set_target_properties(gab PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}
)

target_link_libraries(gab PRIVATE gabcore_static)

find_package(CURL REQUIRED)
target_link_libraries(gab PRIVATE CURL::libcurl)

find_package(Threads REQUIRED)
target_link_libraries(gab PRIVATE Threads::Threads)
