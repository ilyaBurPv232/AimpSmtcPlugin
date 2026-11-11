cmake_minimum_required(VERSION 3.20)
project(AimpSmtcPlugin LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(cppwinrt CONFIG REQUIRED)

add_library(${PROJECT_NAME} SHARED
        src/MyBasePlugin.cpp
        src/MyBasePlugin.def
)

target_include_directories(${PROJECT_NAME} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Добавлена библиотека shell32.lib
target_link_libraries(${PROJECT_NAME} PRIVATE
        Microsoft::CppWinRT
        windowsapp.lib
        runtimeobject.lib
        shell32.lib
)

target_compile_definitions(${PROJECT_NAME} PRIVATE UNICODE _UNICODE)

if(MSVC)
    set_target_properties(${PROJECT_NAME} PROPERTIES
            LINK_FLAGS "/DEF:${CMAKE_CURRENT_SOURCE_DIR}/src/MyBasePlugin.def"
            COMPILE_FLAGS "/utf-8"
    )
endif()

set_target_properties(${PROJECT_NAME} PROPERTIES
        PREFIX ""
        OUTPUT_NAME "AimpSmtcPlugin"
)