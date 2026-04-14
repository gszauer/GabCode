file(GLOB CORE_SOURCES *.cpp)

add_library(gabcore_static STATIC ${CORE_SOURCES})
target_include_directories(gabcore_static
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(gabcore_static
    PUBLIC nlohmann_json
)
