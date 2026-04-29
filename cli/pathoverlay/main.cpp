#include <iostream>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <windows.h>

#include "path_overlay_common.h"

namespace {

constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\PathOverlaySvc";
constexpr wchar_t kServiceName[] = L"PathOverlaySvc";
constexpr wchar_t kDriverName[] = L"PathOverlayFlt";

std::wstring ProgramDataRoot() {
    wchar_t buffer[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(L"ProgramData", buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return L"C:\\ProgramData\\PathOverlay";
    }
    return std::wstring(buffer, length) + L"\\PathOverlay";
}

std::wstring DefaultDiagnosticsRoot() {
    SYSTEMTIME time = {};
    GetLocalTime(&time);

    wchar_t stamp[32] = {};
    swprintf_s(
        stamp,
        L"%04u%02u%02u-%02u%02u%02u",
        time.wYear,
        time.wMonth,
        time.wDay,
        time.wHour,
        time.wMinute,
        time.wSecond);
    return ProgramDataRoot() + L"\\diagnostics\\PathOverlay-" + stamp;
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

std::wstring JoinArgs(int start, int argc, wchar_t* argv[]) {
    std::wstring result;
    for (int i = start; i < argc; ++i) {
        if (!result.empty()) {
            result += L" ";
        }
        result += argv[i];
    }
    return result;
}

int SendRequestRaw(const std::wstring& request, std::wstring* response) {
    if (!WaitNamedPipeW(kPipeName, 3000)) {
        if (response != nullptr) {
            *response = L"PathOverlaySvc is unavailable. Start the service and try again.";
        }
        return 1;
    }

    HANDLE pipe = CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        if (response != nullptr) {
            *response = L"Failed to connect to PathOverlaySvc: " + std::to_wstring(GetLastError());
        }
        return 1;
    }

    const std::string requestUtf8 = WideToUtf8(request);
    DWORD bytesWritten = 0;
    if (!WriteFile(pipe, requestUtf8.data(), static_cast<DWORD>(requestUtf8.size()), &bytesWritten, nullptr)) {
        if (response != nullptr) {
            *response = L"Failed to write request: " + std::to_wstring(GetLastError());
        }
        CloseHandle(pipe);
        return 1;
    }

    char buffer[8192] = {};
    DWORD bytesRead = 0;
    if (!ReadFile(pipe, buffer, sizeof(buffer), &bytesRead, nullptr)) {
        if (response != nullptr) {
            *response = L"Failed to read response: " + std::to_wstring(GetLastError());
        }
        CloseHandle(pipe);
        return 1;
    }

    CloseHandle(pipe);
    *response = Utf8ToWide(std::string(buffer, buffer + bytesRead));
    return response->rfind(L"OK", 0) == 0 ? 0 : 1;
}

int SendRequest(const std::wstring& request) {
    std::wstring response;
    const int result = SendRequestRaw(request, &response);
    if (response.rfind(L"OK", 0) != 0) {
        std::wcerr << response << L"\n";
        return 1;
    }
    std::wcout << response << L"\n";
    return result;
}

bool WriteTextFile(const std::filesystem::path& path, const std::wstring& text, std::wstring* error) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        if (error != nullptr) {
            *error = L"failed to create " + path.wstring();
        }
        return false;
    }

    stream << WideToUtf8(text);
    if (!stream) {
        if (error != nullptr) {
            *error = L"failed to write " + path.wstring();
        }
        return false;
    }
    return true;
}

std::wstring FormatServiceState(DWORD state) {
    switch (state) {
        case SERVICE_STOPPED:
            return L"stopped";
        case SERVICE_START_PENDING:
            return L"start_pending";
        case SERVICE_STOP_PENDING:
            return L"stop_pending";
        case SERVICE_RUNNING:
            return L"running";
        case SERVICE_CONTINUE_PENDING:
            return L"continue_pending";
        case SERVICE_PAUSE_PENDING:
            return L"pause_pending";
        case SERVICE_PAUSED:
            return L"paused";
        default:
            return L"unknown";
    }
}

std::wstring QueryScmStatus(const std::wstring& serviceName) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (scm == nullptr) {
        return L"ERROR OpenSCManager failed: " + std::to_wstring(GetLastError()) + L"\n";
    }

    SC_HANDLE service = OpenServiceW(scm, serviceName.c_str(), SERVICE_QUERY_STATUS);
    if (service == nullptr) {
        const DWORD error = GetLastError();
        CloseServiceHandle(scm);
        return L"ERROR OpenService " + serviceName + L" failed: " + std::to_wstring(error) + L"\n";
    }

    SERVICE_STATUS_PROCESS status = {};
    DWORD bytesNeeded = 0;
    std::wstringstream output;
    if (QueryServiceStatusEx(
            service,
            SC_STATUS_PROCESS_INFO,
            reinterpret_cast<LPBYTE>(&status),
            sizeof(status),
            &bytesNeeded)) {
        output << L"service=" << serviceName
               << L"\nstate=" << FormatServiceState(status.dwCurrentState)
               << L"\nprocess_id=" << status.dwProcessId
               << L"\nwin32_exit_code=" << status.dwWin32ExitCode
               << L"\nservice_exit_code=" << status.dwServiceSpecificExitCode
               << L"\ncheckpoint=" << status.dwCheckPoint
               << L"\nwait_hint=" << status.dwWaitHint
               << L"\n";
    } else {
        output << L"ERROR QueryServiceStatusEx " << serviceName
               << L" failed: " << GetLastError() << L"\n";
    }

    CloseServiceHandle(service);
    CloseServiceHandle(scm);
    return output.str();
}

void CopyIfExists(const std::filesystem::path& source, const std::filesystem::path& target, std::wstring* manifest) {
    std::error_code errorCode;
    if (!std::filesystem::exists(source, errorCode)) {
        *manifest += L"missing " + source.wstring() + L"\n";
        return;
    }

    std::filesystem::copy_file(source, target, std::filesystem::copy_options::overwrite_existing, errorCode);
    if (errorCode) {
        *manifest += L"copy failed " + source.wstring() + L": " + Utf8ToWide(errorCode.message()) + L"\n";
    } else {
        *manifest += L"copied " + source.wstring() + L" -> " + target.wstring() + L"\n";
    }
}

int CollectDiagnostics(const std::wstring& outputPath) {
    const std::filesystem::path diagnosticsRoot(outputPath.empty() ? DefaultDiagnosticsRoot() : outputPath);
    std::error_code errorCode;
    std::filesystem::create_directories(diagnosticsRoot, errorCode);
    if (errorCode) {
        std::wcerr << L"Failed to create diagnostics directory: " << diagnosticsRoot.wstring()
                   << L" error=" << Utf8ToWide(errorCode.message()) << L"\n";
        return 1;
    }

    std::wstring manifest;
    manifest += L"diagnostics_root=" + diagnosticsRoot.wstring() + L"\n";

    const std::vector<std::pair<std::wstring, std::wstring>> requests = {
        {L"rule-show.txt", L"rule show"},
        {L"changes.txt", L"changes"},
        {L"status.txt", L"status"},
        {L"doctor.txt", L"doctor"},
        {L"driver-status.txt", L"driver status"},
    };

    for (const auto& request : requests) {
        std::wstring response;
        const int result = SendRequestRaw(request.second, &response);
        const std::wstring content =
            L"command=pathoverlay " + request.second + L"\nexit_code=" + std::to_wstring(result) + L"\n\n" + response + L"\n";
        std::wstring writeError;
        if (!WriteTextFile(diagnosticsRoot / request.first, content, &writeError)) {
            std::wcerr << writeError << L"\n";
            return 1;
        }
        manifest += L"wrote " + request.first + L"\n";
    }

    std::wstring writeError;
    if (!WriteTextFile(diagnosticsRoot / L"scm-service.txt", QueryScmStatus(kServiceName), &writeError) ||
        !WriteTextFile(diagnosticsRoot / L"scm-driver.txt", QueryScmStatus(kDriverName), &writeError)) {
        std::wcerr << writeError << L"\n";
        return 1;
    }
    manifest += L"wrote scm-service.txt\nwrote scm-driver.txt\n";

    CopyIfExists(
        std::filesystem::path(ProgramDataRoot()) / L"logs" / L"PathOverlaySvc.log",
        diagnosticsRoot / L"PathOverlaySvc.log",
        &manifest);

    if (!WriteTextFile(diagnosticsRoot / L"manifest.txt", manifest, &writeError)) {
        std::wcerr << writeError << L"\n";
        return 1;
    }

    std::wcout << L"OK diagnostics collected: " << diagnosticsRoot.wstring() << L"\n";
    return 0;
}

void PrintUsage() {
    std::wcout
        << L"Usage:\n"
        << L"  pathoverlay rule add <source> [--store <path>]\n"
        << L"  pathoverlay rule enable --rule <id>\n"
        << L"  pathoverlay rule disable --rule <id>\n"
        << L"  pathoverlay rule delete --rule <id>\n"
        << L"  pathoverlay rule del --rule <id>\n"
        << L"  pathoverlay rule show\n"
        << L"  pathoverlay debug service-write <path> <content>\n"
        << L"  pathoverlay debug prepare-cow --rule <id> <path>\n"
        << L"  pathoverlay changes [--rule <id>]\n"
        << L"  pathoverlay status\n"
        << L"  pathoverlay doctor\n"
        << L"  pathoverlay diagnostics collect [--output <directory>]\n"
        << L"  pathoverlay driver status\n"
        << L"  pathoverlay commit [--dry-run] --rule <id> [--confirm-close]\n"
        << L"  pathoverlay discard [--dry-run] --rule <id> [--confirm-close]\n";
}

}  // namespace

int wmain(int argc, wchar_t* argv[]) {
    if (argc < 2) {
        PrintUsage();
        return 1;
    }

    const std::wstring command = argv[1];
    if (command == L"rule") {
        if (argc >= 4 && std::wstring(argv[2]) == L"add") {
            return SendRequest(L"rule add " + JoinArgs(3, argc, argv));
        }
        if (argc == 3 && std::wstring(argv[2]) == L"show") {
            return SendRequest(L"rule show");
        }
        if (argc == 5 && std::wstring(argv[2]) == L"enable" && std::wstring(argv[3]) == L"--rule") {
            return SendRequest(L"rule enable --rule " + std::wstring(argv[4]));
        }
        if (argc == 5 && std::wstring(argv[2]) == L"disable" && std::wstring(argv[3]) == L"--rule") {
            return SendRequest(L"rule disable --rule " + std::wstring(argv[4]));
        }
        if (argc == 5 &&
            (std::wstring(argv[2]) == L"delete" || std::wstring(argv[2]) == L"del") &&
            std::wstring(argv[3]) == L"--rule") {
            return SendRequest(L"rule delete --rule " + std::wstring(argv[4]));
        }
        PrintUsage();
        return 1;
    }

    if (command == L"debug") {
        if (argc >= 5 && std::wstring(argv[2]) == L"service-write") {
            return SendRequest(L"debug service-write " + JoinArgs(3, argc, argv));
        }
        if (argc == 6 && std::wstring(argv[2]) == L"prepare-cow" && std::wstring(argv[3]) == L"--rule") {
            return SendRequest(L"debug prepare-cow --rule " + std::wstring(argv[4]) + L" " + std::wstring(argv[5]));
        }
        PrintUsage();
        return 1;
    }

    if (command == L"changes") {
        if (argc == 2) {
            return SendRequest(L"changes");
        }
        if (argc == 4 && std::wstring(argv[2]) == L"--rule") {
            return SendRequest(L"changes --rule " + std::wstring(argv[3]));
        }
        PrintUsage();
        return 1;
    }
    if (command == L"status") {
        return SendRequest(L"status");
    }
    if (command == L"doctor") {
        return SendRequest(L"doctor");
    }
    if (command == L"diagnostics") {
        if (argc == 3 && std::wstring(argv[2]) == L"collect") {
            return CollectDiagnostics(L"");
        }
        if (argc == 5 && std::wstring(argv[2]) == L"collect" && std::wstring(argv[3]) == L"--output") {
            return CollectDiagnostics(argv[4]);
        }
        PrintUsage();
        return 1;
    }
    if (command == L"driver") {
        if (argc == 3 && std::wstring(argv[2]) == L"status") {
            return SendRequest(L"driver status");
        }
        PrintUsage();
        return 1;
    }
    if (command == L"commit") {
        if (argc >= 3) {
            return SendRequest(L"commit " + JoinArgs(2, argc, argv));
        }
        PrintUsage();
        return 1;
    }
    if (command == L"discard") {
        if (argc >= 3) {
            return SendRequest(L"discard " + JoinArgs(2, argc, argv));
        }
        PrintUsage();
        return 1;
    }

    PrintUsage();
    return 1;
}
