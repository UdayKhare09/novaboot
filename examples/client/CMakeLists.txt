cmake_minimum_required(VERSION 3.25)

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
