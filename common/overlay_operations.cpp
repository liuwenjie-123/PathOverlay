#include "overlay_operations.h"

#include <windows.h>

#include <cwctype>
#include <filesystem>
#include <sstream>
#include <thread>

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

std::wstring SafePathSegment(std::wstring value) {
    if (value.empty()) {
        return L"default";
    }
    for (wchar_t& ch : value) {
        if (!(std::iswalnum(ch) || ch == L'-' || ch == L'_')) {
            ch = L'_';
        }
    }
    return value;
}

std::wstring DiscardCleanupRoot(const OverlayRule& rule) {
    std::wstringstream stream;
    stream << L".discard-cleanup-"
           << SafePathSegment(rule.id)
           << L"-" << GetCurrentProcessId()
           << L"-" << GetTickCount64();
    return NormalizePath(rule.store.empty() ? DefaultStoreRoot() : rule.store) + L"\\" + stream.str();
}

void RemoveAllDetached(const std::wstring& path) {
    std::thread([path]() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }).detach();
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
    ChangeState state,
    const std::wstring& targetPath = L"") {
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
    record.targetPath = targetPath.empty() ? L"" : NormalizePath(targetPath);
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

OperationResult CopyPathWithParents(const std::wstring& source, const std::wstring& target) {
    std::error_code errorCode;
    if (fs::is_directory(source, errorCode)) {
        fs::create_directories(fs::path(target).parent_path(), errorCode);
        if (errorCode) {
            return Fail(L"failed to create target directory");
        }

        fs::copy(
            source,
            target,
            fs::copy_options::recursive | fs::copy_options::overwrite_existing,
            errorCode);
        if (errorCode) {
            return Fail(L"failed to copy directory");
        }
        return Ok();
    }
    if (errorCode) {
        return Fail(L"failed to read source path type");
    }

    return CopyFileWithParents(source, target);
}

OperationResult CopyMissingDirectoryTree(const std::wstring& source, const std::wstring& target) {
    std::error_code errorCode;
    fs::create_directories(target, errorCode);
    if (errorCode) {
        return Fail(L"failed to create target directory");
    }

    for (const fs::directory_entry& entry : fs::recursive_directory_iterator(source, errorCode)) {
        if (errorCode) {
            return Fail(L"failed to enumerate source directory tree");
        }

        const fs::path relative = fs::relative(entry.path(), source, errorCode);
        if (errorCode) {
            return Fail(L"failed to compute relative directory path");
        }
        const fs::path targetPath = fs::path(target) / relative;

        if (entry.is_directory(errorCode)) {
            fs::create_directories(targetPath, errorCode);
            if (errorCode) {
                return Fail(L"failed to create target child directory");
            }
            continue;
        }
        if (errorCode) {
            return Fail(L"failed to read source directory entry type");
        }

        if (entry.is_regular_file(errorCode)) {
            if (!fs::exists(targetPath, errorCode)) {
                fs::create_directories(targetPath.parent_path(), errorCode);
                if (errorCode) {
                    return Fail(L"failed to create target child parent directory");
                }
                fs::copy_file(entry.path(), targetPath, fs::copy_options::none, errorCode);
                if (errorCode) {
                    return Fail(L"failed to copy target child file");
                }
            }
            errorCode.clear();
        }
    }

    return Ok();
}

OperationResult CopyMissingPath(const std::wstring& source, const std::wstring& target) {
    std::error_code errorCode;
    if (!fs::exists(source, errorCode)) {
        return Ok();
    }
    if (errorCode) {
        return Fail(L"failed to read source path state");
    }

    if (fs::is_directory(source, errorCode)) {
        return CopyMissingDirectoryTree(source, target);
    }
    if (errorCode) {
        return Fail(L"failed to read source path type");
    }

    if (fs::exists(target, errorCode)) {
        errorCode.clear();
        return Ok();
    }
    if (errorCode) {
        return Fail(L"failed to read target path state");
    }

    return CopyFileWithParents(source, target);
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
        if (record.state != ChangeState::kTombstone &&
            record.state != ChangeState::kDeleted &&
            record.state != ChangeState::kRenamed) {
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

bool IsSamePathOrDescendant(const std::wstring& parent, const std::wstring& child) {
    const std::wstring normalizedParent = ToLowerPath(NormalizePath(parent));
    const std::wstring normalizedChild = ToLowerPath(NormalizePath(child));
    if (normalizedParent == normalizedChild) {
        return true;
    }

    std::wstring parentWithSlash = normalizedParent;
    if (!parentWithSlash.empty() && parentWithSlash.back() != L'\\') {
        parentWithSlash += L'\\';
    }
    return normalizedChild.rfind(parentWithSlash, 0) == 0;
}

std::wstring RelativePathUnder(const std::wstring& parent, const std::wstring& child) {
    const std::wstring normalizedParent = NormalizePath(parent);
    const std::wstring normalizedChild = NormalizePath(child);
    if (_wcsicmp(normalizedParent.c_str(), normalizedChild.c_str()) == 0) {
        return L"";
    }
    if (!IsSamePathOrDescendant(normalizedParent, normalizedChild)) {
        return L"";
    }

    size_t offset = normalizedParent.size();
    while (offset < normalizedChild.size() && (normalizedChild[offset] == L'\\' || normalizedChild[offset] == L'/')) {
        ++offset;
    }
    return normalizedChild.substr(offset);
}

bool IsDirectoryPath(const std::wstring& path, bool* isDirectory, std::wstring* error) {
    std::error_code errorCode;
    const bool exists = fs::exists(path, errorCode);
    if (errorCode) {
        *error = L"failed to read path state";
        return false;
    }
    if (!exists) {
        *isDirectory = false;
        return true;
    }

    *isDirectory = fs::is_directory(path, errorCode);
    if (errorCode) {
        *error = L"failed to read path type";
        return false;
    }
    return true;
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

        bool realIsDirectory = false;
        std::wstring pathTypeError;
        if (!IsDirectoryPath(record.realPath, &realIsDirectory, &pathTypeError)) {
            return Fail(pathTypeError);
        }

        if (realExists && !realIsDirectory) {
            std::wstring lockError;
            if (!TryOpenExclusive(record.realPath, GENERIC_READ, &lockError)) {
                return Fail(lockError);
            }
        }

        if (record.state == ChangeState::kCreated ||
            record.state == ChangeState::kModified ||
            record.state == ChangeState::kRenamed) {
            bool shadowExists = false;
            unsigned long long shadowSize = 0;
            std::wstring shadowLastWriteTime;
            if (!TryGetFileInfo(record.shadowPath, &shadowExists, &shadowSize, &shadowLastWriteTime)) {
                return Fail(L"failed to read shadow file metadata during commit preflight");
            }
            if (!shadowExists) {
                return Fail(L"shadow file is missing during commit");
            }

            bool shadowIsDirectory = false;
            if (!IsDirectoryPath(record.shadowPath, &shadowIsDirectory, &pathTypeError)) {
                return Fail(pathTypeError);
            }
            if (!shadowIsDirectory) {
                std::wstring lockError;
                if (!TryOpenExclusive(record.shadowPath, GENERIC_READ, &lockError)) {
                    return Fail(lockError);
                }
            }
        }
        if (record.state == ChangeState::kRenamed) {
            if (record.targetPath.empty()) {
                return Fail(L"rename target path is missing during commit preflight");
            }
            bool targetExists = false;
            unsigned long long targetSize = 0;
            std::wstring targetLastWriteTime;
            if (!TryGetFileInfo(record.targetPath, &targetExists, &targetSize, &targetLastWriteTime)) {
                return Fail(L"failed to read rename target metadata during commit preflight");
            }
            if (targetExists) {
                return Fail(L"commit conflict: rename target already exists");
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
        return PersistChange(metadata_, rule, normalizedReal, normalizedShadow, ChangeState::kCreated);
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

OperationResult OverlayOperations::PrepareRenamedTargetPath(
    const OverlayRule& rule,
    const std::wstring& realPath,
    std::wstring* shadowPath) {
    if (shadowPath != nullptr) {
        shadowPath->clear();
    }

    const std::wstring normalizedReal = NormalizePath(realPath);
    std::vector<ChangeRecord> records;
    std::wstring error;
    if (!metadata_.ListChanges(rule.id, &records, &error)) {
        return Fail(error);
    }

    for (const ChangeRecord& record : records) {
        if (record.state != ChangeState::kRenamed || record.targetPath.empty()) {
            continue;
        }
        if (!IsSamePathOrDescendant(record.targetPath, normalizedReal)) {
            continue;
        }

        const std::wstring relative = RelativePathUnder(record.targetPath, normalizedReal);
        const std::wstring targetShadow = relative.empty()
            ? NormalizePath(record.shadowPath)
            : (fs::path(record.shadowPath) / relative).wstring();
        if (shadowPath != nullptr) {
            *shadowPath = targetShadow;
        }

        std::error_code errorCode;
        if (fs::exists(targetShadow, errorCode)) {
            return Ok();
        }
        if (errorCode) {
            return Fail(L"failed to read renamed target shadow state");
        }

        const std::wstring sourcePath = relative.empty()
            ? NormalizePath(record.realPath)
            : (fs::path(record.realPath) / relative).wstring();
        const OperationResult copyResult = CopyMissingPath(sourcePath, targetShadow);
        if (!copyResult.success) {
            return copyResult;
        }
        return Ok();
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
    if (fs::exists(*mapped, errorCode)) {
        fs::remove_all(*mapped, errorCode);
        if (errorCode) {
            return Fail(L"failed to remove existing shadow item for tombstone");
        }
    }

    return PersistChange(metadata_, rule, realPath, *mapped, ChangeState::kTombstone);
}

OperationResult OverlayOperations::RecordRename(
    const OverlayRule& rule,
    const std::wstring& sourceRealPath,
    const std::wstring& targetRealPath) {
    const auto sourceShadow = MapRealPathToShadowPath(rule, sourceRealPath);
    const auto targetShadow = MapRealPathToShadowPath(rule, targetRealPath);
    if (!sourceShadow.has_value() || !targetShadow.has_value()) {
        return Fail(L"rename source and target must be inside the same rule");
    }

    const std::wstring normalizedSource = NormalizePath(sourceRealPath);
    const std::wstring normalizedTarget = NormalizePath(targetRealPath);
    if (_wcsicmp(normalizedSource.c_str(), normalizedTarget.c_str()) == 0) {
        return Ok();
    }
    if (normalizedSource.size() >= 2 && normalizedTarget.size() >= 2 &&
        std::towlower(normalizedSource[0]) != std::towlower(normalizedTarget[0])) {
        return Fail(L"cross-volume rename is not supported");
    }

    std::vector<ChangeRecord> records;
    std::wstring error;
    if (!metadata_.ListChanges(rule.id, &records, &error)) {
        return Fail(error);
    }

    ChangeRecord sourceRecord;
    bool hasSourceRecord = false;
    for (const ChangeRecord& record : records) {
        const std::wstring recordPath = NormalizePath(record.realPath);
        if (_wcsicmp(recordPath.c_str(), normalizedSource.c_str()) == 0) {
            sourceRecord = record;
            hasSourceRecord = true;
        }
        if (_wcsicmp(recordPath.c_str(), normalizedTarget.c_str()) == 0 ||
            (!record.targetPath.empty() &&
             _wcsicmp(NormalizePath(record.targetPath).c_str(), normalizedTarget.c_str()) == 0)) {
            return Fail(L"rename target already has overlay metadata");
        }
    }
    if (hasSourceRecord &&
        (sourceRecord.state == ChangeState::kTombstone || sourceRecord.state == ChangeState::kDeleted)) {
        return Fail(L"tombstoned path cannot be renamed");
    }

    std::error_code errorCode;
    if (fs::exists(normalizedTarget, errorCode) || fs::exists(*targetShadow, errorCode)) {
        return Fail(L"rename target already exists");
    }

    std::wstring activeSourceShadow = *sourceShadow;
    if (!fs::exists(activeSourceShadow, errorCode)) {
        std::wstring preparedShadow;
        OperationResult prepareResult = PrepareCopyOnWrite(rule, normalizedSource, &preparedShadow);
        if (!prepareResult.success) {
            return prepareResult;
        }
        activeSourceShadow = preparedShadow;
    }
    if (!fs::exists(activeSourceShadow, errorCode)) {
        return Fail(L"rename source does not exist");
    }
    const bool sourceIsDirectory = fs::is_directory(activeSourceShadow, errorCode);
    if (errorCode) {
        return Fail(L"failed to read rename source type");
    }
    if (sourceIsDirectory && IsSamePathOrDescendant(normalizedSource, normalizedTarget)) {
        return Fail(L"directory cannot be renamed into itself");
    }
    errorCode.clear();
    if (sourceIsDirectory && fs::exists(normalizedSource, errorCode)) {
        const OperationResult copyTreeResult = CopyMissingDirectoryTree(normalizedSource, activeSourceShadow);
        if (!copyTreeResult.success) {
            return copyTreeResult;
        }
    }
    if (errorCode) {
        return Fail(L"failed to read rename source state");
    }

    fs::create_directories(fs::path(*targetShadow).parent_path(), errorCode);
    if (errorCode) {
        return Fail(L"failed to create rename target parent");
    }
    fs::rename(activeSourceShadow, *targetShadow, errorCode);
    if (errorCode) {
        return Fail(L"failed to move shadow file for rename");
    }

    if (hasSourceRecord && sourceRecord.state == ChangeState::kCreated) {
        if (sourceIsDirectory) {
            for (const ChangeRecord& record : records) {
                const std::wstring recordPath = NormalizePath(record.realPath);
                if (_wcsicmp(recordPath.c_str(), normalizedSource.c_str()) != 0 &&
                    IsSamePathOrDescendant(normalizedSource, recordPath)) {
                    if (!metadata_.DeleteChange(rule.id, recordPath, &error)) {
                        return Fail(error);
                    }
                }
            }
        }
        if (!metadata_.DeleteChange(rule.id, normalizedSource, &error)) {
            return Fail(error);
        }
        return PersistChange(metadata_, rule, normalizedTarget, *targetShadow, ChangeState::kCreated);
    }

    if (sourceIsDirectory) {
        for (const ChangeRecord& record : records) {
            const std::wstring recordPath = NormalizePath(record.realPath);
            if (_wcsicmp(recordPath.c_str(), normalizedSource.c_str()) != 0 &&
                IsSamePathOrDescendant(normalizedSource, recordPath)) {
                if (!metadata_.DeleteChange(rule.id, recordPath, &error)) {
                    return Fail(error);
                }
            }
        }
    }

    return PersistChange(
        metadata_,
        rule,
        normalizedSource,
        *targetShadow,
        ChangeState::kRenamed,
        normalizedTarget);
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
                const OperationResult backupResult = CopyPathWithParents(record.realPath, backupPath);
                if (!backupResult.success) {
                    return FailCommit(metadata_, rule, commitId, backupRoot, backupResult.message);
                }
            }
            fs::create_directories(realPath.parent_path(), errorCode);
            if (errorCode) {
                return FailCommit(metadata_, rule, commitId, backupRoot, L"failed to create real parent directory during commit");
            }
            errorCode.clear();
            if (fs::is_directory(shadowPath, errorCode)) {
                fs::copy(
                    shadowPath,
                    realPath,
                    fs::copy_options::recursive | fs::copy_options::overwrite_existing,
                    errorCode);
                if (errorCode) {
                    return FailCommit(metadata_, rule, commitId, backupRoot, L"failed to create real directory during commit");
                }
            } else {
                if (errorCode) {
                    return FailCommit(metadata_, rule, commitId, backupRoot, L"failed to read shadow path type during commit");
                }
                fs::copy_file(shadowPath, realPath, fs::copy_options::overwrite_existing, errorCode);
                if (errorCode) {
                    return FailCommit(metadata_, rule, commitId, backupRoot, L"failed to write shadow file to real path");
                }
            }
            continue;
        }

        if (record.state == ChangeState::kDeleted || record.state == ChangeState::kTombstone) {
            if (fs::exists(realPath, errorCode)) {
                const std::wstring backupPath = BackupPathFor(rule, commitId, record.realPath);
                const OperationResult backupResult = CopyPathWithParents(record.realPath, backupPath);
                if (!backupResult.success) {
                    return FailCommit(metadata_, rule, commitId, backupRoot, backupResult.message);
                }
                fs::remove_all(realPath, errorCode);
                if (errorCode) {
                    return FailCommit(metadata_, rule, commitId, backupRoot, L"failed to delete real path during commit");
                }
            }
        }

        if (record.state == ChangeState::kRenamed) {
            if (record.targetPath.empty()) {
                return FailCommit(metadata_, rule, commitId, backupRoot, L"rename target path is missing during commit");
            }
            const fs::path targetPath(record.targetPath);
            if (fs::exists(targetPath, errorCode)) {
                return FailCommit(metadata_, rule, commitId, backupRoot, L"rename target exists during commit");
            }
            if (fs::exists(realPath, errorCode)) {
                const std::wstring backupPath = BackupPathFor(rule, commitId, record.realPath);
                const OperationResult backupResult = CopyPathWithParents(record.realPath, backupPath);
                if (!backupResult.success) {
                    return FailCommit(metadata_, rule, commitId, backupRoot, backupResult.message);
                }
            }
            fs::create_directories(targetPath.parent_path(), errorCode);
            if (errorCode) {
                return FailCommit(metadata_, rule, commitId, backupRoot, L"failed to create rename target parent during commit");
            }
            if (fs::is_directory(shadowPath, errorCode)) {
                fs::copy(shadowPath, targetPath, fs::copy_options::recursive, errorCode);
                if (errorCode) {
                    return FailCommit(metadata_, rule, commitId, backupRoot, L"failed to write renamed directory during commit");
                }
            } else {
                if (errorCode) {
                    return FailCommit(metadata_, rule, commitId, backupRoot, L"failed to read renamed shadow path type during commit");
                }
                fs::copy_file(shadowPath, targetPath, fs::copy_options::none, errorCode);
                if (errorCode) {
                    return FailCommit(metadata_, rule, commitId, backupRoot, L"failed to write renamed file during commit");
                }
            }
            if (fs::exists(realPath, errorCode)) {
                fs::remove_all(realPath, errorCode);
                if (errorCode) {
                    return FailCommit(metadata_, rule, commitId, backupRoot, L"failed to remove rename source during commit");
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
    std::wstring cleanupRoot;
    if (fs::exists(driveRoot, errorCode)) {
        if (errorCode) {
            return Fail(L"failed to inspect shadow data");
        }

        cleanupRoot = DiscardCleanupRoot(rule);
        fs::rename(driveRoot, cleanupRoot, errorCode);
        if (errorCode) {
            return Fail(L"failed to isolate shadow data for background cleanup");
        }
    } else if (errorCode) {
        return Fail(L"failed to inspect shadow data");
    }

    if (!metadata_.DeleteChangesForRule(rule.id, &error)) {
        if (!cleanupRoot.empty()) {
            std::error_code rollbackError;
            fs::rename(cleanupRoot, driveRoot, rollbackError);
        }
        return Fail(error);
    }
    if (!cleanupRoot.empty()) {
        RemoveAllDetached(cleanupRoot);
    }
    return Ok();
}

}  // namespace pathoverlay
