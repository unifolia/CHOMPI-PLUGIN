# ctest in the build folder, configured with -DCHIMPO_BUILD_TESTS=ON:
#
#   dsp            OutputStage and RateAdapter against their headers
#   render_<name>  render/<name>.txt played by chompi_render on a fresh factory
#                  card; line out compared with render/<reference>.wav, MIDI
#                  out with .midi
#   plugin         the processor as a host drives it (plugin_test.cpp)
#
# After a deliberate change to what a firmware renders, rewrite the references
# with CHIMPO_UPDATE_REFERENCES=1 ctest -R render, and listen to them before
# committing.

add_executable(chimpo_dsp_test dsp_test.cpp ../plugin/OutputStage.cpp ../plugin/RateAdapter.cpp)
target_include_directories(chimpo_dsp_test PRIVATE ../plugin ../emulator/include)
add_test(NAME dsp COMMAND chimpo_dsp_test)

if(TARGET chompi_render)
    add_executable(chimpo_wav_compare wav_compare.cpp)

    # Floating-point rounding differs between CPUs and compilers: 1 LSB
    # measured, arm64 against x86-64 (2026-10-17).
    set(tolerance 2)

    # chimpo_render_test(<name> <firmware> [SAME_AS <test>]): SAME_AS compares
    # the line out with that test's reference (the same change made another
    # way), not the MIDI out.
    function(chimpo_render_test name firmware)
        cmake_parse_arguments(T "" "SAME_AS" "" ${ARGN})
        set(own ON)
        set(reference ${name})
        if(T_SAME_AS)
            set(own OFF)
            set(reference ${T_SAME_AS})
        endif()
        add_test(NAME render_${name}
            COMMAND ${CMAKE_COMMAND}
                -DRENDER=$<TARGET_FILE:chompi_render> -DCOMPARE=$<TARGET_FILE:chimpo_wav_compare>
                -DFIRMWARE=${firmware} -DCARD=${CHIMPO_CARDS}/${firmware}
                -DSCENARIO=${CMAKE_CURRENT_SOURCE_DIR}/render/${name}.txt
                -DREFERENCE=${CMAKE_CURRENT_SOURCE_DIR}/render/${reference}
                -DOWN=${own} -DTOLERANCE=${tolerance} -DWORK=${CMAKE_CURRENT_BINARY_DIR}/render/${name}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/RenderTest.cmake)
        if(T_SAME_AS)
            set_tests_properties(render_${name} PROPERTIES DEPENDS render_${T_SAME_AS})
        endif()
    endfunction()

    chimpo_render_test(tape tape)
    chimpo_render_test(tape_points tape)
    chimpo_render_test(tape_gain tape)
    chimpo_render_test(tape_gain_control tape SAME_AS tape_gain)
    chimpo_render_test(wave wave)
    chimpo_render_test(wave_pan wave)
    chimpo_render_test(wave_pan_control wave SAME_AS wave_pan)
    chimpo_render_test(tempo tempo)
endif()

if(TARGET Chimpo)
    # As chompi_harness: the plugin's compiled JUCE, its module headers and defines.
    add_executable(chimpo_plugin_test plugin_test.cpp)
    target_include_directories(chimpo_plugin_test PRIVATE $<TARGET_PROPERTY:Chimpo,INCLUDE_DIRECTORIES>)
    target_compile_definitions(chimpo_plugin_test PRIVATE $<TARGET_PROPERTY:Chimpo,COMPILE_DEFINITIONS>)
    target_link_libraries(chimpo_plugin_test PRIVATE Chimpo)

    set(cards ${CMAKE_CURRENT_BINARY_DIR}/plugin-cards)
    add_test(NAME plugin_cards COMMAND ${CMAKE_COMMAND} -E rm -rf ${cards})
    add_test(NAME plugin COMMAND chimpo_plugin_test)
    set_tests_properties(plugin_cards PROPERTIES FIXTURES_SETUP plugin_cards)
    set_tests_properties(plugin PROPERTIES FIXTURES_REQUIRED plugin_cards
        ENVIRONMENT "CHIMPO_TAPE_CARD=${cards}/TAPE;CHIMPO_WAVE_CARD=${cards}/WAVE;CHIMPO_TEMPO_CARD=${cards}/TEMPO")
endif()
