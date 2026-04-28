#pragma once

#include <string>
#include <vector>

#include "metadata_store.h"

namespace pathoverlay {

struct OperationResult {
    bool success = false;
    std::wstring message;
};

class OverlayOperations {
public:
    explicit OverlayOperations(MetadataStore& metadata);

    OperationResult PrepareCopyOnWrite(const OverlayRule& rule, const std::wstring& realPath, std::wstring* shadowPath);
    OperationResult PrepareDirectoryView(const OverlayRule& rule, const std::wstring& realPath, std::wstring* shadowPath);
    OperationResult PrepareRenamedTargetPath(const OverlayRule& rule, const std::wstring& realPath, std::wstring* shadowPath);
    OperationResult RecordCreatedFile(const OverlayRule& rule, const std::wstring& realPath);
    OperationResult RecordDelete(const OverlayRule& rule, const std::wstring& realPath);
    OperationResult RecordRename(const OverlayRule& rule, const std::wstring& sourceRealPath, const std::wstring& targetRealPath);
    OperationResult ListChanges(const std::wstring& ruleId, std::vector<ChangeRecord>* records);
    OperationResult Commit(const OverlayRule& rule, const std::wstring& commitId);
    OperationResult Discard(const OverlayRule& rule);

private:
    MetadataStore& metadata_;
};

}  // namespace pathoverlay
