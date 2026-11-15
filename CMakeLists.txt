cmake_minimum_required(VERSION 3.16)
project(spinjitzu CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-Wall -Wextra)

# -DSANITIZE=thread runs everything under the thread sanitizer.
set(SANITIZE "" CACHE STRING "a sanitizer to build with, e.g. thread")
if(SANITIZE)
    add_compile_options(-fsanitize=${SANITIZE} -g)
    add_link_options(-fsanitize=${SANITIZE})
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(EVDEV REQUIRED libevdev)
pkg_check_modules(RAYLIB REQUIRED raylib)
find_package(Threads REQUIRED)

# Dear ImGui and its raylib backend, kept in third_party/ under their own licences.
add_library(imgui STATIC
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/rlImGui/rlImGui.cpp)
target_include_directories(imgui PUBLIC third_party/imgui third_party/rlImGui ${RAYLIB_INCLUDE_DIRS})
target_compile_definitions(imgui PUBLIC NO_FONT_AWESOME)
target_compile_options(imgui PRIVATE -w)

add_executable(spinjitzu src/app/main.cpp)
target_include_directories(spinjitzu PRIVATE ${EVDEV_INCLUDE_DIRS})
target_link_libraries(spinjitzu imgui ${RAYLIB_LIBRARIES} ${EVDEV_LIBRARIES} Threads::Threads m dl)

add_executable(spike src/spike/main.cpp)
target_include_directories(spike PRIVATE ${EVDEV_INCLUDE_DIRS})
target_link_libraries(spike ${EVDEV_LIBRARIES})

foreach(t engine_test spike_test core_test bridge_test)
    add_executable(${t} tests/${t}.cpp)
    target_include_directories(${t} PRIVATE ${EVDEV_INCLUDE_DIRS})
    target_link_libraries(${t} ${EVDEV_LIBRARIES} Threads::Threads)
endforeach()
