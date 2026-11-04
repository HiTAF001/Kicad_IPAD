# ctest helper: schematic edits on the sample hierarchy (root and sub-sheet), then undo of
# every edit must give back the original files byte for byte, and redo the edited ones.
set( OPS
     move J1 5.08 2.54 rotate R3 mirror R4 x field R3 Value 22K field R3 Reference R33
     dup R4 10.16 0 wire 210.82 68.58 219.71 68.58 219.71 64.77 label 100 100 TEST_NET global 90
     nc 31.75 50 junction 87.63 45.72 del wire 5 delete C3 rotate D4 cw
     --sheet 1 move "#0" 2.54 0 mirror "#1" y )
set( STEPS 15 )

file( REMOVE_RECURSE ${OUT} )
set( UNDO )
set( REDO )

foreach( i RANGE 1 ${STEPS} )
    list( APPEND UNDO undo )
    list( APPEND REDO redo )
endforeach()

execute_process( COMMAND ${KPCLI} schedit ${IN} ${OUT} ${OPS} save edited ${UNDO} save undone ${REDO} save redone
                 RESULT_VARIABLE rc OUTPUT_VARIABLE log )

if( NOT rc EQUAL 0 )
    message( FATAL_ERROR "kpcli schedit failed (${rc}):\n${log}" )
endif()

get_filename_component( dir ${IN} DIRECTORY )

foreach( f pic_programmer.kicad_sch pic_sockets.kicad_sch )
    execute_process( COMMAND ${CMAKE_COMMAND} -E compare_files ${dir}/${f} ${OUT}/undone/${f} RESULT_VARIABLE same )

    if( NOT same EQUAL 0 )
        message( FATAL_ERROR "${f} after undo differs from the original" )
    endif()

    execute_process( COMMAND ${CMAKE_COMMAND} -E compare_files ${OUT}/edited/${f} ${OUT}/redone/${f} RESULT_VARIABLE same )

    if( NOT same EQUAL 0 )
        message( FATAL_ERROR "${f} after redo differs from the edited file" )
    endif()
endforeach()

# the edits changed both files
foreach( f pic_programmer.kicad_sch pic_sockets.kicad_sch )
    execute_process( COMMAND ${CMAKE_COMMAND} -E compare_files ${dir}/${f} ${OUT}/edited/${f} RESULT_VARIABLE same )

    if( same EQUAL 0 )
        message( FATAL_ERROR "${f} was not changed by the edits" )
    endif()
endforeach()
