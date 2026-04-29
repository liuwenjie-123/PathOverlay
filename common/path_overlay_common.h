#pragma once

#include <optional>
#include <string>
#include <vector>

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
    kStoreIsNotDirectory,
    kSourceStoreOverlap,
    kRuleOverlap,
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
std::wstring DefaultStoreRootForRule(const std::wstring& ruleId);
std::wstring GenerateRuleId();
std::wstring NormalizePath(const std::wstring& path);
RuleValidationResult ValidateOverlayRule(const OverlayRule& rule);
RuleValidationResult ValidateOverlayRuleSet(
    const std::vector<OverlayRule>& existingRules,
    const OverlayRule& candidate);
RuleValidationResult ValidateRealPathInRule(const OverlayRule& rule, const std::wstring& realPath);
std::optional<std::wstring> MapRealPathToShadowPath(const OverlayRule& rule, const std::wstring& realPath);
bool IsReparsePointPath(const std::wstring& path);
std::optional<std::wstring> FindFirstReparsePointInRulePath(
    const OverlayRule& rule,
    const std::wstring& realPath);
std::vector<std::wstring> ListReparsePointsUnderRule(
    const OverlayRule& rule,
    size_t maxEntries,
    std::wstring* error);

}  // namespace pathoverlay
