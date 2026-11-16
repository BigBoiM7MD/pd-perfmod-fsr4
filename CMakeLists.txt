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

# Enable /EHa to catch both C++ and SEH exceptions
target_compile_options(${PROJECT_NAME} PRIVATE /EHa)

set_target_properties(${PROJECT_NAME} PROPERTIES
    OUTPUT_NAME "PDPerfPlugin"
    PREFIX ""
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

include(CTest)

if(BUILD_TESTING)
    add_library(mock_ffx_loader SHARED tests/mock_ffx_loader.cpp)
    target_compile_options(mock_ffx_loader PRIVATE /EHsc)
    set_target_properties(mock_ffx_loader PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test_fixtures"
        OUTPUT_NAME "amd_fidelityfx_loader_dx12"
        PREFIX ""
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )

    add_executable(test_phase1_startup tests/test_phase1_startup.cpp)
    target_include_directories(test_phase1_startup PRIVATE include)
    target_compile_options(test_phase1_startup PRIVATE /EHsc)
    set_target_properties(test_phase1_startup PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )

    add_executable(test_phase1_fallback_com tests/test_phase1_fallback_com.cpp)
    target_include_directories(test_phase1_fallback_com PRIVATE include)
    target_compile_options(test_phase1_fallback_com PRIVATE /EHsc)
    set_target_properties(test_phase1_fallback_com PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )

    if(CMAKE_CROSSCOMPILING)
        find_program(WINE_EXECUTABLE wine REQUIRED)
        add_test(NAME phase1_startup
            COMMAND "${CMAKE_COMMAND}" -E env WINEDEBUG=-all "${WINE_EXECUTABLE}"
                "$<TARGET_FILE:test_phase1_startup>"
                "$<TARGET_FILE:${PROJECT_NAME}>"
        )
        add_test(NAME phase1_fallback_com
            COMMAND "${CMAKE_COMMAND}" -E env WINEDEBUG=-all "${WINE_EXECUTABLE}"
                "$<TARGET_FILE:test_phase1_fallback_com>"
                "$<TARGET_FILE:${PROJECT_NAME}>"
                "$<TARGET_FILE:mock_ffx_loader>"
        )
    else()
        add_test(NAME phase1_startup
            COMMAND test_phase1_startup "$<TARGET_FILE:${PROJECT_NAME}>"
        )
        add_test(NAME phase1_fallback_com
            COMMAND test_phase1_fallback_com
                "$<TARGET_FILE:${PROJECT_NAME}>"
                "$<TARGET_FILE:mock_ffx_loader>"
        )
    endif()
endif()
