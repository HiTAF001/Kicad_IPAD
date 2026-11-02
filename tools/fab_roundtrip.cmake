# ctest helper: write the fabrication files of a board, read them back as a Gerber board and
# check that every layer and every drill hit came back.
file( REMOVE_RECURSE ${OUT} )
file( MAKE_DIRECTORY ${OUT} )
execute_process( COMMAND ${KPCLI} fab ${IN} ${OUT} RESULT_VARIABLE rc OUTPUT_QUIET )

if( NOT rc EQUAL 0 )
    message( FATAL_ERROR "kpcli fab failed (${rc})" )
endif()

# the folder (loose files plus the zip of the same files, which must be read once)
execute_process( COMMAND ${KPCLI} stats ${OUT} RESULT_VARIABLE rc OUTPUT_VARIABLE text )

if( NOT rc EQUAL 0 )
    message( FATAL_ERROR "reading the Gerber folder failed (${rc})" )
endif()

foreach( expect "generator: gerber" "holes: 251" "copper layers: 2" "F.SilkS" "B.Mask" "Edge.Cuts" )
    string( FIND "${text}" "${expect}" at )

    if( at EQUAL -1 )
        message( FATAL_ERROR "missing \"${expect}\" in:\n${text}" )
    endif()
endforeach()

# the zip alone
file( GLOB zip ${OUT}/*-fab.zip )
execute_process( COMMAND ${KPCLI} stats ${zip} RESULT_VARIABLE rc OUTPUT_VARIABLE text )
string( FIND "${text}" "holes: 251" at )

if( NOT rc EQUAL 0 OR at EQUAL -1 )
    message( FATAL_ERROR "reading the zip failed:\n${text}" )
endif()
