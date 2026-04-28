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

bool IsDirectory(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
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

    const std::wstring source2 = TempPathOverlayDir(L"source2");
    const std::wstring store2 = TempPathOverlayDir(L"store2");
    EnsureDirectory(source2);
    EnsureDirectory(store2);
    pathoverlay::OverlayRule rule2{
        L"rule-002",
        L"second",
        true,
        source2,
        store2,
    };
    std::vector<pathoverlay::OverlayRule> existingRules{rule};
    ok &= Expect(pathoverlay::ValidateOverlayRuleSet(existingRules, rule2).ok(),
                 L"non-overlapping second rule should be valid");

    const std::wstring generatedId1 = pathoverlay::GenerateRuleId();
    ok &= Expect(!generatedId1.empty() && generatedId1.rfind(L"rule-", 0) == 0,
                 L"generated rule id should use rule prefix");
    ok &= Expect(pathoverlay::DefaultStoreRootForRule(L"rule-001") != pathoverlay::DefaultStoreRootForRule(L"rule-002"),
                 L"default store should be independent per rule");

    const std::wstring childSource = source + L"\\child";
    EnsureDirectory(childSource);
    pathoverlay::OverlayRule overlappingSource = rule2;
    overlappingSource.source = childSource;
    ok &= Expect(!pathoverlay::ValidateOverlayRuleSet(existingRules, overlappingSource).ok(),
                 L"child source should be rejected when it overlaps an existing source");
    pathoverlay::OverlayRule sameSource = rule2;
    sameSource.source = source;
    ok &= Expect(!pathoverlay::ValidateOverlayRuleSet(existingRules, sameSource).ok(),
                 L"same source should be rejected when it overlaps an existing source");

    const std::wstring storeSource = TempPathOverlayDir(L"store_source");
    const std::wstring sourceInsideStore = store + L"\\nested-source";
    EnsureDirectory(storeSource);
    EnsureDirectory(sourceInsideStore);
    pathoverlay::OverlayRule sourceStoreOverlap = rule2;
    sourceStoreOverlap.source = sourceInsideStore;
    sourceStoreOverlap.store = storeSource;
    ok &= Expect(!pathoverlay::ValidateOverlayRuleSet(existingRules, sourceStoreOverlap).ok(),
                 L"new source inside existing store should be rejected");
    pathoverlay::OverlayRule storeInsideExistingSource = rule2;
    storeInsideExistingSource.store = source + L"\\nested-store";
    ok &= Expect(!pathoverlay::ValidateOverlayRuleSet(existingRules, storeInsideExistingSource).ok(),
                 L"new store inside existing source should be rejected");

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
        ok &= Expect(metadata.UpsertRule(rule2, &sqliteError), L"second rule should be persisted");
        ok &= Expect(metadata.SetRuleEnabled(rule.id, false, &sqliteError), L"rule enabled state should update");
        ok &= Expect(metadata.SetRuleEnabled(rule2.id, false, &sqliteError), L"second rule should be disabled");
        ok &= Expect(metadata.SetRuleEnabled(rule2.id, true, &sqliteError), L"second rule should be enabled");

        pathoverlay::OverlayRule persistedRule;
        ok &= Expect(metadata.GetRule(rule.id, &persistedRule, &sqliteError), L"rule should be queried");
        ok &= Expect(!persistedRule.enabled, L"queried rule should reflect enabled state");
        pathoverlay::OverlayRule persistedRule2;
        ok &= Expect(metadata.GetRule(rule2.id, &persistedRule2, &sqliteError), L"second rule should be queried");
        ok &= Expect(persistedRule2.enabled, L"second rule should reflect enabled state");
        std::vector<pathoverlay::OverlayRule> persistedRules;
        ok &= Expect(metadata.ListRules(&persistedRules, &sqliteError), L"multiple rules should be listed");
        ok &= Expect(persistedRules.size() >= 2, L"multiple non-overlapping rules should be persisted");
        pathoverlay::OverlayRule customStoreRule{
            L"rule-custom-store",
            L"custom-store",
            true,
            source2,
            TempPathOverlayDir(L"custom_store"),
        };
        EnsureDirectory(customStoreRule.store);
        ok &= Expect(metadata.UpsertRule(customStoreRule, &sqliteError), L"custom store rule should be persisted");
        pathoverlay::OverlayRule persistedCustomStoreRule;
        ok &= Expect(metadata.GetRule(customStoreRule.id, &persistedCustomStoreRule, &sqliteError),
                     L"custom store rule should be queried");
        ok &= Expect(pathoverlay::NormalizePath(persistedCustomStoreRule.store) == pathoverlay::NormalizePath(customStoreRule.store),
                     L"custom store path should be persisted");

        const pathoverlay::ChangeState states[] = {
            pathoverlay::ChangeState::kCreated,
            pathoverlay::ChangeState::kModified,
            pathoverlay::ChangeState::kDeleted,
            pathoverlay::ChangeState::kTombstone,
            pathoverlay::ChangeState::kRenamed,
        };
        for (int i = 0; i < 5; ++i) {
            pathoverlay::ChangeRecord change;
            change.realPath = source + L"\\change" + std::to_wstring(i) + L".txt";
            change.shadowPath = store + L"\\drive\\C\\change" + std::to_wstring(i) + L".txt";
            if (states[i] == pathoverlay::ChangeState::kRenamed) {
                change.targetPath = source + L"\\renamed-target.txt";
            }
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
        ok &= Expect(changes.size() == 5, L"all change states should be persisted");

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

        const std::wstring renameSourceReal = opsSource + L"\\rename-source.txt";
        const std::wstring renameTargetReal = opsSource + L"\\rename-target.txt";
        ok &= Expect(WriteTextFile(renameSourceReal, "rename-me"), L"rename source real file should be created");
        opResult = operations.RecordRename(opsRule, renameSourceReal, renameTargetReal);
        ok &= Expect(opResult.success, L"file rename should be recorded");
        const auto renameTargetShadow = pathoverlay::MapRealPathToShadowPath(opsRule, renameTargetReal);
        ok &= Expect(renameTargetShadow.has_value() && ReadTextFile(*renameTargetShadow) == "rename-me",
                     L"rename should move file content into target shadow");

        const std::wstring renameConflictSource = opsSource + L"\\rename-conflict-source.txt";
        const std::wstring renameConflictTarget = opsSource + L"\\rename-conflict-target.txt";
        ok &= Expect(WriteTextFile(renameConflictSource, "source"), L"rename conflict source should be created");
        ok &= Expect(WriteTextFile(renameConflictTarget, "target"), L"rename conflict target should be created");
        opResult = operations.RecordRename(opsRule, renameConflictSource, renameConflictTarget);
        ok &= Expect(!opResult.success, L"file rename should fail when target exists");

        const std::wstring createdRenameSourceReal = opsSource + L"\\created-rename-source.txt";
        const std::wstring createdRenameTargetReal = opsSource + L"\\created-rename-target.txt";
        const auto createdRenameSourceShadow = pathoverlay::MapRealPathToShadowPath(opsRule, createdRenameSourceReal);
        ok &= Expect(createdRenameSourceShadow.has_value(), L"created rename source should map to shadow");
        opResult = operations.RecordCreatedFile(opsRule, createdRenameSourceReal);
        ok &= Expect(opResult.success, L"created rename source should be recorded");
        ok &= Expect(WriteTextFile(*createdRenameSourceShadow, "created-rename"), L"created rename source shadow should be writable");
        opResult = operations.RecordRename(opsRule, createdRenameSourceReal, createdRenameTargetReal);
        ok &= Expect(opResult.success, L"created file rename should be merged into created target");

        std::vector<pathoverlay::ChangeRecord> operationChanges;
        opResult = operations.ListChanges(opsRule.id, &operationChanges);
        ok &= Expect(opResult.success, L"operation changes should be listed");
        bool renameWasRecorded = false;
        bool createdRenameWasMerged = false;
        for (const auto& record : operationChanges) {
            if (_wcsicmp(pathoverlay::NormalizePath(record.realPath).c_str(), pathoverlay::NormalizePath(renameSourceReal).c_str()) == 0 &&
                _wcsicmp(pathoverlay::NormalizePath(record.targetPath).c_str(), pathoverlay::NormalizePath(renameTargetReal).c_str()) == 0 &&
                record.state == pathoverlay::ChangeState::kRenamed) {
                renameWasRecorded = true;
            }
            if (_wcsicmp(pathoverlay::NormalizePath(record.realPath).c_str(), pathoverlay::NormalizePath(createdRenameTargetReal).c_str()) == 0 &&
                record.state == pathoverlay::ChangeState::kCreated) {
                createdRenameWasMerged = true;
            }
        }
        ok &= Expect(operationChanges.size() >= 5, L"operation changes should include modified, created, tombstone, and rename records");
        ok &= Expect(renameWasRecorded, L"operation changes should include file rename metadata");
        ok &= Expect(createdRenameWasMerged, L"created file rename should merge to created target metadata");

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
        const std::wstring createdDirectoryReal = opsSource + L"\\created-dir";
        std::wstring createdDirectoryShadow;
        opResult = operations.PrepareDirectoryView(opsRule, createdDirectoryReal, &createdDirectoryShadow);
        ok &= Expect(opResult.success, L"created directory should be recorded");
        ok &= Expect(GetFileAttributesW(createdDirectoryReal.c_str()) == INVALID_FILE_ATTRIBUTES,
                     L"created directory should not exist in source before commit");
        ok &= Expect(IsDirectory(createdDirectoryShadow),
                     L"created directory should exist in shadow before commit");

        const std::wstring deletedDirectoryReal = opsSource + L"\\deleted-dir";
        EnsureDirectory(deletedDirectoryReal);
        ok &= Expect(WriteTextFile(deletedDirectoryReal + L"\\child.txt", "delete-dir-child"),
                     L"directory delete child should be created");
        opResult = operations.RecordDelete(opsRule, deletedDirectoryReal);
        ok &= Expect(opResult.success, L"directory delete should record tombstone");
        ok &= Expect(IsDirectory(deletedDirectoryReal),
                     L"directory tombstone should not delete real directory before commit");
        opResult = operations.PrepareDirectoryView(opsRule, opsSource, &directoryShadow);
        ok &= Expect(opResult.success, L"directory view should refresh after directory tombstone");
        const auto deletedDirectoryShadow = pathoverlay::MapRealPathToShadowPath(opsRule, deletedDirectoryReal);
        const auto deletedDirectoryChildShadow = pathoverlay::MapRealPathToShadowPath(opsRule, deletedDirectoryReal + L"\\child.txt");
        ok &= Expect(deletedDirectoryShadow.has_value() &&
                     GetFileAttributesW(deletedDirectoryShadow->c_str()) == INVALID_FILE_ATTRIBUTES,
                     L"directory view should hide tombstoned directory");
        ok &= Expect(deletedDirectoryChildShadow.has_value() &&
                     GetFileAttributesW(deletedDirectoryChildShadow->c_str()) == INVALID_FILE_ATTRIBUTES,
                     L"directory view should hide tombstoned directory children");
        std::vector<pathoverlay::ChangeRecord> directoryChanges;
        ok &= Expect(operations.ListChanges(opsRule.id, &directoryChanges).success, L"directory change query should succeed");
        bool createdDirectoryWasRecorded = false;
        bool deletedDirectoryWasRecorded = false;
        for (const auto& record : directoryChanges) {
            const std::wstring recordPath = pathoverlay::NormalizePath(record.realPath);
            if (_wcsicmp(recordPath.c_str(), pathoverlay::NormalizePath(createdDirectoryReal).c_str()) == 0 &&
                record.state == pathoverlay::ChangeState::kCreated) {
                createdDirectoryWasRecorded = true;
            }
            if (_wcsicmp(recordPath.c_str(), pathoverlay::NormalizePath(deletedDirectoryReal).c_str()) == 0 &&
                record.state == pathoverlay::ChangeState::kTombstone) {
                deletedDirectoryWasRecorded = true;
            }
        }
        ok &= Expect(createdDirectoryWasRecorded, L"created directory should create commit metadata");
        ok &= Expect(deletedDirectoryWasRecorded, L"deleted directory should create tombstone metadata");

        const std::wstring directoryRenameSource = opsSource + L"\\dir-rename-source";
        const std::wstring directoryRenameTarget = opsSource + L"\\dir-rename-target";
        EnsureDirectory(directoryRenameSource);
        EnsureDirectory(directoryRenameSource + L"\\nested");
        ok &= Expect(WriteTextFile(directoryRenameSource + L"\\child.txt", "dir-child-original"),
                     L"directory rename child should be created");
        ok &= Expect(WriteTextFile(directoryRenameSource + L"\\nested\\grandchild.txt", "dir-grandchild-original"),
                     L"directory rename nested child should be created");
        std::wstring directoryRenameChildShadow;
        opResult = operations.PrepareCopyOnWrite(
            opsRule,
            directoryRenameSource + L"\\child.txt",
            &directoryRenameChildShadow);
        ok &= Expect(opResult.success, L"directory rename child shadow should be prepared");
        ok &= Expect(WriteTextFile(directoryRenameChildShadow, "dir-child-overlay"),
                     L"directory rename child shadow should be writable");
        opResult = operations.RecordRename(opsRule, directoryRenameSource, directoryRenameTarget);
        ok &= Expect(opResult.success, L"directory rename should be recorded");
        const auto directoryRenameTargetShadow = pathoverlay::MapRealPathToShadowPath(opsRule, directoryRenameTarget);
        ok &= Expect(directoryRenameTargetShadow.has_value() &&
                     IsDirectory(*directoryRenameTargetShadow),
                     L"directory rename should move source shadow directory to target");
        ok &= Expect(ReadTextFile(*directoryRenameTargetShadow + L"\\child.txt") == "dir-child-overlay",
                     L"directory rename should preserve modified child in target shadow");
        ok &= Expect(ReadTextFile(*directoryRenameTargetShadow + L"\\nested\\grandchild.txt") == "dir-grandchild-original",
                     L"directory rename should materialize nested children in target shadow");
        std::error_code removeShadowError;
        std::filesystem::remove(*directoryRenameTargetShadow + L"\\nested\\grandchild.txt", removeShadowError);
        std::wstring preparedRenamedTargetShadow;
        opResult = operations.PrepareRenamedTargetPath(
            opsRule,
            directoryRenameTarget + L"\\nested\\grandchild.txt",
            &preparedRenamedTargetShadow);
        ok &= Expect(opResult.success, L"renamed target nested child should be prepared on demand");
        ok &= Expect(ReadTextFile(preparedRenamedTargetShadow) == "dir-grandchild-original",
                     L"renamed target nested child lazy prepare should restore missing shadow child from source");
        std::vector<pathoverlay::ChangeRecord> directoryRenameChanges;
        ok &= Expect(operations.ListChanges(opsRule.id, &directoryRenameChanges).success,
                     L"directory rename changes should be listed");
        bool directoryRenameWasRecorded = false;
        bool directoryRenameChildRecordWasRemoved = true;
        for (const auto& record : directoryRenameChanges) {
            if (_wcsicmp(pathoverlay::NormalizePath(record.realPath).c_str(), pathoverlay::NormalizePath(directoryRenameSource).c_str()) == 0 &&
                _wcsicmp(pathoverlay::NormalizePath(record.targetPath).c_str(), pathoverlay::NormalizePath(directoryRenameTarget).c_str()) == 0 &&
                record.state == pathoverlay::ChangeState::kRenamed) {
                directoryRenameWasRecorded = true;
            }
            if (_wcsicmp(pathoverlay::NormalizePath(record.realPath).c_str(),
                         pathoverlay::NormalizePath(directoryRenameSource + L"\\child.txt").c_str()) == 0) {
                directoryRenameChildRecordWasRemoved = false;
            }
        }
        ok &= Expect(directoryRenameWasRecorded, L"directory rename metadata should be recorded");
        ok &= Expect(directoryRenameChildRecordWasRemoved, L"directory rename should fold child metadata into directory move");

        const std::wstring createdDirectoryRenameSource = opsSource + L"\\created-dir-rename-source";
        const std::wstring createdDirectoryRenameTarget = opsSource + L"\\created-dir-rename-target";
        std::wstring createdDirectoryRenameShadow;
        opResult = operations.PrepareDirectoryView(opsRule, createdDirectoryRenameSource, &createdDirectoryRenameShadow);
        ok &= Expect(opResult.success, L"created directory rename source should be recorded");
        ok &= Expect(WriteTextFile(createdDirectoryRenameShadow + L"\\child.txt", "created-dir-rename"),
                     L"created directory rename shadow child should be writable");
        opResult = operations.RecordRename(opsRule, createdDirectoryRenameSource, createdDirectoryRenameTarget);
        ok &= Expect(opResult.success, L"created directory rename should be merged into created target");

        const std::wstring directoryRenameConflictSource = opsSource + L"\\dir-rename-conflict-source";
        const std::wstring directoryRenameConflictTarget = opsSource + L"\\dir-rename-conflict-target";
        EnsureDirectory(directoryRenameConflictSource);
        EnsureDirectory(directoryRenameConflictTarget);
        opResult = operations.RecordRename(opsRule, directoryRenameConflictSource, directoryRenameConflictTarget);
        ok &= Expect(!opResult.success, L"directory rename should fail when target exists");

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
        const std::wstring discardRenameSource = discardSource + L"\\discard-rename-source.txt";
        const std::wstring discardRenameTarget = discardSource + L"\\discard-rename-target.txt";
        ok &= Expect(WriteTextFile(discardRenameSource, "discard-rename"), L"discard rename source should be created");
        ok &= Expect(operations.RecordRename(discardRule, discardRenameSource, discardRenameTarget).success,
                     L"discard rename should be recorded");
        ok &= Expect(operations.Discard(discardRule).success, L"discard should succeed");
        ok &= Expect(ReadTextFile(discardReal) == "keep", L"discard should not modify real file");
        ok &= Expect(ReadTextFile(discardRenameSource) == "discard-rename",
                     L"discard should keep renamed source real file");
        ok &= Expect(GetFileAttributesW(discardRenameTarget.c_str()) == INVALID_FILE_ATTRIBUTES,
                     L"discard should not create renamed target real file");
        std::vector<pathoverlay::ChangeRecord> discardChanges;
        ok &= Expect(operations.ListChanges(discardRule.id, &discardChanges).success && discardChanges.empty(), L"discard should clear metadata changes");
        const std::wstring discardDirectoryReal = discardSource + L"\\discard-dir";
        std::wstring discardDirectoryShadow;
        ok &= Expect(operations.PrepareDirectoryView(discardRule, discardDirectoryReal, &discardDirectoryShadow).success,
                     L"discard directory shadow should be prepared");
        const std::wstring discardDeletedDirectoryReal = discardSource + L"\\discard-delete-dir";
        EnsureDirectory(discardDeletedDirectoryReal);
        ok &= Expect(operations.RecordDelete(discardRule, discardDeletedDirectoryReal).success,
                     L"discard directory delete should be recorded");
        const std::wstring discardDirectoryRenameSource = discardSource + L"\\discard-dir-rename-source";
        const std::wstring discardDirectoryRenameTarget = discardSource + L"\\discard-dir-rename-target";
        EnsureDirectory(discardDirectoryRenameSource);
        ok &= Expect(WriteTextFile(discardDirectoryRenameSource + L"\\child.txt", "discard-dir-rename"),
                     L"discard directory rename child should be created");
        ok &= Expect(operations.RecordRename(discardRule, discardDirectoryRenameSource, discardDirectoryRenameTarget).success,
                     L"discard directory rename should be recorded");
        ok &= Expect(operations.Discard(discardRule).success, L"directory discard should succeed");
        ok &= Expect(GetFileAttributesW(discardDirectoryReal.c_str()) == INVALID_FILE_ATTRIBUTES,
                     L"discard should not create real directory");
        ok &= Expect(IsDirectory(discardDeletedDirectoryReal),
                     L"discard should not delete real directory");
        ok &= Expect(IsDirectory(discardDirectoryRenameSource),
                     L"discard should keep renamed source real directory");
        ok &= Expect(GetFileAttributesW(discardDirectoryRenameTarget.c_str()) == INVALID_FILE_ATTRIBUTES,
                     L"discard should not create renamed target real directory");

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
        ok &= Expect(GetFileAttributesW(renameSourceReal.c_str()) == INVALID_FILE_ATTRIBUTES,
                     L"commit should remove renamed source file");
        ok &= Expect(ReadTextFile(renameTargetReal) == "rename-me", L"commit should create renamed target file");
        ok &= Expect(ReadTextFile(createdRenameTargetReal) == "created-rename",
                     L"commit should create target for renamed created file");
        ok &= Expect(IsDirectory(createdDirectoryReal),
                     L"commit should create real directory from shadow");
        ok &= Expect(GetFileAttributesW(deletedDirectoryReal.c_str()) == INVALID_FILE_ATTRIBUTES,
                     L"commit should delete tombstoned real directory");
        ok &= Expect(GetFileAttributesW(directoryRenameSource.c_str()) == INVALID_FILE_ATTRIBUTES,
                     L"commit should remove renamed source directory");
        ok &= Expect(ReadTextFile(directoryRenameTarget + L"\\child.txt") == "dir-child-overlay",
                     L"commit should create renamed target directory with modified child");
        ok &= Expect(ReadTextFile(directoryRenameTarget + L"\\nested\\grandchild.txt") == "dir-grandchild-original",
                     L"commit should create renamed target directory with nested child");
        ok &= Expect(ReadTextFile(createdDirectoryRenameTarget + L"\\child.txt") == "created-dir-rename",
                     L"commit should create renamed target for created directory with child");
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
