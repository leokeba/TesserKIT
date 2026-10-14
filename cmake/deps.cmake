# deps.lock: the sibling commits TesserKIT builds against (scripts/pin.sh).
#   tesserkit_pinned(<repo> <var>)      sets <var> to the commit pinned for leokeba/<repo>
#   tesserkit_check_sibling(<repo> <dir>) says when a checkout's HEAD isn't it:
#     a developer's siblings may be ahead (or mid-edit); releases build the pinned ones.
set(TESSERKIT_DEPS_LOCK "${CMAKE_CURRENT_LIST_DIR}/../deps.lock")

function(tesserkit_pinned repo var)
    file(STRINGS "${TESSERKIT_DEPS_LOCK}" lines REGEX "^[^#].*/${repo}@[0-9a-f]+$")
    if(NOT lines)
        message(FATAL_ERROR "deps.lock pins no ${repo}")
    endif()
    list(GET lines 0 line)
    string(REGEX REPLACE "^.*@" "" sha "${line}")
    set(${var} "${sha}" PARENT_SCOPE)
endfunction()

function(tesserkit_check_sibling repo dir)
    tesserkit_pinned(${repo} pinned)
    find_package(Git QUIET)
    if(NOT GIT_FOUND OR NOT EXISTS "${dir}/.git")
        return()
    endif()
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${dir}" rev-parse HEAD
                    OUTPUT_VARIABLE head OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE rc)
    if(rc EQUAL 0 AND NOT head STREQUAL pinned)
        string(SUBSTRING "${head}" 0 7 h)
        string(SUBSTRING "${pinned}" 0 7 p)
        message(WARNING "${repo} at ${dir} is ${h}, deps.lock pins ${p}: releases build ${p} (scripts/pin.sh pins this one)")
    endif()
endfunction()
