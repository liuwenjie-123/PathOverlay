#include <iostream>
#include <filesystem>
#include <string>
#include <vector>
#include <windows.h>

#include "metadata_store.h"
#include "overlay_operations.h"
#include "path_overlay_common.h"

namespace {

std::wstring TempPathOverlayDir(const wchar_t* name) {
    wchar_t tempPath[MAX_PATH] = {};
    DWORD length = GetTempPathW(MAX_PATH, tempPath);
    std::wstring path(tempPath, length);
    if (!path.empty() && path.back() != L'\\') {
        path += L'\\';
    }
    return path + L"PathOverlayTests\\" + name;
}

void EnsureDirectory(const std::wstring& path) {
    CreateDirectoryW(TempPathOverlayDir(L"").c_str(), nullptr);
    CreateDirectoryW(path.c_str(), nullptr);
}

bool Expect(bool condition, const wchar_t* message) {
    if (!condition) {
        std::wcerr << message << L"\n";
        return false;
    }
    return true;
}

bool WriteTextFile(const std::wstring& path, const std::string& text) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    const BOOL ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    CloseHandle(file);
    return ok && written == text.size();
}

std::string ReadTextFile(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return {};
    }
    char buffer[256] = {};
    DWORD read = 0;
    ReadFile(file, buffer, sizeof(buffer), &read, nullptr);
    CloseHandle(file);
    return std::string(buffer, buffer + read);
}

std::wstring BackupPathForTest(const std::wstring& backupRoot, const std::wstring& realPath) {
    std::wstring normalized = pathoverlay::NormalizePath(realPath);
    if (normalized.size() >= 2 && normalized[1] == L':') {
        normalized.erase(1, 1);
    }
    return backupRoot + L"\\drive\\" + normalized;
}

}  // namespace

int wmain() {
    std::error_code cleanupError;
    std::filesystem::remove_all(TempPathOverlayDir(L""), cleanupError);

    if (pathoverlay::ProductName() != std::wstring(L"PathOverlay")) {
        std::wcerr << L"ProductName mismatch\n";
        return 1;
    }

    const std::wstring source = TempPathOverlayDir(L"source");
    const std::wstring store = TempPathOverlayDir(L"store");
    EnsureDirectory(source);
    EnsureDirectory(store);

    pathoverlay::OverlayRule rule{
        L"rule-001",
        L"default",
        true,
        source,
        store,
    };

    bool ok = true;
    ok &= Expect(pathoverlay::ValidateOverlayRule(rule).ok(), L"local directory source should be valid");

    pathoverlay::OverlayRule missingSource = rule;
    missingSource.source = TempPathOverlayDir(L"missing");
    ok &= Expect(!pathoverlay::ValidateOverlayRule(missingSource).ok(), L"missing source should be rejected");

    pathoverlay::OverlayRule uncSource = rule;
    uncSource.source = L"\\\\server\\share";
    ok &= Expect(!pathoverlay::ValidateOverlayRule(uncSource).ok(), L"UNC source should be rejected");

    pathoverlay::OverlayRule driveRoot = rule;
    driveRoot.source = L"C:\\";
    ok &= Expect(!pathoverlay::ValidateOverlayRule(driveRoot).ok(), L"drive root source should be rejected");

    pathoverlay::OverlayRule nestedStore = rule;
    nestedStore.store = source + L"\\store";
    ok &= Expect(!pathoverlay::ValidateOverlayRule(nestedStore).ok(), L"store inside source should be rejected");

    const std::wstring realPath = source + L"\\a.txt";
    const auto shadow1 = pathoverlay::MapRealPathToShadowPath(rule, realPath);
    const auto shadow2 = pathoverlay::MapRealPathToShadowPath(rule, realPath);
    ok &= Expect(shadow1.has_value(), L"real path inside source should map to shadow");
    ok &= Expect(shadow1 == shadow2, L"shadow path mapping should be deterministic");
    if (shadow1.has_value()) {
        ok &= Expect(shadow1->find(pathoverlay::NormalizePath(store)) == 0, L"shadow path should be under store");
        ok &= Expect(shadow1->find(L"drive") != std::wstring::npos, L"shadow path should include drive namespace");
    }

    std::wstring sqliteError;
    pathoverlay::MetadataStore metadata;
    const std::wstring databasePath = TempPathOverlayDir(L"metadata.db");
    if (!metadata.Open(databasePath, &sqliteError)) {
        std::wcout << L"SQLite metadata tests skipped: " << sqliteError << L"\n";
    } else {
        ok &= Expect(metadata.Initialize(&sqliteError), L"metadata database should initialize");
        ok &= Expect(metadata.UpsertRule(rule, &sqliteError), L"rule should be persisted");
        ok &= Expect(metadata.SetRuleEnabled(rule.id, false, &sqliteError), L"rule enabled state should update");

        pathoverlay::OverlayRule persistedRule;
        ok &= Expect(metadata.GetRule(rule.id, &persistedRule, &sqliteError), L"rule should be queried");
        ok &= Expect(!persistedRule.enabled, L"queried rule should reflect enabled state");

        const pathoverlay::ChangeState states[] = {
            pathoverlay::ChangeState::kCreated,
            pathoverlay::ChangeState::kModified,
            pathoverlay::ChangeState::kDeleted,
            pathoverlay::ChangeState::kTombstone,
        };
        for (int i = 0; i < 4; ++i) {
            pathoverlay::ChangeRecord change;
            change.realPath = source + L"\\change" + std::to_wstring(i) + L".txt";
            change.shadowPath = store + L"\\drive\\C\\change" + std::to_wstring(i) + L".txt";
            change.state = states[i];
            change.originalExists = i != 0;
            change.originalSize = 10 + i;
            change.originalLastWriteTime = L"2026-04-24T00:00:00Z";
            change.currentSize = 20 + i;
            change.lastWriteTime = L"2026-04-24T00:01:00Z";
            ok &= Expect(metadata.AddOrUpdateChange(rule.id, change, &sqliteError), L"change should be persisted");
        }

        std::vector<pathoverlay::ChangeRecord> changes;
        ok &= Expect(metadata.ListChanges(rule.id, &changes, &sqliteError), L"changes should be queried");
        ok &= Expect(changes.size() == 4, L"all change states should be persisted");

        pathoverlay::CommitRecord commit;
        commit.id = L"commit-001";
        commit.ruleId = rule.id;
        commit.startTime = L"2026-04-24T00:02:00Z";
        commit.status = L"done";
        commit.operations = L"created,modified,deleted";
        commit.backupPath = store + L"\\backups\\commit-001";
        commit.error = L"";
        ok &= Expect(metadata.AddCommitRecord(commit, &sqliteError), L"commit record should be persisted");

        pathoverlay::CommitRecord queriedCommit;
        ok &= Expect(metadata.GetCommitRecord(commit.id, &queriedCommit, &sqliteError), L"commit record should be queried");
        ok &= Expect(queriedCommit.startTime == commit.startTime &&
                     queriedCommit.status == commit.status &&
                     queriedCommit.operations == commit.operations &&
                     queriedCommit.backupPath == commit.backupPath &&
                     queriedCommit.error == commit.error,
                     L"commit record should contain required fields");

        const std::wstring opsSource = TempPathOverlayDir(L"ops_source");
        const std::wstring opsStore = TempPathOverlayDir(L"ops_store");
        EnsureDirectory(opsSource);
        EnsureDirectory(opsStore);

        pathoverlay::OverlayRule opsRule{
            L"rule-ops",
            L"operations",
            true,
            opsSource,
            opsStore,
        };
        ok &= Expect(metadata.UpsertRule(opsRule, &sqliteError), L"operations rule should be persisted");

        pathoverlay::OverlayOperations operations(metadata);
        const std::wstring existingReal = opsSource + L"\\existing.txt";
        ok &= Expect(WriteTextFile(existingReal, "original"), L"existing real file should be created");

        std::wstring existingShadow;
        pathoverlay::OperationResult opResult = operations.PrepareCopyOnWrite(opsRule, existingReal, &existingShadow);
        ok &= Expect(opResult.success, L"copy-on-write should prepare shadow copy");
        ok &= Expect(ReadTextFile(existingShadow) == "original", L"shadow copy should contain original data");
        ok &= Expect(WriteTextFile(existingShadow, "changed"), L"shadow file should be writable");
        ok &= Expect(ReadTextFile(existingReal) == "original", L"real file should not change before commit");

        const std::wstring createdReal = opsSource + L"\\created.txt";
        const auto createdShadow = pathoverlay::MapRealPathToShadowPath(opsRule, createdReal);
        ok &= Expect(createdShadow.has_value(), L"created file should map to shadow");
        opResult = operations.RecordCreatedFile(opsRule, createdReal);
        ok &= Expect(opResult.success, L"created file should be recorded");
        ok &= Expect(WriteTextFile(*createdShadow, "created"), L"created shadow file should be writable");

        const std::wstring deleteReal = opsSource + L"\\delete.txt";
        ok &= Expect(WriteTextFile(deleteReal, "delete-me"), L"delete real file should be created");
        opResult = operations.RecordDelete(opsRule, deleteReal);
        ok &= Expect(opResult.success, L"delete should record tombstone");
        ok &= Expect(ReadTextFile(deleteReal) == "delete-me", L"tombstone should not delete real file before commit");

        std::vector<pathoverlay::ChangeRecord> operationChanges;
        opResult = operations.ListChanges(opsRule.id, &operationChanges);
        ok &= Expect(opResult.success, L"operation changes should be listed");
        ok &= Expect(operationChanges.size() >= 3, L"operation changes should include modified, created, and tombstone records");

        const std::wstring directoryReal = opsSource;
        const std::wstring viewReal = opsSource + L"\\view-real.txt";
        ok &= Expect(WriteTextFile(viewReal, "real-view"), L"directory view real file should be created");
        const auto viewShadow = pathoverlay::MapRealPathToShadowPath(opsRule, viewReal);
        ok &= Expect(viewShadow.has_value(), L"directory view real file should map to shadow");

        std::wstring directoryShadow;
        opResult = operations.PrepareDirectoryView(opsRule, directoryReal, &directoryShadow);
        ok &= Expect(opResult.success, L"directory view should be prepared");
        ok &= Expect(ReadTextFile(*viewShadow) == "real-view", L"directory view should include real file in shadow");
        ok &= Expect(ReadTextFile(existingShadow) == "changed", L"directory view should not overwrite same-name shadow file");
        const auto deleteShadow = pathoverlay::MapRealPathToShadowPath(opsRule, deleteReal);
        ok &= Expect(deleteShadow.has_value(), L"deleted file should map to shadow");
        ok &= Expect(GetFileAttributesW(deleteShadow->c_str()) == INVALID_FILE_ATTRIBUTES, L"directory view should hide tombstoned file from shadow");
        opResult = operations.RecordDelete(opsRule, directoryReal);
        ok &= Expect(!opResult.success, L"directory delete should be unsupported in MVP");
        std::vector<pathoverlay::ChangeRecord> directoryChanges;
        ok &= Expect(operations.ListChanges(opsRule.id, &directoryChanges).success, L"directory change query should succeed");
        bool directoryWasRecorded = false;
        for (const auto& record : directoryChanges) {
            if (_wcsicmp(pathoverlay::NormalizePath(record.realPath).c_str(), pathoverlay::NormalizePath(directoryReal).c_str()) == 0) {
                directoryWasRecorded = true;
            }
        }
        ok &= Expect(!directoryWasRecorded, L"directory operations should not create commit metadata");

        const std::wstring discardSource = TempPathOverlayDir(L"discard_source");
        const std::wstring discardStore = TempPathOverlayDir(L"discard_store");
        EnsureDirectory(discardSource);
        EnsureDirectory(discardStore);
        pathoverlay::OverlayRule discardRule{L"rule-discard", L"discard", true, discardSource, discardStore};
        ok &= Expect(metadata.UpsertRule(discardRule, &sqliteError), L"discard rule should be persisted");
        const std::wstring discardReal = discardSource + L"\\discard.txt";
        ok &= Expect(WriteTextFile(discardReal, "keep"), L"discard real file should be created");
        std::wstring discardShadow;
        ok &= Expect(operations.PrepareCopyOnWrite(discardRule, discardReal, &discardShadow).success, L"discard shadow should be prepared");
        ok &= Expect(WriteTextFile(discardShadow, "discarded"), L"discard shadow should be changed");
        ok &= Expect(operations.Discard(discardRule).success, L"discard should succeed");
        ok &= Expect(ReadTextFile(discardReal) == "keep", L"discard should not modify real file");
        std::vector<pathoverlay::ChangeRecord> discardChanges;
        ok &= Expect(operations.ListChanges(discardRule.id, &discardChanges).success && discardChanges.empty(), L"discard should clear metadata changes");

        const std::wstring conflictSource = TempPathOverlayDir(L"conflict_source");
        const std::wstring conflictStore = TempPathOverlayDir(L"conflict_store");
        EnsureDirectory(conflictSource);
        EnsureDirectory(conflictStore);
        pathoverlay::OverlayRule conflictRule{L"rule-conflict", L"conflict", true, conflictSource, conflictStore};
        ok &= Expect(metadata.UpsertRule(conflictRule, &sqliteError), L"conflict rule should be persisted");
        const std::wstring conflictReal = conflictSource + L"\\conflict.txt";
        ok &= Expect(WriteTextFile(conflictReal, "base"), L"conflict real file should be created");
        std::wstring conflictShadow;
        ok &= Expect(operations.PrepareCopyOnWrite(conflictRule, conflictReal, &conflictShadow).success, L"conflict shadow should be prepared");
        ok &= Expect(WriteTextFile(conflictShadow, "overlay"), L"conflict shadow should be changed");
        ok &= Expect(WriteTextFile(conflictReal, "external"), L"external change should be written to real file");
        opResult = operations.Commit(conflictRule, L"commit-conflict-001");
        ok &= Expect(!opResult.success, L"commit should fail when real file changed externally");
        ok &= Expect(ReadTextFile(conflictReal) == "external", L"failed conflict commit should not overwrite real file");
        std::vector<pathoverlay::ChangeRecord> conflictChanges;
        ok &= Expect(operations.ListChanges(conflictRule.id, &conflictChanges).success && !conflictChanges.empty(), L"failed conflict commit should preserve change metadata");
        pathoverlay::CommitRecord failedConflictCommit;
        ok &= Expect(metadata.GetCommitRecord(L"commit-conflict-001", &failedConflictCommit, &sqliteError), L"failed conflict commit should record log entry");
        ok &= Expect(failedConflictCommit.status == L"failed" && failedConflictCommit.error.find(L"conflict") != std::wstring::npos,
                     L"failed conflict commit should record failed status and error");

        const std::wstring lockedSource = TempPathOverlayDir(L"locked_source");
        const std::wstring lockedStore = TempPathOverlayDir(L"locked_store");
        EnsureDirectory(lockedSource);
        EnsureDirectory(lockedStore);
        pathoverlay::OverlayRule lockedRule{L"rule-locked", L"locked", true, lockedSource, lockedStore};
        ok &= Expect(metadata.UpsertRule(lockedRule, &sqliteError), L"locked rule should be persisted");
        const std::wstring lockedReal = lockedSource + L"\\locked.txt";
        ok &= Expect(WriteTextFile(lockedReal, "locked-base"), L"locked real file should be created");
        std::wstring lockedShadow;
        ok &= Expect(operations.PrepareCopyOnWrite(lockedRule, lockedReal, &lockedShadow).success, L"locked shadow should be prepared");
        ok &= Expect(WriteTextFile(lockedShadow, "locked-overlay"), L"locked shadow should be changed");
        HANDLE lockedHandle = CreateFileW(lockedReal.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        ok &= Expect(lockedHandle != INVALID_HANDLE_VALUE, L"locked file should be opened exclusively by test");
        opResult = operations.Commit(lockedRule, L"commit-locked-001");
        if (lockedHandle != INVALID_HANDLE_VALUE) {
            CloseHandle(lockedHandle);
        }
        ok &= Expect(!opResult.success, L"commit should fail when real file is occupied");
        std::vector<pathoverlay::ChangeRecord> lockedChanges;
        ok &= Expect(operations.ListChanges(lockedRule.id, &lockedChanges).success && !lockedChanges.empty(), L"failed occupied commit should preserve change metadata");

        const std::wstring commitId = L"commit-ops-001";
        opResult = operations.Commit(opsRule, commitId);
        ok &= Expect(opResult.success, L"commit should succeed");
        ok &= Expect(ReadTextFile(existingReal) == "changed", L"commit should write modified shadow file to real path");
        ok &= Expect(ReadTextFile(createdReal) == "created", L"commit should write created shadow file to real path");
        ok &= Expect(GetFileAttributesW(deleteReal.c_str()) == INVALID_FILE_ATTRIBUTES, L"commit should delete tombstoned real file");
        std::vector<pathoverlay::ChangeRecord> committedChanges;
        ok &= Expect(operations.ListChanges(opsRule.id, &committedChanges).success && committedChanges.empty(), L"commit should clear metadata changes");
        pathoverlay::CommitRecord operationCommit;
        ok &= Expect(metadata.GetCommitRecord(commitId, &operationCommit, &sqliteError), L"commit should record log entry");
        ok &= Expect(operationCommit.status == L"done" && !operationCommit.backupPath.empty(), L"commit log should include status and backup path");
        ok &= Expect(ReadTextFile(BackupPathForTest(operationCommit.backupPath, existingReal)) == "original",
                     L"commit should back up modified real file before overwrite");
        ok &= Expect(ReadTextFile(BackupPathForTest(operationCommit.backupPath, deleteReal)) == "delete-me",
                     L"commit should back up tombstoned real file before delete");
    }

    if (!ok) {
        return 1;
    }

    std::wcout << L"PathOverlay tests passed\n";
    return 0;
}
