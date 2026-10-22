# The examples' build options: what an example runs is in Kconfig (its
# main/Kconfig.projbuild), so a profile is a configuration file (DESIGN.md
# 11, Profiles). idf.py's -D options for them are written as one more
# defaults file:
#   tesserkit_example_defaults(<files var> "CONFIG_X=value" ...)
# appends <build>/args.defaults, holding those lines, to the list of defaults
# files in <files var> (for SDKCONFIG_DEFAULTS). Defaults apply to a new
# sdkconfig only, so an existing one that says otherwise stops the build
# rather than build another variant than asked: each variant gets its own
# build directory (-B, -DSDKCONFIG).

function(tesserkit_example_defaults files)
    if(NOT ARGN)
        return()
    endif()
    if(DEFINED SDKCONFIG)
        set(existing "${SDKCONFIG}")
    else()
        set(existing "${CMAKE_SOURCE_DIR}/sdkconfig")
    endif()
    if(EXISTS "${existing}")
        file(STRINGS "${existing}" lines REGEX "^(# )?CONFIG_")
        foreach(arg ${ARGN})
            string(REGEX MATCH "^[A-Za-z0-9_]+" key "${arg}")
            # sdkconfig writes a bool that is off as a comment.
            set(want "${arg}")
            if(arg STREQUAL "${key}=n")
                set(want "# ${key} is not set")
            endif()
            foreach(line ${lines})
                if((line MATCHES "^${key}=" OR line STREQUAL "# ${key} is not set") AND NOT line STREQUAL want)
                    message(FATAL_ERROR "${existing} has \"${line}\", not ${arg}: build this variant in "
                                        "another build directory (-B, -DSDKCONFIG), or remove that sdkconfig")
                endif()
            endforeach()
        endforeach()
    endif()
    string(REPLACE ";" "\n" text "${ARGN}")
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}")
    file(WRITE "${CMAKE_BINARY_DIR}/args.defaults" "# From idf.py's -D options\n${text}\n")
    set(${files} ${${files}} "${CMAKE_BINARY_DIR}/args.defaults" PARENT_SCOPE)
endfunction()
