#define _POSIX_C_SOURCE 200809L
#include "aegis/model/stream.h"

const char* aegis_model_stream_event_type_str(aegis_model_stream_event_type_t t)
{
    switch (t) {
    case AEGIS_MODEL_STREAM_TEXT_DELTA:
        return "TEXT_DELTA";
    case AEGIS_MODEL_STREAM_REASONING_DELTA:
        return "REASONING_DELTA";
    case AEGIS_MODEL_STREAM_TOOL_CALL_START:
        return "TOOL_CALL_START";
    case AEGIS_MODEL_STREAM_TOOL_CALL_DELTA:
        return "TOOL_CALL_DELTA";
    case AEGIS_MODEL_STREAM_TOOL_CALL_END:
        return "TOOL_CALL_END";
    case AEGIS_MODEL_STREAM_USAGE:
        return "USAGE";
    case AEGIS_MODEL_STREAM_END:
        return "END";
    case AEGIS_MODEL_STREAM_ERROR:
        return "ERROR";
    default:
        return "UNKNOWN";
    }
}
