# ctest helper: apply edits with kpcli, undo all of them, compare with the input.
execute_process( COMMAND ${KPCLI} edit ${IN} ${OUT}
                         move C1 2.54 -1.27 rotate C2 90 field C1 Value 470uF delete P101
                         width 0 0.5 del via 0 del zone 0
                         undo undo undo undo undo undo undo
                 RESULT_VARIABLE rc )

if( NOT rc EQUAL 0 )
    message( FATAL_ERROR "kpcli edit failed (${rc})" )
endif()

execute_process( COMMAND ${CMAKE_COMMAND} -E compare_files ${IN} ${OUT} RESULT_VARIABLE same )

if( NOT same EQUAL 0 )
    message( FATAL_ERROR "file after undo differs from the original" )
endif()
