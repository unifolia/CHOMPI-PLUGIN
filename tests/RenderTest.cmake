# One render test (tests/CMakeLists.txt), run with cmake -P: plays SCENARIO
# (one chompi_render command per line, # comments) on a fresh copy of CARD,
# then compares the line output with REFERENCE.wav and, if OWN, the
# firmware's MIDI output with REFERENCE.midi. With OWN and
# CHIMPO_UPDATE_REFERENCES set in the environment, writes them instead.
file(STRINGS "${SCENARIO}" lines)
set(script "")
foreach(line IN LISTS lines)
    string(REGEX REPLACE "#.*|\r" "" line "${line}")
    string(STRIP "${line}" line)
    if(line)
        string(APPEND script "${line};")
    endif()
endforeach()

# The firmware rewrites options.json/presets.json at boot.
file(REMOVE_RECURSE "${WORK}")
file(COPY "${CARD}/" DESTINATION "${WORK}/card")
execute_process(
    COMMAND "${RENDER}" --firmware ${FIRMWARE} "${WORK}/card" "${WORK}/line.wav" "${script}"
    OUTPUT_VARIABLE log
    ERROR_VARIABLE errors
    RESULT_VARIABLE result)
file(WRITE "${WORK}/render.log" "${log}${errors}")
if(NOT result EQUAL 0 OR errors)
    message(FATAL_ERROR "chompi_render failed (${result}):\n${errors}")
endif()
string(REGEX MATCHALL "midi out[^\n]*" midi "${log}")
list(JOIN midi "\n" midi)

if(OWN AND DEFINED ENV{CHIMPO_UPDATE_REFERENCES})
    file(COPY_FILE "${WORK}/line.wav" "${REFERENCE}.wav")
    file(WRITE "${REFERENCE}.midi" "${midi}\n")
    message(STATUS "updated ${REFERENCE}.wav and .midi")
    return()
endif()

execute_process(COMMAND "${COMPARE}" "${WORK}/line.wav" "${REFERENCE}.wav" ${TOLERANCE} RESULT_VARIABLE differs)
if(OWN)
    file(READ "${REFERENCE}.midi" expected)
    string(REPLACE "\r" "" expected "${expected}") # a Windows checkout
    if(NOT "${midi}\n" STREQUAL expected)
        file(WRITE "${WORK}/midi.txt" "${midi}\n")
        message(SEND_ERROR "MIDI output differs: compare ${WORK}/midi.txt with ${REFERENCE}.midi")
    endif()
endif()
if(NOT differs EQUAL 0)
    message(SEND_ERROR "line output differs from ${REFERENCE}.wav (render: ${WORK})")
endif()
