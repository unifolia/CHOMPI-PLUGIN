cmake_minimum_required(VERSION 3.22)

# Must precede project(), which otherwise caches it empty (= the build machine's
# macOS). FORCE only replaces an empty value, so -D still wins.
if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version" FORCE)
endif()

project(CHIMPO VERSION 1.0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(CHIMPO_BUILD_PLUGIN "Build the AU/VST3 plugin (fetches JUCE)" ON)
option(CHIMPO_BUILD_TOOLS "Build the headless chompi_render tool" ON)
# Off on Windows: its VST3 folder (Common Files) needs an administrator.
if(WIN32)
    set(chimpo_install_default OFF)
else()
    set(chimpo_install_default ON)
endif()
option(CHIMPO_INSTALL_PLUGINS "Copy the built plugins to this user's plug-in folders (JUCE's per-OS defaults)"
    ${chimpo_install_default})
option(CHIMPO_BUILD_HARNESS "Build chompi_harness: offscreen editor snapshots and host-rate renders" OFF)
option(CHIMPO_BUILD_HOSTCHECK "Build chompi_hostcheck: loads the built VST3 as a host and plays each firmware" OFF)
option(CHIMPO_BUILD_TESTS "Build the tests (tests/CMakeLists.txt); run them with ctest" OFF)
set(CHIMPO_FORMATS "AU;VST3;CLAP" CACHE STRING "Plugin formats to build (AU, VST3, CLAP; AU on macOS only)")
set(CHIMPO_CARDS "${CMAKE_CURRENT_SOURCE_DIR}/cards" CACHE PATH
    "Factory SD cards, one folder per firmware, copied into each plugin bundle")

if(WIN32)
    # M_PI, M_E: DaisySP's adsr.cpp and the tools use them unguarded.
    add_compile_definitions(_USE_MATH_DEFINES)
endif()

# ---------------------------------------------------------------------------
# Emulated CHOMPI: what every firmware shares, then each firmware on its own.
# No JUCE dependency.
#
# The firmwares define the same names differently (chompi::NormalPage, the
# daisy::FileStreamingManager each implements, ...); linked side by side,
# their inline definitions would silently merge into one. So each firmware
# compiles with these renamed to <name>_<firmware> by the preprocessor, which
# leaves the sources as released and works with any compiler and linker.
# The classes are global in more than one firmware; KeyRequest only clashes in
# Debug, where its constructors aren't inlined. chompi_firmware_names below
# fails the build on a name still shared.
# ---------------------------------------------------------------------------
set(chompi_renamed chompi daisy daisysp MidiManager clockManager DjFilter KeyRequest)

add_library(chompi_emulator STATIC
    emulator/src/Machine.cpp
    emulator/src/VirtualCard.cpp
    emulator/src/fatfs.cpp)
target_include_directories(chompi_emulator PUBLIC emulator/include PRIVATE emulator/shim)
set_target_properties(chompi_emulator PROPERTIES POSITION_INDEPENDENT_CODE ON)

# The vendored libraries are the same files in all three releases
# (firmware/README.md); each firmware still compiles its own copy.
set(chompi_libs firmware/libs)
set(chompi_lib_sources
    ${chompi_libs}/libDaisy/src/ui/UI.cpp
    ${chompi_libs}/libDaisy/src/hid/midi_parser.cpp
    ${chompi_libs}/coreJSON/source/core_json.c)
set(chompi_lib_includes
    ${chompi_libs}/libDaisy/src
    ${chompi_libs}/DaisySP/Source
    ${chompi_libs}/DaisySP/Source/Utility
    ${chompi_libs}/coreJSON/source/include)

# chompi_firmware(<name> SOURCES <its .cpp files> DAISYSP <DaisySP modules>)
# firmware/<name>/src plus its port in emulator/src/<name>/, with the shims in
# emulator/shim/<name>/ taking precedence over the shared ones.
function(chompi_firmware name)
    cmake_parse_arguments(FW "" "" "SOURCES;DAISYSP" ${ARGN})
    string(TOUPPER ${name} upper)
    file(GLOB port CONFIGURE_DEPENDS emulator/src/${name}/*.cpp)
    list(TRANSFORM FW_SOURCES PREPEND firmware/${name}/src/)
    list(TRANSFORM FW_DAISYSP PREPEND ${chompi_libs}/DaisySP/Source/)

    add_library(chompi_fw_${name} OBJECT ${port} ${FW_SOURCES} ${FW_DAISYSP} ${chompi_lib_sources})
    target_include_directories(chompi_fw_${name} PRIVATE
        emulator/shim/${name} emulator/shim emulator/include firmware/${name}/src ${chompi_lib_includes})
    set(renames "")
    foreach(renamed IN LISTS chompi_renamed)
        list(APPEND renames ${renamed}=${renamed}_${name})
    endforeach()
    # UNIT_TEST selects libDaisy's host-side ScopedIrqBlocker.
    target_compile_definitions(chompi_fw_${name} PRIVATE UNIT_TEST=1 ${renames})
    # Firmware code is compiled as shipped; the port includes its headers.
    target_compile_options(chompi_fw_${name} PRIVATE -w)
    set_target_properties(chompi_fw_${name} PROPERTIES POSITION_INDEPENDENT_CODE ON)

    target_sources(chompi_emulator PRIVATE $<TARGET_OBJECTS:chompi_fw_${name}>)
    target_compile_definitions(chompi_emulator PRIVATE CHOMPI_HAS_${upper}=1)
    set_property(GLOBAL APPEND PROPERTY CHOMPI_FIRMWARES ${name})
endfunction()

chompi_firmware(tape
    SOURCES FileStreamingManager.cpp
    DAISYSP Control/adsr.cpp Filters/svf.cpp Utility/dcblock.cpp Synthesis/oscillator.cpp)

chompi_firmware(wave
    SOURCES FileStreamingManager.cpp
    DAISYSP Control/adsr.cpp Filters/svf.cpp Utility/dcblock.cpp Synthesis/oscillator.cpp)

chompi_firmware(tempo
    SOURCES FileStreamingManager.cpp
    DAISYSP Control/adsr.cpp Filters/svf.cpp Utility/dcblock.cpp Synthesis/oscillator.cpp
            Effects/sampleratereducer.cpp)

# Reads Itanium-mangled names, so not under MSVC; the sources are the same
# everywhere, so the macOS and Linux builds check for Windows too.
if(CMAKE_NM AND NOT MSVC)
    get_property(firmwares GLOBAL PROPERTY CHOMPI_FIRMWARES)
    set(objects "")
    set(object_args "")
    foreach(name IN LISTS firmwares)
        list(APPEND objects $<TARGET_OBJECTS:chompi_fw_${name}>)
        list(APPEND object_args "-DOBJECTS_${name}=$<TARGET_OBJECTS:chompi_fw_${name}>")
    endforeach()
    add_custom_command(OUTPUT firmware-names.stamp
        COMMAND ${CMAKE_COMMAND} -DNM=${CMAKE_NM} "-DFIRMWARES=${firmwares}" ${object_args}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckFirmwareNames.cmake
        COMMAND ${CMAKE_COMMAND} -E touch firmware-names.stamp
        DEPENDS ${objects} cmake/CheckFirmwareNames.cmake
        COMMENT "Checking that no two firmwares define the same name"
        VERBATIM)
    add_custom_target(chompi_firmware_names DEPENDS firmware-names.stamp)
    # The file-level DEPENDS above doesn't build another target's objects.
    list(TRANSFORM firmwares PREPEND chompi_fw_)
    add_dependencies(chompi_firmware_names ${firmwares})
    add_dependencies(chompi_emulator chompi_firmware_names)
endif()

if(CHIMPO_BUILD_TOOLS)
    add_executable(chompi_render tools/chompi_render.cpp)
    target_link_libraries(chompi_render PRIVATE chompi_emulator)
endif()

# ---------------------------------------------------------------------------
# Plugin
# ---------------------------------------------------------------------------
if(CHIMPO_BUILD_PLUGIN)
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 9.0.4
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)

    # JUCE 9 has no CLAP export: clap-juce-extensions (MIT) wraps the same
    # AudioProcessor. Pinned by commit, like JUCE; its two submodules are the
    # CLAP headers and helpers.
    if("CLAP" IN_LIST CHIMPO_FORMATS)
        FetchContent_Declare(clap_juce_extensions
            GIT_REPOSITORY https://github.com/free-audio/clap-juce-extensions.git
            GIT_TAG 6bde108329ff9fc24404b79b8b397261a75cf6a8
            GIT_SUBMODULES "clap-libs")
        FetchContent_MakeAvailable(clap_juce_extensions)
    endif()

    # juce_add_plugin takes the formats JUCE itself exports; the CLAP target is
    # added after it, below.
    set(chimpo_juce_formats "")
    foreach(format IN LISTS CHIMPO_FORMATS)
        if(format MATCHES "^(AU|VST3)$")
            list(APPEND chimpo_juce_formats ${format})
        endif()
    endforeach()

    juce_add_plugin(Chimpo
        PRODUCT_NAME "CHIMPO"
        COMPANY_NAME "CHIMPO"
        BUNDLE_ID "io.github.chimpo.plugin"
        # Never change: hosts find the plugin in saved projects by these codes.
        PLUGIN_MANUFACTURER_CODE Cdaw
        PLUGIN_CODE Tap2
        FORMATS ${chimpo_juce_formats}
        IS_SYNTH TRUE
        NEEDS_MIDI_INPUT TRUE
        NEEDS_MIDI_OUTPUT TRUE
        EDITOR_WANTS_KEYBOARD_FOCUS FALSE
        AU_MAIN_TYPE kAudioUnitType_MusicDevice
        VST3_CATEGORIES Instrument Sampler)

    target_sources(Chimpo PRIVATE
        plugin/PluginProcessor.cpp
        plugin/PluginEditor.cpp
        plugin/AudioImport.cpp
        plugin/PanelComponents.cpp
        plugin/OutputStage.cpp
        plugin/RateAdapter.cpp
        plugin/RecordingCodec.cpp)

    target_compile_definitions(Chimpo PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        CHIMPO_CARDS="${CHIMPO_CARDS}")

    target_link_libraries(Chimpo
        PRIVATE
            chompi_emulator
            juce::juce_audio_utils
        PUBLIC
            juce::juce_recommended_config_flags
            # JUCE gives clang-cl -flto, but Visual Studio's ClangCL toolset
            # links with link.exe, which can't read LLVM bitcode.
            $<$<NOT:$<BOOL:${MSVC}>>:juce::juce_recommended_lto_flags>
            juce::juce_recommended_warning_flags)

    if("CLAP" IN_LIST CHIMPO_FORMATS)
        # Chimpo_CLAP: CHIMPO.clap beside the VST3 and AU. Its ID never changes:
        # hosts save it in projects. Instrument features keep CLAP hosts from
        # offering it as an effect.
        clap_juce_extensions_plugin(TARGET Chimpo
            CLAP_ID "engineering.bellcast.chimpo"
            CLAP_FEATURES instrument sampler)
    endif()

    # Each bundle carries the factory cards, Contents/Resources/Factory
    # Cards/<FIRMWARE>, where factoryCardFolder() finds them: a plugin copied
    # anywhere is complete. They go in before macOS signs the bundle and JUCE
    # installs it, hence no COPY_PLUGIN_AFTER_BUILD above. A CLAP on Windows and
    # Linux is a single file, so its cards sit in <name>.resources beside it.
    get_property(firmwares GLOBAL PROPERTY CHOMPI_FIRMWARES)
    file(GLOB_RECURSE card_files CONFIGURE_DEPENDS "${CHIMPO_CARDS}/*.wav" "${CHIMPO_CARDS}/*.json")
    foreach(format IN LISTS CHIMPO_FORMATS)
        set(target Chimpo_${format})
        if(NOT TARGET ${target}) # JUCE skips formats the OS lacks
            continue()
        endif()
        if(format STREQUAL "CLAP" AND NOT APPLE)
            set(cards_base "$<TARGET_FILE_DIR:${target}>/$<TARGET_FILE_NAME:${target}>.resources")
        else()
            set(cards_base "$<TARGET_FILE_DIR:${target}>/../Resources")
        endif()
        # A changed card relinks, so copies again (Makefile and Ninja generators).
        set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS ${card_files})
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} "-DCARDS=${CHIMPO_CARDS}" "-DFIRMWARES=${firmwares}"
                    "-DDEST=${cards_base}/Factory Cards"
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CopyFactoryCards.cmake
            VERBATIM)
        if(APPLE)
            # New resources break the seal of JUCE's ad-hoc signature.
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND codesign --force --sign - $<TARGET_BUNDLE_DIR:${target}>
                VERBATIM)
        endif()
    endforeach()
    if(CHIMPO_INSTALL_PLUGINS)
        juce_enable_copy_plugin_step(Chimpo)
        # JUCE's copy step knows its own formats only.
        if(TARGET Chimpo_CLAP)
            if(APPLE)
                set(chimpo_clap_dir "$ENV{HOME}/Library/Audio/Plug-Ins/CLAP")
                add_custom_command(TARGET Chimpo_CLAP POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E make_directory "${chimpo_clap_dir}"
                    COMMAND ${CMAKE_COMMAND} -E rm -rf "${chimpo_clap_dir}/CHIMPO.clap"
                    COMMAND ${CMAKE_COMMAND} -E copy_directory
                            $<TARGET_BUNDLE_DIR:Chimpo_CLAP> "${chimpo_clap_dir}/CHIMPO.clap")
            elseif(UNIX)
                set(chimpo_clap_dir "$ENV{HOME}/.clap")
                add_custom_command(TARGET Chimpo_CLAP POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E make_directory "${chimpo_clap_dir}"
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                            $<TARGET_FILE:Chimpo_CLAP> "${chimpo_clap_dir}/CHIMPO.clap"
                    COMMAND ${CMAKE_COMMAND} -E rm -rf "${chimpo_clap_dir}/CHIMPO.clap.resources"
                    COMMAND ${CMAKE_COMMAND} -E copy_directory
                            "$<TARGET_FILE_DIR:Chimpo_CLAP>/$<TARGET_FILE_NAME:Chimpo_CLAP>.resources"
                            "${chimpo_clap_dir}/CHIMPO.clap.resources")
            endif()
        endif()
    endif()

    if(CHIMPO_BUILD_HARNESS)
        # Reuses the plugin's compiled JUCE; JUCE module headers/defines are private to it.
        add_executable(chompi_harness tools/chompi_harness.cpp)
        target_include_directories(chompi_harness PRIVATE $<TARGET_PROPERTY:Chimpo,INCLUDE_DIRECTORIES>)
        target_compile_definitions(chompi_harness PRIVATE $<TARGET_PROPERTY:Chimpo,COMPILE_DEFINITIONS>)
        target_link_libraries(chompi_harness PRIVATE Chimpo)
    endif()

    if(CHIMPO_BUILD_HOSTCHECK)
        # Its own JUCE build, as a host: no plugin client, no GUI.
        juce_add_console_app(chompi_hostcheck)
        target_sources(chompi_hostcheck PRIVATE tools/chompi_hostcheck.cpp)
        target_compile_definitions(chompi_hostcheck PRIVATE JUCE_PLUGINHOST_VST3=1 JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0)
        target_link_libraries(chompi_hostcheck PRIVATE
            juce::juce_audio_processors_headless juce::juce_recommended_config_flags)
    endif()
endif()

if(CHIMPO_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
