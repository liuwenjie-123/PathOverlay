#pragma once

#include <optional>
#include <string>

namespace pathoverlay {

constexpr wchar_t kProductName[] = L"PathOverlay";

enum class RuleValidationCode {
    kOk,
    kEmptyPath,
    kUncPathUnsupported,
    kDriveRootUnsupported,
    kSourceDoesNotExist,
    kSourceIsNotDirectory,
    kSourceIsReparsePoint,
    kSourceStoreOverlap,
    kRealPathOutsideSource,
};

struct RuleValidationResult {
    RuleValidationCode code = RuleValidationCode::kOk;
    std::wstring message;

    bool ok() const {
        return code == RuleValidationCode::kOk;
    }
};

struct OverlayRule {
    std::wstring id;
    std::wstring name;
    bool enabled = false;
    std::wstring source;
    std::wstring store;
};

std::wstring ProductName();
std::wstring DefaultStoreRoot();
std::wstring NormalizePath(const std::wstring& path);
RuleValidationResult ValidateOverlayRule(const OverlayRule& rule);
RuleValidationResult ValidateRealPathInRule(const OverlayRule& rule, const std::wstring& realPath);
std::optional<std::wstring> MapRealPathToShadowPath(const OverlayRule& rule, const std::wstring& realPath);

}  // namespace pathoverlay
