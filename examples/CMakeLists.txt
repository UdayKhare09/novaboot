cmake_minimum_required(VERSION 3.25)

# Include the Component Scanner CMake module from parent directory
include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/ComponentScan.cmake)

# The executable targets all source files in the app
add_executable(sample_app
    src/main.cpp
)

target_include_directories(sample_app 
    PRIVATE 
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Link against the novaboot HTTP/3 framework library and logging dependency
target_link_libraries(sample_app 
    PRIVATE 
        novaboot::novaboot 
        spdlog::spdlog
)

# Generate ODB database mapping for User entity
odb_generate(sample_app ${CMAKE_CURRENT_SOURCE_DIR}/src/model/user.h)

# Fully automatic Spring Boot-style build-time scanning!
# Scans all headers under src/ and generates di_registrations.cpp
novaboot_auto_scan(
    TARGET      sample_app
    DIR         
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/../include/novaboot/data
    OUTPUT_FILE ${CMAKE_CURRENT_BINARY_DIR}/di_registrations.cpp
)

# ─── client_demo ──────────────────────────────────────────────────────────────
add_executable(client_demo
    src/client_demo.cpp
)

target_include_directories(client_demo
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(client_demo
    PRIVATE
        novaboot::novaboot
        spdlog::spdlog
)

