if(NOT DEFINED ROOT OR NOT DEFINED GATE)
    message(FATAL_ERROR "Negative architecture probe requires -DROOT and -DGATE")
endif()

string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef PROBE_SUFFIX)
set(PROBE_ROOT "${CMAKE_CURRENT_BINARY_DIR}/architecture-gate-probe-${PROBE_SUFFIX}")
file(REMOVE_RECURSE "${PROBE_ROOT}")
file(MAKE_DIRECTORY "${PROBE_ROOT}")
file(COPY "${ROOT}/" DESTINATION "${PROBE_ROOT}")

set(PROBE_SOURCE "${PROBE_ROOT}/Editor/Tools/UIDesigner/UIDesigner.cpp")
file(APPEND "${PROBE_SOURCE}"
     "\n// Intentional negative probe; this must be rejected.\n"
     "void PrismatiXArchitectureProbe() { session.History().Execute(command); }\n")

execute_process(
    COMMAND "${CMAKE_COMMAND}" -DROOT=${PROBE_ROOT} -P "${GATE}"
    RESULT_VARIABLE PROBE_RESULT
    OUTPUT_VARIABLE PROBE_OUTPUT
    ERROR_VARIABLE PROBE_ERROR)
file(REMOVE_RECURSE "${PROBE_ROOT}")

if(PROBE_RESULT EQUAL 0)
    message(FATAL_ERROR "Architecture gate accepted an intentional History().Execute() bypass")
endif()
string(CONCAT PROBE_LOG "${PROBE_OUTPUT}" "${PROBE_ERROR}")
if(NOT PROBE_LOG MATCHES "mutation bypass.*History")
    message(FATAL_ERROR "Architecture gate failed for the wrong reason:\n${PROBE_LOG}")
endif()
message(STATUS "Architecture gate negative probe rejected the intentional mutation bypass")
