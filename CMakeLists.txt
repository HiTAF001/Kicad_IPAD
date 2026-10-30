# Desktop build of the KiPad core and its test tool (the iPad app is built with Xcode, see
# project.yml). Usage:
#   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build
#   ctest --test-dir build
cmake_minimum_required( VERSION 3.16 )
project( kipad_core CXX )

set( CMAKE_CXX_STANDARD 17 )
set( CMAKE_CXX_STANDARD_REQUIRED ON )

add_library( kpcore STATIC
    core/src/sexpr.cpp
    core/src/board_parser.cpp
    core/src/stroke_font.cpp
    core/src/newstroke_font.cpp
    core/src/mesh.cpp
    core/src/kpcore.cpp
)
target_include_directories( kpcore PUBLIC core/include PRIVATE core/src )

if( CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" )
    target_compile_options( kpcore PRIVATE -Wall -Wextra -Wno-unused-parameter )
endif()

option( KIPAD_SANITIZE "Build with address/undefined sanitizers" OFF )

if( KIPAD_SANITIZE )
    target_compile_options( kpcore PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer )
    target_link_options( kpcore PUBLIC -fsanitize=address,undefined )
endif()

add_executable( kpcli tools/kpcli.cpp )
target_include_directories( kpcli PRIVATE tools )
target_link_libraries( kpcli PRIVATE kpcore )

enable_testing()
file( GLOB KIPAD_SAMPLES ${CMAKE_SOURCE_DIR}/samples/*.kicad_pcb )
add_test( NAME load_samples COMMAND kpcli check ${KIPAD_SAMPLES} )
