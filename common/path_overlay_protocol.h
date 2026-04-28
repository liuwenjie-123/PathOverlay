#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define PATHOVERLAY_PROTOCOL_VERSION 2
#define PATHOVERLAY_MAX_PATH_CHARS 520
#define PATHOVERLAY_MAX_RULE_ID_CHARS 64
#define PATHOVERLAY_MAX_DRIVER_RULES 16

typedef enum PATHOVERLAY_DRIVER_COMMAND {
    PathOverlayDriverCommandClearRule = 1,
    PathOverlayDriverCommandSetRule = 2
} PATHOVERLAY_DRIVER_COMMAND;

typedef enum PATHOVERLAY_SERVICE_COMMAND {
    PathOverlayServiceCommandQueryPath = 1,
    PathOverlayServiceCommandPrepareCopyOnWrite = 2,
    PathOverlayServiceCommandRecordDelete = 3,
    PathOverlayServiceCommandPrepareDirectoryView = 4,
    PathOverlayServiceCommandRecordRename = 5
} PATHOVERLAY_SERVICE_COMMAND;

typedef enum PATHOVERLAY_PATH_STATE {
    PathOverlayPathStateNormal = 0,
    PathOverlayPathStateTombstone = 1
} PATHOVERLAY_PATH_STATE;

typedef struct PATHOVERLAY_DRIVER_RULE_MESSAGE {
    unsigned long Version;
    unsigned long Command;
    unsigned long Enabled;
    unsigned long ServiceProcessId;
    wchar_t RuleId[PATHOVERLAY_MAX_RULE_ID_CHARS];
    wchar_t SourceNtPath[PATHOVERLAY_MAX_PATH_CHARS];
    wchar_t SourceAliasNtPath[PATHOVERLAY_MAX_PATH_CHARS];
    wchar_t StoreNtPath[PATHOVERLAY_MAX_PATH_CHARS];
} PATHOVERLAY_DRIVER_RULE_MESSAGE;

typedef struct PATHOVERLAY_DRIVER_RESPONSE {
    long Status;
} PATHOVERLAY_DRIVER_RESPONSE;

typedef struct PATHOVERLAY_SERVICE_REQUEST {
    unsigned long Version;
    unsigned long Command;
    wchar_t RuleId[PATHOVERLAY_MAX_RULE_ID_CHARS];
    wchar_t RealNtPath[PATHOVERLAY_MAX_PATH_CHARS];
    wchar_t TargetNtPath[PATHOVERLAY_MAX_PATH_CHARS];
} PATHOVERLAY_SERVICE_REQUEST;

typedef struct PATHOVERLAY_SERVICE_RESPONSE {
    long Status;
    unsigned long PathState;
    wchar_t ShadowNtPath[PATHOVERLAY_MAX_PATH_CHARS];
} PATHOVERLAY_SERVICE_RESPONSE;

#ifdef __cplusplus
}
#endif
