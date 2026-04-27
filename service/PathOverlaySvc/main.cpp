#include <windows.h>
#include <fltuser.h>

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

std::wstring DosPathToNtPath(const std::wstring& dosPath) {
    const std::wstring normalized = pathoverlay::NormalizePath(dosPath);
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

bool IsTombstoned(pathoverlay::MetadataStore& metadata, const std::wstring& realPath, std::wstring* error) {
    std::vector<pathoverlay::ChangeRecord> records;
    if (!metadata.ListChanges(L"default", &records, error)) {
        return false;
    }

    const std::wstring normalizedReal = pathoverlay::NormalizePath(realPath);
    for (const auto& record : records) {
        if (_wcsicmp(pathoverlay::NormalizePath(record.realPath).c_str(), normalizedReal.c_str()) == 0 &&
            (record.state == pathoverlay::ChangeState::kTombstone ||
             record.state == pathoverlay::ChangeState::kDeleted)) {
            return true;
        }
    }

    return false;
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

    pathoverlay::MetadataStore metadata;
    std::wstring error;
    if (!metadata.Open(MetadataPath(), &error) || !metadata.Initialize(&error)) {
        WriteLog(L"driver request: metadata unavailable: " + error);
        response.Status = kNtStatusUnsuccessful;
        return response;
    }

    pathoverlay::OverlayRule rule;
    if (!metadata.GetRule(L"default", &rule, &error) || !rule.enabled) {
        response.Status = kNtStatusObjectNameNotFound;
        return response;
    }

    pathoverlay::OverlayOperations operations(metadata);
    if (request.Command == PathOverlayServiceCommandQueryPath) {
        response.PathState = IsTombstoned(metadata, realPath, &error)
            ? PathOverlayPathStateTombstone
            : PathOverlayPathStateNormal;
        WriteLog(
            std::wstring(L"driver request: query path=") + realPath +
            L" state=" +
            (response.PathState == PathOverlayPathStateTombstone ? L"tombstone" : L"normal"));
        return response;
    }

    pathoverlay::OperationResult result;
    if (request.Command == PathOverlayServiceCommandPrepareCopyOnWrite) {
        std::wstring shadowPath;
        result = operations.PrepareCopyOnWrite(rule, realPath, &shadowPath);
        WriteLog(
            std::wstring(L"driver request: copy-on-write path=") + realPath +
            (result.success ? L" ok" : L" failed: " + result.message));
    } else if (request.Command == PathOverlayServiceCommandPrepareDirectoryView) {
        std::wstring shadowPath;
        result = operations.PrepareDirectoryView(rule, realPath, &shadowPath);
        WriteLog(
            std::wstring(L"driver request: directory-view path=") + realPath +
            (result.success ? L" ok" : L" failed: " + result.message));
    } else if (request.Command == PathOverlayServiceCommandRecordDelete) {
        result = operations.RecordDelete(rule, realPath);
        WriteLog(
            std::wstring(L"driver request: record-delete path=") + realPath +
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

bool SameNormalizedPath(const std::wstring& left, const std::wstring& right) {
    return _wcsicmp(pathoverlay::NormalizePath(left).c_str(), pathoverlay::NormalizePath(right).c_str()) == 0;
}

bool RulePathsChanged(const pathoverlay::OverlayRule& current, const pathoverlay::OverlayRule& next) {
    return !SameNormalizedPath(current.source, next.source) ||
           !SameNormalizedPath(current.store, next.store);
}

bool HasPendingChanges(pathoverlay::MetadataStore& metadata, const std::wstring& ruleId, std::wstring* error) {
    std::vector<pathoverlay::ChangeRecord> records;
    if (!metadata.ListChanges(ruleId, &records, error)) {
        return true;
    }
    return !records.empty();
}

bool RemoveShadowDriveForRule(const pathoverlay::OverlayRule& rule, std::wstring* error) {
    const std::wstring driveRoot =
        pathoverlay::NormalizePath(rule.store.empty() ? pathoverlay::DefaultStoreRoot() : rule.store) + L"\\drive";
    std::error_code errorCode;
    std::filesystem::remove_all(driveRoot, errorCode);
    if (errorCode) {
        if (error != nullptr) {
            *error = L"failed to remove old shadow drive data";
        }
        return false;
    }
    return true;
}

bool PushDriverRule(const pathoverlay::OverlayRule& rule, std::wstring* error) {
    if (!rule.enabled) {
        return ClearDriverRule(error);
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
    if (!CopyProtocolPath(sourceNtPath, message.SourceNtPath, PATHOVERLAY_MAX_PATH_CHARS) ||
        !CopyProtocolPath(shadowSourceNtPath, message.StoreNtPath, PATHOVERLAY_MAX_PATH_CHARS)) {
        if (error != nullptr) {
            *error = L"rule path is too long for driver protocol";
        }
        return false;
    }

    return SendDriverRuleMessage(message, error);
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
    pathoverlay::OverlayRule rule;
    if (!metadata.GetRule(L"default", &rule, &error)) {
        if (!ClearDriverRule(&error)) {
            WriteLog(L"driver: failed to clear missing rule: " + error);
        }
        return;
    }

    if (!PushDriverRule(rule, &error)) {
        WriteLog(L"driver: failed to sync rule: " + error);
        return;
    }

    WriteLog(rule.enabled ? L"driver: default rule synchronized" : L"driver: default rule disabled");
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
        pathoverlay::OverlayRule rule;
        rule.id = L"default";
        rule.name = L"default";
        rule.enabled = true;
        rule.source = request.substr(9);
        rule.store = pathoverlay::DefaultStoreRoot();

        const pathoverlay::RuleValidationResult validation = pathoverlay::ValidateOverlayRule(rule);
        if (!validation.ok()) {
            return L"ERROR " + validation.message;
        }

        pathoverlay::OverlayRule currentRule;
        const bool hasCurrentRule = metadata.GetRule(L"default", &currentRule, &error);
        if (!hasCurrentRule && error != L"rule was not found") {
            return L"ERROR " + error;
        }
        if (hasCurrentRule && RulePathsChanged(currentRule, rule)) {
            std::wstring pendingError;
            if (HasPendingChanges(metadata, currentRule.id, &pendingError)) {
                if (!pendingError.empty()) {
                    return L"ERROR " + pendingError;
                }
                return L"ERROR pending changes exist for current rule; run pathoverlay commit or pathoverlay discard before changing rule";
            }

            pathoverlay::OverlayRule pausedRule = currentRule;
            pausedRule.enabled = false;
            if (currentRule.enabled && !PushDriverRule(pausedRule, &error)) {
                return L"ERROR failed to pause current driver rule: " + error;
            }
            if (!RemoveShadowDriveForRule(currentRule, &error)) {
                if (currentRule.enabled) {
                    PushDriverRule(currentRule, &error);
                }
                return L"ERROR " + error;
            }
        } else if (!hasCurrentRule) {
            error.clear();
        }

        if (!metadata.UpsertRule(rule, &error)) {
            if (hasCurrentRule && currentRule.enabled) {
                PushDriverRule(currentRule, &error);
            }
            return L"ERROR " + error;
        }
        if (!PushDriverRule(rule, &error)) {
            if (hasCurrentRule) {
                std::wstring restoreError;
                metadata.UpsertRule(currentRule, &restoreError);
                if (currentRule.enabled) {
                    PushDriverRule(currentRule, &restoreError);
                }
            }
            return L"ERROR rule saved but driver sync failed: " + error;
        }
        return L"OK rule added: " + pathoverlay::NormalizePath(rule.source);
    }

    if (request == L"rule enable" || request == L"rule disable") {
        pathoverlay::OverlayRule rule;
        if (!metadata.GetRule(L"default", &rule, &error)) {
            return L"ERROR " + error;
        }

        rule.enabled = request == L"rule enable";
        if (!metadata.SetRuleEnabled(rule.id, rule.enabled, &error)) {
            return L"ERROR " + error;
        }
        if (!PushDriverRule(rule, &error)) {
            return L"ERROR rule updated but driver sync failed: " + error;
        }
        return rule.enabled ? L"OK rule enabled" : L"OK rule disabled";
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

    if (request == L"changes") {
        std::vector<pathoverlay::ChangeRecord> records;
        if (!metadata.ListChanges(L"default", &records, &error)) {
            return L"ERROR " + error;
        }
        if (records.empty()) {
            return L"OK no changes";
        }

        std::wstringstream output;
        output << L"OK";
        for (const auto& record : records) {
            output << L"\n" << pathoverlay::ChangeStateToString(record.state) << L" " << record.realPath;
        }
        return output.str();
    }

    if (request == L"commit" || request == L"discard") {
        pathoverlay::OverlayRule rule;
        if (!metadata.GetRule(L"default", &rule, &error)) {
            return L"ERROR " + error;
        }

        const bool restoreEnabled = rule.enabled;
        if (restoreEnabled) {
            if (!metadata.SetRuleEnabled(rule.id, false, &error)) {
                return L"ERROR failed to pause rule: " + error;
            }

            pathoverlay::OverlayRule pausedRule = rule;
            pausedRule.enabled = false;
            if (!PushDriverRule(pausedRule, &error)) {
                metadata.SetRuleEnabled(rule.id, true, &error);
                return L"ERROR failed to pause driver rule: " + error;
            }
            WriteLog(L"rule paused for " + request);
        }

        pathoverlay::OverlayOperations operations(metadata);
        pathoverlay::OperationResult result;
        if (request == L"commit") {
            result = operations.Commit(rule, MakeCommitId());
        } else {
            result = operations.Discard(rule);
        }

        if (restoreEnabled) {
            std::wstring restoreError;
            if (!metadata.SetRuleEnabled(rule.id, true, &restoreError)) {
                return L"ERROR " + request + L" completed but failed to restore rule: " + restoreError;
            }
            rule.enabled = true;
            if (!PushDriverRule(rule, &restoreError)) {
                return L"ERROR " + request + L" completed but failed to restore driver rule: " + restoreError;
            }
            WriteLog(L"rule restored after " + request);
        }

        return result.success ? L"OK " + request + L" completed" : L"ERROR " + result.message;
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

    SetServiceState(SERVICE_START_PENDING, NO_ERROR, 3000);
    gStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (gStopEvent == nullptr) {
        SetServiceState(SERVICE_STOPPED, GetLastError());
        return;
    }

    SetServiceState(SERVICE_RUNNING);
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
    for (DWORD attempt = 0; attempt < 50; ++attempt) {
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

bool WaitForNamedPipeReady(DWORD timeoutMilliseconds) {
    const DWORD start = GetTickCount();
    while (GetTickCount() - start < timeoutMilliseconds) {
        if (WaitNamedPipeW(kPipeName, 100)) {
            return true;
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

    if (!WaitForNamedPipeReady(5000)) {
        std::wcerr << L"Service started but IPC pipe was not ready. Check " << LogPath() << L"\n";
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
