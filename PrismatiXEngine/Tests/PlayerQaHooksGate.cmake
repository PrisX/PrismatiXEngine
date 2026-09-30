# The shipped Player compiles PRISMATIX_E2E_* reads out (Applications/Player/
# QaHooks.h); only PrismatiXPlayerQA may name them. Checking the QA build too
# proves the scan would find the names if they were there.
foreach(variable SHIPPED QA)
    if(NOT EXISTS "${${variable}}")
        message(FATAL_ERROR "Missing Player executable: ${${variable}}")
    endif()
endforeach()
file(STRINGS "${SHIPPED}" shipped REGEX "PRISMATIX_E2E_[A-Z_]+")
if(shipped)
    message(FATAL_ERROR "PrismatiXPlayer still contains QA hooks: ${shipped}")
endif()
file(STRINGS "${QA}" qa REGEX "PRISMATIX_E2E_START_LABEL")
if(NOT qa)
    message(FATAL_ERROR "PrismatiXPlayerQA lost its QA hooks; the scan is not meaningful")
endif()
