cmake_minimum_required(VERSION 3.20)
project(lw_signal_capture VERSION 1.1.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC_SEARCH_PATHS "${CMAKE_CURRENT_SOURCE_DIR}/ui")

find_package(Qt6 REQUIRED COMPONENTS Widgets)

# LW Driver SDK. Qt Creator can override these two cache variables if the SDK
# is installed in a non-standard location.
find_path(LW39X0_INCLUDE_DIR
    NAMES lw39x0.h
    HINTS /usr/local/include /usr/include /opt/luowave/include
)
find_library(LW39X0_LIBRARY
    NAMES lw39x0
    HINTS /usr/local/lib /usr/lib /usr/lib/x86_64-linux-gnu /opt/luowave/lib
)

if(NOT LW39X0_INCLUDE_DIR)
    message(FATAL_ERROR
        "lw39x0.h not found. Set LW39X0_INCLUDE_DIR in Qt Creator -> Projects -> CMake.")
endif()
if(NOT LW39X0_LIBRARY)
    message(FATAL_ERROR
        "liblw39x0 not found. Set LW39X0_LIBRARY in Qt Creator -> Projects -> CMake.")
endif()

message(STATUS "LW39X0 include: ${LW39X0_INCLUDE_DIR}")
message(STATUS "LW39X0 library: ${LW39X0_LIBRARY}")

add_executable(lw_signal_capture
    src/main.cpp
    include/app_translator.hpp
    src/app_translator.cpp
    ui/main_window.ui
    include/main_window.hpp
    src/main_window.cpp
    include/rx_config.hpp
    include/rx_worker.hpp
    src/rx_worker.cpp
    include/data_validator.hpp
    src/data_validator.cpp
    include/signal_processor.hpp
    src/signal_processor.cpp
    include/plot_widgets.hpp
    src/plot_widgets.cpp
    resources/resources.qrc
)

target_include_directories(lw_signal_capture PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${LW39X0_INCLUDE_DIR}"
)

target_link_libraries(lw_signal_capture PRIVATE
    Qt6::Widgets
    "${LW39X0_LIBRARY}"
    pthread
    m
)

target_compile_options(lw_signal_capture PRIVATE
    -Wall -Wextra -Wpedantic
)

# Make the SDK shared library discoverable when Qt Creator launches the app.
get_filename_component(LW39X0_LIBRARY_DIR "${LW39X0_LIBRARY}" DIRECTORY)
set_target_properties(lw_signal_capture PROPERTIES
    BUILD_RPATH "${LW39X0_LIBRARY_DIR}"
)

# The release binary embeds the company logo through resources.qrc so branding
# is always available. Keep an external assets copy beside the executable as an
# optional field/customization override.
add_custom_command(TARGET lw_signal_capture POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/assets"
            "$<TARGET_FILE_DIR:lw_signal_capture>/assets"
)
