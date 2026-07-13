cmake_minimum_required(VERSION 3.25)

# The executable targets all source files in the app
add_executable(sample_app
    src/main.cpp
    src/middleware/request_id_middleware.cpp
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
