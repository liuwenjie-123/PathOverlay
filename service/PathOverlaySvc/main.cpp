#include <windows.h>
#include <fltuser.h>
#include <RestartManager.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "metadata_store.h"
#include "overlay_operations.h"
#include "path_overlay_common.h"
#include "path_overlay_protocol.h"

namespace {

constexpr wchar_t kServiceName[] = L"PathOverlaySvc";
constexpr wchar_t kServiceDisplayName[] = L"PathOverlay Service";
constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\PathOverlaySvc";
constexpr wchar_t kDriverPortName[] = L"\\PathOverlayPort";

SERVICE_STATUS_HANDLE gStatusHandle = nullptr;
SERVICE_STATUS gServiceStatus = {};
HANDLE gStopEvent = nullptr;
HANDLE gDriverPort = INVALID_HANDLE_VALUE;
HANDLE gDriverMessageThread = nullptr;

constexpr long kNtStatusSuccess = 0x00000000L;
constexpr long kNtStatusUnsuccessful = static_cast<long>(0xC0000001L);
constexpr long kNtStatusObjectNameNotFound = static_cast<long>(0xC0000034L);

struct DriverMessage {
    FILTER_MESSAGE_HEADER header;
    PATHOVERLAY_SERVICE_REQUEST request;
};

struct DriverReply {
    FILTER_REPLY_HEADER header;
    PATHOVERLAY_SERVICE_RESPONSE response;
};

std::wstring ProgramDataRoot() {
    wchar_t buffer[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(L"ProgramData", buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return L"C:\\ProgramData\\PathOverlay";
    }
    return std::wstring(buffer, length) + L"\\PathOverlay";
}

std::wstring LogPath() {
    return ProgramDataRoot() + L"\\logs\\PathOverlaySvc.log";
}

std::wstring MetadataPath() {
    return ProgramDataRoot() + L"\\metadata.db";
}

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int required = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string output(static_cast<size_t>(required - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, output.data(), required, nullptr, nullptr);
    return output;
}

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const int required = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring output(static_cast<size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), output.data(), required);
    return output;
}

void WriteLog(const std::wstring& message) {
    const std::filesystem::path path(LogPath());
    std::error_code errorCode;
    std::filesystem::create_directories(path.parent_path(), errorCode);

    std::wofstream log(path, std::ios::app);
    if (log) {
        log << message << L"\n";
    }
}

void DisconnectDriverPort() {
    if (gDriverPort != INVALID_HANDLE_VALUE) {
        CloseHandle(gDriverPort);
        gDriverPort = INVALID_HANDLE_VALUE;
        WriteLog(L"driver: disconnected from communication port");
    }
}

void ConnectDriverPort() {
    DisconnectDriverPort();

    HANDLE port = INVALID_HANDLE_VALUE;
    const HRESULT result = FilterConnectCommunicationPort(
        kDriverPortName,
        0,
        nullptr,
        0,
        nullptr,
        &port);

    if (FAILED(result)) {
        std::wstringstream message;
        message << L"driver: failed to connect communication port, hr=0x"
                << std::hex << static_cast<unsigned long>(result);
        WriteLog(message.str());
        return;
    }

    gDriverPort = port;
    WriteLog(L"driver: connected to communication port");
}

bool IsDriverPortConnected() {
    return gDriverPort != INVALID_HANDLE_VALUE;
}

bool CopyProtocolPath(const std::wstring& source, wchar_t* destination, size_t destinationChars) {
    if (source.size() >= destinationChars) {
        return false;
    }
    std::wmemset(destination, 0, destinationChars);
    std::wmemcpy(destination, source.c_str(), source.size());
    return true;
}

std::wstring TrimTrailingSlashes(std::wstring value) {
    while (value.size() > 3 && value.back() == L'\\') {
        value.pop_back();
    }
    return value;
}

std::wstring NormalizeAbsoluteDosPath(const std::wstring& dosPath, bool expandLongPath) {
    if (dosPath.empty()) {
        return {};
    }

    std::wstring slashNormalized = dosPath;
    std::replace(slashNormalized.begin(), slashNormalized.end(), L'/', L'\\');

    DWORD required = GetFullPathNameW(slashNormalized.c_str(), 0, nullptr, nullptr);
    if (required == 0) {
        return {};
    }

    std::vector<wchar_t> buffer(required);
    DWORD length = GetFullPathNameW(slashNormalized.c_str(), required, buffer.data(), nullptr);
    if (length == 0 || length >= required) {
        return {};
    }

    std::wstring absolutePath = TrimTrailingSlashes(std::wstring(buffer.data(), length));
    if (expandLongPath) {
        absolutePath = pathoverlay::NormalizePath(absolutePath);
    }

    return absolutePath;
}

std::wstring DosPathToNtPathInternal(const std::wstring& dosPath, bool expandLongPath) {
    const std::wstring normalized = NormalizeAbsoluteDosPath(dosPath, expandLongPath);
    if (normalized.size() < 3 || normalized[1] != L':' || normalized[2] != L'\\') {
        return {};
    }

    const std::wstring drive = normalized.substr(0, 2);
    wchar_t deviceName[PATHOVERLAY_MAX_PATH_CHARS] = {};
    const DWORD length = QueryDosDeviceW(drive.c_str(), deviceName, PATHOVERLAY_MAX_PATH_CHARS);
    if (length == 0 || length >= PATHOVERLAY_MAX_PATH_CHARS) {
        return {};
    }

    return TrimTrailingSlashes(std::wstring(deviceName)) + normalized.substr(2);
}

std::wstring DosPathToNtPath(const std::wstring& dosPath) {
    return DosPathToNtPathInternal(dosPath, true);
}

std::wstring DosPathToNtPathPreservingAlias(const std::wstring& dosPath) {
    return DosPathToNtPathInternal(dosPath, false);
}

std::wstring GetShortDosPathIfAvailable(const std::wstring& dosPath) {
    const std::wstring normalized = pathoverlay::NormalizePath(dosPath);
    if (normalized.empty()) {
        return {};
    }

    DWORD required = GetShortPathNameW(normalized.c_str(), nullptr, 0);
    if (required == 0) {
        return {};
    }

    std::vector<wchar_t> buffer(required);
    DWORD length = GetShortPathNameW(normalized.c_str(), buffer.data(), required);
    if (length == 0 || length >= required) {
        return {};
    }

    const std::wstring shortPath = TrimTrailingSlashes(std::wstring(buffer.data(), length));
    if (_wcsicmp(shortPath.c_str(), normalized.c_str()) == 0) {
        return {};
    }

    return shortPath;
}

std::wstring NtPathToDosPath(const std::wstring& ntPath) {
    for (wchar_t driveLetter = L'A'; driveLetter <= L'Z'; ++driveLetter) {
        const std::wstring drive = std::wstring(1, driveLetter) + L":";
        wchar_t deviceName[PATHOVERLAY_MAX_PATH_CHARS] = {};
        const DWORD length = QueryDosDeviceW(drive.c_str(), deviceName, PATHOVERLAY_MAX_PATH_CHARS);
        if (length == 0 || length >= PATHOVERLAY_MAX_PATH_CHARS) {
            continue;
        }

        const std::wstring device = TrimTrailingSlashes(std::wstring(deviceName));
        if (_wcsnicmp(ntPath.c_str(), device.c_str(), device.size()) == 0 &&
            (ntPath.size() == device.size() || ntPath[device.size()] == L'\\')) {
            return drive + ntPath.substr(device.size());
        }
    }

    return {};
}

bool IsHiddenByRenameSource(const pathoverlay::ChangeRecord& record, const std::wstring& realPath);

bool IsTombstoned(
    pathoverlay::MetadataStore& metadata,
    const std::wstring& ruleId,
    const std::wstring& realPath,
    std::wstring* error) {
    std::vector<pathoverlay::ChangeRecord> records;
    if (!metadata.ListChanges(ruleId, &records, error)) {
        return false;
    }

    const std::wstring normalizedReal = pathoverlay::NormalizePath(realPath);
    for (const auto& record : records) {
        if ((_wcsicmp(pathoverlay::NormalizePath(record.realPath).c_str(), normalizedReal.c_str()) == 0 &&
             (record.state == pathoverlay::ChangeState::kTombstone ||
              record.state == pathoverlay::ChangeState::kDeleted)) ||
            IsHiddenByRenameSource(record, realPath)) {
            return true;
        }
    }

    return false;
}

bool IsSamePathOrDescendant(const std::wstring& parent, const std::wstring& child) {
    std::wstring normalizedParent = pathoverlay::NormalizePath(parent);
    std::wstring normalizedChild = pathoverlay::NormalizePath(child);
    for (wchar_t& ch : normalizedParent) {
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
    for (wchar_t& ch : normalizedChild) {
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
    if (normalizedParent == normalizedChild) {
        return true;
    }
    if (!normalizedParent.empty() && normalizedParent.back() != L'\\') {
        normalizedParent += L'\\';
    }
    return normalizedChild.rfind(normalizedParent, 0) == 0;
}

bool IsHiddenByRenameSource(const pathoverlay::ChangeRecord& record, const std::wstring& realPath) {
    if (record.state != pathoverlay::ChangeState::kRenamed) {
        return false;
    }
    if (_wcsicmp(pathoverlay::NormalizePath(record.realPath).c_str(), pathoverlay::NormalizePath(realPath).c_str()) == 0) {
        return true;
    }

    std::error_code errorCode;
    if (!std::filesystem::is_directory(record.shadowPath, errorCode) || errorCode) {
        return false;
    }
    return IsSamePathOrDescendant(record.realPath, realPath);
}

PATHOVERLAY_SERVICE_RESPONSE ProcessDriverRequest(const PATHOVERLAY_SERVICE_REQUEST& request) {
    PATHOVERLAY_SERVICE_RESPONSE response = {};
    response.Status = kNtStatusSuccess;
    response.PathState = PathOverlayPathStateNormal;

    if (request.Version != PATHOVERLAY_PROTOCOL_VERSION) {
        response.Status = kNtStatusUnsuccessful;
        return response;
    }

    const std::wstring realPath = NtPathToDosPath(request.RealNtPath);
    if (realPath.empty()) {
        response.Status = kNtStatusUnsuccessful;
        return response;
    }
    std::wstring targetPath;
    if (request.Command == PathOverlayServiceCommandRecordRename) {
        targetPath = NtPathToDosPath(request.TargetNtPath);
        if (targetPath.empty()) {
            response.Status = kNtStatusUnsuccessful;
            return response;
        }
    }

    pathoverlay::MetadataStore metadata;
    std::wstring error;
    if (!metadata.Open(MetadataPath(), &error) || !metadata.Initialize(&error)) {
        WriteLog(L"driver request: metadata unavailable: " + error);
        response.Status = kNtStatusUnsuccessful;
        return response;
    }

    const std::wstring ruleId = request.RuleId;
    if (ruleId.empty()) {
        response.Status = kNtStatusUnsuccessful;
        return response;
    }

    pathoverlay::OverlayRule rule;
    if (!metadata.GetRule(ruleId, &rule, &error) || !rule.enabled) {
        response.Status = kNtStatusObjectNameNotFound;
        return response;
    }

    pathoverlay::OverlayOperations operations(metadata);
    if (request.Command == PathOverlayServiceCommandQueryPath) {
        response.PathState = IsTombstoned(metadata, rule.id, realPath, &error)
            ? PathOverlayPathStateTombstone
            : PathOverlayPathStateNormal;
        std::wstring renamedShadowPath;
        pathoverlay::OperationResult renamedTargetResult{true, L""};
        if (response.PathState == PathOverlayPathStateNormal) {
            renamedTargetResult = operations.PrepareRenamedTargetPath(rule, realPath, &renamedShadowPath);
            if (!renamedTargetResult.success) {
                WriteLog(
                    std::wstring(L"driver request: query renamed-target prepare failed rule=") + rule.id +
                    L" path=" + realPath + L" error=" + renamedTargetResult.message);
                response.Status = kNtStatusUnsuccessful;
                return response;
            }
            if (!renamedShadowPath.empty()) {
                const std::wstring renamedShadowNtPath = DosPathToNtPath(renamedShadowPath);
                if (renamedShadowNtPath.empty() ||
                    !CopyProtocolPath(renamedShadowNtPath, response.ShadowNtPath, PATHOVERLAY_MAX_PATH_CHARS)) {
                    WriteLog(
                        std::wstring(L"driver request: query renamed-target shadow path conversion failed rule=") +
                        rule.id + L" path=" + realPath + L" shadow=" + renamedShadowPath);
                    response.Status = kNtStatusUnsuccessful;
                    return response;
                }
            }
        }
        WriteLog(
            std::wstring(L"driver request: query rule=") + rule.id + L" path=" + realPath +
            L" state=" +
            (response.PathState == PathOverlayPathStateTombstone ? L"tombstone" : L"normal") +
            (renamedShadowPath.empty() ? L"" : L" renamed-shadow=" + renamedShadowPath));
        return response;
    }

    pathoverlay::OperationResult result;
    if (request.Command == PathOverlayServiceCommandPrepareCopyOnWrite) {
        std::wstring shadowPath;
        result = operations.PrepareCopyOnWrite(rule, realPath, &shadowPath);
        WriteLog(
            std::wstring(L"driver request: copy-on-write rule=") + rule.id + L" path=" + realPath +
            (result.success ? L" ok" : L" failed: " + result.message));
    } else if (request.Command == PathOverlayServiceCommandPrepareDirectoryView) {
        std::wstring shadowPath;
        result = operations.PrepareDirectoryView(rule, realPath, &shadowPath);
        WriteLog(
            std::wstring(L"driver request: directory-view rule=") + rule.id + L" path=" + realPath +
            (result.success ? L" ok" : L" failed: " + result.message));
    } else if (request.Command == PathOverlayServiceCommandRecordDelete) {
        result = operations.RecordDelete(rule, realPath);
        WriteLog(
            std::wstring(L"driver request: record-delete rule=") + rule.id + L" path=" + realPath +
            (result.success ? L" ok" : L" failed: " + result.message));
    } else if (request.Command == PathOverlayServiceCommandRecordRename) {
        result = operations.RecordRename(rule, realPath, targetPath);
        WriteLog(
            std::wstring(L"driver request: record-rename rule=") + rule.id + L" source=" + realPath +
            L" target=" + targetPath +
            (result.success ? L" ok" : L" failed: " + result.message));
    } else {
        response.Status = kNtStatusUnsuccessful;
        return response;
    }

    if (!result.success) {
        WriteLog(L"driver request failed: " + result.message);
        response.Status = kNtStatusUnsuccessful;
    }

    return response;
}

bool SendDriverRuleMessage(const PATHOVERLAY_DRIVER_RULE_MESSAGE& message, std::wstring* error) {
    if (!IsDriverPortConnected()) {
        if (error != nullptr) {
            *error = L"driver not connected";
        }
        return false;
    }

    PATHOVERLAY_DRIVER_RESPONSE response = {};
    DWORD bytesReturned = 0;
    const HRESULT result = FilterSendMessage(
        gDriverPort,
        const_cast<PATHOVERLAY_DRIVER_RULE_MESSAGE*>(&message),
        sizeof(message),
        &response,
        sizeof(response),
        &bytesReturned);

    if (FAILED(result)) {
        if (error != nullptr) {
            std::wstringstream stream;
            stream << L"FilterSendMessage failed, hr=0x" << std::hex << static_cast<unsigned long>(result);
            *error = stream.str();
        }
        return false;
    }

    if (bytesReturned >= sizeof(response) && response.Status < 0) {
        if (error != nullptr) {
            std::wstringstream stream;
            stream << L"driver rejected rule, status=0x" << std::hex << static_cast<unsigned long>(response.Status);
            *error = stream.str();
        }
        return false;
    }

    return true;
}

bool ClearDriverRule(std::wstring* error) {
    PATHOVERLAY_DRIVER_RULE_MESSAGE message = {};
    message.Version = PATHOVERLAY_PROTOCOL_VERSION;
    message.Command = PathOverlayDriverCommandClearRule;
    message.ServiceProcessId = GetCurrentProcessId();
    return SendDriverRuleMessage(message, error);
}

std::wstring MakeCommitId() {
    SYSTEMTIME time = {};
    GetSystemTime(&time);

    std::wstringstream stream;
    stream << L"cli-commit-"
           << time.wYear
           << (time.wMonth < 10 ? L"0" : L"") << time.wMonth
           << (time.wDay < 10 ? L"0" : L"") << time.wDay
           << L"-"
           << (time.wHour < 10 ? L"0" : L"") << time.wHour
           << (time.wMinute < 10 ? L"0" : L"") << time.wMinute
           << (time.wSecond < 10 ? L"0" : L"") << time.wSecond
           << L"-" << GetCurrentProcessId()
           << L"-" << GetTickCount64();
    return stream.str();
}

std::wstring MakeTimestamp() {
    SYSTEMTIME time = {};
    GetSystemTime(&time);

    std::wstringstream stream;
    stream << time.wYear
           << L"-" << (time.wMonth < 10 ? L"0" : L"") << time.wMonth
           << L"-" << (time.wDay < 10 ? L"0" : L"") << time.wDay
           << L"T" << (time.wHour < 10 ? L"0" : L"") << time.wHour
           << L":" << (time.wMinute < 10 ? L"0" : L"") << time.wMinute
           << L":" << (time.wSecond < 10 ? L"0" : L"") << time.wSecond
           << L"Z";
    return stream.str();
}

std::wstring MakeOperationId(const std::wstring& action) {
    SYSTEMTIME time = {};
    GetSystemTime(&time);

    std::wstringstream stream;
    stream << L"cli-" << action << L"-"
           << time.wYear
           << (time.wMonth < 10 ? L"0" : L"") << time.wMonth
           << (time.wDay < 10 ? L"0" : L"") << time.wDay
           << L"-"
           << (time.wHour < 10 ? L"0" : L"") << time.wHour
           << (time.wMinute < 10 ? L"0" : L"") << time.wMinute
           << (time.wSecond < 10 ? L"0" : L"") << time.wSecond
           << L"-" << GetCurrentProcessId()
           << L"-" << GetTickCount64();
    return stream.str();
}

bool StartOperationRecord(
    pathoverlay::MetadataStore& metadata,
    const std::wstring& operationId,
    const pathoverlay::OverlayRule& rule,
    const std::wstring& action,
    const std::wstring& backupRoot,
    std::wstring* error) {
    const std::wstring now = MakeTimestamp();
    pathoverlay::OperationRecord record;
    record.id = operationId;
    record.ruleId = rule.id;
    record.action = action;
    record.status = L"running";
    record.phase = L"created";
    record.startedAt = now;
    record.updatedAt = now;
    record.finishedAt = L"";
    record.backupRoot = backupRoot;
    record.error = L"";
    record.ruleWasEnabled = rule.enabled;
    return metadata.AddOperationRecord(record, error);
}

bool UpdateOperationRecord(
    pathoverlay::MetadataStore& metadata,
    const std::wstring& operationId,
    const std::wstring& status,
    const std::wstring& phase,
    const std::wstring& backupRoot,
    const std::wstring& operationError,
    std::wstring* error) {
    const std::wstring now = MakeTimestamp();
    const std::wstring finished = (status == L"running") ? L"" : now;
    std::wstring ignoredError;
    return metadata.UpdateOperationRecord(
        operationId,
        status,
        phase,
        now,
        finished,
        backupRoot,
        operationError,
        error != nullptr ? error : &ignoredError);
}

struct ParsedRuleAdd {
    std::wstring source;
    std::wstring store;
};

struct ParsedRuleOperation {
    bool valid = false;
    std::wstring ruleId;
    bool confirmClose = false;
};

struct OccupyingProcess {
    DWORD processId = 0;
    std::wstring applicationName;
    RM_APP_TYPE applicationType = RmUnknownApp;
    bool protectedProcess = false;
};

ParsedRuleAdd ParseRuleAddPayload(const std::wstring& payload) {
    constexpr wchar_t kStoreSwitch[] = L" --store ";
    const size_t storeSwitch = payload.rfind(kStoreSwitch);
    if (storeSwitch == std::wstring::npos) {
        return ParsedRuleAdd{payload, L""};
    }

    ParsedRuleAdd parsed;
    parsed.source = payload.substr(0, storeSwitch);
    parsed.store = payload.substr(storeSwitch + wcslen(kStoreSwitch));
    return parsed;
}

bool TryParseRuleIdCommand(
    const std::wstring& request,
    const std::wstring& command,
    std::wstring* ruleId) {
    const std::wstring prefix = command + L" --rule ";
    if (request.rfind(prefix, 0) != 0) {
        return false;
    }

    *ruleId = request.substr(prefix.size());
    return !ruleId->empty();
}

std::vector<std::wstring> SplitWhitespace(const std::wstring& value) {
    std::vector<std::wstring> tokens;
    size_t offset = 0;
    while (offset < value.size()) {
        while (offset < value.size() && iswspace(value[offset])) {
            ++offset;
        }
        if (offset >= value.size()) {
            break;
        }
        const size_t start = offset;
        while (offset < value.size() && !iswspace(value[offset])) {
            ++offset;
        }
        tokens.push_back(value.substr(start, offset - start));
    }
    return tokens;
}

ParsedRuleOperation ParseRuleOperation(const std::wstring& request, const std::wstring& command) {
    ParsedRuleOperation parsed;
    const std::vector<std::wstring> tokens = SplitWhitespace(request);
    if (tokens.empty() || tokens[0] != command) {
        return parsed;
    }

    for (size_t index = 1; index < tokens.size(); ++index) {
        if (tokens[index] == L"--rule") {
            if (index + 1 >= tokens.size() || tokens[index + 1].empty()) {
                return ParsedRuleOperation{};
            }
            parsed.ruleId = tokens[index + 1];
            ++index;
            continue;
        }
        if (tokens[index] == L"--confirm-close") {
            parsed.confirmClose = true;
            continue;
        }
        return ParsedRuleOperation{};
    }

    parsed.valid = !parsed.ruleId.empty();
    return parsed;
}

bool IsExistingRegularFile(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

void AddUniquePath(std::vector<std::wstring>* paths, const std::wstring& path) {
    if (path.empty() || !IsExistingRegularFile(path)) {
        return;
    }
    const std::wstring normalized = pathoverlay::NormalizePath(path);
    for (const auto& existing : *paths) {
        if (_wcsicmp(existing.c_str(), normalized.c_str()) == 0) {
            return;
        }
    }
    paths->push_back(normalized);
}

std::vector<std::wstring> CollectOccupancyResourcePaths(
    const std::vector<pathoverlay::ChangeRecord>& records,
    bool isCommit) {
    std::vector<std::wstring> paths;
    for (const auto& record : records) {
        if (isCommit) {
            AddUniquePath(&paths, record.realPath);
            AddUniquePath(&paths, record.shadowPath);
            AddUniquePath(&paths, record.targetPath);
        } else {
            AddUniquePath(&paths, record.shadowPath);
        }
    }
    return paths;
}

std::wstring ApplicationTypeText(RM_APP_TYPE type) {
    switch (type) {
    case RmMainWindow:
        return L"main-window";
    case RmOtherWindow:
        return L"other-window";
    case RmService:
        return L"service";
    case RmExplorer:
        return L"explorer";
    case RmConsole:
        return L"console";
    case RmCritical:
        return L"critical";
    default:
        return L"unknown";
    }
}

bool IsProtectedOccupyingProcess(const RM_PROCESS_INFO& process) {
    const DWORD processId = process.Process.dwProcessId;
    if (processId <= 4 || processId == GetCurrentProcessId()) {
        return true;
    }
    if (process.ApplicationType == RmCritical || process.ApplicationType == RmService) {
        return true;
    }

    const std::wstring name = process.strAppName;
    return _wcsicmp(name.c_str(), L"System") == 0 ||
           _wcsicmp(name.c_str(), L"Registry") == 0 ||
           _wcsicmp(name.c_str(), L"smss.exe") == 0 ||
           _wcsicmp(name.c_str(), L"csrss.exe") == 0 ||
           _wcsicmp(name.c_str(), L"wininit.exe") == 0 ||
           _wcsicmp(name.c_str(), L"services.exe") == 0 ||
           _wcsicmp(name.c_str(), L"lsass.exe") == 0 ||
           _wcsicmp(name.c_str(), L"winlogon.exe") == 0 ||
           _wcsicmp(name.c_str(), L"PathOverlaySvc.exe") == 0;
}

bool QueryOccupyingProcesses(
    const std::vector<std::wstring>& paths,
    std::vector<OccupyingProcess>* processes,
    std::wstring* error) {
    processes->clear();
    if (paths.empty()) {
        return true;
    }

    DWORD sessionHandle = 0;
    wchar_t sessionKey[CCH_RM_SESSION_KEY + 1] = {};
    DWORD result = RmStartSession(&sessionHandle, 0, sessionKey);
    if (result != ERROR_SUCCESS) {
        std::wstringstream stream;
        stream << L"failed to start Restart Manager session: " << result;
        *error = stream.str();
        return false;
    }

    std::vector<LPCWSTR> pathPointers;
    pathPointers.reserve(paths.size());
    for (const auto& path : paths) {
        pathPointers.push_back(path.c_str());
    }

    result = RmRegisterResources(
        sessionHandle,
        static_cast<UINT>(pathPointers.size()),
        pathPointers.data(),
        0,
        nullptr,
        0,
        nullptr);
    if (result != ERROR_SUCCESS) {
        RmEndSession(sessionHandle);
        std::wstringstream stream;
        stream << L"failed to register resources for occupancy detection: " << result;
        *error = stream.str();
        return false;
    }

    UINT processInfoNeeded = 0;
    UINT processInfoCount = 0;
    DWORD rebootReasons = 0;
    result = RmGetList(sessionHandle, &processInfoNeeded, &processInfoCount, nullptr, &rebootReasons);
    if (result == ERROR_MORE_DATA && processInfoNeeded > 0) {
        std::vector<RM_PROCESS_INFO> processInfo(processInfoNeeded);
        processInfoCount = processInfoNeeded;
        result = RmGetList(
            sessionHandle,
            &processInfoNeeded,
            &processInfoCount,
            processInfo.data(),
            &rebootReasons);
        if (result == ERROR_SUCCESS) {
            for (UINT index = 0; index < processInfoCount; ++index) {
                OccupyingProcess process;
                process.processId = processInfo[index].Process.dwProcessId;
                process.applicationName = processInfo[index].strAppName;
                process.applicationType = processInfo[index].ApplicationType;
                process.protectedProcess = IsProtectedOccupyingProcess(processInfo[index]);
                processes->push_back(process);
            }
        }
    }

    RmEndSession(sessionHandle);
    if (result != ERROR_SUCCESS && result != ERROR_MORE_DATA) {
        std::wstringstream stream;
        stream << L"failed to query occupying processes: " << result;
        *error = stream.str();
        return false;
    }

    return true;
}

std::wstring FormatOccupyingProcesses(const std::vector<OccupyingProcess>& processes) {
    std::wstringstream output;
    output << L"occupied files detected";
    for (const auto& process : processes) {
        output << L"\n pid=" << process.processId
               << L" name=" << (process.applicationName.empty() ? L"<unknown>" : process.applicationName)
               << L" type=" << ApplicationTypeText(process.applicationType);
        if (process.protectedProcess) {
            output << L" protected=true";
        }
    }
    return output.str();
}

bool TerminateOccupyingUserProcesses(
    const std::vector<OccupyingProcess>& processes,
    std::wstring* error) {
    for (const auto& process : processes) {
        if (process.protectedProcess) {
            *error = L"refusing to close protected process: pid=" + std::to_wstring(process.processId);
            return false;
        }
    }

    for (const auto& process : processes) {

        HANDLE handle = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, process.processId);
        if (handle == nullptr) {
            std::wstringstream stream;
            stream << L"failed to open process for close: pid=" << process.processId
                   << L" error=" << GetLastError();
            *error = stream.str();
            return false;
        }

        if (!TerminateProcess(handle, 1)) {
            const DWORD lastError = GetLastError();
            CloseHandle(handle);
            std::wstringstream stream;
            stream << L"failed to close process: pid=" << process.processId
                   << L" error=" << lastError;
            *error = stream.str();
            return false;
        }
        WaitForSingleObject(handle, 3000);
        CloseHandle(handle);
    }

    return true;
}

bool EnsureNoOccupyingProcesses(
    const std::vector<pathoverlay::ChangeRecord>& records,
    bool isCommit,
    bool confirmClose,
    std::wstring* responseError) {
    const std::vector<std::wstring> paths = CollectOccupancyResourcePaths(records, isCommit);
    std::vector<OccupyingProcess> processes;
    std::wstring error;
    if (!QueryOccupyingProcesses(paths, &processes, &error)) {
        *responseError = error;
        return false;
    }
    if (processes.empty()) {
        return true;
    }
    if (!confirmClose) {
        *responseError = FormatOccupyingProcesses(processes) + L"\nuse --confirm-close to close non-critical user processes";
        return false;
    }

    if (!TerminateOccupyingUserProcesses(processes, &error)) {
        *responseError = error + L"\n" + FormatOccupyingProcesses(processes);
        return false;
    }

    Sleep(250);
    processes.clear();
    if (!QueryOccupyingProcesses(paths, &processes, &error)) {
        *responseError = error;
        return false;
    }
    if (!processes.empty()) {
        *responseError = L"files are still occupied after close attempt\n" + FormatOccupyingProcesses(processes);
        return false;
    }

    return true;
}

std::wstring AppendDriverSyncWarning(std::wstring response, const std::wstring& syncError) {
    if (syncError.empty()) {
        return response;
    }
    return response + L" warning=driver sync failed: " + syncError;
}

bool PushDriverRule(const pathoverlay::OverlayRule& rule, std::wstring* error) {
    if (!rule.enabled) {
        return true;
    }

    const auto shadowSourcePath = pathoverlay::MapRealPathToShadowPath(rule, rule.source);
    if (!shadowSourcePath.has_value()) {
        if (error != nullptr) {
            *error = L"failed to map source path to shadow root";
        }
        return false;
    }

    const std::wstring sourceNtPath = DosPathToNtPath(rule.source);
    const std::wstring shadowSourceNtPath = DosPathToNtPath(*shadowSourcePath);
    const std::wstring sourceAliasDosPath = GetShortDosPathIfAvailable(rule.source);
    std::wstring sourceAliasNtPath;
    if (!sourceAliasDosPath.empty()) {
        sourceAliasNtPath = DosPathToNtPathPreservingAlias(sourceAliasDosPath);
        if (_wcsicmp(sourceAliasNtPath.c_str(), sourceNtPath.c_str()) == 0) {
            sourceAliasNtPath.clear();
        }
    }
    if (sourceNtPath.empty() || shadowSourceNtPath.empty()) {
        if (error != nullptr) {
            *error = L"failed to convert rule paths to NT device paths";
        }
        return false;
    }

    PATHOVERLAY_DRIVER_RULE_MESSAGE message = {};
    message.Version = PATHOVERLAY_PROTOCOL_VERSION;
    message.Command = PathOverlayDriverCommandSetRule;
    message.Enabled = 1;
    message.ServiceProcessId = GetCurrentProcessId();
    if (!CopyProtocolPath(rule.id, message.RuleId, PATHOVERLAY_MAX_RULE_ID_CHARS) ||
        !CopyProtocolPath(sourceNtPath, message.SourceNtPath, PATHOVERLAY_MAX_PATH_CHARS) ||
        (!sourceAliasNtPath.empty() &&
         !CopyProtocolPath(sourceAliasNtPath, message.SourceAliasNtPath, PATHOVERLAY_MAX_PATH_CHARS)) ||
        !CopyProtocolPath(shadowSourceNtPath, message.StoreNtPath, PATHOVERLAY_MAX_PATH_CHARS)) {
        if (error != nullptr) {
            *error = L"rule path is too long for driver protocol";
        }
        return false;
    }

    return SendDriverRuleMessage(message, error);
}

bool PushDriverRules(const std::vector<pathoverlay::OverlayRule>& rules, std::wstring* error) {
    if (!ClearDriverRule(error)) {
        return false;
    }

    std::vector<pathoverlay::OverlayRule> enabledRules;
    enabledRules.reserve(rules.size());
    for (const auto& rule : rules) {
        if (rule.enabled) {
            enabledRules.push_back(rule);
        }
    }

    if (enabledRules.size() > PATHOVERLAY_MAX_DRIVER_RULES) {
        if (error != nullptr) {
            *error = L"too many enabled rules for driver protocol";
        }
        ClearDriverRule(nullptr);
        return false;
    }

    std::vector<pathoverlay::OverlayRule> validatedRules;
    validatedRules.reserve(enabledRules.size());
    for (const auto& rule : enabledRules) {
        const pathoverlay::RuleValidationResult validation =
            pathoverlay::ValidateOverlayRuleSet(validatedRules, rule);
        if (!validation.ok()) {
            if (error != nullptr) {
                *error = L"enabled rule set is invalid: " + validation.message;
            }
            ClearDriverRule(nullptr);
            return false;
        }
        validatedRules.push_back(rule);
    }

    for (const auto& rule : enabledRules) {
        if (!PushDriverRule(rule, error)) {
            ClearDriverRule(nullptr);
            return false;
        }
    }

    return true;
}

DWORD WINAPI DriverMessageThread(LPVOID parameter) {
    HANDLE stopEvent = static_cast<HANDLE>(parameter);

    while (WaitForSingleObject(stopEvent, 0) == WAIT_TIMEOUT) {
        if (!IsDriverPortConnected()) {
            Sleep(250);
            continue;
        }

        DriverMessage message = {};
        const HRESULT getResult = FilterGetMessage(
            gDriverPort,
            &message.header,
            sizeof(message),
            nullptr);
        if (FAILED(getResult)) {
            if (WaitForSingleObject(stopEvent, 0) == WAIT_TIMEOUT) {
                Sleep(250);
            }
            continue;
        }

        DriverReply reply = {};
        reply.header.Status = 0;
        reply.header.MessageId = message.header.MessageId;
        reply.response = ProcessDriverRequest(message.request);

        const HRESULT replyResult = FilterReplyMessage(gDriverPort, &reply.header, sizeof(reply));
        if (FAILED(replyResult)) {
            WriteLog(L"driver request: FilterReplyMessage failed");
        }
    }

    return 0;
}

void SyncDriverRuleCache(pathoverlay::MetadataStore& metadata) {
    if (!IsDriverPortConnected()) {
        return;
    }

    std::wstring error;
    std::vector<pathoverlay::OverlayRule> rules;
    if (!metadata.ListRules(&rules, &error)) {
        if (!ClearDriverRule(&error)) {
            WriteLog(L"driver: failed to clear after rule query error: " + error);
        }
        return;
    }

    if (!PushDriverRules(rules, &error)) {
        WriteLog(L"driver: failed to sync rules: " + error);
        return;
    }

    WriteLog(L"driver: rules synchronized");
}

void SetServiceState(DWORD state, DWORD win32ExitCode = NO_ERROR, DWORD waitHint = 0) {
    gServiceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    gServiceStatus.dwCurrentState = state;
    gServiceStatus.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
    gServiceStatus.dwWin32ExitCode = win32ExitCode;
    gServiceStatus.dwWaitHint = waitHint;

    if (gStatusHandle != nullptr) {
        SetServiceStatus(gStatusHandle, &gServiceStatus);
    }
}

bool InitializeRuntime() {
    std::error_code errorCode;
    std::filesystem::create_directories(ProgramDataRoot(), errorCode);
    if (errorCode) {
        WriteLog(L"fatal: failed to create ProgramData root");
        return false;
    }

    pathoverlay::MetadataStore metadata;
    std::wstring error;
    if (!metadata.Open(MetadataPath(), &error)) {
        WriteLog(L"fatal: failed to open metadata database: " + error);
        return false;
    }
    if (!metadata.Initialize(&error)) {
        WriteLog(L"fatal: failed to initialize metadata database: " + error);
        return false;
    }

    std::vector<pathoverlay::OperationRecord> recoveredOperations;
    if (!metadata.RecoverInterruptedOperations(&recoveredOperations, &error)) {
        WriteLog(L"fatal: failed to recover interrupted operations: " + error);
        return false;
    }
    for (const auto& operation : recoveredOperations) {
        WriteLog(
            L"recovery: operation=" + operation.id +
            L" action=" + operation.action +
            L" rule=" + operation.ruleId +
            L" status=" + operation.status +
            L" phase=" + operation.phase +
            L" error=" + operation.error);
    }

    WriteLog(L"service runtime initialized");
    ConnectDriverPort();
    SyncDriverRuleCache(metadata);
    return true;
}

std::wstring ProcessRequest(const std::wstring& request) {
    pathoverlay::MetadataStore metadata;
    std::wstring error;
    if (!metadata.Open(MetadataPath(), &error) || !metadata.Initialize(&error)) {
        return L"ERROR metadata: " + error;
    }

    if (request == L"ping") {
        return L"OK pong";
    }

    if (request == L"driver status") {
        return IsDriverPortConnected() ? L"OK driver connected" : L"ERROR driver not connected";
    }

    if (request.rfind(L"rule add ", 0) == 0) {
        const ParsedRuleAdd parsed = ParseRuleAddPayload(request.substr(9));
        pathoverlay::OverlayRule rule;
        rule.id = pathoverlay::GenerateRuleId();
        rule.name = rule.id;
        rule.enabled = true;
        rule.source = parsed.source;
        rule.store = parsed.store.empty() ? pathoverlay::DefaultStoreRootForRule(rule.id) : parsed.store;

        std::vector<pathoverlay::OverlayRule> existingRules;
        if (!metadata.ListRules(&existingRules, &error)) {
            return L"ERROR " + error;
        }

        const pathoverlay::RuleValidationResult validation = pathoverlay::ValidateOverlayRuleSet(existingRules, rule);
        if (!validation.ok()) {
            return L"ERROR " + validation.message;
        }

        if (!metadata.UpsertRule(rule, &error)) {
            return L"ERROR " + error;
        }
        std::wstring syncError;
        std::vector<pathoverlay::OverlayRule> rules;
        if (!metadata.ListRules(&rules, &syncError) || !PushDriverRules(rules, &syncError)) {
            WriteLog(L"driver: failed to sync added rule: " + syncError);
        }
        return AppendDriverSyncWarning(
            L"OK rule added: " + rule.id +
                L" source=" + pathoverlay::NormalizePath(rule.source) +
                L" store=" + pathoverlay::NormalizePath(rule.store),
            syncError);
    }

    if (request == L"rule enable" || request == L"rule disable") {
        return L"ERROR rule id is required: use --rule <id>";
    }

    std::wstring ruleId;
    if (TryParseRuleIdCommand(request, L"rule enable", &ruleId) ||
        TryParseRuleIdCommand(request, L"rule disable", &ruleId)) {
        pathoverlay::OverlayRule rule;
        if (!metadata.GetRule(ruleId, &rule, &error)) {
            return L"ERROR rule not found: " + ruleId;
        }

        rule.enabled = request.rfind(L"rule enable ", 0) == 0;
        if (!metadata.SetRuleEnabled(rule.id, rule.enabled, &error)) {
            return L"ERROR " + error;
        }
        std::wstring syncError;
        std::vector<pathoverlay::OverlayRule> rules;
        if (!metadata.ListRules(&rules, &syncError) || !PushDriverRules(rules, &syncError)) {
            WriteLog(L"driver: failed to sync updated rule: " + syncError);
        }
        return AppendDriverSyncWarning(
            rule.enabled ? L"OK rule enabled" : L"OK rule disabled",
            syncError);
    }

    if (request == L"rule delete" || request == L"rule del") {
        return L"ERROR rule id is required: use --rule <id>";
    }

    if (TryParseRuleIdCommand(request, L"rule delete", &ruleId) ||
        TryParseRuleIdCommand(request, L"rule del", &ruleId)) {
        pathoverlay::OverlayRule rule;
        if (!metadata.GetRule(ruleId, &rule, &error)) {
            return L"ERROR rule not found: " + ruleId;
        }

        std::vector<pathoverlay::ChangeRecord> records;
        if (!metadata.ListChanges(rule.id, &records, &error)) {
            return L"ERROR " + error;
        }
        if (!records.empty()) {
            return L"ERROR rule has pending changes: discard or commit before deleting rule=" + rule.id;
        }

        if (!metadata.DeleteRule(rule.id, &error)) {
            return L"ERROR " + error;
        }

        std::wstring syncError;
        std::vector<pathoverlay::OverlayRule> rules;
        if (!metadata.ListRules(&rules, &syncError) || !PushDriverRules(rules, &syncError)) {
            WriteLog(L"driver: failed to sync deleted rule: " + syncError);
        }
        return AppendDriverSyncWarning(L"OK rule deleted: " + rule.id, syncError);
    }

    if (request == L"rule show") {
        std::vector<pathoverlay::OverlayRule> rules;
        if (!metadata.ListRules(&rules, &error)) {
            return L"ERROR " + error;
        }
        if (rules.empty()) {
            return L"OK no rules";
        }

        std::wstringstream output;
        output << L"OK";
        for (const auto& rule : rules) {
            output << L"\n" << rule.id << L" enabled=" << (rule.enabled ? L"true" : L"false")
                   << L" source=" << rule.source << L" store=" << rule.store;
        }
        return output.str();
    }

    if (request.rfind(L"debug service-write ", 0) == 0) {
        const std::wstring payload = request.substr(20);
        const size_t separator = payload.find(L' ');
        if (separator == std::wstring::npos) {
            return L"ERROR debug service-write requires path and content";
        }

        const std::wstring path = payload.substr(0, separator);
        const std::wstring content = payload.substr(separator + 1);
        std::ofstream stream(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
        if (!stream) {
            return L"ERROR failed to open debug service-write target";
        }

        stream << WideToUtf8(content);
        if (!stream) {
            return L"ERROR failed to write debug service-write target";
        }
        return L"OK service wrote file";
    }

    if (request.rfind(L"debug prepare-cow --rule ", 0) == 0) {
        const std::wstring payload = request.substr(25);
        const size_t separator = payload.find(L' ');
        if (separator == std::wstring::npos) {
            return L"ERROR debug prepare-cow requires rule id and path";
        }

        const std::wstring debugRuleId = payload.substr(0, separator);
        const std::wstring path = payload.substr(separator + 1);
        pathoverlay::OverlayRule rule;
        if (!metadata.GetRule(debugRuleId, &rule, &error)) {
            return L"ERROR rule not found: " + debugRuleId;
        }

        pathoverlay::OverlayOperations operations(metadata);
        std::wstring shadowPath;
        const pathoverlay::OperationResult result = operations.PrepareCopyOnWrite(rule, path, &shadowPath);
        return result.success ? L"OK shadow=" + shadowPath : L"ERROR " + result.message;
    }

    if (request == L"changes") {
        std::vector<pathoverlay::OverlayRule> rules;
        if (!metadata.ListRules(&rules, &error)) {
            return L"ERROR " + error;
        }

        std::wstringstream output;
        output << L"OK";
        bool anyChanges = false;
        for (const auto& rule : rules) {
            std::vector<pathoverlay::ChangeRecord> records;
            if (!metadata.ListChanges(rule.id, &records, &error)) {
                return L"ERROR " + error;
            }
            if (records.empty()) {
                continue;
            }

            anyChanges = true;
            output << L"\nrule=" << rule.id
                   << L" enabled=" << (rule.enabled ? L"true" : L"false")
                   << L" source=" << rule.source
                   << L" store=" << rule.store;
            for (const auto& record : records) {
                output << L"\n  " << pathoverlay::ChangeStateToString(record.state) << L" " << record.realPath;
                if (!record.targetPath.empty()) {
                    output << L" -> " << record.targetPath;
                }
            }
        }

        if (!anyChanges) {
            return L"OK no changes";
        }
        return output.str();
    }

    if (request == L"commit" || request == L"discard") {
        return L"ERROR rule id is required: use --rule <id>";
    }

    const ParsedRuleOperation commitOperation = ParseRuleOperation(request, L"commit");
    const ParsedRuleOperation discardOperation = ParseRuleOperation(request, L"discard");
    const bool isCommit = commitOperation.valid;
    const bool isDiscard = discardOperation.valid;
    if (isCommit || isDiscard) {
        const ParsedRuleOperation operation = isCommit ? commitOperation : discardOperation;
        pathoverlay::OverlayRule rule;
        if (!metadata.GetRule(operation.ruleId, &rule, &error)) {
            return L"ERROR rule not found: " + operation.ruleId;
        }

        const std::wstring operationName = isCommit ? L"commit" : L"discard";
        const std::wstring operationId = isCommit ? MakeCommitId() : MakeOperationId(L"discard");
        const std::wstring backupRoot = isCommit
            ? pathoverlay::NormalizePath(rule.store.empty() ? pathoverlay::DefaultStoreRoot() : rule.store) +
                  L"\\backups\\" + operationId
            : L"";
        if (!StartOperationRecord(metadata, operationId, rule, operationName, backupRoot, &error)) {
            return L"ERROR failed to start " + operationName + L" operation: " + error;
        }

        std::vector<pathoverlay::ChangeRecord> records;
        if (!metadata.ListChanges(rule.id, &records, &error)) {
            UpdateOperationRecord(metadata, operationId, L"failed", L"created", backupRoot, error, nullptr);
            return L"ERROR " + error;
        }
        if (!EnsureNoOccupyingProcesses(records, isCommit, operation.confirmClose, &error)) {
            UpdateOperationRecord(metadata, operationId, L"failed", L"created", backupRoot, error, nullptr);
            return L"ERROR " + operationName + L" failed rule=" + rule.id +
                L" source=" + rule.source + L" store=" + rule.store + L" error=" + error;
        }
        if (!UpdateOperationRecord(metadata, operationId, L"running", L"prechecked", backupRoot, L"", &error)) {
            return L"ERROR failed to update " + operationName + L" operation: " + error;
        }

        const bool restoreEnabled = rule.enabled;
        std::wstring syncWarning;
        if (restoreEnabled) {
            if (!metadata.SetRuleEnabled(rule.id, false, &error)) {
                UpdateOperationRecord(metadata, operationId, L"failed", L"prechecked", backupRoot, error, nullptr);
                return L"ERROR failed to pause rule: " + error;
            }

            pathoverlay::OverlayRule pausedRule = rule;
            pausedRule.enabled = false;
            std::vector<pathoverlay::OverlayRule> rules;
            if (!metadata.ListRules(&rules, &error)) {
                metadata.SetRuleEnabled(rule.id, true, &error);
                UpdateOperationRecord(metadata, operationId, L"failed", L"prechecked", backupRoot, error, nullptr);
                return L"ERROR failed to pause driver rule: " + error;
            }
            if (IsDriverPortConnected() && !PushDriverRules(rules, &error)) {
                metadata.SetRuleEnabled(rule.id, true, &error);
                UpdateOperationRecord(metadata, operationId, L"failed", L"prechecked", backupRoot, error, nullptr);
                return L"ERROR failed to pause driver rule: " + error;
            }
            if (!IsDriverPortConnected()) {
                syncWarning = L"driver sync skipped: driver not connected";
            }
            if (!UpdateOperationRecord(metadata, operationId, L"running", L"rule_paused", backupRoot, L"", &error)) {
                metadata.SetRuleEnabled(rule.id, true, &error);
                return L"ERROR failed to update " + operationName + L" operation: " + error;
            }
            WriteLog(L"rule " + rule.id + L" paused for " + (isCommit ? L"commit" : L"discard"));
        }

        pathoverlay::OverlayOperations operations(metadata);
        pathoverlay::OperationResult result;
        if (!UpdateOperationRecord(metadata, operationId, L"running", L"applying", backupRoot, L"", &error)) {
            return L"ERROR failed to update " + operationName + L" operation: " + error;
        }
        if (isCommit) {
            result = operations.Commit(rule, operationId);
        } else {
            result = operations.Discard(rule);
        }
        if (!result.success) {
            UpdateOperationRecord(metadata, operationId, L"failed", L"applying", backupRoot, result.message, nullptr);
        } else if (!UpdateOperationRecord(metadata, operationId, L"running", L"cleanup", backupRoot, L"", &error)) {
            return L"ERROR " + operationName + L" completed but failed to update operation: " + error;
        }

        if (restoreEnabled) {
            std::wstring restoreError;
            UpdateOperationRecord(metadata, operationId, L"running", L"restoring_rule", backupRoot, L"", nullptr);
            if (!metadata.SetRuleEnabled(rule.id, true, &restoreError)) {
                UpdateOperationRecord(metadata, operationId, L"failed", L"restoring_rule", backupRoot, restoreError, nullptr);
                return L"ERROR " + std::wstring(isCommit ? L"commit" : L"discard") +
                    L" completed but failed to restore rule: " + restoreError;
            }
            rule.enabled = true;
            std::vector<pathoverlay::OverlayRule> rules;
            if (!metadata.ListRules(&rules, &restoreError)) {
                UpdateOperationRecord(metadata, operationId, L"failed", L"restoring_rule", backupRoot, restoreError, nullptr);
                return L"ERROR " + std::wstring(isCommit ? L"commit" : L"discard") +
                    L" completed but failed to restore driver rule: " + restoreError;
            }
            if (IsDriverPortConnected() && !PushDriverRules(rules, &restoreError)) {
                UpdateOperationRecord(metadata, operationId, L"failed", L"restoring_rule", backupRoot, restoreError, nullptr);
                return L"ERROR " + std::wstring(isCommit ? L"commit" : L"discard") +
                    L" completed but failed to restore driver rule: " + restoreError;
            }
            if (!IsDriverPortConnected()) {
                syncWarning = L"driver sync skipped: driver not connected";
            }
            WriteLog(L"rule " + rule.id + L" restored after " + (isCommit ? L"commit" : L"discard"));
        }

        const std::wstring ruleContext =
            L" rule=" + rule.id + L" source=" + rule.source + L" store=" + rule.store;
        if (result.success &&
            !UpdateOperationRecord(metadata, operationId, L"done", L"finished", backupRoot, L"", &error)) {
            return L"ERROR " + operationName + L" completed but failed to update operation: " + error;
        }
        return result.success
            ? AppendDriverSyncWarning(L"OK " + operationName + L" completed" + ruleContext, syncWarning)
            : L"ERROR " + operationName + L" failed" + ruleContext + L" error=" + result.message;
    }

    return L"ERROR unknown command";
}

DWORD WINAPI PipeServerThread(LPVOID parameter) {
    HANDLE stopEvent = static_cast<HANDLE>(parameter);

    while (WaitForSingleObject(stopEvent, 0) == WAIT_TIMEOUT) {
        HANDLE pipe = CreateNamedPipeW(
            kPipeName,
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            1,
            8192,
            8192,
            1000,
            nullptr);

        if (pipe == INVALID_HANDLE_VALUE) {
            WriteLog(L"pipe: CreateNamedPipe failed");
            Sleep(1000);
            continue;
        }

        const BOOL connected = ConnectNamedPipe(pipe, nullptr) ? TRUE : GetLastError() == ERROR_PIPE_CONNECTED;
        if (connected) {
            char buffer[4096] = {};
            DWORD bytesRead = 0;
            if (ReadFile(pipe, buffer, sizeof(buffer), &bytesRead, nullptr) && bytesRead > 0) {
                const std::wstring response = ProcessRequest(Utf8ToWide(std::string(buffer, buffer + bytesRead)));
                const std::string responseUtf8 = WideToUtf8(response);
                DWORD bytesWritten = 0;
                WriteFile(pipe, responseUtf8.data(), static_cast<DWORD>(responseUtf8.size()), &bytesWritten, nullptr);
            }
        }

        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }

    return 0;
}

DWORD ServiceMainLoop() {
    WriteLog(L"service starting");
    if (!InitializeRuntime()) {
        return ERROR_SERVICE_SPECIFIC_ERROR;
    }

    HANDLE pipeThread = CreateThread(nullptr, 0, PipeServerThread, gStopEvent, 0, nullptr);
    if (pipeThread == nullptr) {
        WriteLog(L"fatal: failed to start pipe server");
        return GetLastError();
    }

    gDriverMessageThread = CreateThread(nullptr, 0, DriverMessageThread, gStopEvent, 0, nullptr);
    if (gDriverMessageThread == nullptr) {
        WriteLog(L"fatal: failed to start driver message thread");
        SetEvent(gStopEvent);
        WaitForSingleObject(pipeThread, 3000);
        CloseHandle(pipeThread);
        return GetLastError();
    }

    SetServiceState(SERVICE_RUNNING);
    WriteLog(L"service started");
    WaitForSingleObject(gStopEvent, INFINITE);
    DisconnectDriverPort();
    HANDLE wakePipe = CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (wakePipe != INVALID_HANDLE_VALUE) {
        CloseHandle(wakePipe);
    }
    WaitForSingleObject(pipeThread, 3000);
    WaitForSingleObject(gDriverMessageThread, 3000);
    CloseHandle(pipeThread);
    CloseHandle(gDriverMessageThread);
    gDriverMessageThread = nullptr;
    WriteLog(L"service stopping");
    return NO_ERROR;
}

void WINAPI ServiceControlHandler(DWORD controlCode) {
    switch (controlCode) {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
            SetServiceState(SERVICE_STOP_PENDING, NO_ERROR, 3000);
            if (gStopEvent != nullptr) {
                SetEvent(gStopEvent);
            }
            return;
        default:
            break;
    }

    SetServiceStatus(gStatusHandle, &gServiceStatus);
}

void WINAPI ServiceMain(DWORD argc, LPWSTR* argv) {
    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(argv);

    gStatusHandle = RegisterServiceCtrlHandlerW(kServiceName, ServiceControlHandler);
    if (gStatusHandle == nullptr) {
        return;
    }

    SetServiceState(SERVICE_START_PENDING, NO_ERROR, 10000);
    gStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (gStopEvent == nullptr) {
        SetServiceState(SERVICE_STOPPED, GetLastError());
        return;
    }

    const DWORD result = ServiceMainLoop();

    CloseHandle(gStopEvent);
    gStopEvent = nullptr;
    SetServiceState(SERVICE_STOPPED, result);
}

std::wstring CurrentExecutablePath() {
    std::wstring buffer(MAX_PATH, L'\0');
    DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    while (length == buffer.size()) {
        buffer.resize(buffer.size() * 2);
        length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    }
    buffer.resize(length);
    return buffer;
}

int InstallService() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (scm == nullptr) {
        std::wcerr << L"OpenSCManager failed: " << GetLastError() << L"\n";
        return 1;
    }

    const std::wstring binaryPath = L"\"" + CurrentExecutablePath() + L"\"";
    SC_HANDLE service = CreateServiceW(
        scm,
        kServiceName,
        kServiceDisplayName,
        SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS,
        SERVICE_DEMAND_START,
        SERVICE_ERROR_NORMAL,
        binaryPath.c_str(),
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr);

    if (service == nullptr) {
        const DWORD error = GetLastError();
        CloseServiceHandle(scm);
        std::wcerr << L"CreateService failed: " << error << L"\n";
        return 1;
    }

    CloseServiceHandle(service);
    CloseServiceHandle(scm);
    std::wcout << L"Service installed\n";
    return 0;
}

int UninstallService() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (scm == nullptr) {
        std::wcerr << L"OpenSCManager failed: " << GetLastError() << L"\n";
        return 1;
    }

    SC_HANDLE service = OpenServiceW(scm, kServiceName, DELETE | SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (service == nullptr) {
        const DWORD error = GetLastError();
        CloseServiceHandle(scm);
        std::wcerr << L"OpenService failed: " << error << L"\n";
        return 1;
    }

    SERVICE_STATUS status = {};
    ControlService(service, SERVICE_CONTROL_STOP, &status);
    const BOOL deleted = DeleteService(service);
    const DWORD error = GetLastError();
    CloseServiceHandle(service);
    CloseServiceHandle(scm);

    if (!deleted) {
        std::wcerr << L"DeleteService failed: " << error << L"\n";
        return 1;
    }

    std::wcout << L"Service uninstalled\n";
    return 0;
}

int StartInstalledService() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (scm == nullptr) {
        std::wcerr << L"OpenSCManager failed: " << GetLastError() << L"\n";
        return 1;
    }

    SC_HANDLE service = OpenServiceW(scm, kServiceName, SERVICE_START | SERVICE_QUERY_STATUS);
    if (service == nullptr) {
        const DWORD error = GetLastError();
        CloseServiceHandle(scm);
        std::wcerr << L"OpenService failed: " << error << L"\n";
        return 1;
    }

    const BOOL started = StartServiceW(service, 0, nullptr);
    const DWORD error = GetLastError();
    if (!started && error != ERROR_SERVICE_ALREADY_RUNNING) {
        CloseServiceHandle(service);
        CloseServiceHandle(scm);
        std::wcerr << L"StartService failed: " << error << L"\n";
        return 1;
    }

    SERVICE_STATUS_PROCESS status = {};
    DWORD bytesNeeded = 0;
    for (DWORD attempt = 0; attempt < 100; ++attempt) {
        if (!QueryServiceStatusEx(
                service,
                SC_STATUS_PROCESS_INFO,
                reinterpret_cast<LPBYTE>(&status),
                sizeof(status),
                &bytesNeeded)) {
            const DWORD queryError = GetLastError();
            CloseServiceHandle(service);
            CloseServiceHandle(scm);
            std::wcerr << L"QueryServiceStatusEx failed: " << queryError << L"\n";
            return 1;
        }

        if (status.dwCurrentState == SERVICE_RUNNING) {
            CloseServiceHandle(service);
            CloseServiceHandle(scm);
            std::wcout << L"Service started\n";
            return 0;
        }

        if (status.dwCurrentState == SERVICE_STOPPED) {
            const DWORD exitCode =
                status.dwWin32ExitCode != NO_ERROR ? status.dwWin32ExitCode : status.dwServiceSpecificExitCode;
            CloseServiceHandle(service);
            CloseServiceHandle(scm);
            std::wcerr << L"Service stopped during startup. ExitCode=" << exitCode << L"\n";
            return 1;
        }

        Sleep(100);
    }

    CloseServiceHandle(service);
    CloseServiceHandle(scm);
    std::wcerr << L"Service did not reach running state before timeout\n";
    return 1;
}

bool WaitForNamedPipeReady(DWORD timeoutMilliseconds, DWORD* lastError) {
    const ULONGLONG start = GetTickCount64();
    while (GetTickCount64() - start < timeoutMilliseconds) {
        HANDLE pipe = CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
            CloseHandle(pipe);
            return true;
        }

        const DWORD error = GetLastError();
        if (lastError != nullptr) {
            *lastError = error;
        }
        if (error == ERROR_PIPE_BUSY) {
            WaitNamedPipeW(kPipeName, 100);
        }
        Sleep(100);
    }

    return false;
}

int StartInstalledServiceAndWaitForIpc() {
    const int startResult = StartInstalledService();
    if (startResult != 0) {
        return startResult;
    }

    DWORD pipeError = ERROR_SUCCESS;
    if (!WaitForNamedPipeReady(10000, &pipeError)) {
        std::wcerr << L"Service started but IPC pipe was not ready. LastError=" << pipeError
                   << L". Check " << LogPath() << L"\n";
        return 1;
    }

    return 0;
}

int StopInstalledService() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (scm == nullptr) {
        std::wcerr << L"OpenSCManager failed: " << GetLastError() << L"\n";
        return 1;
    }

    SC_HANDLE service = OpenServiceW(scm, kServiceName, SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (service == nullptr) {
        const DWORD error = GetLastError();
        CloseServiceHandle(scm);
        std::wcerr << L"OpenService failed: " << error << L"\n";
        return 1;
    }

    SERVICE_STATUS status = {};
    const BOOL stopped = ControlService(service, SERVICE_CONTROL_STOP, &status);
    const DWORD error = GetLastError();
    CloseServiceHandle(service);
    CloseServiceHandle(scm);

    if (!stopped && error != ERROR_SERVICE_NOT_ACTIVE) {
        std::wcerr << L"ControlService failed: " << error << L"\n";
        return 1;
    }

    std::wcout << L"Service stopped\n";
    return 0;
}

int RunConsoleOnce() {
    WriteLog(L"console validation starting");
    if (!InitializeRuntime()) {
        std::wcerr << L"Runtime initialization failed. Check " << LogPath() << L"\n";
        return 1;
    }
    DisconnectDriverPort();
    WriteLog(L"console validation stopped");
    std::wcout << L"Runtime initialized\n";
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t* argv[]) {
    if (argc > 1) {
        const std::wstring command = argv[1];
        if (command == L"install") {
            return InstallService();
        }
        if (command == L"uninstall") {
            return UninstallService();
        }
        if (command == L"start") {
            return StartInstalledServiceAndWaitForIpc();
        }
        if (command == L"stop") {
            return StopInstalledService();
        }
        if (command == L"console") {
            return RunConsoleOnce();
        }

        std::wcerr << L"Usage: PathOverlaySvc.exe [install|uninstall|start|stop|console]\n";
        return 1;
    }

    SERVICE_TABLE_ENTRYW serviceTable[] = {
        {const_cast<LPWSTR>(kServiceName), ServiceMain},
        {nullptr, nullptr}
    };

    if (!StartServiceCtrlDispatcherW(serviceTable)) {
        const DWORD error = GetLastError();
        std::wcerr << L"StartServiceCtrlDispatcher failed: " << error << L"\n";
        return 1;
    }

    return 0;
}
