# Copies the factory cards into a plugin bundle (CMakeLists.txt): <CARDS>/<name>
# to <DEST>/<NAME>, card files only (kCardFiles in plugin/PluginProcessor.cpp,
# hidden ones skipped like VirtualCard does). Unchanged files are skipped by
# timestamp; files gone from the source are removed.
#
#   cmake -DCARDS=<dir> -DFIRMWARES=<names> -DDEST=<dir> -P CopyFactoryCards.cmake
cmake_minimum_required(VERSION 3.22)

get_filename_component(DEST "${DEST}" ABSOLUTE)
foreach(name IN LISTS FIRMWARES)
    string(TOUPPER ${name} upper)
    set(from "${CARDS}/${name}")
    set(to "${DEST}/${upper}")

    file(GLOB_RECURSE wanted RELATIVE "${from}" "${from}/*.wav" "${from}/*.json")
    list(FILTER wanted EXCLUDE REGEX "(^|/)\\.")
    if(NOT wanted) # else the bundle's copy would be emptied below
        message(FATAL_ERROR "No card files in ${from} (CHIMPO_CARDS).")
    endif()
    file(GLOB_RECURSE present RELATIVE "${to}" "${to}/*")
    foreach(file IN LISTS present)
        if(NOT file IN_LIST wanted)
            file(REMOVE "${to}/${file}")
        endif()
    endforeach()

    file(COPY "${from}/" DESTINATION "${to}"
        FILES_MATCHING PATTERN "*.wav" PATTERN "*.json" PATTERN ".*" EXCLUDE)
endforeach()
