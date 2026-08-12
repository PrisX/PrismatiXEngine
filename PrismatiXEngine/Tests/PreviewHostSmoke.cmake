if(NOT DEFINED PREVIEW_HOST OR NOT EXISTS "${PREVIEW_HOST}")
    message(FATAL_ERROR "PreviewHost executable is missing")
endif()
execute_process(
    COMMAND "${PREVIEW_HOST}"
    INPUT_FILE "${INPUT_FILE}"
    OUTPUT_VARIABLE OUTPUT
    ERROR_VARIABLE ERRORS
    RESULT_VARIABLE RESULT
    TIMEOUT 15
)
if(NOT RESULT EQUAL 0)
    message(FATAL_ERROR "PreviewHost failed (${RESULT}): ${ERRORS}")
endif()

string(REPLACE "\r\n" "\n" OUTPUT "${OUTPUT}")
string(REGEX REPLACE "\n+$" "" OUTPUT "${OUTPUT}")
string(REPLACE "\n" ";" OUTPUT_LINES "${OUTPUT}")

set(READY_SEEN FALSE)
set(PLAY_ERROR_SEEN FALSE)
set(SHUTDOWN_SEEN FALSE)
set(OUTPUT_EVENT_SEEN FALSE)
set(STATE_EVENT_SEEN FALSE)
set(DEBUG_EVENT_SEEN FALSE)
set(DIAGNOSTICS_EVENT_SEEN FALSE)
set(EXITED_EVENT_SEEN FALSE)

foreach(LINE IN LISTS OUTPUT_LINES)
    string(JSON MESSAGE_TYPE GET "${LINE}" type)
    string(JSON PROTOCOL GET "${LINE}" protocol)
    string(JSON PROTOCOL_VERSION GET "${LINE}" protocolVersion)
    string(JSON SESSION_ID GET "${LINE}" sessionId)
    string(JSON REQUEST_ID GET "${LINE}" requestId)
    string(JSON DOCUMENT_ID GET "${LINE}" documentId)
    string(JSON REVISION GET "${LINE}" revision)
    string(JSON REQUEST_ID_TYPE TYPE "${LINE}" requestId)
    string(JSON DOCUMENT_ID_TYPE TYPE "${LINE}" documentId)
    string(JSON REVISION_TYPE TYPE "${LINE}" revision)

    if(NOT PROTOCOL STREQUAL "PrismatiXPreviewProtocol" OR
       NOT PROTOCOL_VERSION EQUAL 1 OR
       NOT SESSION_ID STREQUAL "smoke-session" OR
       NOT REQUEST_ID_TYPE STREQUAL "STRING" OR
       NOT DOCUMENT_ID_TYPE STREQUAL "STRING" OR
       NOT REVISION_TYPE STREQUAL "NUMBER")
        message(FATAL_ERROR "PreviewHost emitted an incomplete envelope: ${LINE}")
    endif()

    if(MESSAGE_TYPE STREQUAL "ready")
        if(NOT REQUEST_ID STREQUAL "hello-smoke" OR
           NOT DOCUMENT_ID STREQUAL "" OR NOT REVISION EQUAL 0)
            message(FATAL_ERROR "PreviewHost ready ack lost hello correlation: ${LINE}")
        endif()
        set(READY_SEEN TRUE)
    elseif(MESSAGE_TYPE STREQUAL "error")
        string(JSON ERROR_CODE GET "${LINE}" code)
        if(ERROR_CODE STREQUAL "runtime-not-loaded")
            if(NOT REQUEST_ID STREQUAL "play-smoke" OR
               NOT DOCUMENT_ID STREQUAL "document-smoke" OR NOT REVISION EQUAL 7)
                message(FATAL_ERROR "PreviewHost error ack lost play correlation: ${LINE}")
            endif()
            if(NOT READY_SEEN)
                message(FATAL_ERROR "PreviewHost play error preceded the hello ack")
            endif()
            set(PLAY_ERROR_SEEN TRUE)
        endif()
    elseif(MESSAGE_TYPE STREQUAL "shutdownAccepted")
        if(NOT REQUEST_ID STREQUAL "shutdown-smoke" OR
           NOT DOCUMENT_ID STREQUAL "" OR NOT REVISION EQUAL 0)
            message(FATAL_ERROR "PreviewHost shutdown ack lost shutdown correlation: ${LINE}")
        endif()
        if(NOT PLAY_ERROR_SEEN)
            message(FATAL_ERROR "PreviewHost shutdown ack preceded the play ack")
        endif()
        set(SHUTDOWN_SEEN TRUE)
    elseif(MESSAGE_TYPE STREQUAL "output")
        string(JSON ASYNC GET "${LINE}" async)
        string(JSON EVENT_SEQUENCE_TYPE TYPE "${LINE}" eventSequence)
        if(NOT ASYNC OR NOT EVENT_SEQUENCE_TYPE STREQUAL "NUMBER" OR
           NOT REQUEST_ID STREQUAL "hello-smoke" OR
           NOT DOCUMENT_ID STREQUAL "" OR NOT REVISION EQUAL 0)
            message(FATAL_ERROR "PreviewHost output event has invalid correlation: ${LINE}")
        endif()
        if(NOT READY_SEEN)
            message(FATAL_ERROR "PreviewHost output event preceded its hello ack")
        endif()
        set(OUTPUT_EVENT_SEEN TRUE)
    elseif(MESSAGE_TYPE STREQUAL "state")
        string(JSON ASYNC GET "${LINE}" async)
        string(JSON EVENT_SEQUENCE_TYPE TYPE "${LINE}" eventSequence)
        if(NOT ASYNC OR NOT EVENT_SEQUENCE_TYPE STREQUAL "NUMBER" OR
           NOT REQUEST_ID STREQUAL "hello-smoke" OR
           NOT DOCUMENT_ID STREQUAL "" OR NOT REVISION EQUAL 0)
            message(FATAL_ERROR "PreviewHost state event has invalid correlation: ${LINE}")
        endif()
        if(NOT READY_SEEN)
            message(FATAL_ERROR "PreviewHost state event preceded its hello ack")
        endif()
        set(STATE_EVENT_SEEN TRUE)
    elseif(MESSAGE_TYPE STREQUAL "debug")
        string(JSON ASYNC GET "${LINE}" async)
        string(JSON EVENT_SEQUENCE_TYPE TYPE "${LINE}" eventSequence)
        if(NOT ASYNC OR NOT EVENT_SEQUENCE_TYPE STREQUAL "NUMBER" OR
           NOT REQUEST_ID STREQUAL "hello-smoke" OR
           NOT DOCUMENT_ID STREQUAL "" OR NOT REVISION EQUAL 0)
            message(FATAL_ERROR "PreviewHost debug event has invalid correlation: ${LINE}")
        endif()
        if(NOT READY_SEEN)
            message(FATAL_ERROR "PreviewHost debug event preceded its hello ack")
        endif()
        set(DEBUG_EVENT_SEEN TRUE)
    elseif(MESSAGE_TYPE STREQUAL "diagnostics")
        string(JSON ASYNC GET "${LINE}" async)
        string(JSON EVENT_SEQUENCE_TYPE TYPE "${LINE}" eventSequence)
        if(NOT ASYNC OR NOT EVENT_SEQUENCE_TYPE STREQUAL "NUMBER" OR
           NOT REQUEST_ID STREQUAL "play-smoke" OR
           NOT DOCUMENT_ID STREQUAL "document-smoke" OR NOT REVISION EQUAL 7)
            message(FATAL_ERROR "PreviewHost diagnostics event has invalid correlation: ${LINE}")
        endif()
        if(NOT PLAY_ERROR_SEEN)
            message(FATAL_ERROR "PreviewHost diagnostics event preceded its rejected request ack")
        endif()
        set(DIAGNOSTICS_EVENT_SEEN TRUE)
    elseif(MESSAGE_TYPE STREQUAL "exited")
        string(JSON ASYNC GET "${LINE}" async)
        string(JSON EVENT_SEQUENCE_TYPE TYPE "${LINE}" eventSequence)
        if(NOT ASYNC OR NOT EVENT_SEQUENCE_TYPE STREQUAL "NUMBER" OR
           NOT REQUEST_ID STREQUAL "shutdown-smoke" OR
           NOT DOCUMENT_ID STREQUAL "" OR NOT REVISION EQUAL 0)
            message(FATAL_ERROR "PreviewHost exited event has invalid correlation: ${LINE}")
        endif()
        if(NOT SHUTDOWN_SEEN)
            message(FATAL_ERROR "PreviewHost exited event preceded its shutdown ack")
        endif()
        set(EXITED_EVENT_SEEN TRUE)
    endif()
endforeach()

if(NOT READY_SEEN OR NOT PLAY_ERROR_SEEN OR NOT SHUTDOWN_SEEN)
    message(FATAL_ERROR "PreviewHost synchronous acknowledgements are incomplete: ${OUTPUT}")
endif()
if(NOT OUTPUT_EVENT_SEEN OR NOT STATE_EVENT_SEEN OR NOT DEBUG_EVENT_SEEN OR
   NOT DIAGNOSTICS_EVENT_SEEN OR NOT EXITED_EVENT_SEEN)
    message(FATAL_ERROR "PreviewHost async event contract is incomplete: ${OUTPUT}")
endif()
