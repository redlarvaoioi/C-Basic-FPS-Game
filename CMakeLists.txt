cmake_minimum_required(VERSION 3.13)
project(sandbox_fps)

set(CMAKE_CXX_STANDARD 17)
set(SOURCES
    src/main.cpp
)

add_executable(sandbox_fps ${SOURCES})

# If building with emscripten (use emcmake when configuring)
if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Emscripten")
    message(STATUS "Configuring for Emscripten")
    set_target_properties(sandbox_fps PROPERTIES
        SUFFIX ".html"
    )
    # Linker and compile options tuned for web
    target_compile_options(sandbox_fps PRIVATE -s USE_WEBGL2=1 -s ALLOW_MEMORY_GROWTH=1)
    target_link_libraries(sandbox_fps PRIVATE "-s USE_WEBGL2=1" "-s ALLOW_MEMORY_GROWTH=1")
endif()

# Copy a tiny index.html wrapper if present (emscripten will generate one)
add_custom_command(TARGET sandbox_fps POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:sandbox_fps>/static
)
