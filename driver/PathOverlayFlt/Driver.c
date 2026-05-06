#include <fltKernel.h>
#include <ntstrsafe.h>

#include "path_overlay_protocol.h"

#define PATHOVERLAY_PORT_NAME L"\\PathOverlayPort"

DRIVER_INITIALIZE DriverEntry;

typedef struct PATHOVERLAY_RULE_ENTRY {
    WCHAR RuleId[PATHOVERLAY_MAX_RULE_ID_CHARS];
    WCHAR SourceNtPath[PATHOVERLAY_MAX_PATH_CHARS];
    WCHAR SourceAliasNtPath[PATHOVERLAY_MAX_PATH_CHARS];
    WCHAR StoreNtPath[PATHOVERLAY_MAX_PATH_CHARS];
} PATHOVERLAY_RULE_ENTRY;

typedef struct PATHOVERLAY_RULE_CACHE {
    ULONG Count;
    ULONG ServiceProcessId;
    PATHOVERLAY_RULE_ENTRY Rules[PATHOVERLAY_MAX_DRIVER_RULES];
} PATHOVERLAY_RULE_CACHE;

static PFLT_FILTER gFilterHandle = NULL;
static PFLT_PORT gServerPort = NULL;
static PFLT_PORT gClientPort = NULL;
static FAST_MUTEX gRuleLock;
static PATHOVERLAY_RULE_CACHE gRuleCache = { 0 };

static NTSTATUS
PathOverlayUnload(
    _In_ FLT_FILTER_UNLOAD_FLAGS Flags
    );

static FLT_PREOP_CALLBACK_STATUS
PathOverlayPreCreate(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID* CompletionContext
    );

static FLT_PREOP_CALLBACK_STATUS
PathOverlayPreSetInformation(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID* CompletionContext
    );

static FLT_PREOP_CALLBACK_STATUS
PathOverlayPreDirectoryControl(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID* CompletionContext
    );

static FLT_PREOP_CALLBACK_STATUS
PathOverlayPreNetworkQueryOpen(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID* CompletionContext
    );

static FLT_PREOP_CALLBACK_STATUS
PathOverlayPreQueryOpen(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID* CompletionContext
    );

static NTSTATUS
PathOverlayResolveQueryShadowPath(
    _In_ PFLT_CALLBACK_DATA Data,
    _Out_ PUNICODE_STRING ShadowPath,
    _Out_ PBOOLEAN Tombstone
    );

static NTSTATUS
PathOverlayQueryPathInformation(
    _In_ PFLT_INSTANCE Instance,
    _In_ PCUNICODE_STRING NtPath,
    _Out_writes_bytes_(Length) PVOID FileInformation,
    _In_ ULONG Length,
    _In_ FILE_INFORMATION_CLASS FileInformationClass,
    _Out_opt_ PULONG LengthReturned
    );

static NTSTATUS
PathOverlayBuildShadowPath(
    _In_ PCUNICODE_STRING Store,
    _In_ PCUNICODE_STRING Source,
    _In_ PCUNICODE_STRING RealPath,
    _Out_ PUNICODE_STRING ShadowPath
    );

static NTSTATUS
PathOverlayAllocateUnicodeStringCopy(
    _In_ PCUNICODE_STRING Source,
    _Out_ PUNICODE_STRING Destination,
    _In_ ULONG Tag
    );

static NTSTATUS
PathOverlayConnect(
    _In_ PFLT_PORT ClientPort,
    _In_opt_ PVOID ServerPortCookie,
    _In_reads_bytes_opt_(SizeOfContext) PVOID ConnectionContext,
    _In_ ULONG SizeOfContext,
    _Outptr_result_maybenull_ PVOID* ConnectionCookie
    );

static VOID
PathOverlayDisconnect(
    _In_opt_ PVOID ConnectionCookie
    );

static NTSTATUS
PathOverlayMessage(
    _In_opt_ PVOID PortCookie,
    _In_reads_bytes_opt_(InputBufferLength) PVOID InputBuffer,
    _In_ ULONG InputBufferLength,
    _Out_writes_bytes_to_opt_(OutputBufferLength, *ReturnOutputBufferLength) PVOID OutputBuffer,
    _In_ ULONG OutputBufferLength,
    _Out_ PULONG ReturnOutputBufferLength
    );

static CONST FLT_OPERATION_REGISTRATION kCallbacks[] = {
    {
        IRP_MJ_CREATE,
        0,
        PathOverlayPreCreate,
        NULL
    },
    {
        IRP_MJ_SET_INFORMATION,
        0,
        PathOverlayPreSetInformation,
        NULL
    },
    {
        IRP_MJ_DIRECTORY_CONTROL,
        0,
        PathOverlayPreDirectoryControl,
        NULL
    },
    {
        IRP_MJ_NETWORK_QUERY_OPEN,
        0,
        PathOverlayPreNetworkQueryOpen,
        NULL
    },
    {
        IRP_MJ_QUERY_OPEN,
        0,
        PathOverlayPreQueryOpen,
        NULL
    },
    {
        IRP_MJ_OPERATION_END
    }
};

static CONST FLT_REGISTRATION kFilterRegistration = {
    sizeof(FLT_REGISTRATION),
    FLT_REGISTRATION_VERSION,
    0,
    NULL,
    kCallbacks,
    PathOverlayUnload,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL
};

static VOID
PathOverlayCopyNullTerminated(
    _Out_writes_(DestinationChars) PWCHAR Destination,
    _In_ ULONG DestinationChars,
    _In_reads_(SourceChars) PCWCH Source,
    _In_ ULONG SourceChars
    )
{
    ULONG index;
    ULONG charsToCopy = SourceChars;

    if (DestinationChars == 0) {
        return;
    }

    if (charsToCopy >= DestinationChars) {
        charsToCopy = DestinationChars - 1;
    }

    for (index = 0; index < charsToCopy; ++index) {
        Destination[index] = Source[index];
    }
    Destination[charsToCopy] = UNICODE_NULL;
}

static ULONG
PathOverlayNullTerminatedLength(
    _In_reads_(MaxChars) const WCHAR* Value,
    _In_ ULONG MaxChars
    )
{
    ULONG length = 0;
    while (length < MaxChars && Value[length] != UNICODE_NULL) {
        ++length;
    }
    return length;
}

static VOID
PathOverlayTrimTrailingSlashes(
    _Inout_updates_(PATHOVERLAY_MAX_PATH_CHARS) WCHAR* Value
    )
{
    ULONG length = PathOverlayNullTerminatedLength(Value, PATHOVERLAY_MAX_PATH_CHARS);
    while (length > 0 && Value[length - 1] == L'\\') {
        Value[length - 1] = UNICODE_NULL;
        --length;
    }
}

static VOID
PathOverlayClearRule()
{
    ExAcquireFastMutex(&gRuleLock);
    RtlZeroMemory(&gRuleCache, sizeof(gRuleCache));
    ExReleaseFastMutex(&gRuleLock);
}

static NTSTATUS
PathOverlaySetRule(
    _In_ const PATHOVERLAY_DRIVER_RULE_MESSAGE* Message
    )
{
    PATHOVERLAY_RULE_ENTRY* entry;

    if (Message->Enabled == 0) {
        return STATUS_SUCCESS;
    }

    if (Message->RuleId[0] == UNICODE_NULL ||
        Message->SourceNtPath[0] == UNICODE_NULL ||
        Message->StoreNtPath[0] == UNICODE_NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    ExAcquireFastMutex(&gRuleLock);
    if (gRuleCache.Count >= PATHOVERLAY_MAX_DRIVER_RULES) {
        ExReleaseFastMutex(&gRuleLock);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    gRuleCache.ServiceProcessId = Message->ServiceProcessId;
    entry = &gRuleCache.Rules[gRuleCache.Count];
    RtlZeroMemory(entry, sizeof(*entry));
    PathOverlayCopyNullTerminated(
        entry->RuleId,
        PATHOVERLAY_MAX_RULE_ID_CHARS,
        Message->RuleId,
        PathOverlayNullTerminatedLength(Message->RuleId, PATHOVERLAY_MAX_RULE_ID_CHARS));
    PathOverlayCopyNullTerminated(
        entry->SourceNtPath,
        PATHOVERLAY_MAX_PATH_CHARS,
        Message->SourceNtPath,
        PathOverlayNullTerminatedLength(Message->SourceNtPath, PATHOVERLAY_MAX_PATH_CHARS));
    PathOverlayCopyNullTerminated(
        entry->SourceAliasNtPath,
        PATHOVERLAY_MAX_PATH_CHARS,
        Message->SourceAliasNtPath,
        PathOverlayNullTerminatedLength(Message->SourceAliasNtPath, PATHOVERLAY_MAX_PATH_CHARS));
    PathOverlayCopyNullTerminated(
        entry->StoreNtPath,
        PATHOVERLAY_MAX_PATH_CHARS,
        Message->StoreNtPath,
        PathOverlayNullTerminatedLength(Message->StoreNtPath, PATHOVERLAY_MAX_PATH_CHARS));
    PathOverlayTrimTrailingSlashes(entry->SourceNtPath);
    PathOverlayTrimTrailingSlashes(entry->SourceAliasNtPath);
    PathOverlayTrimTrailingSlashes(entry->StoreNtPath);
    ++gRuleCache.Count;
    ExReleaseFastMutex(&gRuleLock);

    return STATUS_SUCCESS;
}

static BOOLEAN
PathOverlaySameOrChildPath(
    _In_ PCUNICODE_STRING Parent,
    _In_ PCUNICODE_STRING Child
    )
{
    UNICODE_STRING childPrefix;
    USHORT parentChars;

    if (Parent->Length == 0 || Child->Length < Parent->Length) {
        return FALSE;
    }

    childPrefix.Buffer = Child->Buffer;
    childPrefix.Length = Parent->Length;
    childPrefix.MaximumLength = Parent->Length;
    if (!RtlEqualUnicodeString(Parent, &childPrefix, TRUE)) {
        return FALSE;
    }

    if (Child->Length == Parent->Length) {
        return TRUE;
    }

    parentChars = Parent->Length / sizeof(WCHAR);
    return Child->Buffer[parentChars] == L'\\';
}

static BOOLEAN
PathOverlayNextPathComponent(
    _In_reads_(TotalChars) const WCHAR* Value,
    _In_ USHORT TotalChars,
    _Inout_ USHORT* OffsetChars,
    _Out_ USHORT* ComponentStartChars,
    _Out_ USHORT* ComponentLengthChars
    )
{
    USHORT index = *OffsetChars;

    while (index < TotalChars && Value[index] == L'\\') {
        ++index;
    }
    if (index >= TotalChars) {
        *OffsetChars = index;
        return FALSE;
    }

    *ComponentStartChars = index;
    while (index < TotalChars && Value[index] != L'\\') {
        ++index;
    }

    *ComponentLengthChars = index - *ComponentStartChars;
    *OffsetChars = index;
    return TRUE;
}

static BOOLEAN
PathOverlayEqualPathComponent(
    _In_reads_(LeftChars) const WCHAR* Left,
    _In_ USHORT LeftChars,
    _In_reads_(RightChars) const WCHAR* Right,
    _In_ USHORT RightChars
    )
{
    UNICODE_STRING leftString;
    UNICODE_STRING rightString;

    leftString.Buffer = (PWCHAR)Left;
    leftString.Length = LeftChars * sizeof(WCHAR);
    leftString.MaximumLength = leftString.Length;

    rightString.Buffer = (PWCHAR)Right;
    rightString.Length = RightChars * sizeof(WCHAR);
    rightString.MaximumLength = rightString.Length;

    return RtlEqualUnicodeString(&leftString, &rightString, TRUE);
}

static BOOLEAN
PathOverlayMatchSourcePathPrefix(
    _In_ PCUNICODE_STRING Source,
    _In_opt_ PCUNICODE_STRING SourceAlias,
    _In_ PCUNICODE_STRING Path,
    _Out_ USHORT* MatchedPrefixChars
    )
{
    USHORT sourceChars = Source->Length / sizeof(WCHAR);
    USHORT sourceAliasChars = SourceAlias != NULL ? SourceAlias->Length / sizeof(WCHAR) : 0;
    USHORT pathChars = Path->Length / sizeof(WCHAR);
    USHORT sourceOffset = 0;
    USHORT sourceAliasOffset = 0;
    USHORT pathOffset = 0;
    USHORT sourceStart;
    USHORT sourceLength;
    USHORT sourceAliasStart = 0;
    USHORT sourceAliasLength = 0;
    USHORT pathStart;
    USHORT pathLength;
    BOOLEAN hasSourceAlias = SourceAlias != NULL && SourceAlias->Length > 0;

    if (Source->Length == 0 || Path->Length == 0) {
        return FALSE;
    }

    while (PathOverlayNextPathComponent(
        Source->Buffer,
        sourceChars,
        &sourceOffset,
        &sourceStart,
        &sourceLength)) {
        BOOLEAN longMatched;
        BOOLEAN aliasMatched = FALSE;
        BOOLEAN aliasComponentAvailable = FALSE;

        if (!PathOverlayNextPathComponent(
            Path->Buffer,
            pathChars,
            &pathOffset,
            &pathStart,
            &pathLength)) {
            return FALSE;
        }

        if (hasSourceAlias) {
            aliasComponentAvailable = PathOverlayNextPathComponent(
                SourceAlias->Buffer,
                sourceAliasChars,
                &sourceAliasOffset,
                &sourceAliasStart,
                &sourceAliasLength);
        }

        longMatched = PathOverlayEqualPathComponent(
            Path->Buffer + pathStart,
            pathLength,
            Source->Buffer + sourceStart,
            sourceLength);
        if (!longMatched && aliasComponentAvailable) {
            aliasMatched = PathOverlayEqualPathComponent(
                Path->Buffer + pathStart,
                pathLength,
                SourceAlias->Buffer + sourceAliasStart,
                sourceAliasLength);
        }

        if (!longMatched && !aliasMatched) {
            return FALSE;
        }
    }

    *MatchedPrefixChars = pathOffset;
    return TRUE;
}

static BOOLEAN
PathOverlayFindSourceRule(
    _In_ PCUNICODE_STRING Path,
    _Out_ PATHOVERLAY_RULE_ENTRY* MatchedRule,
    _Out_writes_opt_(PATHOVERLAY_MAX_PATH_CHARS) PWCHAR MatchedSourceNtPath
    )
{
    ULONG index;
    BOOLEAN found = FALSE;

    if (MatchedSourceNtPath != NULL) {
        MatchedSourceNtPath[0] = UNICODE_NULL;
    }

    for (index = 0; index < gRuleCache.Count; ++index) {
        UNICODE_STRING source;
        UNICODE_STRING sourceAlias;
        USHORT matchedPrefixChars = 0;
        BOOLEAN matched;

        RtlInitUnicodeString(&source, gRuleCache.Rules[index].SourceNtPath);
        if (gRuleCache.Rules[index].SourceAliasNtPath[0] != UNICODE_NULL) {
            RtlInitUnicodeString(&sourceAlias, gRuleCache.Rules[index].SourceAliasNtPath);
            matched = PathOverlayMatchSourcePathPrefix(&source, &sourceAlias, Path, &matchedPrefixChars);
        } else {
            matched = PathOverlayMatchSourcePathPrefix(&source, NULL, Path, &matchedPrefixChars);
        }

        if (!matched) {
            continue;
        }

        if (found) {
            return FALSE;
        }

        *MatchedRule = gRuleCache.Rules[index];
        if (MatchedSourceNtPath != NULL) {
            PathOverlayCopyNullTerminated(
                MatchedSourceNtPath,
                PATHOVERLAY_MAX_PATH_CHARS,
                Path->Buffer,
                matchedPrefixChars);
        }
        found = TRUE;
    }

    return found;
}

static BOOLEAN
PathOverlayFindStoreRule(
    _In_ PCUNICODE_STRING Path,
    _Out_ PATHOVERLAY_RULE_ENTRY* MatchedRule
    )
{
    ULONG index;
    BOOLEAN found = FALSE;

    for (index = 0; index < gRuleCache.Count; ++index) {
        UNICODE_STRING store;
        RtlInitUnicodeString(&store, gRuleCache.Rules[index].StoreNtPath);
        if (!PathOverlaySameOrChildPath(&store, Path)) {
            continue;
        }

        if (found) {
            return FALSE;
        }

        *MatchedRule = gRuleCache.Rules[index];
        found = TRUE;
    }

    return found;
}

static BOOLEAN
PathOverlayIsStorePathLocked(
    _In_ PCUNICODE_STRING Path
    )
{
    ULONG index;

    for (index = 0; index < gRuleCache.Count; ++index) {
        UNICODE_STRING store;
        RtlInitUnicodeString(&store, gRuleCache.Rules[index].StoreNtPath);
        if (PathOverlaySameOrChildPath(&store, Path)) {
            return TRUE;
        }
    }

    return FALSE;
}

static BOOLEAN
PathOverlayIsServiceProcess(
    _In_ ULONG ProcessId
    )
{
    BOOLEAN result;

    ExAcquireFastMutex(&gRuleLock);
    result = gRuleCache.Count > 0 && ProcessId == gRuleCache.ServiceProcessId;
    ExReleaseFastMutex(&gRuleLock);

    return result;
}

static BOOLEAN
PathOverlayFindSourceRuleForPath(
    _In_ PCUNICODE_STRING Path,
    _Out_ PATHOVERLAY_RULE_ENTRY* MatchedRule,
    _Out_writes_opt_(PATHOVERLAY_MAX_PATH_CHARS) PWCHAR MatchedSourceNtPath
    )
{
    BOOLEAN found;

    ExAcquireFastMutex(&gRuleLock);
    if (gRuleCache.Count == 0 || PathOverlayIsStorePathLocked(Path)) {
        ExReleaseFastMutex(&gRuleLock);
        return FALSE;
    }

    found = PathOverlayFindSourceRule(Path, MatchedRule, MatchedSourceNtPath);
    ExReleaseFastMutex(&gRuleLock);
    return found;
}

static BOOLEAN
PathOverlayFindSourceOrStoreRuleForPath(
    _In_ PCUNICODE_STRING Path,
    _Out_ PATHOVERLAY_RULE_ENTRY* MatchedRule,
    _Out_ BOOLEAN* SourcePath,
    _Out_ BOOLEAN* StorePath
    )
{
    BOOLEAN sourcePath;
    BOOLEAN storePath = FALSE;

    ExAcquireFastMutex(&gRuleLock);
    if (gRuleCache.Count == 0) {
        ExReleaseFastMutex(&gRuleLock);
        return FALSE;
    }

    sourcePath = PathOverlayFindSourceRule(Path, MatchedRule, NULL);
    if (!sourcePath) {
        storePath = PathOverlayFindStoreRule(Path, MatchedRule);
    }
    ExReleaseFastMutex(&gRuleLock);

    *SourcePath = sourcePath;
    *StorePath = storePath;
    return sourcePath || storePath;
}

static BOOLEAN
PathOverlayHasWriteIntent(
    _In_ PFLT_CALLBACK_DATA Data
    )
{
    ACCESS_MASK desiredAccess;
    ULONG options = Data->Iopb->Parameters.Create.Options;
    ULONG disposition = (options >> 24) & 0xff;

    if (disposition == FILE_CREATE ||
        disposition == FILE_OPEN_IF ||
        disposition == FILE_OVERWRITE ||
        disposition == FILE_OVERWRITE_IF ||
        disposition == FILE_SUPERSEDE) {
        return TRUE;
    }

    if (Data->Iopb->Parameters.Create.SecurityContext == NULL) {
        return FALSE;
    }

    desiredAccess = Data->Iopb->Parameters.Create.SecurityContext->DesiredAccess;
    if ((desiredAccess & (FILE_WRITE_DATA |
                          FILE_APPEND_DATA |
                          FILE_WRITE_EA |
                          FILE_WRITE_ATTRIBUTES |
                          FILE_DELETE_CHILD |
                          DELETE |
                          WRITE_DAC |
                          WRITE_OWNER |
                          GENERIC_WRITE |
                          GENERIC_ALL)) != 0) {
        return TRUE;
    }

    return FALSE;
}

static ACCESS_MASK
PathOverlayGetCreateDesiredAccess(
    _In_ PFLT_CALLBACK_DATA Data
    )
{
    if (Data->Iopb->Parameters.Create.SecurityContext == NULL) {
        return 0;
    }

    return Data->Iopb->Parameters.Create.SecurityContext->DesiredAccess;
}

static BOOLEAN
PathOverlayIsDirectoryOpen(
    _In_ PFLT_CALLBACK_DATA Data
    )
{
    return (Data->Iopb->Parameters.Create.Options & FILE_DIRECTORY_FILE) != 0;
}

static BOOLEAN
PathOverlayHasDirectoryCreateHint(
    _In_ PFLT_CALLBACK_DATA Data
    )
{
    return (Data->Iopb->Parameters.Create.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
           (Data->Iopb->Parameters.Create.Options & FILE_NON_DIRECTORY_FILE) == 0;
}

static BOOLEAN
PathOverlayShouldPassThroughReparsePointOpen(
    _In_ PFLT_CALLBACK_DATA Data
    )
{
    ULONG options = Data->Iopb->Parameters.Create.Options;
    ACCESS_MASK desiredAccess;
    ULONG disposition;

    if ((options & FILE_OPEN_REPARSE_POINT) == 0) {
        return FALSE;
    }

    disposition = (options >> 24) & 0xff;
    if (disposition == FILE_CREATE ||
        disposition == FILE_OPEN_IF ||
        disposition == FILE_OVERWRITE ||
        disposition == FILE_OVERWRITE_IF ||
        disposition == FILE_SUPERSEDE) {
        desiredAccess = PathOverlayGetCreateDesiredAccess(Data);
        if ((desiredAccess & (FILE_WRITE_ATTRIBUTES | GENERIC_WRITE | GENERIC_ALL)) != 0) {
            return TRUE;
        }
        return FALSE;
    }

    return TRUE;
}

static BOOLEAN
PathOverlayIsSameUnicodeStringInsensitive(
    _In_ PCUNICODE_STRING Left,
    _In_ PCUNICODE_STRING Right
    )
{
    return RtlCompareUnicodeString(Left, Right, TRUE) == 0;
}

static BOOLEAN
PathOverlayUnicodeStringContainsInsensitive(
    _In_opt_ PCUNICODE_STRING Value,
    _In_z_ PCWSTR Needle
    )
{
    ULONG valueChars;
    ULONG needleChars = 0;
    ULONG valueIndex;
    ULONG needleIndex;

    if (Value == NULL || Value->Buffer == NULL || Needle == NULL) {
        return FALSE;
    }

    valueChars = Value->Length / sizeof(WCHAR);
    while (Needle[needleChars] != UNICODE_NULL) {
        ++needleChars;
    }
    if (needleChars == 0 || valueChars < needleChars) {
        return FALSE;
    }

    for (valueIndex = 0; valueIndex <= valueChars - needleChars; ++valueIndex) {
        BOOLEAN matched = TRUE;
        for (needleIndex = 0; needleIndex < needleChars; ++needleIndex) {
            if (RtlUpcaseUnicodeChar(Value->Buffer[valueIndex + needleIndex]) !=
                RtlUpcaseUnicodeChar(Needle[needleIndex])) {
                matched = FALSE;
                break;
            }
        }
        if (matched) {
            return TRUE;
        }
    }

    return FALSE;
}

static BOOLEAN
PathOverlayNtPathExists(
    _In_ PCUNICODE_STRING NtPath
    )
{
    OBJECT_ATTRIBUTES attributes;
    IO_STATUS_BLOCK ioStatus = { 0 };
    HANDLE fileHandle = NULL;
    PFILE_OBJECT fileObject = NULL;
    NTSTATUS status;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return FALSE;
    }

    InitializeObjectAttributes(
        &attributes,
        (PUNICODE_STRING)NtPath,
        OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
        NULL,
        NULL);

    status = FltCreateFileEx(
        gFilterHandle,
        NULL,
        &fileHandle,
        &fileObject,
        FILE_READ_ATTRIBUTES,
        &attributes,
        &ioStatus,
        NULL,
        FILE_ATTRIBUTE_NORMAL,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        FILE_OPEN,
        FILE_OPEN_REPARSE_POINT | FILE_OPEN_FOR_BACKUP_INTENT,
        NULL,
        0,
        0);

    if (fileObject != NULL) {
        ObDereferenceObject(fileObject);
    }
    if (fileHandle != NULL) {
        FltClose(fileHandle);
    }

    return NT_SUCCESS(status);
}

static NTSTATUS
PathOverlaySendServiceRequest(
    _In_ ULONG Command,
    _In_reads_(PATHOVERLAY_MAX_RULE_ID_CHARS) const WCHAR* RuleId,
    _In_ PCUNICODE_STRING RealPath,
    _In_opt_ PCUNICODE_STRING TargetPath,
    _Out_opt_ PATHOVERLAY_SERVICE_RESPONSE* ServiceResponse
    )
{
    NTSTATUS status;
    PATHOVERLAY_SERVICE_REQUEST request;
    PATHOVERLAY_SERVICE_RESPONSE response = { 0 };
    ULONG replyLength = sizeof(response);
    LARGE_INTEGER timeout;

    if (gClientPort == NULL) {
        return STATUS_PORT_DISCONNECTED;
    }

    if (RealPath->Length >= sizeof(request.RealNtPath)) {
        return STATUS_NAME_TOO_LONG;
    }
    if (TargetPath != NULL && TargetPath->Length >= sizeof(request.TargetNtPath)) {
        return STATUS_NAME_TOO_LONG;
    }

    RtlZeroMemory(&request, sizeof(request));
    request.Version = PATHOVERLAY_PROTOCOL_VERSION;
    request.Command = Command;
    PathOverlayCopyNullTerminated(
        request.RuleId,
        PATHOVERLAY_MAX_RULE_ID_CHARS,
        RuleId,
        PathOverlayNullTerminatedLength(RuleId, PATHOVERLAY_MAX_RULE_ID_CHARS));
    PathOverlayCopyNullTerminated(
        request.RealNtPath,
        PATHOVERLAY_MAX_PATH_CHARS,
        RealPath->Buffer,
        RealPath->Length / sizeof(WCHAR));
    if (TargetPath != NULL) {
        PathOverlayCopyNullTerminated(
            request.TargetNtPath,
            PATHOVERLAY_MAX_PATH_CHARS,
            TargetPath->Buffer,
            TargetPath->Length / sizeof(WCHAR));
    }

    timeout.QuadPart = -5 * 1000 * 1000 * 10LL;
    status = FltSendMessage(
        gFilterHandle,
        &gClientPort,
        &request,
        sizeof(request),
        &response,
        &replyLength,
        &timeout);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    if (ServiceResponse != NULL) {
        *ServiceResponse = response;
    }

    return (NTSTATUS)response.Status;
}

static VOID
PathOverlayCopyUnicodeStringSnippet(
    _In_opt_ PCUNICODE_STRING Source,
    _Out_writes_(DestinationChars) PWCHAR Destination,
    _In_ ULONG DestinationChars
    )
{
    ULONG charsToCopy;

    if (DestinationChars == 0) {
        return;
    }

    Destination[0] = UNICODE_NULL;
    if (Source == NULL || Source->Buffer == NULL) {
        return;
    }

    charsToCopy = Source->Length / sizeof(WCHAR);
    if (charsToCopy >= DestinationChars) {
        charsToCopy = DestinationChars - 1;
    }
    if (charsToCopy > 0) {
        RtlCopyMemory(Destination, Source->Buffer, charsToCopy * sizeof(WCHAR));
    }
    Destination[charsToCopy] = UNICODE_NULL;
}

static BOOLEAN
PathOverlayShouldTracePreCreate(
    _In_opt_ PCUNICODE_STRING Path,
    _In_opt_ PCFLT_RELATED_OBJECTS FltObjects
    )
{
    if (PathOverlayUnicodeStringContainsInsensitive(Path, L"empty-root")) {
        return TRUE;
    }
    if (PathOverlayUnicodeStringContainsInsensitive(Path, L"junction-out")) {
        return TRUE;
    }
    if (FltObjects != NULL &&
        FltObjects->FileObject != NULL &&
        (PathOverlayUnicodeStringContainsInsensitive(&FltObjects->FileObject->FileName, L"empty-root") ||
         PathOverlayUnicodeStringContainsInsensitive(&FltObjects->FileObject->FileName, L"junction-out"))) {
        return TRUE;
    }

    return FALSE;
}

static VOID
PathOverlaySendTraceRequest(
    _In_opt_ const WCHAR* RuleId,
    _In_ PCUNICODE_STRING RealPath,
    _In_z_ PCWSTR Detail
    )
{
    NTSTATUS status;
    PATHOVERLAY_SERVICE_REQUEST request;
    PATHOVERLAY_SERVICE_RESPONSE response = { 0 };
    ULONG replyLength = sizeof(response);
    LARGE_INTEGER timeout;

    if (gClientPort == NULL || RealPath == NULL || Detail == NULL) {
        return;
    }
    if (RealPath->Length >= sizeof(request.RealNtPath)) {
        return;
    }

    RtlZeroMemory(&request, sizeof(request));
    request.Version = PATHOVERLAY_PROTOCOL_VERSION;
    request.Command = PathOverlayServiceCommandTraceCreate;
    if (RuleId != NULL) {
        PathOverlayCopyNullTerminated(
            request.RuleId,
            PATHOVERLAY_MAX_RULE_ID_CHARS,
            RuleId,
            PathOverlayNullTerminatedLength(RuleId, PATHOVERLAY_MAX_RULE_ID_CHARS));
    }
    PathOverlayCopyNullTerminated(
        request.RealNtPath,
        PATHOVERLAY_MAX_PATH_CHARS,
        RealPath->Buffer,
        RealPath->Length / sizeof(WCHAR));
    PathOverlayCopyNullTerminated(
        request.TargetNtPath,
        PATHOVERLAY_MAX_PATH_CHARS,
        Detail,
        PathOverlayNullTerminatedLength(Detail, PATHOVERLAY_MAX_PATH_CHARS));

    timeout.QuadPart = -100 * 1000 * 10LL;
    status = FltSendMessage(
        gFilterHandle,
        &gClientPort,
        &request,
        sizeof(request),
        &response,
        &replyLength,
        &timeout);
    UNREFERENCED_PARAMETER(status);
}

static VOID
PathOverlayTracePreCreateDecision(
    _In_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_ PCUNICODE_STRING Path,
    _In_opt_ const WCHAR* RuleId,
    _In_z_ PCWSTR Action,
    _In_ NTSTATUS Status,
    _In_ ULONG WriteIntent,
    _In_ ULONG RealPathExists,
    _In_ ULONG ShadowPathExists
    )
{
    NTSTATUS formatStatus;
    WCHAR detail[PATHOVERLAY_MAX_PATH_CHARS];
    WCHAR fileObjectName[128];
    ULONG options = Data->Iopb->Parameters.Create.Options;
    ULONG disposition = (options >> 24) & 0xff;
    ACCESS_MASK desiredAccess = PathOverlayGetCreateDesiredAccess(Data);

    if (!PathOverlayShouldTracePreCreate(Path, FltObjects)) {
        return;
    }

    PathOverlayCopyUnicodeStringSnippet(
        FltObjects != NULL && FltObjects->FileObject != NULL ? &FltObjects->FileObject->FileName : NULL,
        fileObjectName,
        RTL_NUMBER_OF(fileObjectName));

    formatStatus = RtlStringCchPrintfW(
        detail,
        RTL_NUMBER_OF(detail),
        L"precreate action=%ws status=0x%08X opts=0x%08X disp=%lu attrs=0x%08X access=0x%08X dir=%lu hint=%lu write=%lu real=%lu shadow=%lu fo=%ws",
        Action,
        (ULONG)Status,
        options,
        disposition,
        Data->Iopb->Parameters.Create.FileAttributes,
        desiredAccess,
        PathOverlayIsDirectoryOpen(Data) ? 1UL : 0UL,
        PathOverlayHasDirectoryCreateHint(Data) ? 1UL : 0UL,
        WriteIntent,
        RealPathExists,
        ShadowPathExists,
        fileObjectName);
    if (!NT_SUCCESS(formatStatus)) {
        return;
    }

    PathOverlaySendTraceRequest(RuleId, Path, detail);
}

static NTSTATUS
PathOverlayGetFileNameInformationWithFallback(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _Outptr_ PFLT_FILE_NAME_INFORMATION* NameInfo
    )
{
    NTSTATUS status;

    status = FltGetFileNameInformation(
        Data,
        FLT_FILE_NAME_NORMALIZED | FLT_FILE_NAME_QUERY_DEFAULT,
        NameInfo);
    if (NT_SUCCESS(status)) {
        return status;
    }

    return FltGetFileNameInformation(
        Data,
        FLT_FILE_NAME_OPENED | FLT_FILE_NAME_QUERY_DEFAULT,
        NameInfo);
}

static BOOLEAN
PathOverlayShouldDeferQueryOpen(
    _In_ PFLT_CALLBACK_DATA Data
    )
{
    NTSTATUS status;
    UNICODE_STRING shadowPath = { 0 };
    BOOLEAN tombstone = FALSE;
    BOOLEAN shouldDefer = FALSE;

    status = PathOverlayResolveQueryShadowPath(Data, &shadowPath, &tombstone);
    if (tombstone) {
        shouldDefer = TRUE;
    } else if (NT_SUCCESS(status) && shadowPath.Buffer != NULL) {
        shouldDefer = TRUE;
    }

    if (shadowPath.Buffer != NULL) {
        ExFreePoolWithTag(shadowPath.Buffer, 'OPhP');
    }

    return shouldDefer;
}

static NTSTATUS
PathOverlayResolveQueryShadowPath(
    _In_ PFLT_CALLBACK_DATA Data,
    _Out_ PUNICODE_STRING ShadowPath,
    _Out_ PBOOLEAN Tombstone
    )
{
    NTSTATUS status;
    PFLT_FILE_NAME_INFORMATION nameInfo = NULL;
    PATHOVERLAY_RULE_ENTRY rule;
    WCHAR matchedSourceNtPath[PATHOVERLAY_MAX_PATH_CHARS] = { 0 };
    UNICODE_STRING source;
    UNICODE_STRING store;
    PATHOVERLAY_SERVICE_RESPONSE serviceResponse = { 0 };

    ShadowPath->Buffer = NULL;
    ShadowPath->Length = 0;
    ShadowPath->MaximumLength = 0;
    *Tombstone = FALSE;

    if (PathOverlayIsServiceProcess(FltGetRequestorProcessId(Data))) {
        return STATUS_NOT_FOUND;
    }

    status = PathOverlayGetFileNameInformationWithFallback(Data, &nameInfo);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    if (!PathOverlayFindSourceRuleForPath(&nameInfo->Name, &rule, matchedSourceNtPath)) {
        FltReleaseFileNameInformation(nameInfo);
        return STATUS_NOT_FOUND;
    }

    RtlInitUnicodeString(
        &source,
        matchedSourceNtPath[0] != UNICODE_NULL ? matchedSourceNtPath : rule.SourceNtPath);
    RtlInitUnicodeString(&store, rule.StoreNtPath);

    status = PathOverlaySendServiceRequest(
        PathOverlayServiceCommandQueryPath,
        rule.RuleId,
        &nameInfo->Name,
        NULL,
        &serviceResponse);

    if (NT_SUCCESS(status) && serviceResponse.PathState == PathOverlayPathStateTombstone) {
        *Tombstone = TRUE;
        goto Exit;
    }
    if (NT_SUCCESS(status) && serviceResponse.PathState == PathOverlayPathStatePassthrough) {
        status = STATUS_NOT_FOUND;
        goto Exit;
    }
    if (NT_SUCCESS(status) && serviceResponse.ShadowNtPath[0] != UNICODE_NULL) {
        UNICODE_STRING responseShadowPath;

        RtlInitUnicodeString(&responseShadowPath, serviceResponse.ShadowNtPath);
        status = PathOverlayAllocateUnicodeStringCopy(&responseShadowPath, ShadowPath, 'OPhP');
        if (!NT_SUCCESS(status)) {
            goto Exit;
        }
        if (!PathOverlayNtPathExists(ShadowPath)) {
            ExFreePoolWithTag(ShadowPath->Buffer, 'OPhP');
            ShadowPath->Buffer = NULL;
            ShadowPath->Length = 0;
            ShadowPath->MaximumLength = 0;
            status = STATUS_NOT_FOUND;
        }
        goto Exit;
    }

    status = PathOverlayBuildShadowPath(&store, &source, &nameInfo->Name, ShadowPath);
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }
    if (!PathOverlayNtPathExists(ShadowPath)) {
        ExFreePoolWithTag(ShadowPath->Buffer, 'OPhP');
        ShadowPath->Buffer = NULL;
        ShadowPath->Length = 0;
        ShadowPath->MaximumLength = 0;
        status = STATUS_NOT_FOUND;
    }

Exit:
    FltReleaseFileNameInformation(nameInfo);

    if (*Tombstone) {
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    return status;
}

static NTSTATUS
PathOverlayQueryPathInformation(
    _In_ PFLT_INSTANCE Instance,
    _In_ PCUNICODE_STRING NtPath,
    _Out_writes_bytes_(Length) PVOID FileInformation,
    _In_ ULONG Length,
    _In_ FILE_INFORMATION_CLASS FileInformationClass,
    _Out_opt_ PULONG LengthReturned
    )
{
    OBJECT_ATTRIBUTES attributes;
    IO_STATUS_BLOCK ioStatus = { 0 };
    HANDLE fileHandle = NULL;
    PFILE_OBJECT fileObject = NULL;
    ULONG localLengthReturned = 0;
    NTSTATUS status;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    if (FileInformation == NULL || Length == 0) {
        return STATUS_INVALID_PARAMETER;
    }

    if (LengthReturned != NULL) {
        *LengthReturned = 0;
    }
    RtlZeroMemory(FileInformation, Length);
    InitializeObjectAttributes(
        &attributes,
        (PUNICODE_STRING)NtPath,
        OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
        NULL,
        NULL);

    status = FltCreateFileEx(
        gFilterHandle,
        Instance,
        &fileHandle,
        &fileObject,
        FILE_READ_ATTRIBUTES,
        &attributes,
        &ioStatus,
        NULL,
        FILE_ATTRIBUTE_NORMAL,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        FILE_OPEN,
        FILE_OPEN_REPARSE_POINT | FILE_OPEN_FOR_BACKUP_INTENT,
        NULL,
        0,
        0);
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }

    status = FltQueryInformationFile(
        Instance,
        fileObject,
        FileInformation,
        Length,
        FileInformationClass,
        &localLengthReturned);
    if (NT_SUCCESS(status) && LengthReturned != NULL) {
        *LengthReturned = localLengthReturned;
    }

Exit:
    if (fileObject != NULL) {
        ObDereferenceObject(fileObject);
    }
    if (fileHandle != NULL) {
        FltClose(fileHandle);
    }
    return status;
}

static NTSTATUS
PathOverlayBuildShadowPath(
    _In_ PCUNICODE_STRING Store,
    _In_ PCUNICODE_STRING Source,
    _In_ PCUNICODE_STRING RealPath,
    _Out_ PUNICODE_STRING ShadowPath
    )
{
    if (RealPath->Length < Source->Length) {
        return STATUS_OBJECT_PATH_INVALID;
    }

    USHORT suffixLength = RealPath->Length - Source->Length;
    USHORT totalLength = Store->Length + suffixLength;
    PWCHAR buffer;

    if (totalLength == 0 || totalLength > (PATHOVERLAY_MAX_PATH_CHARS - 1) * sizeof(WCHAR)) {
        return STATUS_NAME_TOO_LONG;
    }

    buffer = ExAllocatePool2(POOL_FLAG_PAGED, totalLength + sizeof(WCHAR), 'OPhP');
    if (buffer == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlCopyMemory(buffer, Store->Buffer, Store->Length);
    if (suffixLength > 0) {
        RtlCopyMemory((PUCHAR)buffer + Store->Length, (PUCHAR)RealPath->Buffer + Source->Length, suffixLength);
    }
    buffer[totalLength / sizeof(WCHAR)] = UNICODE_NULL;

    ShadowPath->Buffer = buffer;
    ShadowPath->Length = totalLength;
    ShadowPath->MaximumLength = totalLength + sizeof(WCHAR);
    return STATUS_SUCCESS;
}

static NTSTATUS
PathOverlayAllocateUnicodeStringCopy(
    _In_ PCUNICODE_STRING Source,
    _Out_ PUNICODE_STRING Destination,
    _In_ ULONG Tag
    )
{
    PWCHAR buffer;

    if (Source->Length == 0 || Source->Length > (PATHOVERLAY_MAX_PATH_CHARS - 1) * sizeof(WCHAR)) {
        return STATUS_NAME_TOO_LONG;
    }

    buffer = ExAllocatePool2(POOL_FLAG_PAGED, Source->Length + sizeof(WCHAR), Tag);
    if (buffer == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlCopyMemory(buffer, Source->Buffer, Source->Length);
    buffer[Source->Length / sizeof(WCHAR)] = UNICODE_NULL;

    Destination->Buffer = buffer;
    Destination->Length = Source->Length;
    Destination->MaximumLength = Source->Length + sizeof(WCHAR);
    return STATUS_SUCCESS;
}

static NTSTATUS
PathOverlayBuildRealPathFromShadow(
    _In_ PCUNICODE_STRING Source,
    _In_ PCUNICODE_STRING Store,
    _In_ PCUNICODE_STRING ShadowPath,
    _Out_ PUNICODE_STRING RealPath
    )
{
    USHORT suffixLength = ShadowPath->Length - Store->Length;
    USHORT totalLength = Source->Length + suffixLength;
    PWCHAR buffer;

    if (totalLength == 0 || totalLength > (PATHOVERLAY_MAX_PATH_CHARS - 1) * sizeof(WCHAR)) {
        return STATUS_NAME_TOO_LONG;
    }

    buffer = ExAllocatePool2(POOL_FLAG_PAGED, totalLength + sizeof(WCHAR), 'OPrP');
    if (buffer == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlCopyMemory(buffer, Source->Buffer, Source->Length);
    if (suffixLength > 0) {
        RtlCopyMemory((PUCHAR)buffer + Source->Length, (PUCHAR)ShadowPath->Buffer + Store->Length, suffixLength);
    }
    buffer[totalLength / sizeof(WCHAR)] = UNICODE_NULL;

    RealPath->Buffer = buffer;
    RealPath->Length = totalLength;
    RealPath->MaximumLength = totalLength + sizeof(WCHAR);
    return STATUS_SUCCESS;
}

static VOID
PathOverlayCloseCommunicationPort()
{
    if (gClientPort != NULL) {
        FltCloseClientPort(gFilterHandle, &gClientPort);
        gClientPort = NULL;
    }

    if (gServerPort != NULL) {
        FltCloseCommunicationPort(gServerPort);
        gServerPort = NULL;
    }
}

static NTSTATUS
PathOverlayCreateCommunicationPort()
{
    NTSTATUS status;
    PSECURITY_DESCRIPTOR securityDescriptor = NULL;
    OBJECT_ATTRIBUTES objectAttributes;
    UNICODE_STRING portName;

    status = FltBuildDefaultSecurityDescriptor(&securityDescriptor, FLT_PORT_ALL_ACCESS);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    RtlInitUnicodeString(&portName, PATHOVERLAY_PORT_NAME);
    InitializeObjectAttributes(
        &objectAttributes,
        &portName,
        OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
        NULL,
        securityDescriptor);

    status = FltCreateCommunicationPort(
        gFilterHandle,
        &gServerPort,
        &objectAttributes,
        NULL,
        PathOverlayConnect,
        PathOverlayDisconnect,
        PathOverlayMessage,
        1);

    FltFreeSecurityDescriptor(securityDescriptor);
    return status;
}

static NTSTATUS
PathOverlayUnload(
    _In_ FLT_FILTER_UNLOAD_FLAGS Flags
    )
{
    UNREFERENCED_PARAMETER(Flags);

    PathOverlayCloseCommunicationPort();

    if (gFilterHandle != NULL) {
        FltUnregisterFilter(gFilterHandle);
        gFilterHandle = NULL;
    }

    return STATUS_SUCCESS;
}

static FLT_PREOP_CALLBACK_STATUS
PathOverlayCompleteCreateWithStatus(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ NTSTATUS Status
    )
{
    Data->IoStatus.Status = Status;
    Data->IoStatus.Information = 0;
    FltSetCallbackDataDirty(Data);
    return FLT_PREOP_COMPLETE;
}

static FLT_PREOP_CALLBACK_STATUS
PathOverlayPreCreate(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID* CompletionContext
    )
{
    NTSTATUS status;
    PFLT_FILE_NAME_INFORMATION nameInfo = NULL;
    PATHOVERLAY_RULE_ENTRY rule;
    WCHAR matchedSourceNtPath[PATHOVERLAY_MAX_PATH_CHARS] = { 0 };
    UNICODE_STRING source;
    UNICODE_STRING store;
    UNICODE_STRING shadowPath = { 0 };
    ULONG requestorProcessId;
    BOOLEAN writeIntent;

    *CompletionContext = NULL;

    requestorProcessId = FltGetRequestorProcessId(Data);
    if (PathOverlayIsServiceProcess(requestorProcessId)) {
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    status = PathOverlayGetFileNameInformationWithFallback(Data, &nameInfo);
    if (!NT_SUCCESS(status)) {
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    if (!PathOverlayFindSourceRuleForPath(&nameInfo->Name, &rule, matchedSourceNtPath)) {
        PathOverlayTracePreCreateDecision(
            Data,
            FltObjects,
            &nameInfo->Name,
            NULL,
            L"no_rule",
            STATUS_SUCCESS,
            2,
            2,
            2);
        FltReleaseFileNameInformation(nameInfo);
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    RtlInitUnicodeString(
        &source,
        matchedSourceNtPath[0] != UNICODE_NULL ? matchedSourceNtPath : rule.SourceNtPath);
    RtlInitUnicodeString(&store, rule.StoreNtPath);

    status = PathOverlayBuildShadowPath(&store, &source, &nameInfo->Name, &shadowPath);
    if (!NT_SUCCESS(status)) {
        PathOverlayTracePreCreateDecision(
            Data,
            FltObjects,
            &nameInfo->Name,
            rule.RuleId,
            L"build_shadow_failed",
            status,
            2,
            2,
            2);
        FltReleaseFileNameInformation(nameInfo);
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    PATHOVERLAY_SERVICE_RESPONSE serviceResponse = { 0 };
    status = PathOverlaySendServiceRequest(
        PathOverlayServiceCommandQueryPath,
        rule.RuleId,
        &nameInfo->Name,
        NULL,
        &serviceResponse);
    PathOverlayTracePreCreateDecision(
        Data,
        FltObjects,
        &nameInfo->Name,
        rule.RuleId,
        L"query",
        status,
        2,
        2,
        2);
    if (NT_SUCCESS(status) && serviceResponse.PathState == PathOverlayPathStateTombstone) {
        PathOverlayTracePreCreateDecision(
            Data,
            FltObjects,
            &nameInfo->Name,
            rule.RuleId,
            L"tombstone",
            status,
            2,
            2,
            2);
        FltReleaseFileNameInformation(nameInfo);
        ExFreePoolWithTag(shadowPath.Buffer, 'OPhP');
        Data->IoStatus.Status = STATUS_OBJECT_NAME_NOT_FOUND;
        Data->IoStatus.Information = 0;
        FltSetCallbackDataDirty(Data);
        return FLT_PREOP_COMPLETE;
    }
    if (NT_SUCCESS(status) && serviceResponse.PathState == PathOverlayPathStatePassthrough) {
        PathOverlayTracePreCreateDecision(
            Data,
            FltObjects,
            &nameInfo->Name,
            rule.RuleId,
            L"passthrough_state",
            status,
            2,
            2,
            2);
        FltReleaseFileNameInformation(nameInfo);
        ExFreePoolWithTag(shadowPath.Buffer, 'OPhP');
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }
    if (PathOverlayShouldPassThroughReparsePointOpen(Data)) {
        PathOverlayTracePreCreateDecision(
            Data,
            FltObjects,
            &nameInfo->Name,
            rule.RuleId,
            L"reparse_point_open_passthrough",
            status,
            2,
            2,
            2);
        FltReleaseFileNameInformation(nameInfo);
        ExFreePoolWithTag(shadowPath.Buffer, 'OPhP');
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }
    if (NT_SUCCESS(status) && serviceResponse.ShadowNtPath[0] != UNICODE_NULL) {
        UNICODE_STRING responseShadowPath;

        ExFreePoolWithTag(shadowPath.Buffer, 'OPhP');
        shadowPath.Buffer = NULL;
        shadowPath.Length = 0;
        shadowPath.MaximumLength = 0;
        RtlInitUnicodeString(&responseShadowPath, serviceResponse.ShadowNtPath);
        status = PathOverlayAllocateUnicodeStringCopy(&responseShadowPath, &shadowPath, 'OPhP');
        if (!NT_SUCCESS(status)) {
            PathOverlayTracePreCreateDecision(
                Data,
                FltObjects,
                &nameInfo->Name,
                rule.RuleId,
                L"response_shadow_alloc_failed",
                status,
                2,
                2,
                2);
            FltReleaseFileNameInformation(nameInfo);
            return FLT_PREOP_SUCCESS_NO_CALLBACK;
        }
    }

    writeIntent = PathOverlayHasWriteIntent(Data);
    if (writeIntent && PathOverlayIsSameUnicodeStringInsensitive(&source, &nameInfo->Name)) {
        writeIntent = FALSE;
    }
    {
        BOOLEAN directoryOpen = PathOverlayIsDirectoryOpen(Data);
        BOOLEAN directoryCreateHint = PathOverlayHasDirectoryCreateHint(Data);
        BOOLEAN realPathExists = PathOverlayNtPathExists(&nameInfo->Name);
        BOOLEAN shadowPathExists = PathOverlayNtPathExists(&shadowPath);

        PathOverlayTracePreCreateDecision(
            Data,
            FltObjects,
            &nameInfo->Name,
            rule.RuleId,
            L"decision",
            STATUS_SUCCESS,
            writeIntent ? 1UL : 0UL,
            realPathExists ? 1UL : 0UL,
            shadowPathExists ? 1UL : 0UL);

        if ((directoryOpen || directoryCreateHint) && !realPathExists && shadowPathExists) {
            PathOverlayTracePreCreateDecision(
                Data,
                FltObjects,
                &nameInfo->Name,
                rule.RuleId,
                L"direct_shadow_reparse",
                STATUS_SUCCESS,
                writeIntent ? 1UL : 0UL,
                0,
                1);
            FltReleaseFileNameInformation(nameInfo);
            status = IoReplaceFileObjectName(FltObjects->FileObject, shadowPath.Buffer, shadowPath.Length);
            ExFreePoolWithTag(shadowPath.Buffer, 'OPhP');
            if (NT_SUCCESS(status)) {
                Data->IoStatus.Status = STATUS_REPARSE;
                Data->IoStatus.Information = IO_REPARSE;
                FltSetCallbackDataDirty(Data);
                return FLT_PREOP_COMPLETE;
            }
            return PathOverlayCompleteCreateWithStatus(Data, status);
        }

        if (!writeIntent && directoryCreateHint && !realPathExists && !shadowPathExists) {
            writeIntent = TRUE;
            PathOverlayTracePreCreateDecision(
                Data,
                FltObjects,
                &nameInfo->Name,
                rule.RuleId,
                L"force_write_dir_hint",
                STATUS_SUCCESS,
                1,
                0,
                0);
        }

        if (!writeIntent && directoryOpen && !realPathExists && !shadowPathExists) {
            PathOverlayTracePreCreateDecision(
                Data,
                FltObjects,
                &nameInfo->Name,
                rule.RuleId,
                L"missing_directory_open_not_found",
                STATUS_OBJECT_NAME_NOT_FOUND,
                0,
                0,
                0);
            FltReleaseFileNameInformation(nameInfo);
            ExFreePoolWithTag(shadowPath.Buffer, 'OPhP');
            Data->IoStatus.Status = STATUS_OBJECT_NAME_NOT_FOUND;
            Data->IoStatus.Information = 0;
            FltSetCallbackDataDirty(Data);
            return FLT_PREOP_COMPLETE;
        }
    }

    if (!writeIntent && PathOverlayIsDirectoryOpen(Data)) {
        status = PathOverlaySendServiceRequest(
            PathOverlayServiceCommandPrepareDirectoryView,
            rule.RuleId,
            &nameInfo->Name,
            NULL,
            NULL);
        PathOverlayTracePreCreateDecision(
            Data,
            FltObjects,
            &nameInfo->Name,
            rule.RuleId,
            NT_SUCCESS(status) ? L"directory_view_prepare_ok" : L"directory_view_prepare_failed",
            status,
            0,
            2,
            2);
        if (NT_SUCCESS(status)) {
            PathOverlayTracePreCreateDecision(
                Data,
                FltObjects,
                &nameInfo->Name,
                rule.RuleId,
                L"directory_view_reparse",
                status,
                0,
                2,
                2);
            FltReleaseFileNameInformation(nameInfo);
            status = IoReplaceFileObjectName(FltObjects->FileObject, shadowPath.Buffer, shadowPath.Length);
            ExFreePoolWithTag(shadowPath.Buffer, 'OPhP');
            if (NT_SUCCESS(status)) {
                Data->IoStatus.Status = STATUS_REPARSE;
                Data->IoStatus.Information = IO_REPARSE;
                FltSetCallbackDataDirty(Data);
                return FLT_PREOP_COMPLETE;
            }
            return PathOverlayCompleteCreateWithStatus(Data, status);
        }
    }

    if (writeIntent) {
        status = PathOverlaySendServiceRequest(
            PathOverlayServiceCommandPrepareCopyOnWrite,
            rule.RuleId,
            &nameInfo->Name,
            NULL,
            NULL);
        PathOverlayTracePreCreateDecision(
            Data,
            FltObjects,
            &nameInfo->Name,
            rule.RuleId,
            NT_SUCCESS(status) ? L"copy_on_write_ok" : L"copy_on_write_failed",
            status,
            1,
            2,
            2);
        if (!NT_SUCCESS(status)) {
            FltReleaseFileNameInformation(nameInfo);
            ExFreePoolWithTag(shadowPath.Buffer, 'OPhP');
            Data->IoStatus.Status = status;
            Data->IoStatus.Information = 0;
            FltSetCallbackDataDirty(Data);
            return FLT_PREOP_COMPLETE;
        }
    }

    if (!writeIntent && !PathOverlayNtPathExists(&shadowPath)) {
        if (!PathOverlayNtPathExists(&nameInfo->Name)) {
            PathOverlayTracePreCreateDecision(
                Data,
                FltObjects,
                &nameInfo->Name,
                rule.RuleId,
                L"missing_shadow_and_real_not_found",
                STATUS_OBJECT_NAME_NOT_FOUND,
                0,
                0,
                0);
            FltReleaseFileNameInformation(nameInfo);
            ExFreePoolWithTag(shadowPath.Buffer, 'OPhP');
            Data->IoStatus.Status = STATUS_OBJECT_NAME_NOT_FOUND;
            Data->IoStatus.Information = 0;
            FltSetCallbackDataDirty(Data);
            return FLT_PREOP_COMPLETE;
        }

        PathOverlayTracePreCreateDecision(
            Data,
            FltObjects,
            &nameInfo->Name,
            rule.RuleId,
            L"passthrough_existing_real",
            STATUS_SUCCESS,
            0,
            1,
            0);
        FltReleaseFileNameInformation(nameInfo);
        ExFreePoolWithTag(shadowPath.Buffer, 'OPhP');
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    PathOverlayTracePreCreateDecision(
        Data,
        FltObjects,
        &nameInfo->Name,
        rule.RuleId,
        L"final_reparse",
        STATUS_SUCCESS,
        writeIntent ? 1UL : 0UL,
        2,
        1);
    FltReleaseFileNameInformation(nameInfo);
    status = IoReplaceFileObjectName(FltObjects->FileObject, shadowPath.Buffer, shadowPath.Length);
    ExFreePoolWithTag(shadowPath.Buffer, 'OPhP');
    if (!NT_SUCCESS(status)) {
        return PathOverlayCompleteCreateWithStatus(Data, status);
    }

    Data->IoStatus.Status = STATUS_REPARSE;
    Data->IoStatus.Information = IO_REPARSE;
    FltSetCallbackDataDirty(Data);
    return FLT_PREOP_COMPLETE;
}

static FLT_PREOP_CALLBACK_STATUS
PathOverlayPreSetInformation(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID* CompletionContext
    )
{
    NTSTATUS status;
    PFLT_FILE_NAME_INFORMATION nameInfo = NULL;
    PATHOVERLAY_RULE_ENTRY rule;
    UNICODE_STRING source;
    UNICODE_STRING store;
    UNICODE_STRING deleteRealPath = { 0 };
    UNICODE_STRING renameSourceRealPath = { 0 };
    UNICODE_STRING renameTargetRealPath = { 0 };
    FILE_INFORMATION_CLASS informationClass;
    BOOLEAN sourcePath;
    BOOLEAN storePath;

    *CompletionContext = NULL;

    if (PathOverlayIsServiceProcess(FltGetRequestorProcessId(Data))) {
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    status = FltGetFileNameInformation(
        Data,
        FLT_FILE_NAME_NORMALIZED | FLT_FILE_NAME_QUERY_DEFAULT,
        &nameInfo);
    if (!NT_SUCCESS(status)) {
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    if (!PathOverlayFindSourceOrStoreRuleForPath(&nameInfo->Name, &rule, &sourcePath, &storePath)) {
        FltReleaseFileNameInformation(nameInfo);
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    RtlInitUnicodeString(&source, rule.SourceNtPath);
    RtlInitUnicodeString(&store, rule.StoreNtPath);

    informationClass = Data->Iopb->Parameters.SetFileInformation.FileInformationClass;
    if (informationClass == FileRenameInformation ||
        informationClass == FileRenameInformationEx ||
        informationClass == FileRenameInformationBypassAccessCheck ||
        informationClass == FileRenameInformationExBypassAccessCheck) {
        FILE_RENAME_INFORMATION* renameInfo;
        PFLT_FILE_NAME_INFORMATION destinationInfo = NULL;
        PATHOVERLAY_RULE_ENTRY targetRule;
        BOOLEAN targetSourcePath = FALSE;
        BOOLEAN targetStorePath = FALSE;

        renameInfo = (FILE_RENAME_INFORMATION*)Data->Iopb->Parameters.SetFileInformation.InfoBuffer;
        if (renameInfo == NULL || renameInfo->FileNameLength == 0) {
            FltReleaseFileNameInformation(nameInfo);
            Data->IoStatus.Status = STATUS_INVALID_PARAMETER;
            Data->IoStatus.Information = 0;
            FltSetCallbackDataDirty(Data);
            return FLT_PREOP_COMPLETE;
        }

        status = FltGetDestinationFileNameInformation(
            FltObjects->Instance,
            FltObjects->FileObject,
            renameInfo->RootDirectory,
            renameInfo->FileName,
            renameInfo->FileNameLength,
            FLT_FILE_NAME_NORMALIZED | FLT_FILE_NAME_QUERY_DEFAULT,
            &destinationInfo);
        if (!NT_SUCCESS(status)) {
            FltReleaseFileNameInformation(nameInfo);
            Data->IoStatus.Status = status;
            Data->IoStatus.Information = 0;
            FltSetCallbackDataDirty(Data);
            return FLT_PREOP_COMPLETE;
        }

        if (!PathOverlayFindSourceOrStoreRuleForPath(&destinationInfo->Name, &targetRule, &targetSourcePath, &targetStorePath) ||
            RtlCompareMemory(rule.RuleId, targetRule.RuleId, sizeof(rule.RuleId)) != sizeof(rule.RuleId)) {
            FltReleaseFileNameInformation(destinationInfo);
            FltReleaseFileNameInformation(nameInfo);
            Data->IoStatus.Status = STATUS_NOT_SUPPORTED;
            Data->IoStatus.Information = 0;
            FltSetCallbackDataDirty(Data);
            return FLT_PREOP_COMPLETE;
        }

        if (sourcePath) {
            renameSourceRealPath = nameInfo->Name;
        } else {
            status = PathOverlayBuildRealPathFromShadow(&source, &store, &nameInfo->Name, &renameSourceRealPath);
            if (!NT_SUCCESS(status)) {
                FltReleaseFileNameInformation(destinationInfo);
                FltReleaseFileNameInformation(nameInfo);
                Data->IoStatus.Status = status;
                Data->IoStatus.Information = 0;
                FltSetCallbackDataDirty(Data);
                return FLT_PREOP_COMPLETE;
            }
        }

        if (targetSourcePath) {
            renameTargetRealPath = destinationInfo->Name;
        } else {
            status = PathOverlayBuildRealPathFromShadow(&source, &store, &destinationInfo->Name, &renameTargetRealPath);
            if (!NT_SUCCESS(status)) {
                if (renameSourceRealPath.Buffer != nameInfo->Name.Buffer && renameSourceRealPath.Buffer != NULL) {
                    ExFreePoolWithTag(renameSourceRealPath.Buffer, 'OPrP');
                }
                FltReleaseFileNameInformation(destinationInfo);
                FltReleaseFileNameInformation(nameInfo);
                Data->IoStatus.Status = status;
                Data->IoStatus.Information = 0;
                FltSetCallbackDataDirty(Data);
                return FLT_PREOP_COMPLETE;
            }
        }

        {
            PATHOVERLAY_SERVICE_RESPONSE queryResponse = { 0 };
            status = PathOverlaySendServiceRequest(
                PathOverlayServiceCommandQueryPath,
                rule.RuleId,
                &renameSourceRealPath,
                NULL,
                &queryResponse);
            if (NT_SUCCESS(status) && queryResponse.PathState == PathOverlayPathStatePassthrough) {
                if (renameSourceRealPath.Buffer != nameInfo->Name.Buffer && renameSourceRealPath.Buffer != NULL) {
                    ExFreePoolWithTag(renameSourceRealPath.Buffer, 'OPrP');
                }
                if (renameTargetRealPath.Buffer != destinationInfo->Name.Buffer && renameTargetRealPath.Buffer != NULL) {
                    ExFreePoolWithTag(renameTargetRealPath.Buffer, 'OPrP');
                }
                FltReleaseFileNameInformation(destinationInfo);
                FltReleaseFileNameInformation(nameInfo);
                return FLT_PREOP_SUCCESS_NO_CALLBACK;
            }

            status = PathOverlaySendServiceRequest(
                PathOverlayServiceCommandQueryPath,
                rule.RuleId,
                &renameTargetRealPath,
                NULL,
                &queryResponse);
            if (NT_SUCCESS(status) && queryResponse.PathState == PathOverlayPathStatePassthrough) {
                if (renameSourceRealPath.Buffer != nameInfo->Name.Buffer && renameSourceRealPath.Buffer != NULL) {
                    ExFreePoolWithTag(renameSourceRealPath.Buffer, 'OPrP');
                }
                if (renameTargetRealPath.Buffer != destinationInfo->Name.Buffer && renameTargetRealPath.Buffer != NULL) {
                    ExFreePoolWithTag(renameTargetRealPath.Buffer, 'OPrP');
                }
                FltReleaseFileNameInformation(destinationInfo);
                FltReleaseFileNameInformation(nameInfo);
                return FLT_PREOP_SUCCESS_NO_CALLBACK;
            }
        }

        status = PathOverlaySendServiceRequest(
            PathOverlayServiceCommandRecordRename,
            rule.RuleId,
            &renameSourceRealPath,
            &renameTargetRealPath,
            NULL);
        if (renameSourceRealPath.Buffer != nameInfo->Name.Buffer && renameSourceRealPath.Buffer != NULL) {
            ExFreePoolWithTag(renameSourceRealPath.Buffer, 'OPrP');
        }
        if (renameTargetRealPath.Buffer != destinationInfo->Name.Buffer && renameTargetRealPath.Buffer != NULL) {
            ExFreePoolWithTag(renameTargetRealPath.Buffer, 'OPrP');
        }
        FltReleaseFileNameInformation(destinationInfo);
        FltReleaseFileNameInformation(nameInfo);
        Data->IoStatus.Status = status;
        Data->IoStatus.Information = 0;
        FltSetCallbackDataDirty(Data);
        return FLT_PREOP_COMPLETE;
    }

    if (informationClass != FileDispositionInformation &&
        informationClass != FileDispositionInformationEx) {
        FltReleaseFileNameInformation(nameInfo);
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    if (informationClass == FileDispositionInformation) {
        FILE_DISPOSITION_INFORMATION* disposition =
            (FILE_DISPOSITION_INFORMATION*)Data->Iopb->Parameters.SetFileInformation.InfoBuffer;
        if (disposition == NULL || !disposition->DeleteFile) {
            FltReleaseFileNameInformation(nameInfo);
            return FLT_PREOP_SUCCESS_NO_CALLBACK;
        }
    } else {
        FILE_DISPOSITION_INFORMATION_EX* dispositionEx =
            (FILE_DISPOSITION_INFORMATION_EX*)Data->Iopb->Parameters.SetFileInformation.InfoBuffer;
        if (dispositionEx == NULL || (dispositionEx->Flags & FILE_DISPOSITION_DELETE) == 0) {
            FltReleaseFileNameInformation(nameInfo);
            return FLT_PREOP_SUCCESS_NO_CALLBACK;
        }
    }

    if (sourcePath) {
        deleteRealPath = nameInfo->Name;
    } else {
        status = PathOverlayBuildRealPathFromShadow(&source, &store, &nameInfo->Name, &deleteRealPath);
        if (!NT_SUCCESS(status)) {
            FltReleaseFileNameInformation(nameInfo);
            Data->IoStatus.Status = status;
            Data->IoStatus.Information = 0;
            FltSetCallbackDataDirty(Data);
            return FLT_PREOP_COMPLETE;
        }
    }

    {
        PATHOVERLAY_SERVICE_RESPONSE queryResponse = { 0 };
        status = PathOverlaySendServiceRequest(
            PathOverlayServiceCommandQueryPath,
            rule.RuleId,
            &deleteRealPath,
            NULL,
            &queryResponse);
        if (NT_SUCCESS(status) && queryResponse.PathState == PathOverlayPathStatePassthrough) {
            if (deleteRealPath.Buffer != nameInfo->Name.Buffer && deleteRealPath.Buffer != NULL) {
                ExFreePoolWithTag(deleteRealPath.Buffer, 'OPrP');
            }
            FltReleaseFileNameInformation(nameInfo);
            return FLT_PREOP_SUCCESS_NO_CALLBACK;
        }
    }

    status = PathOverlaySendServiceRequest(
        PathOverlayServiceCommandRecordDelete,
        rule.RuleId,
        &deleteRealPath,
        NULL,
        NULL);
    if (deleteRealPath.Buffer != nameInfo->Name.Buffer && deleteRealPath.Buffer != NULL) {
        ExFreePoolWithTag(deleteRealPath.Buffer, 'OPrP');
    }
    FltReleaseFileNameInformation(nameInfo);
    Data->IoStatus.Status = status;
    Data->IoStatus.Information = 0;
    FltSetCallbackDataDirty(Data);
    return FLT_PREOP_COMPLETE;
}

static FLT_PREOP_CALLBACK_STATUS
PathOverlayPreDirectoryControl(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID* CompletionContext
    )
{
    UNREFERENCED_PARAMETER(Data);
    UNREFERENCED_PARAMETER(FltObjects);
    *CompletionContext = NULL;

    return FLT_PREOP_SUCCESS_NO_CALLBACK;
}

static FLT_PREOP_CALLBACK_STATUS
PathOverlayPreNetworkQueryOpen(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID* CompletionContext
    )
{
    NTSTATUS status;
    UNICODE_STRING shadowPath = { 0 };
    BOOLEAN tombstone = FALSE;
    ULONG lengthReturned = 0;

    *CompletionContext = NULL;

    status = PathOverlayResolveQueryShadowPath(Data, &shadowPath, &tombstone);
    if (tombstone) {
        if (Data->Iopb->Parameters.NetworkQueryOpen.Irp != NULL) {
            Data->Iopb->Parameters.NetworkQueryOpen.Irp->IoStatus.Status = STATUS_OBJECT_NAME_NOT_FOUND;
            Data->Iopb->Parameters.NetworkQueryOpen.Irp->IoStatus.Information = 0;
        }
        Data->IoStatus.Status = STATUS_SUCCESS;
        Data->IoStatus.Information = 0;
        FltSetCallbackDataDirty(Data);
        return FLT_PREOP_COMPLETE;
    }
    if (NT_SUCCESS(status) && shadowPath.Buffer != NULL) {
        status = PathOverlayQueryPathInformation(
            FltObjects->Instance,
            &shadowPath,
            Data->Iopb->Parameters.NetworkQueryOpen.NetworkInformation,
            sizeof(FILE_NETWORK_OPEN_INFORMATION),
            FileNetworkOpenInformation,
            &lengthReturned);
        ExFreePoolWithTag(shadowPath.Buffer, 'OPhP');
        if (NT_SUCCESS(status)) {
            if (Data->Iopb->Parameters.NetworkQueryOpen.Irp != NULL) {
                Data->Iopb->Parameters.NetworkQueryOpen.Irp->IoStatus.Status = STATUS_SUCCESS;
                Data->Iopb->Parameters.NetworkQueryOpen.Irp->IoStatus.Information =
                    lengthReturned != 0 ? lengthReturned : sizeof(FILE_NETWORK_OPEN_INFORMATION);
            }
            Data->IoStatus.Status = STATUS_SUCCESS;
            Data->IoStatus.Information =
                lengthReturned != 0 ? lengthReturned : sizeof(FILE_NETWORK_OPEN_INFORMATION);
            FltSetCallbackDataDirty(Data);
            return FLT_PREOP_COMPLETE;
        }
        return FLT_PREOP_DISALLOW_FASTIO;
    }

    return FLT_PREOP_SUCCESS_NO_CALLBACK;
}

static FLT_PREOP_CALLBACK_STATUS
PathOverlayPreQueryOpen(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID* CompletionContext
    )
{
    NTSTATUS status;
    UNICODE_STRING shadowPath = { 0 };
    BOOLEAN tombstone = FALSE;
    ULONG lengthReturned = 0;

    *CompletionContext = NULL;

    status = PathOverlayResolveQueryShadowPath(Data, &shadowPath, &tombstone);
    if (tombstone) {
        Data->IoStatus.Status = STATUS_OBJECT_NAME_NOT_FOUND;
        Data->IoStatus.Information = 0;
        FltSetCallbackDataDirty(Data);
        return FLT_PREOP_COMPLETE;
    }
    if (NT_SUCCESS(status) && shadowPath.Buffer != NULL) {
        if (Data->Iopb->Parameters.QueryOpen.Length == NULL) {
            ExFreePoolWithTag(shadowPath.Buffer, 'OPhP');
            return FLT_PREOP_DISALLOW_FSFILTER_IO;
        }

        status = PathOverlayQueryPathInformation(
            FltObjects->Instance,
            &shadowPath,
            Data->Iopb->Parameters.QueryOpen.FileInformation,
            *Data->Iopb->Parameters.QueryOpen.Length,
            Data->Iopb->Parameters.QueryOpen.FileInformationClass,
            &lengthReturned);
        ExFreePoolWithTag(shadowPath.Buffer, 'OPhP');
        if (NT_SUCCESS(status)) {
            *Data->Iopb->Parameters.QueryOpen.Length = lengthReturned;
            Data->IoStatus.Status = STATUS_SUCCESS;
            Data->IoStatus.Information = lengthReturned;
            FltSetCallbackDataDirty(Data);
            return FLT_PREOP_COMPLETE;
        }
        return FLT_PREOP_DISALLOW_FSFILTER_IO;
    }

    return FLT_PREOP_SUCCESS_NO_CALLBACK;
}

static NTSTATUS
PathOverlayConnect(
    _In_ PFLT_PORT ClientPort,
    _In_opt_ PVOID ServerPortCookie,
    _In_reads_bytes_opt_(SizeOfContext) PVOID ConnectionContext,
    _In_ ULONG SizeOfContext,
    _Outptr_result_maybenull_ PVOID* ConnectionCookie
    )
{
    UNREFERENCED_PARAMETER(ServerPortCookie);
    UNREFERENCED_PARAMETER(ConnectionContext);
    UNREFERENCED_PARAMETER(SizeOfContext);

    if (gClientPort != NULL) {
        return STATUS_CONNECTION_COUNT_LIMIT;
    }

    gClientPort = ClientPort;
    *ConnectionCookie = NULL;
    return STATUS_SUCCESS;
}

static VOID
PathOverlayDisconnect(
    _In_opt_ PVOID ConnectionCookie
    )
{
    UNREFERENCED_PARAMETER(ConnectionCookie);
    FltCloseClientPort(gFilterHandle, &gClientPort);
}

static NTSTATUS
PathOverlayMessage(
    _In_opt_ PVOID PortCookie,
    _In_reads_bytes_opt_(InputBufferLength) PVOID InputBuffer,
    _In_ ULONG InputBufferLength,
    _Out_writes_bytes_to_opt_(OutputBufferLength, *ReturnOutputBufferLength) PVOID OutputBuffer,
    _In_ ULONG OutputBufferLength,
    _Out_ PULONG ReturnOutputBufferLength
    )
{
    NTSTATUS status;
    PATHOVERLAY_DRIVER_RESPONSE response;
    PATHOVERLAY_DRIVER_RULE_MESSAGE* message;

    UNREFERENCED_PARAMETER(PortCookie);

    *ReturnOutputBufferLength = 0;

    if (InputBuffer == NULL || InputBufferLength < sizeof(PATHOVERLAY_DRIVER_RULE_MESSAGE)) {
        return STATUS_INVALID_PARAMETER;
    }

    message = (PATHOVERLAY_DRIVER_RULE_MESSAGE*)InputBuffer;
    if (message->Version != PATHOVERLAY_PROTOCOL_VERSION) {
        return STATUS_REVISION_MISMATCH;
    }

    switch (message->Command) {
        case PathOverlayDriverCommandClearRule:
            PathOverlayClearRule();
            status = STATUS_SUCCESS;
            break;
        case PathOverlayDriverCommandSetRule:
            status = PathOverlaySetRule(message);
            break;
        default:
            status = STATUS_INVALID_PARAMETER;
            break;
    }

    if (OutputBuffer != NULL && OutputBufferLength >= sizeof(response)) {
        response.Status = status;
        RtlCopyMemory(OutputBuffer, &response, sizeof(response));
        *ReturnOutputBufferLength = sizeof(response);
    }

    return status;
}

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
{
    NTSTATUS status;

    ExInitializeFastMutex(&gRuleLock);

    status = FltRegisterFilter(DriverObject, &kFilterRegistration, &gFilterHandle);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = PathOverlayCreateCommunicationPort();
    if (!NT_SUCCESS(status)) {
        FltUnregisterFilter(gFilterHandle);
        gFilterHandle = NULL;
        return status;
    }

    status = FltStartFiltering(gFilterHandle);
    if (!NT_SUCCESS(status)) {
        PathOverlayCloseCommunicationPort();
        FltUnregisterFilter(gFilterHandle);
        gFilterHandle = NULL;
        return status;
    }

    UNREFERENCED_PARAMETER(RegistryPath);
    return STATUS_SUCCESS;
}
