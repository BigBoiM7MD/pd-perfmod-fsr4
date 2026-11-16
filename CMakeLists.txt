cmake_minimum_required(VERSION 3.10)

if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(pd-perfmod-fsr4 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Strip /EHsc or -EHsc injected by toolchain files before targets select /EHa or /EHsc
foreach(flag_var
    CMAKE_CXX_FLAGS
    CMAKE_CXX_FLAGS_DEBUG
    CMAKE_CXX_FLAGS_RELEASE
    CMAKE_CXX_FLAGS_RELWITHDEBINFO
    CMAKE_CXX_FLAGS_MINSIZEREL
)
    if(DEFINED ${flag_var})
        while(${flag_var} MATCHES "(^|[ \t])[/-]EHsc([ \t]|$)")
            string(REGEX REPLACE "(^|[ \t])[/-]EHsc([ \t]|$)" " " ${flag_var} "${${flag_var}}")
        endwhile()
        string(STRIP "${${flag_var}}" ${flag_var})
    endif()
endforeach()

add_library(${PROJECT_NAME} SHARED
    src/dllmain.cpp
    src/PDPerfPlugin.cpp
    src/FSR4Backend.cpp
    src/Logging.cpp
)

target_include_directories(${PROJECT_NAME} PUBLIC include)

target_link_libraries(${PROJECT_NAME} PRIVATE d3d12 dxgi)

# Enable /EHa to catch both C++ and SEH exceptions
target_compile_options(${PROJECT_NAME} PRIVATE /EHa)

set_target_properties(${PROJECT_NAME} PROPERTIES
    OUTPUT_NAME "PDPerfPlugin"
    PREFIX ""
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)
