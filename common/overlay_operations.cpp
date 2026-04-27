#include "overlay_operations.h"

#include <windows.h>

#include <cwctype>
#include <filesystem>
#include <sstream>

namespace pathoverlay {
namespace {

namespace fs = std::filesystem;

OperationResult Ok() {
    return OperationResult{true, L""};
}

OperationResult Fail(const std::wstring& message) {
    return OperationResult{false, message};
}

std::wstring FileTimeToText(const FILETIME& time) {
    ULARGE_INTEGER value;
    value.LowPart = time.dwLowDateTime;
    value.HighPart = time.dwHighDateTime;
    return std::to_wstring(value.QuadPart);
}

bool TryGetFileInfo(const std::wstring& path, bool* exists, unsigned long long* size, std::wstring* lastWriteTime) {
    WIN32_FILE_ATTRIBUTE_DATA data = {};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            *exists = false;
            *size = 0;
            *lastWriteTime = L"";
            return true;
        }
        return false;
    }

    ULARGE_INTEGER fileSize;
    fileSize.LowPart = data.nFileSizeLow;
    fileSize.HighPart = data.nFileSizeHigh;
    *exists = true;
    *size = fileSize.QuadPart;
    *lastWriteTime = FileTimeToText(data.ftLastWriteTime);
    return true;
}

bool FileInfoMatches(const ChangeRecord& record, bool exists, unsigned long long size, const std::wstring& lastWriteTime) {
    if (record.originalExists != exists) {
        return false;
    }
    if (!exists) {
        return true;
    }
    return record.originalSize == size && record.originalLastWriteTime == lastWriteTime;
}

bool TryOpenExclusive(const std::wstring& path, DWORD desiredAccess, std::wstring* error) {
    HANDLE file = CreateFileW(
        path.c_str(),
        desiredAccess,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        std::wstringstream stream;
        stream << L"file is occupied or inaccessible: " << path << L" error=" << GetLastError();
        *error = stream.str();
        return false;
    }

    CloseHandle(file);
    return true;
}

std::wstring BackupRoot(const OverlayRule& rule, const std::wstring& commitId) {
    return NormalizePath(rule.store.empty() ? DefaultStoreRoot() : rule.store) + L"\\backups\\" + commitId;
}

std::wstring BackupPathFor(const OverlayRule& rule, const std::wstring& commitId, const std::wstring& realPath) {
    std::wstring backupRoot = BackupRoot(rule, commitId);
    std::wstring normalizedReal = NormalizePath(realPath);
    if (normalizedReal.size() >= 2 && normalizedReal[1] == L':') {
        normalizedReal.erase(1, 1);
    }
    return backupRoot + L"\\drive\\" + normalizedReal;
}

OperationResult PersistChange(
    MetadataStore& metadata,
    const OverlayRule& rule,
    const std::wstring& realPath,
    const std::wstring& shadowPath,
    ChangeState state) {
    bool originalExists = false;
    unsigned long long originalSize = 0;
    std::wstring originalLastWriteTime;
    if (!TryGetFileInfo(realPath, &originalExists, &originalSize, &originalLastWriteTime)) {
        return Fail(L"failed to read original file metadata");
    }

    bool currentExists = false;
    unsigned long long currentSize = 0;
    std::wstring currentLastWriteTime;
    TryGetFileInfo(shadowPath, &currentExists, &currentSize, &currentLastWriteTime);

    ChangeRecord record;
    record.realPath = NormalizePath(realPath);
    record.shadowPath = NormalizePath(shadowPath);
    record.state = state;
    record.originalExists = originalExists;
    record.originalSize = originalSize;
    record.originalLastWriteTime = originalLastWriteTime;
    record.currentSize = currentExists ? currentSize : 0;
    record.lastWriteTime = currentExists ? currentLastWriteTime : L"";

    std::wstring error;
    if (!metadata.AddOrUpdateChange(rule.id, record, &error)) {
        return Fail(error);
    }
    return Ok();
}

OperationResult CopyFileWithParents(const std::wstring& source, const std::wstring& target) {
    std::error_code errorCode;
    fs::create_directories(fs::path(target).parent_path(), errorCode);
    if (errorCode) {
        return Fail(L"failed to create target directory");
    }

    fs::copy_file(source, target, fs::copy_options::overwrite_existing, errorCode);
    if (errorCode) {
        return Fail(L"failed to copy file");
    }
    return Ok();
}

std::wstring ToLowerPath(std::wstring value) {
    for (wchar_t& ch : value) {
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
    return value;
}

bool IsDirectTombstoneChild(
    const std::vector<ChangeRecord>& records,
    const std::wstring& directory,
    const std::wstring& child) {
    const std::wstring normalizedDirectory = ToLowerPath(NormalizePath(directory));
    const std::wstring normalizedChild = ToLowerPath(NormalizePath(child));
    for (const ChangeRecord& record : records) {
        if (record.state != ChangeState::kTombstone && record.state != ChangeState::kDeleted) {
            continue;
        }

        const std::wstring normalizedRecord = ToLowerPath(NormalizePath(record.realPath));
        if (fs::path(normalizedRecord).parent_path().wstring() != normalizedDirectory) {
            continue;
        }

        if (normalizedRecord == normalizedChild) {
            return true;
        }
    }

    return false;
}

bool AddCommitLog(
    MetadataStore& metadata,
    const OverlayRule& rule,
    const std::wstring& commitId,
    const std::wstring& status,
    const std::wstring& backupRoot,
    const std::wstring& message,
    std::wstring* error) {
    CommitRecord commit;
    commit.id = commitId;
    commit.ruleId = rule.id;
    commit.startTime = L"manual";
    commit.status = status;
    commit.operations = L"commit";
    commit.backupPath = backupRoot;
    commit.error = message;
    return metadata.AddCommitRecord(commit, error);
}

OperationResult FailCommit(
    MetadataStore& metadata,
    const OverlayRule& rule,
    const std::wstring& commitId,
    const std::wstring& backupRoot,
    const std::wstring& message) {
    std::wstring logError;
    AddCommitLog(metadata, rule, commitId, L"failed", backupRoot, message, &logError);
    return Fail(message);
}

OperationResult ValidateCommitPreconditions(const std::vector<ChangeRecord>& records) {
    for (const ChangeRecord& record : records) {
        bool realExists = false;
        unsigned long long realSize = 0;
        std::wstring realLastWriteTime;
        if (!TryGetFileInfo(record.realPath, &realExists, &realSize, &realLastWriteTime)) {
            return Fail(L"failed to read real file metadata during commit preflight");
        }

        if (!FileInfoMatches(record, realExists, realSize, realLastWriteTime)) {
            return Fail(L"commit conflict: real file changed since overlay record was created");
        }

        if (realExists) {
            std::wstring lockError;
            if (!TryOpenExclusive(record.realPath, GENERIC_READ, &lockError)) {
                return Fail(lockError);
            }
        }

        if (record.state == ChangeState::kCreated || record.state == ChangeState::kModified) {
            bool shadowExists = false;
            unsigned long long shadowSize = 0;
            std::wstring shadowLastWriteTime;
            if (!TryGetFileInfo(record.shadowPath, &shadowExists, &shadowSize, &shadowLastWriteTime)) {
                return Fail(L"failed to read shadow file metadata during commit preflight");
            }
            if (!shadowExists) {
                return Fail(L"shadow file is missing during commit");
            }

            std::wstring lockError;
            if (!TryOpenExclusive(record.shadowPath, GENERIC_READ, &lockError)) {
                return Fail(lockError);
            }
        }
    }

    return Ok();
}

}  // namespace

OverlayOperations::OverlayOperations(MetadataStore& metadata) : metadata_(metadata) {}

OperationResult OverlayOperations::PrepareCopyOnWrite(const OverlayRule& rule, const std::wstring& realPath, std::wstring* shadowPath) {
    const auto mapped = MapRealPathToShadowPath(rule, realPath);
    if (!mapped.has_value()) {
        return Fail(L"real path is outside rule or rule is invalid");
    }
    *shadowPath = *mapped;

    const std::wstring normalizedReal = NormalizePath(realPath);
    const std::wstring normalizedShadow = NormalizePath(*shadowPath);
    std::error_code errorCode;
    fs::create_directories(fs::path(normalizedShadow).parent_path(), errorCode);
    if (errorCode) {
        return Fail(L"failed to create shadow parent directory");
    }

    const bool realExists = fs::exists(normalizedReal, errorCode);
    if (errorCode) {
        return Fail(L"failed to read real path state");
    }
    if (realExists && fs::is_directory(normalizedReal, errorCode)) {
        return PrepareDirectoryView(rule, normalizedReal, shadowPath);
    }
    if (errorCode) {
        return Fail(L"failed to read real path type");
    }

    errorCode.clear();
    const bool shadowExists = fs::exists(normalizedShadow, errorCode);
    if (realExists && !shadowExists) {
        const OperationResult copyResult = CopyFileWithParents(normalizedReal, normalizedShadow);
        if (!copyResult.success) {
            return copyResult;
        }
    }

    return PersistChange(
        metadata_,
        rule,
        normalizedReal,
        normalizedShadow,
        realExists ? ChangeState::kModified : ChangeState::kCreated);
}

OperationResult OverlayOperations::PrepareDirectoryView(const OverlayRule& rule, const std::wstring& realPath, std::wstring* shadowPath) {
    const auto mapped = MapRealPathToShadowPath(rule, realPath);
    if (!mapped.has_value()) {
        return Fail(L"real path is outside rule or rule is invalid");
    }
    *shadowPath = *mapped;

    const std::wstring normalizedReal = NormalizePath(realPath);
    const std::wstring normalizedShadow = NormalizePath(*shadowPath);
    std::error_code errorCode;
    fs::create_directories(normalizedShadow, errorCode);
    if (errorCode) {
        return Fail(L"failed to create shadow directory view");
    }

    if (!fs::exists(normalizedReal, errorCode)) {
        return Ok();
    }
    if (!fs::is_directory(normalizedReal, errorCode)) {
        return Fail(L"directory view target is not a directory");
    }

    std::vector<ChangeRecord> records;
    std::wstring error;
    if (!metadata_.ListChanges(rule.id, &records, &error)) {
        return Fail(error);
    }

    for (const ChangeRecord& record : records) {
        if (record.state != ChangeState::kTombstone && record.state != ChangeState::kDeleted) {
            continue;
        }
        if (!IsDirectTombstoneChild(records, normalizedReal, record.realPath)) {
            continue;
        }

        const auto mappedTombstone = MapRealPathToShadowPath(rule, record.realPath);
        if (!mappedTombstone.has_value()) {
            continue;
        }

        fs::remove_all(*mappedTombstone, errorCode);
        if (errorCode) {
            return Fail(L"failed to remove existing shadow item for tombstone");
        }
    }

    for (const fs::directory_entry& entry : fs::directory_iterator(normalizedReal, errorCode)) {
        if (errorCode) {
            return Fail(L"failed to enumerate real directory");
        }

        const std::wstring realChild = entry.path().wstring();
        if (IsDirectTombstoneChild(records, normalizedReal, realChild)) {
            continue;
        }

        const auto mappedChild = MapRealPathToShadowPath(rule, realChild);
        if (!mappedChild.has_value()) {
            continue;
        }

        if (fs::exists(*mappedChild, errorCode)) {
            errorCode.clear();
            continue;
        }

        if (entry.is_directory(errorCode)) {
            fs::create_directories(*mappedChild, errorCode);
            if (errorCode) {
                return Fail(L"failed to create shadow child directory");
            }
            continue;
        }

        errorCode.clear();
        if (entry.is_regular_file(errorCode)) {
            const OperationResult copyResult = CopyFileWithParents(realChild, *mappedChild);
            if (!copyResult.success) {
                return copyResult;
            }
        }
    }

    return Ok();
}

OperationResult OverlayOperations::RecordCreatedFile(const OverlayRule& rule, const std::wstring& realPath) {
    const auto mapped = MapRealPathToShadowPath(rule, realPath);
    if (!mapped.has_value()) {
        return Fail(L"real path is outside rule or rule is invalid");
    }

    std::error_code errorCode;
    fs::create_directories(fs::path(*mapped).parent_path(), errorCode);
    if (errorCode) {
        return Fail(L"failed to create shadow parent directory");
    }

    return PersistChange(metadata_, rule, realPath, *mapped, ChangeState::kCreated);
}

OperationResult OverlayOperations::RecordDelete(const OverlayRule& rule, const std::wstring& realPath) {
    const auto mapped = MapRealPathToShadowPath(rule, realPath);
    if (!mapped.has_value()) {
        return Fail(L"real path is outside rule or rule is invalid");
    }

    std::error_code errorCode;
    const std::wstring normalizedReal = NormalizePath(realPath);
    if (fs::exists(normalizedReal, errorCode) && fs::is_directory(normalizedReal, errorCode)) {
        return Fail(L"directory delete is not supported in MVP");
    }
    if (errorCode) {
        return Fail(L"failed to read delete target type");
    }

    if (fs::exists(*mapped, errorCode)) {
        fs::remove(*mapped, errorCode);
        if (errorCode) {
            return Fail(L"failed to remove existing shadow file for tombstone");
        }
    }

    return PersistChange(metadata_, rule, realPath, *mapped, ChangeState::kTombstone);
}

OperationResult OverlayOperations::ListChanges(const std::wstring& ruleId, std::vector<ChangeRecord>* records) {
    std::wstring error;
    if (!metadata_.ListChanges(ruleId, records, &error)) {
        return Fail(error);
    }
    return Ok();
}

OperationResult OverlayOperations::Commit(const OverlayRule& rule, const std::wstring& commitId) {
    std::vector<ChangeRecord> records;
    std::wstring error;
    if (!metadata_.ListChanges(rule.id, &records, &error)) {
        return Fail(error);
    }

    const std::wstring backupRoot = BackupRoot(rule, commitId);
    const OperationResult preflight = ValidateCommitPreconditions(records);
    if (!preflight.success) {
        return FailCommit(metadata_, rule, commitId, backupRoot, preflight.message);
    }

    std::error_code errorCode;
    fs::create_directories(backupRoot, errorCode);
    if (errorCode) {
        return FailCommit(metadata_, rule, commitId, backupRoot, L"failed to create commit backup directory");
    }

    for (const ChangeRecord& record : records) {
        const fs::path realPath(record.realPath);
        const fs::path shadowPath(record.shadowPath);

        if (record.state == ChangeState::kCreated || record.state == ChangeState::kModified) {
            if (!fs::exists(shadowPath, errorCode)) {
                return Fail(L"shadow file is missing during commit");
            }
            if (fs::exists(realPath, errorCode)) {
                const std::wstring backupPath = BackupPathFor(rule, commitId, record.realPath);
                const OperationResult backupResult = CopyFileWithParents(record.realPath, backupPath);
                if (!backupResult.success) {
                    return FailCommit(metadata_, rule, commitId, backupRoot, backupResult.message);
                }
            }
            fs::create_directories(realPath.parent_path(), errorCode);
            if (errorCode) {
                return FailCommit(metadata_, rule, commitId, backupRoot, L"failed to create real parent directory during commit");
            }
            fs::copy_file(shadowPath, realPath, fs::copy_options::overwrite_existing, errorCode);
            if (errorCode) {
                return FailCommit(metadata_, rule, commitId, backupRoot, L"failed to write shadow file to real path");
            }
            continue;
        }

        if (record.state == ChangeState::kDeleted || record.state == ChangeState::kTombstone) {
            if (fs::exists(realPath, errorCode)) {
                const std::wstring backupPath = BackupPathFor(rule, commitId, record.realPath);
                const OperationResult backupResult = CopyFileWithParents(record.realPath, backupPath);
                if (!backupResult.success) {
                    return FailCommit(metadata_, rule, commitId, backupRoot, backupResult.message);
                }
                fs::remove(realPath, errorCode);
                if (errorCode) {
                    return FailCommit(metadata_, rule, commitId, backupRoot, L"failed to delete real file during commit");
                }
            }
        }
    }

    const std::wstring driveRoot = NormalizePath(rule.store.empty() ? DefaultStoreRoot() : rule.store) + L"\\drive";
    fs::remove_all(driveRoot, errorCode);
    if (errorCode) {
        return FailCommit(metadata_, rule, commitId, backupRoot, L"failed to clean shadow data after commit");
    }
    if (!metadata_.DeleteChangesForRule(rule.id, &error)) {
        return FailCommit(metadata_, rule, commitId, backupRoot, error);
    }

    if (!AddCommitLog(metadata_, rule, commitId, L"done", backupRoot, L"", &error)) {
        return Fail(error);
    }

    return Ok();
}

OperationResult OverlayOperations::Discard(const OverlayRule& rule) {
    std::wstring error;
    std::error_code errorCode;
    const std::wstring driveRoot = NormalizePath(rule.store.empty() ? DefaultStoreRoot() : rule.store) + L"\\drive";
    fs::remove_all(driveRoot, errorCode);
    if (errorCode) {
        return Fail(L"failed to remove shadow data");
    }
    if (!metadata_.DeleteChangesForRule(rule.id, &error)) {
        return Fail(error);
    }
    return Ok();
}

}  // namespace pathoverlay
