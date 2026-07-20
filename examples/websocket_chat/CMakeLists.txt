cmake_minimum_required(VERSION 3.25)

add_executable(websocket_chat_app
    src/main.cpp
)

target_link_libraries(websocket_chat_app
    PRIVATE
        novaboot::novaboot
        spdlog::spdlog
)
