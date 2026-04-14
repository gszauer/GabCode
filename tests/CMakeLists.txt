file(GLOB TEST_SOURCES test_*.cpp)

foreach(test_file ${TEST_SOURCES})
    get_filename_component(test_name ${test_file} NAME_WE)
    add_executable(${test_name} ${test_file})
    target_link_libraries(${test_name} PRIVATE gabcore_static)
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()
