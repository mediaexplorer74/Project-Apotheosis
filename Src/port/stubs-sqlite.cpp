// ============================================================================
// stubs-sqlite.cpp — sqlite3 C-API stubs for the UWP App Container
// (EdgeHTML Reborn — x64-uwp / ARM32 thumbv7 Win10-Mobile).
//
// WHY:
//   The July-1 upstream storage refactor made WebCore compile platform/sql/*
//   unconditionally, so WebCore.dll now needs the sqlite3 C API. libsqlite3 is
//   not available for the UWP App Container:
//     * vcpkg sqlite3:x64-uwp   — BUILD_FAILED (amalgamation win32 VFS uses
//       CreateFileA/GetFileSize/HeapValidate, unavailable under
//       WINAPI_FAMILY_APP unless SQLITE_OS_WINRT is set; vcpkg port does not).
//     * build-x64-gpu/libs/sqlite3.lib (17.07, 2 MB) — desktop build, same
//       desktop-only Win32 syscalls, cannot link into the App-Container DLL.
//   SQL storage was never functional in this port (12.08 WebCore.dll imports
//   zero sqlite3 functions), so we stub the C API to keep the DLL linkable.
//   All DB opens fail (SQLITE_CANTOPEN) → SQLiteDatabase::open() returns
//   false → storage degrades gracefully to "unavailable", no crash.
//
//   If real SQL is needed later: build the amalgamation with
//   -DSQLITE_OS_WINRT=1 (official WinRT VFS, CreateFile2 path) and link
//   runtimeobject.lib into WebCore.
// ============================================================================

#include "config.h"

#include <sqlite3.h>

namespace WebCore {
}

extern "C" {

int sqlite3_initialize(void)
{
    return SQLITE_OK;
}

int sqlite3_config(int, ...)
{
    return SQLITE_OK;
}

int sqlite3_open_v2(const char*, sqlite3**, int, const char*)
{
    return SQLITE_CANTOPEN;
}

int sqlite3_close(sqlite3*)
{
    return SQLITE_OK;
}

int sqlite3_errcode(sqlite3*)
{
    return SQLITE_ERROR;
}

const char* sqlite3_errmsg(sqlite3*)
{
    return "sqlite3 unavailable (Apotheosis UWP stub)";
}

const char* sqlite3_errstr(int)
{
    return "sqlite3 unavailable (Apotheosis UWP stub)";
}

int sqlite3_extended_result_codes(sqlite3*, int)
{
    return SQLITE_OK;
}

int sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**)
{
    return SQLITE_ERROR;
}

int sqlite3_finalize(sqlite3_stmt*)
{
    return SQLITE_OK;
}

int sqlite3_step(sqlite3_stmt*)
{
    return SQLITE_ERROR;
}

int sqlite3_reset(sqlite3_stmt*)
{
    return SQLITE_OK;
}

int sqlite3_clear_bindings(sqlite3_stmt*)
{
    return SQLITE_OK;
}

int sqlite3_bind_int(sqlite3_stmt*, int, int)
{
    return SQLITE_ERROR;
}

int sqlite3_bind_int64(sqlite3_stmt*, int, sqlite3_int64)
{
    return SQLITE_ERROR;
}

int sqlite3_bind_double(sqlite3_stmt*, int, double)
{
    return SQLITE_ERROR;
}

int sqlite3_bind_null(sqlite3_stmt*, int)
{
    return SQLITE_ERROR;
}

int sqlite3_bind_parameter_count(sqlite3_stmt*)
{
    return 0;
}

int sqlite3_bind_blob64(sqlite3_stmt*, int, const void*, sqlite3_uint64, void(*)(void*))
{
    return SQLITE_ERROR;
}

int sqlite3_bind_text(sqlite3_stmt*, int, const char*, int, void(*)(void*))
{
    return SQLITE_ERROR;
}

int sqlite3_data_count(sqlite3_stmt*)
{
    return 0;
}

const char* sqlite3_column_decltype(sqlite3_stmt*, int)
{
    return nullptr;
}

const void* sqlite3_column_blob(sqlite3_stmt*, int)
{
    return nullptr;
}

int sqlite3_column_bytes(sqlite3_stmt*, int)
{
    return 0;
}

const char* sqlite3_column_name(sqlite3_stmt*, int)
{
    return nullptr;
}

const unsigned char* sqlite3_column_text(sqlite3_stmt*, int)
{
    return nullptr;
}

int sqlite3_value_bytes(sqlite3_value*)
{
    return 0;
}

const unsigned char* sqlite3_value_text(sqlite3_value*)
{
    return nullptr;
}

sqlite3_value* sqlite3_column_value(sqlite3_stmt*, int)
{
    return nullptr;
}

int sqlite3_value_type(sqlite3_value*)
{
    return SQLITE_NULL;
}

double sqlite3_value_double(sqlite3_value*)
{
    return 0.0;
}

double sqlite3_column_double(sqlite3_stmt*, int)
{
    return 0.0;
}

int sqlite3_column_int(sqlite3_stmt*, int)
{
    return 0;
}

sqlite3_int64 sqlite3_column_int64(sqlite3_stmt*, int)
{
    return 0;
}

int sqlite3_stmt_busy(sqlite3_stmt*)
{
    return 0;
}

int sqlite3_stmt_readonly(sqlite3_stmt*)
{
    return 1;
}

int sqlite3_file_control(sqlite3*, const char*, int, void*)
{
    return SQLITE_ERROR;
}

void* sqlite3_user_data(sqlite3_context*)
{
    return nullptr;
}

void sqlite3_result_error(sqlite3_context*, const char*, int)
{
}

int sqlite3_wal_checkpoint_v2(sqlite3*, const char*, int, int* pnLog, int* pnCkpt)
{
    if (pnLog)
        *pnLog = 0;
    if (pnCkpt)
        *pnCkpt = 0;
    return SQLITE_OK;
}

sqlite3_stmt* sqlite3_next_stmt(sqlite3*, sqlite3_stmt*)
{
    return nullptr;
}

const char* sqlite3_sql(sqlite3_stmt*)
{
    return nullptr;
}

void* sqlite3_wal_hook(sqlite3*, int(*)(void*, sqlite3*, const char*, int), void*)
{
    return nullptr;
}

int sqlite3_create_function(sqlite3*, const char*, int, int, void*, void(*)(sqlite3_context*, int, sqlite3_value**), void(*)(sqlite3_context*, int, sqlite3_value**), void(*)(sqlite3_context*))
{
    return SQLITE_ERROR;
}

int sqlite3_busy_timeout(sqlite3*, int)
{
    return SQLITE_OK;
}

int sqlite3_busy_handler(sqlite3*, int(*)(void*, int), void*)
{
    return SQLITE_OK;
}

void sqlite3_interrupt(sqlite3*)
{
}

sqlite3_int64 sqlite3_last_insert_rowid(sqlite3*)
{
    return 0;
}

int sqlite3_changes(sqlite3*)
{
    return 0;
}

int sqlite3_set_authorizer(sqlite3*, int(*)(void*, int, const char*, const char*, const char*, const char*), void*)
{
    return SQLITE_OK;
}

int sqlite3_get_autocommit(sqlite3*)
{
    return 1;
}

int sqlite3_create_collation_v2(sqlite3*, const char*, int, void*, int(*)(void*, int, const void*, int, const void*), void(*)(void*))
{
    return SQLITE_OK;
}

int sqlite3_db_release_memory(sqlite3*)
{
    return SQLITE_OK;
}

} // extern "C"