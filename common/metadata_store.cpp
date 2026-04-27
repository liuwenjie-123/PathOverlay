#include "metadata_store.h"

#include <windows.h>

#include <cstdint>
#include <sstream>

namespace pathoverlay {
namespace {

constexpr int SQLITE_OK_VALUE = 0;
constexpr int SQLITE_ROW_VALUE = 100;
constexpr int SQLITE_DONE_VALUE = 101;
constexpr int SQLITE_OPEN_READWRITE_VALUE = 0x00000002;
constexpr int SQLITE_OPEN_CREATE_VALUE = 0x00000004;

using sqlite3 = void;
using sqlite3_stmt = void;
using sqlite3_destructor_type = void (*)(void*);

using sqlite3_open_v2_fn = int (*)(const char*, sqlite3**, int, const char*);
using sqlite3_close_fn = int (*)(sqlite3*);
using sqlite3_exec_fn = int (*)(sqlite3*, const char*, int (*)(void*, int, char**, char**), void*, char**);
using sqlite3_free_fn = void (*)(void*);
using sqlite3_prepare_v2_fn = int (*)(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
using sqlite3_step_fn = int (*)(sqlite3_stmt*);
using sqlite3_finalize_fn = int (*)(sqlite3_stmt*);
using sqlite3_bind_text_fn = int (*)(sqlite3_stmt*, int, const char*, int, sqlite3_destructor_type);
using sqlite3_bind_int_fn = int (*)(sqlite3_stmt*, int, int);
using sqlite3_bind_int64_fn = int (*)(sqlite3_stmt*, int, long long);
using sqlite3_column_text_fn = const unsigned char* (*)(sqlite3_stmt*, int);
using sqlite3_column_int_fn = int (*)(sqlite3_stmt*, int);
using sqlite3_column_int64_fn = long long (*)(sqlite3_stmt*, int);
using sqlite3_errmsg_fn = const char* (*)(sqlite3*);

struct SqliteApi {
    sqlite3_open_v2_fn open_v2 = nullptr;
    sqlite3_close_fn close = nullptr;
    sqlite3_exec_fn exec = nullptr;
    sqlite3_free_fn free = nullptr;
    sqlite3_prepare_v2_fn prepare_v2 = nullptr;
    sqlite3_step_fn step = nullptr;
    sqlite3_finalize_fn finalize = nullptr;
    sqlite3_bind_text_fn bind_text = nullptr;
    sqlite3_bind_int_fn bind_int = nullptr;
    sqlite3_bind_int64_fn bind_int64 = nullptr;
    sqlite3_column_text_fn column_text = nullptr;
    sqlite3_column_int_fn column_int = nullptr;
    sqlite3_column_int64_fn column_int64 = nullptr;
    sqlite3_errmsg_fn errmsg = nullptr;
};

SqliteApi g_sqlite;

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int required = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (required <= 1) {
        return {};
    }
    std::string output(static_cast<size_t>(required - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, output.data(), required, nullptr, nullptr);
    return output;
}

std::wstring Utf8ToWide(const char* value) {
    if (value == nullptr || value[0] == '\0') {
        return {};
    }
    const int required = MultiByteToWideChar(CP_UTF8, 0, value, -1, nullptr, 0);
    if (required <= 1) {
        return {};
    }
    std::wstring output(static_cast<size_t>(required - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value, -1, output.data(), required);
    return output;
}

std::wstring LastErrorMessage(sqlite3* database, const wchar_t* prefix) {
    std::wstringstream stream;
    stream << prefix;
    if (database != nullptr && g_sqlite.errmsg != nullptr) {
        stream << L": " << Utf8ToWide(g_sqlite.errmsg(database));
    }
    return stream.str();
}

bool LoadSymbol(HMODULE library, const char* name, void** target) {
    *target = reinterpret_cast<void*>(GetProcAddress(library, name));
    return *target != nullptr;
}

bool LoadSqlite(void** library, std::wstring* error) {
    if (*library != nullptr) {
        return true;
    }

    HMODULE loaded = LoadLibraryW(L"sqlite3.dll");
    if (loaded == nullptr) {
        if (error != nullptr) {
            *error = L"sqlite3.dll was not found. Install SQLite runtime or place sqlite3.dll next to the executable.";
        }
        return false;
    }

    bool ok = true;
    ok &= LoadSymbol(loaded, "sqlite3_open_v2", reinterpret_cast<void**>(&g_sqlite.open_v2));
    ok &= LoadSymbol(loaded, "sqlite3_close", reinterpret_cast<void**>(&g_sqlite.close));
    ok &= LoadSymbol(loaded, "sqlite3_exec", reinterpret_cast<void**>(&g_sqlite.exec));
    ok &= LoadSymbol(loaded, "sqlite3_free", reinterpret_cast<void**>(&g_sqlite.free));
    ok &= LoadSymbol(loaded, "sqlite3_prepare_v2", reinterpret_cast<void**>(&g_sqlite.prepare_v2));
    ok &= LoadSymbol(loaded, "sqlite3_step", reinterpret_cast<void**>(&g_sqlite.step));
    ok &= LoadSymbol(loaded, "sqlite3_finalize", reinterpret_cast<void**>(&g_sqlite.finalize));
    ok &= LoadSymbol(loaded, "sqlite3_bind_text", reinterpret_cast<void**>(&g_sqlite.bind_text));
    ok &= LoadSymbol(loaded, "sqlite3_bind_int", reinterpret_cast<void**>(&g_sqlite.bind_int));
    ok &= LoadSymbol(loaded, "sqlite3_bind_int64", reinterpret_cast<void**>(&g_sqlite.bind_int64));
    ok &= LoadSymbol(loaded, "sqlite3_column_text", reinterpret_cast<void**>(&g_sqlite.column_text));
    ok &= LoadSymbol(loaded, "sqlite3_column_int", reinterpret_cast<void**>(&g_sqlite.column_int));
    ok &= LoadSymbol(loaded, "sqlite3_column_int64", reinterpret_cast<void**>(&g_sqlite.column_int64));
    ok &= LoadSymbol(loaded, "sqlite3_errmsg", reinterpret_cast<void**>(&g_sqlite.errmsg));

    if (!ok) {
        FreeLibrary(loaded);
        if (error != nullptr) {
            *error = L"sqlite3.dll is missing required SQLite C API exports.";
        }
        return false;
    }

    *library = loaded;
    return true;
}

bool Exec(sqlite3* database, const char* sql, std::wstring* error) {
    char* sqliteError = nullptr;
    const int result = g_sqlite.exec(database, sql, nullptr, nullptr, &sqliteError);
    if (result == SQLITE_OK_VALUE) {
        return true;
    }

    if (error != nullptr) {
        *error = sqliteError != nullptr ? Utf8ToWide(sqliteError) : LastErrorMessage(database, L"SQLite exec failed");
    }
    if (sqliteError != nullptr) {
        g_sqlite.free(sqliteError);
    }
    return false;
}

bool BindText(sqlite3_stmt* statement, int index, const std::wstring& value) {
    const std::string utf8 = WideToUtf8(value);
    const auto transient = reinterpret_cast<sqlite3_destructor_type>(static_cast<intptr_t>(-1));
    return g_sqlite.bind_text(statement, index, utf8.c_str(), -1, transient) == SQLITE_OK_VALUE;
}

std::wstring ColumnText(sqlite3_stmt* statement, int index) {
    return Utf8ToWide(reinterpret_cast<const char*>(g_sqlite.column_text(statement, index)));
}

}  // namespace

std::wstring ChangeStateToString(ChangeState state) {
    switch (state) {
        case ChangeState::kCreated:
            return L"created";
        case ChangeState::kModified:
            return L"modified";
        case ChangeState::kDeleted:
            return L"deleted";
        case ChangeState::kTombstone:
            return L"tombstone";
    }
    return L"created";
}

bool TryParseChangeState(const std::wstring& value, ChangeState* state) {
    if (value == L"created") {
        *state = ChangeState::kCreated;
        return true;
    }
    if (value == L"modified") {
        *state = ChangeState::kModified;
        return true;
    }
    if (value == L"deleted") {
        *state = ChangeState::kDeleted;
        return true;
    }
    if (value == L"tombstone") {
        *state = ChangeState::kTombstone;
        return true;
    }
    return false;
}

MetadataStore::MetadataStore() = default;

MetadataStore::~MetadataStore() {
    Close();
    if (sqliteLibrary_ != nullptr) {
        FreeLibrary(static_cast<HMODULE>(sqliteLibrary_));
        sqliteLibrary_ = nullptr;
    }
}

bool MetadataStore::Open(const std::wstring& databasePath, std::wstring* error) {
    if (!LoadSqlite(&sqliteLibrary_, error)) {
        return false;
    }

    Close();
    sqlite3* opened = nullptr;
    const int flags = SQLITE_OPEN_READWRITE_VALUE | SQLITE_OPEN_CREATE_VALUE;
    const int result = g_sqlite.open_v2(WideToUtf8(databasePath).c_str(), &opened, flags, nullptr);
    if (result != SQLITE_OK_VALUE) {
        if (error != nullptr) {
            *error = LastErrorMessage(opened, L"failed to open metadata database");
        }
        if (opened != nullptr) {
            g_sqlite.close(opened);
        }
        return false;
    }

    database_ = opened;
    return true;
}

void MetadataStore::Close() {
    if (database_ != nullptr) {
        g_sqlite.close(static_cast<sqlite3*>(database_));
        database_ = nullptr;
    }
}

bool MetadataStore::Initialize(std::wstring* error) {
    if (database_ == nullptr) {
        if (error != nullptr) {
            *error = L"metadata database is not open";
        }
        return false;
    }

    static constexpr char kSchema[] =
        "PRAGMA foreign_keys = ON;"
        "CREATE TABLE IF NOT EXISTS rules ("
        "  id TEXT PRIMARY KEY,"
        "  name TEXT NOT NULL,"
        "  enabled INTEGER NOT NULL,"
        "  source TEXT NOT NULL,"
        "  store TEXT NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS changes ("
        "  rule_id TEXT NOT NULL,"
        "  real_path TEXT NOT NULL,"
        "  shadow_path TEXT NOT NULL,"
        "  state TEXT NOT NULL,"
        "  original_exists INTEGER NOT NULL,"
        "  original_size INTEGER NOT NULL,"
        "  original_last_write_time TEXT NOT NULL,"
        "  current_size INTEGER NOT NULL,"
        "  last_write_time TEXT NOT NULL,"
        "  PRIMARY KEY(rule_id, real_path),"
        "  FOREIGN KEY(rule_id) REFERENCES rules(id) ON DELETE CASCADE"
        ");"
        "CREATE TABLE IF NOT EXISTS commits ("
        "  id TEXT PRIMARY KEY,"
        "  rule_id TEXT NOT NULL,"
        "  start_time TEXT NOT NULL,"
        "  status TEXT NOT NULL,"
        "  operations TEXT NOT NULL,"
        "  backup_path TEXT NOT NULL,"
        "  error TEXT NOT NULL"
        ");";

    return Exec(static_cast<sqlite3*>(database_), kSchema, error);
}

bool MetadataStore::UpsertRule(const OverlayRule& rule, std::wstring* error) {
    static constexpr char kSql[] =
        "INSERT INTO rules(id, name, enabled, source, store) VALUES(?, ?, ?, ?, ?) "
        "ON CONFLICT(id) DO UPDATE SET "
        "name = excluded.name, enabled = excluded.enabled, source = excluded.source, store = excluded.store;";

    sqlite3_stmt* statement = nullptr;
    sqlite3* db = static_cast<sqlite3*>(database_);
    if (g_sqlite.prepare_v2(db, kSql, -1, &statement, nullptr) != SQLITE_OK_VALUE) {
        *error = LastErrorMessage(db, L"failed to prepare rule upsert");
        return false;
    }

    const bool bindOk = BindText(statement, 1, rule.id) &&
                        BindText(statement, 2, rule.name) &&
                        g_sqlite.bind_int(statement, 3, rule.enabled ? 1 : 0) == SQLITE_OK_VALUE &&
                        BindText(statement, 4, NormalizePath(rule.source)) &&
                        BindText(statement, 5, NormalizePath(rule.store.empty() ? DefaultStoreRoot() : rule.store));
    const bool ok = bindOk && g_sqlite.step(statement) == SQLITE_DONE_VALUE;
    if (!ok && error != nullptr) {
        *error = LastErrorMessage(db, L"failed to upsert rule");
    }
    g_sqlite.finalize(statement);
    return ok;
}

bool MetadataStore::SetRuleEnabled(const std::wstring& ruleId, bool enabled, std::wstring* error) {
    static constexpr char kSql[] = "UPDATE rules SET enabled = ? WHERE id = ?;";
    sqlite3_stmt* statement = nullptr;
    sqlite3* db = static_cast<sqlite3*>(database_);
    if (g_sqlite.prepare_v2(db, kSql, -1, &statement, nullptr) != SQLITE_OK_VALUE) {
        *error = LastErrorMessage(db, L"failed to prepare rule enable update");
        return false;
    }

    const bool bindOk = g_sqlite.bind_int(statement, 1, enabled ? 1 : 0) == SQLITE_OK_VALUE &&
                        BindText(statement, 2, ruleId);
    const bool ok = bindOk && g_sqlite.step(statement) == SQLITE_DONE_VALUE;
    if (!ok && error != nullptr) {
        *error = LastErrorMessage(db, L"failed to update rule enabled state");
    }
    g_sqlite.finalize(statement);
    return ok;
}

bool MetadataStore::GetRule(const std::wstring& ruleId, OverlayRule* rule, std::wstring* error) {
    static constexpr char kSql[] = "SELECT id, name, enabled, source, store FROM rules WHERE id = ?;";
    sqlite3_stmt* statement = nullptr;
    sqlite3* db = static_cast<sqlite3*>(database_);
    if (g_sqlite.prepare_v2(db, kSql, -1, &statement, nullptr) != SQLITE_OK_VALUE) {
        *error = LastErrorMessage(db, L"failed to prepare rule query");
        return false;
    }

    BindText(statement, 1, ruleId);
    const int step = g_sqlite.step(statement);
    if (step == SQLITE_ROW_VALUE) {
        rule->id = ColumnText(statement, 0);
        rule->name = ColumnText(statement, 1);
        rule->enabled = g_sqlite.column_int(statement, 2) != 0;
        rule->source = ColumnText(statement, 3);
        rule->store = ColumnText(statement, 4);
        g_sqlite.finalize(statement);
        return true;
    }

    if (error != nullptr) {
        *error = step == SQLITE_DONE_VALUE ? L"rule was not found" : LastErrorMessage(db, L"failed to query rule");
    }
    g_sqlite.finalize(statement);
    return false;
}

bool MetadataStore::ListRules(std::vector<OverlayRule>* rules, std::wstring* error) {
    static constexpr char kSql[] = "SELECT id, name, enabled, source, store FROM rules ORDER BY id;";
    rules->clear();

    sqlite3_stmt* statement = nullptr;
    sqlite3* db = static_cast<sqlite3*>(database_);
    if (g_sqlite.prepare_v2(db, kSql, -1, &statement, nullptr) != SQLITE_OK_VALUE) {
        *error = LastErrorMessage(db, L"failed to prepare rule list");
        return false;
    }

    while (true) {
        const int step = g_sqlite.step(statement);
        if (step == SQLITE_DONE_VALUE) {
            g_sqlite.finalize(statement);
            return true;
        }
        if (step != SQLITE_ROW_VALUE) {
            if (error != nullptr) {
                *error = LastErrorMessage(db, L"failed to list rules");
            }
            g_sqlite.finalize(statement);
            return false;
        }

        OverlayRule rule;
        rule.id = ColumnText(statement, 0);
        rule.name = ColumnText(statement, 1);
        rule.enabled = g_sqlite.column_int(statement, 2) != 0;
        rule.source = ColumnText(statement, 3);
        rule.store = ColumnText(statement, 4);
        rules->push_back(rule);
    }
}

bool MetadataStore::AddOrUpdateChange(const std::wstring& ruleId, const ChangeRecord& record, std::wstring* error) {
    static constexpr char kSql[] =
        "INSERT INTO changes(rule_id, real_path, shadow_path, state, original_exists, original_size, original_last_write_time, current_size, last_write_time) "
        "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(rule_id, real_path) DO UPDATE SET "
        "shadow_path = excluded.shadow_path, state = excluded.state, "
        "current_size = excluded.current_size, last_write_time = excluded.last_write_time;";

    sqlite3_stmt* statement = nullptr;
    sqlite3* db = static_cast<sqlite3*>(database_);
    if (g_sqlite.prepare_v2(db, kSql, -1, &statement, nullptr) != SQLITE_OK_VALUE) {
        *error = LastErrorMessage(db, L"failed to prepare change upsert");
        return false;
    }

    const bool bindOk = BindText(statement, 1, ruleId) &&
                        BindText(statement, 2, NormalizePath(record.realPath)) &&
                        BindText(statement, 3, NormalizePath(record.shadowPath)) &&
                        BindText(statement, 4, ChangeStateToString(record.state)) &&
                        g_sqlite.bind_int(statement, 5, record.originalExists ? 1 : 0) == SQLITE_OK_VALUE &&
                        g_sqlite.bind_int64(statement, 6, static_cast<long long>(record.originalSize)) == SQLITE_OK_VALUE &&
                        BindText(statement, 7, record.originalLastWriteTime) &&
                        g_sqlite.bind_int64(statement, 8, static_cast<long long>(record.currentSize)) == SQLITE_OK_VALUE &&
                        BindText(statement, 9, record.lastWriteTime);
    const bool ok = bindOk && g_sqlite.step(statement) == SQLITE_DONE_VALUE;
    if (!ok && error != nullptr) {
        *error = LastErrorMessage(db, L"failed to upsert change record");
    }
    g_sqlite.finalize(statement);
    return ok;
}

bool MetadataStore::ListChanges(const std::wstring& ruleId, std::vector<ChangeRecord>* records, std::wstring* error) {
    static constexpr char kSql[] =
        "SELECT real_path, shadow_path, state, original_exists, original_size, original_last_write_time, current_size, last_write_time "
        "FROM changes WHERE rule_id = ? ORDER BY real_path;";

    records->clear();
    sqlite3_stmt* statement = nullptr;
    sqlite3* db = static_cast<sqlite3*>(database_);
    if (g_sqlite.prepare_v2(db, kSql, -1, &statement, nullptr) != SQLITE_OK_VALUE) {
        *error = LastErrorMessage(db, L"failed to prepare change query");
        return false;
    }

    BindText(statement, 1, ruleId);
    while (true) {
        const int step = g_sqlite.step(statement);
        if (step == SQLITE_DONE_VALUE) {
            g_sqlite.finalize(statement);
            return true;
        }
        if (step != SQLITE_ROW_VALUE) {
            if (error != nullptr) {
                *error = LastErrorMessage(db, L"failed to query changes");
            }
            g_sqlite.finalize(statement);
            return false;
        }

        ChangeRecord record;
        record.realPath = ColumnText(statement, 0);
        record.shadowPath = ColumnText(statement, 1);
        TryParseChangeState(ColumnText(statement, 2), &record.state);
        record.originalExists = g_sqlite.column_int(statement, 3) != 0;
        record.originalSize = static_cast<unsigned long long>(g_sqlite.column_int64(statement, 4));
        record.originalLastWriteTime = ColumnText(statement, 5);
        record.currentSize = static_cast<unsigned long long>(g_sqlite.column_int64(statement, 6));
        record.lastWriteTime = ColumnText(statement, 7);
        records->push_back(record);
    }
}

bool MetadataStore::DeleteChangesForRule(const std::wstring& ruleId, std::wstring* error) {
    static constexpr char kSql[] = "DELETE FROM changes WHERE rule_id = ?;";
    sqlite3_stmt* statement = nullptr;
    sqlite3* db = static_cast<sqlite3*>(database_);
    if (g_sqlite.prepare_v2(db, kSql, -1, &statement, nullptr) != SQLITE_OK_VALUE) {
        *error = LastErrorMessage(db, L"failed to prepare change delete");
        return false;
    }

    const bool bindOk = BindText(statement, 1, ruleId);
    const bool ok = bindOk && g_sqlite.step(statement) == SQLITE_DONE_VALUE;
    if (!ok && error != nullptr) {
        *error = LastErrorMessage(db, L"failed to delete change records");
    }
    g_sqlite.finalize(statement);
    return ok;
}

bool MetadataStore::AddCommitRecord(const CommitRecord& record, std::wstring* error) {
    static constexpr char kSql[] =
        "INSERT INTO commits(id, rule_id, start_time, status, operations, backup_path, error) "
        "VALUES(?, ?, ?, ?, ?, ?, ?);";

    sqlite3_stmt* statement = nullptr;
    sqlite3* db = static_cast<sqlite3*>(database_);
    if (g_sqlite.prepare_v2(db, kSql, -1, &statement, nullptr) != SQLITE_OK_VALUE) {
        *error = LastErrorMessage(db, L"failed to prepare commit insert");
        return false;
    }

    const bool bindOk = BindText(statement, 1, record.id) &&
                        BindText(statement, 2, record.ruleId) &&
                        BindText(statement, 3, record.startTime) &&
                        BindText(statement, 4, record.status) &&
                        BindText(statement, 5, record.operations) &&
                        BindText(statement, 6, record.backupPath) &&
                        BindText(statement, 7, record.error);
    const bool ok = bindOk && g_sqlite.step(statement) == SQLITE_DONE_VALUE;
    if (!ok && error != nullptr) {
        *error = LastErrorMessage(db, L"failed to insert commit record");
    }
    g_sqlite.finalize(statement);
    return ok;
}

bool MetadataStore::GetCommitRecord(const std::wstring& commitId, CommitRecord* record, std::wstring* error) {
    static constexpr char kSql[] =
        "SELECT id, rule_id, start_time, status, operations, backup_path, error "
        "FROM commits WHERE id = ?;";

    sqlite3_stmt* statement = nullptr;
    sqlite3* db = static_cast<sqlite3*>(database_);
    if (g_sqlite.prepare_v2(db, kSql, -1, &statement, nullptr) != SQLITE_OK_VALUE) {
        *error = LastErrorMessage(db, L"failed to prepare commit query");
        return false;
    }

    BindText(statement, 1, commitId);
    const int step = g_sqlite.step(statement);
    if (step == SQLITE_ROW_VALUE) {
        record->id = ColumnText(statement, 0);
        record->ruleId = ColumnText(statement, 1);
        record->startTime = ColumnText(statement, 2);
        record->status = ColumnText(statement, 3);
        record->operations = ColumnText(statement, 4);
        record->backupPath = ColumnText(statement, 5);
        record->error = ColumnText(statement, 6);
        g_sqlite.finalize(statement);
        return true;
    }

    if (error != nullptr) {
        *error = step == SQLITE_DONE_VALUE ? L"commit record was not found" : LastErrorMessage(db, L"failed to query commit record");
    }
    g_sqlite.finalize(statement);
    return false;
}

}  // namespace pathoverlay
