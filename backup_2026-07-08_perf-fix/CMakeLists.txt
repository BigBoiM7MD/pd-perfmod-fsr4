cmake_minimum_required(VERSION 3.10)
project(pd-perfmod-fsr4 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(${PROJECT_NAME} SHARED
    src/dllmain.cpp
    src/PDPerfPlugin.cpp
    src/FSR4Backend.cpp
    src/fsr4_overlay.cpp
    src/OriginalPlugin.cpp
    src/Logging.cpp
)

target_include_directories(${PROJECT_NAME} PUBLIC include)

target_link_libraries(${PROJECT_NAME} PRIVATE d3d12 dxgi)

# Enable /EHa to catch both C++ and SEH exceptions
target_compile_options(${PROJECT_NAME} PRIVATE /EHa)

set_target_properties(${PROJECT_NAME} PROPERTIES
    OUTPUT_NAME "PDPerfPlugin"
    PREFIX ""
)
