# Writes OUT with BENCHMARK_COMMIT: this checkout's commit, -dirty with changes in the sources (new files too), "unknown"
# where git can't say. Run on every build; the file changes only when the value does.
execute_process(COMMAND git rev-parse --short=12 HEAD WORKING_DIRECTORY ${R} OUTPUT_VARIABLE c RESULT_VARIABLE r1
                OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
execute_process(COMMAND git status --porcelain -- runtime android tools third_party WORKING_DIRECTORY ${R} OUTPUT_VARIABLE d
                RESULT_VARIABLE r2 OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT "${r1}" STREQUAL "0" OR NOT "${r2}" STREQUAL "0" OR NOT c)
    set(c unknown)
elseif(d)
    set(c ${c}-dirty)
endif()
set(text "#define BENCHMARK_COMMIT \"${c}\"\n")
set(old "")
if(EXISTS ${OUT})
    file(READ ${OUT} old)
endif()
if(NOT old STREQUAL text)
    file(WRITE ${OUT} "${text}")
endif()
