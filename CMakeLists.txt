# Desktop build of the Netloom core and its test tool (the iPad app is built with Xcode, see
# project.yml). Usage:
#   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build
#   ctest --test-dir build
cmake_minimum_required( VERSION 3.16 )
project( netloom_core CXX )

set( CMAKE_CXX_STANDARD 17 )
set( CMAKE_CXX_STANDARD_REQUIRED ON )

add_library( kpcore STATIC
    core/src/sexpr.cpp
    core/src/board_parser.cpp
    core/src/stroke_font.cpp
    core/src/stroke_glyphs.cpp
    core/src/mesh.cpp
    core/src/pick.cpp
    core/src/connectivity.cpp
    core/src/document.cpp
    core/src/edit.cpp
    core/src/geom.cpp
    core/src/json.cpp
    core/src/drc.cpp
    core/src/fab.cpp
    core/src/route.cpp
    core/src/gerber.cpp
    core/src/sch_parser.cpp
    core/src/sch_mesh.cpp
    core/src/sch_connectivity.cpp
    core/src/sch_pick.cpp
    core/src/i18n.cpp
    core/src/kpcore.cpp
    core/src/sch_to_pcb.cpp
)
target_include_directories( kpcore PUBLIC core/include PRIVATE core/src )

# zip archives of fabrication files are inflated with zlib (part of the iOS SDK too)
find_package( ZLIB REQUIRED )
target_link_libraries( kpcore PUBLIC ZLIB::ZLIB )

if( CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" )
    target_compile_options( kpcore PRIVATE -Wall -Wextra -Wno-unused-parameter )
endif()

option( NETLOOM_SANITIZE "Build with address/undefined sanitizers" OFF )

if( NETLOOM_SANITIZE )
    target_compile_options( kpcore PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer )
    target_link_options( kpcore PUBLIC -fsanitize=address,undefined )
endif()

add_executable( kpcli tools/kpcli.cpp )
target_include_directories( kpcli PRIVATE tools )
target_link_libraries( kpcli PRIVATE kpcore )

enable_testing()
file( GLOB NETLOOM_SAMPLES ${CMAKE_SOURCE_DIR}/samples/*.kicad_pcb )
add_test( NAME load_samples COMMAND kpcli check ${NETLOOM_SAMPLES} )

# Edits, then undo of every edit, must give back the original file byte for byte.
set( NETLOOM_PIC ${CMAKE_SOURCE_DIR}/samples/pic_programmer.kicad_pcb )
add_test( NAME edit_undo_roundtrip
          COMMAND ${CMAKE_COMMAND}
                  -DKPCLI=$<TARGET_FILE:kpcli> -DIN=${NETLOOM_PIC} -DOUT=${CMAKE_BINARY_DIR}/undo_roundtrip.kicad_pcb
                  -P ${CMAKE_SOURCE_DIR}/tools/edit_roundtrip.cmake )

# Fabrication files written for a board must read back as a Gerber board (folder and zip).
add_test( NAME fab_gerber_roundtrip
          COMMAND ${CMAKE_COMMAND}
                  -DKPCLI=$<TARGET_FILE:kpcli> -DIN=${NETLOOM_PIC} -DOUT=${CMAKE_BINARY_DIR}/fab_roundtrip
                  -P ${CMAKE_SOURCE_DIR}/tools/fab_roundtrip.cmake )
