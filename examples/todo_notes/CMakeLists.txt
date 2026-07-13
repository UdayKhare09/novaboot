cmake_minimum_required(VERSION 3.25)

add_executable(todo_notes_app
    src/main.cpp
)

target_include_directories(todo_notes_app 
    PRIVATE 
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(todo_notes_app 
    PRIVATE 
        novaboot::novaboot 
        spdlog::spdlog
)

# Generate database mapping using ODB compiler macros for the entities
odb_generate(todo_notes_app ${CMAKE_CURRENT_SOURCE_DIR}/src/model/app_user.h)
odb_generate(todo_notes_app ${CMAKE_CURRENT_SOURCE_DIR}/src/model/todo.h)
odb_generate(todo_notes_app ${CMAKE_CURRENT_SOURCE_DIR}/src/model/note.h)
