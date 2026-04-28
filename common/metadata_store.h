#pragma once

#include <string>
#include <vector>

#include "path_overlay_common.h"

namespace pathoverlay {

enum class ChangeState {
    kCreated,
    kModified,
    kDeleted,
    kTombstone,
    kRenamed,
};

struct ChangeRecord {
    std::wstring realPath;
    std::wstring shadowPath;
    std::wstring targetPath;
    ChangeState state = ChangeState::kCreated;
    bool originalExists = false;
    unsigned long long originalSize = 0;
    std::wstring originalLastWriteTime;
    unsigned long long currentSize = 0;
    std::wstring lastWriteTime;
};

struct CommitRecord {
    std::wstring id;
    std::wstring ruleId;
    std::wstring startTime;
    std::wstring status;
    std::wstring operations;
    std::wstring backupPath;
    std::wstring error;
};

class MetadataStore {
public:
    MetadataStore();
    ~MetadataStore();

    MetadataStore(const MetadataStore&) = delete;
    MetadataStore& operator=(const MetadataStore&) = delete;

    bool Open(const std::wstring& databasePath, std::wstring* error);
    void Close();

    bool Initialize(std::wstring* error);
    bool UpsertRule(const OverlayRule& rule, std::wstring* error);
    bool SetRuleEnabled(const std::wstring& ruleId, bool enabled, std::wstring* error);
    bool GetRule(const std::wstring& ruleId, OverlayRule* rule, std::wstring* error);
    bool ListRules(std::vector<OverlayRule>* rules, std::wstring* error);
    bool DeleteRule(const std::wstring& ruleId, std::wstring* error);
    bool AddOrUpdateChange(const std::wstring& ruleId, const ChangeRecord& record, std::wstring* error);
    bool ListChanges(const std::wstring& ruleId, std::vector<ChangeRecord>* records, std::wstring* error);
    bool DeleteChange(const std::wstring& ruleId, const std::wstring& realPath, std::wstring* error);
    bool DeleteChangesForRule(const std::wstring& ruleId, std::wstring* error);
    bool AddCommitRecord(const CommitRecord& record, std::wstring* error);
    bool GetCommitRecord(const std::wstring& commitId, CommitRecord* record, std::wstring* error);

private:
    void* sqliteLibrary_ = nullptr;
    void* database_ = nullptr;
};

std::wstring ChangeStateToString(ChangeState state);
bool TryParseChangeState(const std::wstring& value, ChangeState* state);

}  // namespace pathoverlay
