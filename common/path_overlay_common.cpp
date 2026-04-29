#include "path_overlay_common.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <sstream>
#include <vector>

namespace pathoverlay {
namespace {

bool IsSlash(wchar_t ch) {
    return ch == L'\\' || ch == L'/';
}

bool IsDriveRootPath(const std::wstring& path) {
    return path.size() == 3 && std::iswalpha(path[0]) && path[1] == L':' && IsSlash(path[2]);
}

bool IsUncPath(const std::wstring& path) {
    return path.rfind(L"\\\\", 0) == 0;
}

std::wstring ToLower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return value;
}

std::wstring TrimTrailingSlashes(std::wstring path) {
    while (path.size() > 3 && IsSlash(path.back())) {
        path.pop_back();
    }
    return path;
}

std::wstring ExpandLongPathIfExists(const std::wstring& path) {
    DWORD required = GetLongPathNameW(path.c_str(), nullptr, 0);
    if (required == 0) {
        return path;
    }

    std::vector<wchar_t> buffer(required);
    DWORD length = GetLongPathNameW(path.c_str(), buffer.data(), required);
    if (length == 0 || length >= required) {
        return path;
    }

    return std::wstring(buffer.data(), length);
}

std::wstring ExpandLongPathBestEffort(const std::wstring& path) {
    const std::wstring expanded = ExpandLongPathIfExists(path);
    if (expanded != path) {
        return expanded;
    }

    const size_t separator = path.find_last_of(L"\\/");
    if (separator == std::wstring::npos || separator < 3 || separator + 1 >= path.size()) {
        return path;
    }

    const std::wstring parent = ExpandLongPathBestEffort(path.substr(0, separator));
    const std::wstring leaf = path.substr(separator + 1);
    return IsSlash(parent.back()) ? parent + leaf : parent + L"\\" + leaf;
}

bool SameOrChildPath(const std::wstring& maybeParent, const std::wstring& maybeChild) {
    const std::wstring parent = ToLower(TrimTrailingSlashes(maybeParent));
    const std::wstring child = ToLower(TrimTrailingSlashes(maybeChild));

    if (parent == child) {
        return true;
    }

    if (child.size() <= parent.size()) {
        return false;
    }

    return child.compare(0, parent.size(), parent) == 0 && IsSlash(child[parent.size()]);
}

RuleValidationResult Error(RuleValidationCode code, const wchar_t* message) {
    return RuleValidationResult{code, message};
}

RuleValidationResult Error(RuleValidationCode code, const std::wstring& message) {
    return RuleValidationResult{code, message};
}

std::wstring JoinPath(const std::wstring& left, const std::wstring& right) {
    if (left.empty()) {
        return right;
    }
    if (right.empty()) {
        return left;
    }
    if (IsSlash(left.back())) {
        return left + right;
    }
    return left + L"\\" + right;
}

std::wstring PathWithoutDriveColon(const std::wstring& path) {
    std::wstring value = path;
    if (value.size() >= 2 && value[1] == L':') {
        value.erase(1, 1);
    }
    return value;
}

}  // namespace

std::wstring ProductName() {
    return kProductName;
}

std::wstring DefaultStoreRoot() {
    std::vector<wchar_t> buffer(MAX_PATH);
    DWORD length = GetEnvironmentVariableW(L"ProgramData", buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0) {
        return L"C:\\ProgramData\\PathOverlay\\Boxes\\Default";
    }
    if (length >= buffer.size()) {
        buffer.resize(length + 1);
        length = GetEnvironmentVariableW(L"ProgramData", buffer.data(), static_cast<DWORD>(buffer.size()));
    }
    return JoinPath(std::wstring(buffer.data(), length), L"PathOverlay\\Boxes\\Default");
}

std::wstring DefaultStoreRootForRule(const std::wstring& ruleId) {
    std::wstring safeId = ruleId.empty() ? L"default" : ruleId;
    for (wchar_t& ch : safeId) {
        if (!(std::iswalnum(ch) || ch == L'-' || ch == L'_')) {
            ch = L'_';
        }
    }

    const std::wstring root = DefaultStoreRoot();
    const size_t leaf = root.find_last_of(L"\\/");
    const std::wstring boxesRoot = leaf == std::wstring::npos ? root : root.substr(0, leaf);
    return JoinPath(boxesRoot, safeId);
}

std::wstring GenerateRuleId() {
    SYSTEMTIME time = {};
    GetSystemTime(&time);

    std::wstringstream stream;
    stream << L"rule-"
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

std::wstring NormalizePath(const std::wstring& path) {
    if (path.empty()) {
        return L"";
    }

    std::wstring slashNormalized = path;
    std::replace(slashNormalized.begin(), slashNormalized.end(), L'/', L'\\');

    DWORD required = GetFullPathNameW(slashNormalized.c_str(), 0, nullptr, nullptr);
    if (required == 0) {
        return TrimTrailingSlashes(slashNormalized);
    }

    std::vector<wchar_t> buffer(required);
    DWORD length = GetFullPathNameW(slashNormalized.c_str(), required, buffer.data(), nullptr);
    if (length == 0 || length >= required) {
        return TrimTrailingSlashes(slashNormalized);
    }

    return TrimTrailingSlashes(ExpandLongPathBestEffort(std::wstring(buffer.data(), length)));
}

RuleValidationResult ValidateOverlayRule(const OverlayRule& rule) {
    const std::wstring source = NormalizePath(rule.source);
    const std::wstring store = NormalizePath(rule.store.empty() ? DefaultStoreRoot() : rule.store);

    if (source.empty() || store.empty()) {
        return Error(RuleValidationCode::kEmptyPath, L"source and store must not be empty");
    }
    if (IsUncPath(source) || IsUncPath(store)) {
        return Error(RuleValidationCode::kUncPathUnsupported, L"UNC paths are not supported in MVP");
    }
    if (IsDriveRootPath(source)) {
        return Error(RuleValidationCode::kDriveRootUnsupported, L"drive root source is not supported in MVP");
    }
    if (SameOrChildPath(source, store) || SameOrChildPath(store, source)) {
        return Error(RuleValidationCode::kSourceStoreOverlap, L"source and store must not contain each other");
    }

    const DWORD attributes = GetFileAttributesW(source.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return Error(RuleValidationCode::kSourceDoesNotExist, L"source path does not exist");
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return Error(RuleValidationCode::kSourceIsNotDirectory, L"source path must be a directory");
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return Error(RuleValidationCode::kSourceIsReparsePoint, L"source reparse points are not supported in MVP");
    }

    const DWORD storeAttributes = GetFileAttributesW(store.c_str());
    if (storeAttributes != INVALID_FILE_ATTRIBUTES &&
        (storeAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return Error(RuleValidationCode::kStoreIsNotDirectory, L"store path must be a directory");
    }

    return RuleValidationResult{};
}

RuleValidationResult ValidateOverlayRuleSet(
    const std::vector<OverlayRule>& existingRules,
    const OverlayRule& candidate) {
    const RuleValidationResult candidateResult = ValidateOverlayRule(candidate);
    if (!candidateResult.ok()) {
        return candidateResult;
    }

    const std::wstring candidateSource = NormalizePath(candidate.source);
    const std::wstring candidateStore = NormalizePath(candidate.store.empty() ? DefaultStoreRoot() : candidate.store);

    for (const auto& existing : existingRules) {
        if (existing.id == candidate.id) {
            continue;
        }

        const RuleValidationResult existingResult = ValidateOverlayRule(existing);
        if (!existingResult.ok()) {
            return Error(
                existingResult.code,
                L"existing rule '" + existing.id + L"' is invalid: " + existingResult.message);
        }

        const std::wstring existingSource = NormalizePath(existing.source);
        const std::wstring existingStore = NormalizePath(existing.store.empty() ? DefaultStoreRoot() : existing.store);

        if (SameOrChildPath(existingSource, candidateSource) ||
            SameOrChildPath(candidateSource, existingSource)) {
            return Error(RuleValidationCode::kRuleOverlap, L"rule sources must not overlap");
        }

        if (SameOrChildPath(existingSource, candidateStore) ||
            SameOrChildPath(candidateStore, existingSource) ||
            SameOrChildPath(candidateSource, existingStore) ||
            SameOrChildPath(existingStore, candidateSource)) {
            return Error(RuleValidationCode::kSourceStoreOverlap, L"rule sources and stores must not contain each other");
        }
    }

    return RuleValidationResult{};
}

RuleValidationResult ValidateRealPathInRule(const OverlayRule& rule, const std::wstring& realPath) {
    const RuleValidationResult ruleResult = ValidateOverlayRule(rule);
    if (!ruleResult.ok()) {
        return ruleResult;
    }

    const std::wstring source = NormalizePath(rule.source);
    const std::wstring real = NormalizePath(realPath);
    if (!SameOrChildPath(source, real)) {
        return Error(RuleValidationCode::kRealPathOutsideSource, L"real path is outside source");
    }

    return RuleValidationResult{};
}

std::optional<std::wstring> MapRealPathToShadowPath(const OverlayRule& rule, const std::wstring& realPath) {
    if (!ValidateRealPathInRule(rule, realPath).ok()) {
        return std::nullopt;
    }

    const std::wstring real = NormalizePath(realPath);
    const std::wstring store = NormalizePath(rule.store.empty() ? DefaultStoreRoot() : rule.store);
    return JoinPath(JoinPath(store, L"drive"), PathWithoutDriveColon(real));
}

bool IsReparsePointPath(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(NormalizePath(path).c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

std::optional<std::wstring> FindFirstReparsePointInRulePath(
    const OverlayRule& rule,
    const std::wstring& realPath) {
    const std::wstring source = NormalizePath(rule.source);
    const std::wstring path = NormalizePath(realPath);
    if (!SameOrChildPath(source, path) || ToLower(source) == ToLower(path)) {
        return std::nullopt;
    }

    std::wstring cursor = source;
    size_t offset = source.size();
    while (offset < path.size()) {
        if (IsSlash(path[offset])) {
            ++offset;
            continue;
        }

        const size_t next = path.find(L'\\', offset);
        const std::wstring component = next == std::wstring::npos
            ? path.substr(offset)
            : path.substr(offset, next - offset);
        cursor = JoinPath(cursor, component);
        if (IsReparsePointPath(cursor)) {
            return cursor;
        }
        if (next == std::wstring::npos) {
            break;
        }
        offset = next + 1;
    }

    return std::nullopt;
}

std::vector<std::wstring> ListReparsePointsUnderRule(
    const OverlayRule& rule,
    size_t maxEntries,
    std::wstring* error) {
    std::vector<std::wstring> result;
    const std::wstring source = NormalizePath(rule.source);
    if (source.empty() || maxEntries == 0 || GetFileAttributesW(source.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return result;
    }

    std::error_code errorCode;
    std::filesystem::recursive_directory_iterator iterator(
        std::filesystem::path(source),
        std::filesystem::directory_options::skip_permission_denied,
        errorCode);
    const std::filesystem::recursive_directory_iterator end;
    if (errorCode) {
        if (error != nullptr) {
            *error = L"failed to enumerate rule source for reparse diagnostics";
        }
        return result;
    }

    while (iterator != end && result.size() < maxEntries) {
        const std::wstring path = NormalizePath(iterator->path().wstring());
        if (IsReparsePointPath(path)) {
            result.push_back(path);
            iterator.disable_recursion_pending();
        }

        iterator.increment(errorCode);
        if (errorCode) {
            if (error != nullptr) {
                *error = L"failed to enumerate rule source for reparse diagnostics";
            }
            break;
        }
    }

    return result;
}

}  // namespace pathoverlay
