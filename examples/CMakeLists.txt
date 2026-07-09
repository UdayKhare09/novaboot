include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/ComponentScan.cmake)

add_executable(hello_world hello_world.cpp)
target_link_libraries(hello_world PRIVATE novaboot::novaboot spdlog::spdlog)

# Fully automatic Spring Boot-style scanning!
novaboot_auto_scan(
    TARGET hello_world
    DIR    ${CMAKE_CURRENT_SOURCE_DIR}
)
