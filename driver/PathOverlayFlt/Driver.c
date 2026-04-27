#include <fltKernel.h>
#include <ntstrsafe.h>

#include "path_overlay_protocol.h"

#define PATHOVERLAY_PORT_NAME L"\\PathOverlayPort"

DRIVER_INITIALIZE DriverEntry;

typedef struct PATHOVERLAY_RULE_CACHE {
    BOOLEAN Enabled;
    ULONG ServiceProcessId;
    WCHAR SourceNtPath[PATHOVERLAY_MAX_PATH_CHARS];
    WCHAR StoreNtPath[PATHOVERLAY_MAX_PATH_CHARS];
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

static BOOLEAN
PathOverlayRuleSnapshot(
    _Out_ PATHOVERLAY_RULE_CACHE* Snapshot
    )
{
    BOOLEAN enabled;

    ExAcquireFastMutex(&gRuleLock);
    *Snapshot = gRuleCache;
    enabled = gRuleCache.Enabled;
    ExReleaseFastMutex(&gRuleLock);

    return enabled;
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
    if (Message->SourceNtPath[0] == UNICODE_NULL || Message->StoreNtPath[0] == UNICODE_NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    ExAcquireFastMutex(&gRuleLock);
    RtlZeroMemory(&gRuleCache, sizeof(gRuleCache));
    gRuleCache.Enabled = Message->Enabled != 0;
    gRuleCache.ServiceProcessId = Message->ServiceProcessId;
    PathOverlayCopyNullTerminated(
        gRuleCache.SourceNtPath,
        PATHOVERLAY_MAX_PATH_CHARS,
        Message->SourceNtPath,
        PathOverlayNullTerminatedLength(Message->SourceNtPath, PATHOVERLAY_MAX_PATH_CHARS));
    PathOverlayCopyNullTerminated(
        gRuleCache.StoreNtPath,
        PATHOVERLAY_MAX_PATH_CHARS,
        Message->StoreNtPath,
        PathOverlayNullTerminatedLength(Message->StoreNtPath, PATHOVERLAY_MAX_PATH_CHARS));
    PathOverlayTrimTrailingSlashes(gRuleCache.SourceNtPath);
    PathOverlayTrimTrailingSlashes(gRuleCache.StoreNtPath);
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
PathOverlayHasWriteIntent(
    _In_ PFLT_CALLBACK_DATA Data
    )
{
    ACCESS_MASK desiredAccess;
    ULONG options = Data->Iopb->Parameters.Create.Options;
    ULONG disposition = (options >> 24) & 0xff;

    if (Data->Iopb->Parameters.Create.SecurityContext == NULL) {
        return FALSE;
    }

    desiredAccess = Data->Iopb->Parameters.Create.SecurityContext->DesiredAccess;

    if ((desiredAccess & (FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA)) != 0) {
        return TRUE;
    }

    return disposition == FILE_CREATE ||
           disposition == FILE_OPEN_IF ||
           disposition == FILE_OVERWRITE ||
           disposition == FILE_OVERWRITE_IF ||
           disposition == FILE_SUPERSEDE;
}

static BOOLEAN
PathOverlayIsDirectoryOpen(
    _In_ PFLT_CALLBACK_DATA Data
    )
{
    return (Data->Iopb->Parameters.Create.Options & FILE_DIRECTORY_FILE) != 0;
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
    _In_ PCUNICODE_STRING RealPath,
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

    RtlZeroMemory(&request, sizeof(request));
    request.Version = PATHOVERLAY_PROTOCOL_VERSION;
    request.Command = Command;
    PathOverlayCopyNullTerminated(
        request.RealNtPath,
        PATHOVERLAY_MAX_PATH_CHARS,
        RealPath->Buffer,
        RealPath->Length / sizeof(WCHAR));

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

static BOOLEAN
PathOverlayIsTombstonedOverlayPath(
    _In_ PFLT_CALLBACK_DATA Data
    )
{
    NTSTATUS status;
    PFLT_FILE_NAME_INFORMATION nameInfo = NULL;
    PATHOVERLAY_RULE_CACHE rule;
    UNICODE_STRING source;
    UNICODE_STRING store;
    PATHOVERLAY_SERVICE_RESPONSE serviceResponse = { 0 };

    if (!PathOverlayRuleSnapshot(&rule)) {
        return FALSE;
    }

    if (FltGetRequestorProcessId(Data) == rule.ServiceProcessId) {
        return FALSE;
    }

    RtlInitUnicodeString(&source, rule.SourceNtPath);
    RtlInitUnicodeString(&store, rule.StoreNtPath);

    status = FltGetFileNameInformation(
        Data,
        FLT_FILE_NAME_NORMALIZED | FLT_FILE_NAME_QUERY_DEFAULT,
        &nameInfo);
    if (!NT_SUCCESS(status)) {
        return FALSE;
    }

    if (!PathOverlaySameOrChildPath(&source, &nameInfo->Name) ||
        PathOverlaySameOrChildPath(&store, &nameInfo->Name)) {
        FltReleaseFileNameInformation(nameInfo);
        return FALSE;
    }

    status = PathOverlaySendServiceRequest(PathOverlayServiceCommandQueryPath, &nameInfo->Name, &serviceResponse);
    FltReleaseFileNameInformation(nameInfo);

    return NT_SUCCESS(status) && serviceResponse.PathState == PathOverlayPathStateTombstone;
}

static NTSTATUS
PathOverlayBuildShadowPath(
    _In_ PCUNICODE_STRING Store,
    _In_ PCUNICODE_STRING Source,
    _In_ PCUNICODE_STRING RealPath,
    _Out_ PUNICODE_STRING ShadowPath
    )
{
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
PathOverlayPreCreate(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID* CompletionContext
    )
{
    NTSTATUS status;
    PFLT_FILE_NAME_INFORMATION nameInfo = NULL;
    PATHOVERLAY_RULE_CACHE rule;
    UNICODE_STRING source;
    UNICODE_STRING store;
    UNICODE_STRING shadowPath = { 0 };
    ULONG requestorProcessId;
    BOOLEAN writeIntent;

    *CompletionContext = NULL;

    if (!PathOverlayRuleSnapshot(&rule)) {
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    requestorProcessId = FltGetRequestorProcessId(Data);
    if (requestorProcessId == rule.ServiceProcessId) {
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    RtlInitUnicodeString(&source, rule.SourceNtPath);
    RtlInitUnicodeString(&store, rule.StoreNtPath);

    status = FltGetFileNameInformation(
        Data,
        FLT_FILE_NAME_NORMALIZED | FLT_FILE_NAME_QUERY_DEFAULT,
        &nameInfo);
    if (!NT_SUCCESS(status)) {
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    if (!PathOverlaySameOrChildPath(&source, &nameInfo->Name) ||
        PathOverlaySameOrChildPath(&store, &nameInfo->Name)) {
        FltReleaseFileNameInformation(nameInfo);
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    status = PathOverlayBuildShadowPath(&store, &source, &nameInfo->Name, &shadowPath);
    if (!NT_SUCCESS(status)) {
        FltReleaseFileNameInformation(nameInfo);
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    PATHOVERLAY_SERVICE_RESPONSE serviceResponse = { 0 };
    status = PathOverlaySendServiceRequest(PathOverlayServiceCommandQueryPath, &nameInfo->Name, &serviceResponse);
    if (NT_SUCCESS(status) && serviceResponse.PathState == PathOverlayPathStateTombstone) {
        FltReleaseFileNameInformation(nameInfo);
        ExFreePoolWithTag(shadowPath.Buffer, 'OPhP');
        Data->IoStatus.Status = STATUS_OBJECT_NAME_NOT_FOUND;
        Data->IoStatus.Information = 0;
        FltSetCallbackDataDirty(Data);
        return FLT_PREOP_COMPLETE;
    }

    if (PathOverlayIsDirectoryOpen(Data)) {
        status = PathOverlaySendServiceRequest(PathOverlayServiceCommandPrepareDirectoryView, &nameInfo->Name, NULL);
        if (NT_SUCCESS(status)) {
            FltReleaseFileNameInformation(nameInfo);
            status = IoReplaceFileObjectName(FltObjects->FileObject, shadowPath.Buffer, shadowPath.Length);
            ExFreePoolWithTag(shadowPath.Buffer, 'OPhP');
            if (NT_SUCCESS(status)) {
                Data->IoStatus.Status = STATUS_REPARSE;
                Data->IoStatus.Information = IO_REPARSE;
                FltSetCallbackDataDirty(Data);
                return FLT_PREOP_COMPLETE;
            }
            return FLT_PREOP_SUCCESS_NO_CALLBACK;
        }
    }

    writeIntent = PathOverlayHasWriteIntent(Data);
    if (writeIntent) {
        status = PathOverlaySendServiceRequest(PathOverlayServiceCommandPrepareCopyOnWrite, &nameInfo->Name, NULL);
        if (!NT_SUCCESS(status)) {
            FltReleaseFileNameInformation(nameInfo);
            ExFreePoolWithTag(shadowPath.Buffer, 'OPhP');
            Data->IoStatus.Status = status;
            Data->IoStatus.Information = 0;
            FltSetCallbackDataDirty(Data);
            return FLT_PREOP_COMPLETE;
        }
    }

    FltReleaseFileNameInformation(nameInfo);
    if (!writeIntent && !PathOverlayNtPathExists(&shadowPath)) {
        ExFreePoolWithTag(shadowPath.Buffer, 'OPhP');
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    status = IoReplaceFileObjectName(FltObjects->FileObject, shadowPath.Buffer, shadowPath.Length);
    ExFreePoolWithTag(shadowPath.Buffer, 'OPhP');
    if (!NT_SUCCESS(status)) {
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
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
    PATHOVERLAY_RULE_CACHE rule;
    UNICODE_STRING source;
    UNICODE_STRING store;
    UNICODE_STRING deleteRealPath = { 0 };
    FILE_INFORMATION_CLASS informationClass;
    BOOLEAN sourcePath;
    BOOLEAN storePath;

    UNREFERENCED_PARAMETER(FltObjects);
    *CompletionContext = NULL;

    if (!PathOverlayRuleSnapshot(&rule)) {
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    if (FltGetRequestorProcessId(Data) == rule.ServiceProcessId) {
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    RtlInitUnicodeString(&source, rule.SourceNtPath);
    RtlInitUnicodeString(&store, rule.StoreNtPath);

    status = FltGetFileNameInformation(
        Data,
        FLT_FILE_NAME_NORMALIZED | FLT_FILE_NAME_QUERY_DEFAULT,
        &nameInfo);
    if (!NT_SUCCESS(status)) {
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    sourcePath = PathOverlaySameOrChildPath(&source, &nameInfo->Name);
    storePath = PathOverlaySameOrChildPath(&store, &nameInfo->Name);
    if (!sourcePath && !storePath) {
        FltReleaseFileNameInformation(nameInfo);
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    informationClass = Data->Iopb->Parameters.SetFileInformation.FileInformationClass;
    if (sourcePath &&
        (informationClass == FileRenameInformation ||
         informationClass == FileRenameInformationEx ||
         informationClass == FileRenameInformationBypassAccessCheck ||
         informationClass == FileRenameInformationExBypassAccessCheck)) {
        FltReleaseFileNameInformation(nameInfo);
        Data->IoStatus.Status = STATUS_NOT_SUPPORTED;
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

    status = PathOverlaySendServiceRequest(PathOverlayServiceCommandRecordDelete, &deleteRealPath, NULL);
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
    UNREFERENCED_PARAMETER(FltObjects);
    *CompletionContext = NULL;

    if (PathOverlayIsTombstonedOverlayPath(Data)) {
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
    UNREFERENCED_PARAMETER(FltObjects);
    *CompletionContext = NULL;

    if (PathOverlayIsTombstonedOverlayPath(Data)) {
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
