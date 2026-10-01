# The shipped Player compiles the QA IPC test interface out (Applications/
# Player/QaIpc.h); only PrismatiXPlayerQA may name its environment variables,
# log lines or error codes. Checking the QA build too proves the scan would
# find the names if they were there.
foreach(variable SHIPPED QA)
    if(NOT EXISTS "${${variable}}")
        message(FATAL_ERROR "Missing Player executable: ${${variable}}")
    endif()
endforeach()
# The compiled-in E2E journeys the IPC replaced must not come back either.
file(STRINGS "${SHIPPED}" shipped
     REGEX "PRISMATIX_QA_IPC|QA IPC|PXQA[0-9]+|PRISMATIX_E2E_[A-Z_]+|E2E journey")
if(shipped)
    message(FATAL_ERROR "PrismatiXPlayer still contains QA hooks: ${shipped}")
endif()
foreach(expected "PRISMATIX_QA_IPC" "QA IPC listening" "PXQA1100")
    file(STRINGS "${QA}" qa REGEX "${expected}")
    if(NOT qa)
        message(FATAL_ERROR
                "PrismatiXPlayerQA lost its QA IPC (${expected}); the scan is not meaningful")
    endif()
endforeach()
