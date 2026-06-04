cmake_minimum_required(VERSION 3.24)

project(qrcode_reader LANGUAGES CXX)

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/config.template.json
    ${CMAKE_CURRENT_BINARY_DIR}/config.json
    COPYONLY
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC AND DEFINED VCPKG_TARGET_TRIPLET AND VCPKG_TARGET_TRIPLET MATCHES "static")
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

find_package(OpenCV REQUIRED COMPONENTS core imgproc highgui objdetect videoio)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(ZXing CONFIG REQUIRED)

add_executable(qrcode_reader
    src/main.cpp
)

if(MSVC)
    set_target_properties(qrcode_reader PROPERTIES
        OUTPUT_NAME "qrcode-reader"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/Debug/X64"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Release/X64"
        RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}/MinSizeRel/X64"
        RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/RelWithDebInfo/X64"
    )
endif()

target_include_directories(qrcode_reader
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(qrcode_reader
    PRIVATE
        ${OpenCV_LIBS}
        nlohmann_json::nlohmann_json
        ZXing::ZXing
        ws2_32
        bcrypt
)

if(WIN32 AND NOT VCPKG_TARGET_TRIPLET MATCHES "static")
    add_custom_command(TARGET qrcode_reader POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:qrcode_reader>
            $<TARGET_FILE_DIR:qrcode_reader>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_BINARY_DIR}/config.json
            $<TARGET_FILE_DIR:qrcode_reader>/config.json
        COMMAND_EXPAND_LISTS
    )
else()
    add_custom_command(TARGET qrcode_reader POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_BINARY_DIR}/config.json
            $<TARGET_FILE_DIR:qrcode_reader>/config.json
    )
endif()

if(MSVC)
    target_compile_options(qrcode_reader PRIVATE /W4 /permissive-)
else()
    target_compile_options(qrcode_reader PRIVATE -Wall -Wextra -Wpedantic)
endif()
