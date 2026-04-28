#include <iostream>
#include <string>
#include <windows.h>

#include "path_overlay_common.h"

namespace {

constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\PathOverlaySvc";

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

int SendRequest(const std::wstring& request) {
    if (!WaitNamedPipeW(kPipeName, 3000)) {
        std::wcerr << L"PathOverlaySvc is unavailable. Start the service and try again.\n";
        return 1;
    }

    HANDLE pipe = CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        std::wcerr << L"Failed to connect to PathOverlaySvc: " << GetLastError() << L"\n";
        return 1;
    }

    const std::string requestUtf8 = WideToUtf8(request);
    DWORD bytesWritten = 0;
    if (!WriteFile(pipe, requestUtf8.data(), static_cast<DWORD>(requestUtf8.size()), &bytesWritten, nullptr)) {
        std::wcerr << L"Failed to write request: " << GetLastError() << L"\n";
        CloseHandle(pipe);
        return 1;
    }

    char buffer[8192] = {};
    DWORD bytesRead = 0;
    if (!ReadFile(pipe, buffer, sizeof(buffer), &bytesRead, nullptr)) {
        std::wcerr << L"Failed to read response: " << GetLastError() << L"\n";
        CloseHandle(pipe);
        return 1;
    }

    CloseHandle(pipe);
    const std::wstring response = Utf8ToWide(std::string(buffer, buffer + bytesRead));
    std::wcout << response << L"\n";
    return response.rfind(L"OK", 0) == 0 ? 0 : 1;
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
        << L"  pathoverlay changes\n"
        << L"  pathoverlay driver status\n"
        << L"  pathoverlay commit --rule <id> [--confirm-close]\n"
        << L"  pathoverlay discard --rule <id> [--confirm-close]\n";
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
        return SendRequest(L"changes");
    }
    if (command == L"driver") {
        if (argc == 3 && std::wstring(argv[2]) == L"status") {
            return SendRequest(L"driver status");
        }
        PrintUsage();
        return 1;
    }
    if (command == L"commit") {
        if ((argc == 4 || argc == 5) && std::wstring(argv[2]) == L"--rule") {
            std::wstring request = L"commit --rule " + std::wstring(argv[3]);
            if (argc == 5) {
                if (std::wstring(argv[4]) != L"--confirm-close") {
                    PrintUsage();
                    return 1;
                }
                request += L" --confirm-close";
            }
            return SendRequest(request);
        }
        PrintUsage();
        return 1;
    }
    if (command == L"discard") {
        if ((argc == 4 || argc == 5) && std::wstring(argv[2]) == L"--rule") {
            std::wstring request = L"discard --rule " + std::wstring(argv[3]);
            if (argc == 5) {
                if (std::wstring(argv[4]) != L"--confirm-close") {
                    PrintUsage();
                    return 1;
                }
                request += L" --confirm-close";
            }
            return SendRequest(request);
        }
        PrintUsage();
        return 1;
    }

    PrintUsage();
    return 1;
}
