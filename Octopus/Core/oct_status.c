/*
 * oct_status.c - names for oct_status values.
 *
 * Its own translation unit so that code which only needs to report a status — the bridge client
 * linked into ptpd — doesn't have to link the storage engine that oct_store.c holds. Declared in
 * oct_store.h, next to the oct_status enum.
 */
#include "oct_store.h"

const char *oct_status_name(oct_status status)
{
    switch (status) {
    case OCT_OK: return "ok";
    case OCT_E_INVALID_ARGUMENT: return "invalid argument";
    case OCT_E_NO_MEMORY: return "out of memory";
    case OCT_E_INVALID_HANDLE: return "invalid object handle";
    case OCT_E_INVALID_PARENT: return "invalid parent";
    case OCT_E_NOT_DIRECTORY: return "not a directory";
    case OCT_E_IS_DIRECTORY: return "is a directory";
    case OCT_E_INVALID_NAME: return "invalid name";
    case OCT_E_EXISTS: return "name already exists";
    case OCT_E_WRITE_PROTECTED: return "write protected";
    case OCT_E_PARTIAL_DELETION: return "partial deletion";
    case OCT_E_OUT_OF_RANGE: return "out of range";
    case OCT_E_TOO_LARGE: return "too large";
    case OCT_E_NO_SPACE: return "no space";
    case OCT_E_IO: return "I/O error";
    case OCT_E_UNSUPPORTED: return "unsupported";
    case OCT_E_STATE: return "invalid state";
    case OCT_E_MALFORMED: return "malformed input";
    }
    return "unknown";
}
