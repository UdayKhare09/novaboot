cmake_minimum_required(VERSION 3.25)

# Include the Component Scanner CMake module
include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/ComponentScan.cmake)

# The executable targets all source files in the gateway
add_executable(client_gateway
    src/main.cpp
)

target_include_directories(client_gateway
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Link against the novaboot library and logging dependency
target_link_libraries(client_gateway
    PRIVATE
        novaboot::novaboot
        spdlog::spdlog
)

# Auto-scan client directory to run Lombok preprocessor on model/user.h,
# REST client preprocessor on client/user_service_client.h,
# and register controllers / services with DI container!
novaboot_auto_scan(
    TARGET      client_gateway
    DIR         
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    OUTPUT_FILE ${CMAKE_CURRENT_BINARY_DIR}/di_registrations.cpp
)
