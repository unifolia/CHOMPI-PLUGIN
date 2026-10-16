cmake_minimum_required(VERSION 3.22)

# Must precede project(), which otherwise caches it empty (= the build machine's
# macOS). FORCE only replaces an empty value, so -D still wins.
if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version" FORCE)
endif()

project(CHIMPO VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(CHIMPO_BUILD_PLUGIN "Build the AU/VST3 plugin (fetches JUCE)" ON)
option(CHIMPO_BUILD_TOOLS "Build the headless chompi_render tool" ON)
option(CHIMPO_INSTALL_PLUGINS "Copy built plugins to ~/Library/Audio/Plug-Ins and the factory card to ~/Library/Application Support" ON)
option(CHIMPO_BUILD_HARNESS "Build chompi_harness: offscreen editor snapshots and host-rate renders" OFF)
set(CHIMPO_FORMATS "AU;VST3" CACHE STRING "Plugin formats to build (AU, VST3 or both)")
set(CHIMPO_CARDS "${CMAKE_CURRENT_SOURCE_DIR}/cards" CACHE PATH
    "Factory SD cards, one folder per firmware, copied into new user card folders")

if(NOT APPLE)
    message(FATAL_ERROR "CHIMPO builds on macOS only (the firmware prelink below uses Apple's linker).")
endif()

# ---------------------------------------------------------------------------
# Emulated CHOMPI: what every firmware shares, then each firmware on its own.
# No JUCE dependency.
#
# The firmwares, and the libDaisy/DaisySP versions each vendors, define the
# same names differently; linked side by side they would silently share one
# definition. So each is compiled into an object library and prelinked
# (ld -r) with an export list: only its factory, chompi_create_<name>, stays
# global, the rest becomes local to that object.
# ---------------------------------------------------------------------------
add_library(chompi_emulator STATIC
    emulator/src/Machine.cpp
    emulator/src/VirtualCard.cpp
    emulator/src/fatfs.cpp)
target_include_directories(chompi_emulator PUBLIC emulator/include PRIVATE emulator/shim)
set_target_properties(chompi_emulator PROPERTIES POSITION_INDEPENDENT_CODE ON)

# The prelink drives the linker directly, so it needs the build's archs, SDK
# and minimum macOS spelled out.
set(chompi_prelink_flags -mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET})
foreach(arch IN LISTS CMAKE_OSX_ARCHITECTURES)
    list(APPEND chompi_prelink_flags -arch ${arch})
endforeach()
if(CMAKE_OSX_SYSROOT)
    list(APPEND chompi_prelink_flags -isysroot ${CMAKE_OSX_SYSROOT})
endif()

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
    # Selects libDaisy's host-side ScopedIrqBlocker.
    target_compile_definitions(chompi_fw_${name} PRIVATE UNIT_TEST=1)
    # Firmware code is compiled as shipped; the port includes its headers.
    target_compile_options(chompi_fw_${name} PRIVATE -w)
    set_target_properties(chompi_fw_${name} PROPERTIES POSITION_INDEPENDENT_CODE ON)

    set(exports ${CMAKE_CURRENT_BINARY_DIR}/firmware_${name}.exports)
    set(object ${CMAKE_CURRENT_BINARY_DIR}/firmware_${name}.o)
    file(WRITE ${exports} "_chompi_create_${name}\n")
    add_custom_command(OUTPUT ${object}
        COMMAND ${CMAKE_CXX_COMPILER} ${chompi_prelink_flags} -r -nostdlib
                -Wl,-exported_symbols_list,${exports} $<TARGET_OBJECTS:chompi_fw_${name}> -o ${object}
        DEPENDS $<TARGET_OBJECTS:chompi_fw_${name}> ${exports}
        COMMENT "Prelinking the ${upper} firmware"
        COMMAND_EXPAND_LISTS VERBATIM)
    target_sources(chompi_emulator PRIVATE ${object})
    # The file-level DEPENDS above doesn't build another target's objects.
    add_dependencies(chompi_emulator chompi_fw_${name})
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

    juce_add_plugin(Chimpo
        PRODUCT_NAME "CHIMPO"
        COMPANY_NAME "CHIMPO"
        BUNDLE_ID "io.github.chimpo.plugin"
        # Never change: hosts find the plugin in saved projects by these codes.
        PLUGIN_MANUFACTURER_CODE Cdaw
        PLUGIN_CODE Tap2
        FORMATS ${CHIMPO_FORMATS}
        IS_SYNTH TRUE
        NEEDS_MIDI_INPUT TRUE
        NEEDS_MIDI_OUTPUT TRUE
        EDITOR_WANTS_KEYBOARD_FOCUS FALSE
        AU_MAIN_TYPE kAudioUnitType_MusicDevice
        VST3_CATEGORIES Instrument Sampler
        COPY_PLUGIN_AFTER_BUILD ${CHIMPO_INSTALL_PLUGINS})

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
            juce::juce_recommended_lto_flags
            juce::juce_recommended_warning_flags)

    # The plugin finds the factory cards here first (factoryCardFolder()), so
    # they survive the source tree being moved or deleted. cp -c clones on
    # APFS: no extra disk space while cards/ exists, and a plain copy elsewhere.
    if(CHIMPO_INSTALL_PLUGINS)
        get_property(firmwares GLOBAL PROPERTY CHOMPI_FIRMWARES)
        set(stamps "")
        foreach(name IN LISTS firmwares)
            string(TOUPPER ${name} upper)
            set(source "${CHIMPO_CARDS}/${name}")
            set(dest "$ENV{HOME}/Library/Application Support/CHIMPO/Factory Cards/${upper}")
            file(GLOB_RECURSE files CONFIGURE_DEPENDS "${source}/*.wav" "${source}/*.json")
            add_custom_command(OUTPUT factory-card-${name}.stamp
                COMMAND ${CMAKE_COMMAND} -E rm -rf "${dest}"
                COMMAND ${CMAKE_COMMAND} -E make_directory "${dest}/.."
                COMMAND cp -cR "${source}/" "${dest}"
                COMMAND ${CMAKE_COMMAND} -E touch factory-card-${name}.stamp
                DEPENDS ${files}
                COMMENT "Installing the ${upper} factory card to ${dest}"
                VERBATIM)
            list(APPEND stamps factory-card-${name}.stamp)
        endforeach()
        add_custom_target(Chimpo_FactoryCards ALL DEPENDS ${stamps})
    endif()

    if(CHIMPO_BUILD_HARNESS)
        # Reuses the plugin's compiled JUCE; JUCE module headers/defines are private to it.
        add_executable(chompi_harness tools/chompi_harness.cpp)
        target_include_directories(chompi_harness PRIVATE $<TARGET_PROPERTY:Chimpo,INCLUDE_DIRECTORIES>)
        target_compile_definitions(chompi_harness PRIVATE $<TARGET_PROPERTY:Chimpo,COMPILE_DEFINITIONS>)
        target_link_libraries(chompi_harness PRIVATE Chimpo)
    endif()
endif()
