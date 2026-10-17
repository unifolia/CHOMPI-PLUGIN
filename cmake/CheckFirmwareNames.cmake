# Fails if two firmwares define the same external name: a static link keeps
# one definition and drops the other unannounced (CMakeLists.txt, chompi_renamed).
#
#   cmake -DNM=<nm> -DFIRMWARES=<names> -DOBJECTS_<name>=<objects>... -P CheckFirmwareNames.cmake
cmake_minimum_required(VERSION 3.22)

# Shared by design: the C++ library (std::, libstdc++'s __gnu_cxx::, the C
# string functions' C++ overloads), the emulator's own headers (emu::; each
# port's names there are its own, its helpers in an anonymous namespace) and
# its global shims, coreJSON (the same file in all three), clang's terminate
# helper and GCC's pointer to the exception personality routine. Itanium
# mangling; Mach-O adds a leading '_'. Debug builds emit the most inline
# functions, so check one after changing this.
set(shared "^_?(_Z[A-Z]*(St|3emu|9__gnu_cxx)\
|_Z6(strstr|strchr|memchr|wcschr|wcsstr)|_Z7(strrchr|strpbrk|wcsrchr|wcspbrk|wmemchr)\
|_Z(11disk_status|12__enable_irq|13__disable_irq|23SCB_CleanDCache_by_Addr)\
|JSON_|__clang_call_terminate$|DW\\.ref\\.__gxx_personality_v0$)")

set(seen "")
set(clashes "")
foreach(firmware IN LISTS FIRMWARES)
    execute_process(COMMAND ${NM} -g -P --defined-only ${OBJECTS_${firmware}}
        OUTPUT_VARIABLE listing RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${NM} failed on the ${firmware} objects.")
    endif()
    # -P lines are "<name> <type> ..."; per-file headers end in ':' instead.
    string(REGEX MATCHALL "[^\n]+" lines "${listing}")
    set(names "")
    foreach(line IN LISTS lines)
        # A failed MATCHES clears CMAKE_MATCH_1, hence the copy.
        if(line MATCHES "^([^ ]+) [A-Za-z]")
            set(name ${CMAKE_MATCH_1})
            if(NOT name MATCHES "${shared}")
                list(APPEND names ${name})
            endif()
        endif()
    endforeach()
    list(REMOVE_DUPLICATES names)
    foreach(name IN LISTS names)
        if(name IN_LIST seen)
            list(APPEND clashes ${name})
        endif()
    endforeach()
    list(APPEND seen ${names})
endforeach()

if(clashes)
    list(REMOVE_DUPLICATES clashes)
    list(JOIN clashes "\n  " text)
    message(FATAL_ERROR "Defined by more than one firmware (c++filt demangles):\n  ${text}\n"
        "Add the namespace or class to chompi_renamed in CMakeLists.txt.")
endif()
