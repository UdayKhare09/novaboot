cmake_minimum_required(VERSION 3.25)

add_executable(knowledge_hub_app
    src/main.cpp
)

target_include_directories(knowledge_hub_app
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(knowledge_hub_app
    PRIVATE
        novaboot::novaboot
        novaboot::postgres
        spdlog::spdlog
)
