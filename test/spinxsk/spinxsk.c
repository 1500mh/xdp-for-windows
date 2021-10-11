//
// Copyright (C) Microsoft Corporation. All rights reserved.
//

#include <windows.h>
#include <assert.h>
#include <stdio.h>
#define _CRT_RAND_S
#include <stdlib.h>
#include <afxdp_helper.h>
#include <afxdp_experimental.h>
#include <msxdp.h>
#include <msxdp_internal.h>

#include "trace.h"
#include "util.h"
#include "spinxsk.tmh"

#pragma warning(disable:4200) // nonstandard extension used: zero-sized array in struct/union

#define SHALLOW_STR_OF(x) #x
#define STR_OF(x) SHALLOW_STR_OF(x)

#define ALIGN_DOWN_BY(length, alignment) \
    ((ULONG_PTR)(length)& ~(alignment - 1))
#define ALIGN_UP_BY(length, alignment) \
    (ALIGN_DOWN_BY(((ULONG_PTR)(length)+alignment - 1), alignment))

#define STRUCT_FIELD_OFFSET(structPtr, field) \
    ((UCHAR *)&(structPtr)->field - (UCHAR *)(structPtr))

#define DEFAULT_IO_BATCH 1
#define DEFAULT_DURATION ULONG_MAX
#define DEFAULT_QUEUE_COUNT 4
#define DEFAULT_FUZZER_COUNT 3

CHAR *HELP =
"spinxsk.exe -IfIndex <ifindex> [OPTIONS]\n"
"\n"
"OPTIONS: \n"
"   -Minutes <minutes>    Duration of execution in minutes\n"
"                         Default: infinite\n"
"   -Stats                Periodic socket statistics output\n"
"                         Default: off\n"
"   -Verbose              Verbose logging\n"
"                         Default: off\n"
"   -QueueCount <count>   Number of queues to spin\n"
"                         Default: " STR_OF(DEFAULT_QUEUE_COUNT) "\n"
"   -FuzzerCount <count>  Number of fuzzer threads per queue\n"
"                         Default: " STR_OF(DEFAULT_FUZZER_COUNT) "\n"
"   -CleanDatapath        Avoid actions that invalidate the datapath\n"
"                         Default: off\n"
"   -WatchdogCmd <cmd>    Execute a system command after a watchdog violation\n"
"                         Default: \"\"\n"
;

#define ASSERT_FRE(expr) \
    if (!(expr)) { printf("("#expr") failed line %d\n", __LINE__);  exit(1);}

#define Usage() PrintUsage(__LINE__)

#ifndef TraceVerbose
#define TraceVerbose(s, ...) \
    if (verbose) { \
        SYSTEMTIME system_time; \
        char timestamp_buf[21] = { 0 }; \
        GetLocalTime(&system_time); \
        sprintf_s(timestamp_buf, ARRAYSIZE(timestamp_buf), "%d/%d/%d %d:%d:%d ", \
            system_time.wMonth, system_time.wDay, system_time.wYear, \
            system_time.wHour, system_time.wMinute, system_time.wSecond); \
        printf("%s" s, timestamp_buf, __VA_ARGS__); \
    }
#endif

#define WAIT_DRIVER_TIMEOUT_MS 1050
#define ADMIN_THREAD_TIMEOUT_SEC 1
#define WATCHDOG_THREAD_TIMEOUT_SEC 10

typedef struct QUEUE_CONTEXT QUEUE_CONTEXT;

typedef enum {
    XdpModeSystem,
    XdpModeGeneric,
    XdpModeNative,
} XDP_MODE;

CHAR *XdpModeToString[] = {
    "System",
    "Generic",
    "Native",
};

typedef struct {
    //
    // Describes the configured end state.
    //
    BOOLEAN sockRx;
    BOOLEAN sockTx;
    BOOLEAN sharedUmemSockRx;
    BOOLEAN sharedUmemSockTx;
    BOOLEAN sockRxTxSeparateThreads;

    //
    // Describes progress towards the end state.
    //
    BOOLEAN isSockRxSet;
    BOOLEAN isSockTxSet;
    BOOLEAN isSharedUmemSockRxSet;
    BOOLEAN isSharedUmemSockTxSet;
    BOOLEAN isSockBound;
    BOOLEAN isSharedUmemSockBound;
    BOOLEAN isUmemRegistered;
    HANDLE completeEvent;
} SCENARIO_CONFIG;

typedef enum {
    ThreadStateRun,
    ThreadStatePause,
    ThreadStateReturn,
} THREAD_STATE;

typedef struct {
    THREAD_STATE state;
    QUEUE_CONTEXT *queue;
} XSK_DATAPATH_SHARED;

typedef struct {
    HANDLE threadHandle;
    XSK_DATAPATH_SHARED *shared;

    HANDLE sock;
    HANDLE *rxProgram;

    struct {
        BOOLEAN rx : 1;
        BOOLEAN tx : 1;
        BOOLEAN wait : 1;
    } flags;
    ULONG txiosize;
    ULONG iobatchsize;
    XSK_POLL_MODE pollMode;

    ULONGLONG rxPacketCount;
    ULONGLONG txPacketCount;

    ULONGLONG rxWatchdogPerfCount;
    ULONGLONG txWatchdogPerfCount;

    XSK_RING rxRing;
    XSK_RING txRing;
    XSK_RING fillRing;
    XSK_RING compRing;
    XSK_RING rxFreeRing;
    XSK_RING txFreeRing;

    BYTE *rxFreeRingBase;
    BYTE *txFreeRingBase;
} XSK_DATAPATH_WORKER;

typedef struct {
    THREAD_STATE state;
    HANDLE pauseEvent;
    QUEUE_CONTEXT *queue;
} XSK_FUZZER_SHARED;

typedef struct {
    HANDLE threadHandle;
    XSK_FUZZER_SHARED *shared;
} XSK_FUZZER_WORKER;

struct QUEUE_CONTEXT {
    UINT32 queueId;

    HANDLE sock;
    HANDLE sharedUmemSock;

    HANDLE rxProgram;
    HANDLE sharedUmemRxProgram;

    XSK_UMEM_REG umemReg;
    XDP_MODE xdpMode;
    SCENARIO_CONFIG scenarioConfig;

    XSK_FUZZER_SHARED fuzzerShared;
    UINT32 fuzzerCount;
    XSK_FUZZER_WORKER *fuzzers;

    XSK_DATAPATH_SHARED datapathShared;
    XSK_DATAPATH_WORKER datapath1;
    XSK_DATAPATH_WORKER datapath2;
};

typedef struct {
    ULONG initSuccess;
    ULONG umemSuccess;
    ULONG umemTotal;
    ULONG rxSuccess;
    ULONG rxTotal;
    ULONG txSuccess;
    ULONG txTotal;
    ULONG bindSuccess;
    ULONG bindTotal;
    ULONG sharedRxSuccess;
    ULONG sharedRxTotal;
    ULONG sharedTxSuccess;
    ULONG sharedTxTotal;
    ULONG sharedBindSuccess;
    ULONG sharedBindTotal;
    ULONG setupSuccess;
} SETUP_STATS;

typedef struct {
    HANDLE threadHandle;
    UINT32 queueId;
    ULONGLONG watchdogPerfCount;
    SETUP_STATS setupStats;
} QUEUE_WORKER;

INT ifindex = -1;
ULONG duration = DEFAULT_DURATION;
BOOLEAN verbose = FALSE;
BOOLEAN cleanDatapath = FALSE;
BOOLEAN done = FALSE;
BOOLEAN extraStats = FALSE;
HANDLE stopEvent;
HANDLE workersDoneEvent;
QUEUE_WORKER *queueWorkers;
UINT32 queueCount = DEFAULT_QUEUE_COUNT;
UINT32 fuzzerCount = DEFAULT_FUZZER_COUNT;
ULONGLONG perfFreq;
CONST CHAR *watchdogCmd = "";
CONST CHAR *powershellPrefix;

ULONG
RandUlong(
    VOID
    )
{
    unsigned int r = 0;
    rand_s(&r);
    return r;
}

ULONG
Pct(
    ULONG Dividend,
    ULONG Divisor
    )
{
    return (Divisor == 0) ? 0 : Dividend * 100 / Divisor;
}

BOOLEAN
ScenarioConfigBindReady(
    _In_ CONST SCENARIO_CONFIG *ScenarioConfig
    )
{
    if (ScenarioConfig->sockRx &&
        !ReadBooleanAcquire(&ScenarioConfig->isSockRxSet)) {
        return FALSE;
    }
    if (ScenarioConfig->sockTx &&
        !ReadBooleanAcquire(&ScenarioConfig->isSockTxSet)) {
        return FALSE;
    }
    if (ScenarioConfig->sharedUmemSockRx &&
        !ReadBooleanAcquire(&ScenarioConfig->isSharedUmemSockRxSet)) {
        return FALSE;
    }
    if (ScenarioConfig->sharedUmemSockTx &&
        !ReadBooleanAcquire(&ScenarioConfig->isSharedUmemSockTxSet)) {
        return FALSE;
    }

    return TRUE;
}

BOOLEAN
ScenarioConfigComplete(
    _In_ CONST SCENARIO_CONFIG *ScenarioConfig
    )
{
    if (!ReadBooleanAcquire(&ScenarioConfig->isUmemRegistered)) {
        return FALSE;
    }
    if (!ReadBooleanAcquire(&ScenarioConfig->isSockBound)) {
        return FALSE;
    }
    if ((ScenarioConfig->sharedUmemSockRx || ScenarioConfig->sharedUmemSockTx) &&
        !ReadBooleanAcquire(&ScenarioConfig->isSharedUmemSockBound)) {
        return FALSE;
    }

    return TRUE;
}

UINT32
RingPairReserve(
    _In_ XSK_RING *ConsumerRing,
    _Out_ UINT32 *ConsumerIndex,
    _In_ XSK_RING *ProducerRing,
    _Out_ UINT32 *ProducerIndex,
    _In_ UINT32 MaxCount
    )
{
    MaxCount = XskRingConsumerReserve(ConsumerRing, MaxCount, ConsumerIndex);
    MaxCount = XskRingProducerReserve(ProducerRing, MaxCount, ProducerIndex);
    return MaxCount;
}

static
VOID
FuzzHookId(
    _Inout_ XDP_HOOK_ID *HookId
    )
{
    if (!(RandUlong() % 8)) {
        HookId->Layer = RandUlong() % 4;
    } else if (!(RandUlong() % 8)) {
        HookId->Layer = RandUlong();
    }

    if (!(RandUlong() % 8)) {
        HookId->Direction = RandUlong() % 4;
    } else if (!(RandUlong() % 8)) {
        HookId->Direction = RandUlong();
    }

    if (!(RandUlong() % 8)) {
        HookId->SubLayer = RandUlong() % 4;
    } else if (!(RandUlong() % 8)) {
        HookId->SubLayer = RandUlong();
    }
}

HRESULT
AttachXdpProgram(
    _In_ QUEUE_CONTEXT *Queue,
    _In_ HANDLE Sock,
    _In_ BOOLEAN RxRequired,
    _Inout_ HANDLE *RxProgramHandle
    )
{
    XDP_RULE rule = {0};
    XDP_HOOK_ID hookId = {0};
    UINT32 hookIdSize = sizeof(hookId);
    UINT32 flags = 0;
    HANDLE handle;
    HRESULT res;

    rule.Match = XDP_MATCH_ALL;
    rule.Action = XDP_PROGRAM_ACTION_REDIRECT;
    rule.Redirect.TargetType = XDP_REDIRECT_TARGET_TYPE_XSK;
    rule.Redirect.Target = Sock;

    if (Queue->xdpMode == XdpModeGeneric) {
        flags |= XDP_ATTACH_GENERIC;
    } else if (Queue->xdpMode == XdpModeNative) {
        flags |= XDP_ATTACH_NATIVE;
    }

    res = XskGetSockopt(Sock, XSK_SOCKOPT_RX_HOOK_ID, &hookId, &hookIdSize);
    if (FAILED(res)) {
        goto Exit;
    }
    ASSERT_FRE(hookIdSize == sizeof(hookId));

    FuzzHookId(&hookId);

    res = XdpCreateProgram(ifindex, &hookId, Queue->queueId, flags, &rule, 1, &handle);
    if (SUCCEEDED(res)) {
        if (InterlockedCompareExchangePointer(RxProgramHandle, handle, NULL) != NULL) {
            CloseHandle(handle);
        }
    }

Exit:

    return RxRequired ? res : S_OK;
}

VOID
DetachXdpProgram(
    _Inout_ HANDLE *RxProgramHandle
    )
{
    HANDLE handle = InterlockedExchangePointer(RxProgramHandle, NULL);

    if (handle != NULL) {
        ASSERT_FRE(CloseHandle(handle));
    }
}

HRESULT
FreeRingInitialize(
    _Inout_ XSK_RING *FreeRing,
    _Out_ BYTE **FreeRingBase,
    _In_ UINT32 DescriptorStart,
    _In_ UINT32 DescriptorStride,
    _In_ UINT32 DescriptorCount
    )
{
    XSK_RING_INFO freeRingInfo = {0};
    UINT64 desc = DescriptorStart;

    struct {
        UINT32 Producer;
        UINT32 Consumer;
        UINT32 Flags;
        UINT64 Descriptors[0];
    } *FreeRingLayout;

    FreeRingLayout =
        calloc(1, sizeof(*FreeRingLayout) + DescriptorCount * sizeof(*FreeRingLayout->Descriptors));
    if (FreeRingLayout == NULL) {
        return E_OUTOFMEMORY;
    }
    *FreeRingBase = (BYTE *)FreeRingLayout;

    freeRingInfo.ring = (BYTE *)FreeRingLayout;
    freeRingInfo.producerIndexOffset = (UINT32)STRUCT_FIELD_OFFSET(FreeRingLayout, Producer);
    freeRingInfo.consumerIndexOffset = (UINT32)STRUCT_FIELD_OFFSET(FreeRingLayout, Consumer);
    freeRingInfo.flagsOffset = (UINT32)STRUCT_FIELD_OFFSET(FreeRingLayout, Flags);
    freeRingInfo.descriptorsOffset = (UINT32)STRUCT_FIELD_OFFSET(FreeRingLayout, Descriptors[0]);
    freeRingInfo.size = DescriptorCount;
    freeRingInfo.elementStride = sizeof(*FreeRingLayout->Descriptors);
    XskRingInitialize(FreeRing, &freeRingInfo);

    for (UINT32 i = 0; i < DescriptorCount; i++) {
        UINT64 *Descriptor = XskRingGetElement(FreeRing, i);
        *Descriptor = desc;
        desc += DescriptorStride;
    }
    XskRingProducerSubmit(FreeRing, DescriptorCount);

    return S_OK;
}

VOID
CleanupQueue(
    _In_ QUEUE_CONTEXT *Queue
    )
{
    BOOL res;

    DetachXdpProgram(&Queue->rxProgram);
    DetachXdpProgram(&Queue->sharedUmemRxProgram);

    if (Queue->sock != NULL) {
        CloseHandle(Queue->sock);
    }
    if (Queue->sharedUmemSock != NULL) {
        CloseHandle(Queue->sharedUmemSock);
    }

    if (Queue->fuzzerShared.pauseEvent != NULL) {
        CloseHandle(Queue->fuzzerShared.pauseEvent);
    }
    if (Queue->scenarioConfig.completeEvent != NULL) {
        CloseHandle(Queue->scenarioConfig.completeEvent);
    }

    if (Queue->fuzzers != NULL) {
        free(Queue->fuzzers);
    }

    if (Queue->umemReg.address != NULL) {
        res = VirtualFree(Queue->umemReg.address, 0, MEM_RELEASE);
        ASSERT_FRE(res);
    }

    free(Queue);
}

HRESULT
InitializeQueue(
    _In_ UINT32 QueueId,
    _Out_ QUEUE_CONTEXT **Queue
    )
{
    HRESULT res;
    QUEUE_CONTEXT *queue;

    queue = malloc(sizeof(*queue));
    if (queue == NULL) {
        res = E_OUTOFMEMORY;
        goto Exit;
    }
    RtlZeroMemory(queue, sizeof(*queue));

    queue->queueId = QueueId;
    queue->fuzzerCount = fuzzerCount;

    queue->fuzzers = calloc(queue->fuzzerCount, sizeof(*queue->fuzzers));
    if (queue->fuzzers == NULL) {
        res = E_OUTOFMEMORY;
        goto Exit;
    }

    queue->fuzzerShared.pauseEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    ASSERT_FRE(queue->fuzzerShared.pauseEvent != NULL);
    queue->scenarioConfig.completeEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    ASSERT_FRE(queue->scenarioConfig.completeEvent != NULL);
    queue->fuzzerShared.queue = queue;
    for (UINT32 i = 0; i < queue->fuzzerCount; i++) {
        queue->fuzzers[i].shared = &queue->fuzzerShared;
    }

    switch (RandUlong() % 3) {
    case 0:
        queue->xdpMode = XdpModeSystem;
        break;
    case 1:
        queue->xdpMode = XdpModeGeneric;
        break;
    case 2:
        queue->xdpMode = XdpModeNative;
        break;
    }

    //
    // Fuzz the IO scenario.
    //
    switch (RandUlong() % 3) {
    case 0:
        queue->scenarioConfig.sockRx = TRUE;
        break;
    case 1:
        queue->scenarioConfig.sockTx = TRUE;
        break;
    case 2:
        queue->scenarioConfig.sockRx = TRUE;
        queue->scenarioConfig.sockTx = TRUE;
        if (RandUlong() % 2) {
            queue->scenarioConfig.sockRxTxSeparateThreads = TRUE;
        }
        break;
    }
    if (!queue->scenarioConfig.sockRx && (RandUlong() % 2)) {
        queue->scenarioConfig.sharedUmemSockRx = TRUE;
    }
    if (!queue->scenarioConfig.sockTx && (RandUlong() % 2)) {
        queue->scenarioConfig.sharedUmemSockTx = TRUE;
    }

    res = XskCreate(&queue->sock);
    if (FAILED(res)) {
        goto Exit;
    }

    if (queue->scenarioConfig.sharedUmemSockRx || queue->scenarioConfig.sharedUmemSockTx) {
        res = XskCreate(&queue->sharedUmemSock);
        if (FAILED(res)) {
            goto Exit;
        }
    }

    queue->datapathShared.queue = queue;
    queue->datapath1.shared = &queue->datapathShared;
    queue->datapath2.shared = &queue->datapathShared;

    queue->datapath1.sock = queue->sock;
    queue->datapath1.rxProgram = &queue->rxProgram;
    if (queue->scenarioConfig.sockRxTxSeparateThreads) {
        queue->datapath1.flags.rx = TRUE;
        queue->datapath2.sock = queue->sock;
        queue->datapath2.rxProgram = &queue->rxProgram;
        queue->datapath2.flags.tx = TRUE;
    } else {
        if (queue->scenarioConfig.sockRx) {
            queue->datapath1.flags.rx = TRUE;
        }
        if (queue->scenarioConfig.sockTx) {
            queue->datapath1.flags.tx = TRUE;
        }
        if (queue->scenarioConfig.sharedUmemSockRx) {
            queue->datapath2.sock = queue->sharedUmemSock;
            queue->datapath2.rxProgram = &queue->sharedUmemRxProgram;
            queue->datapath2.flags.rx = TRUE;
        }
        if (queue->scenarioConfig.sharedUmemSockTx) {
            queue->datapath2.sock = queue->sharedUmemSock;
            queue->datapath2.rxProgram = &queue->sharedUmemRxProgram;
            queue->datapath2.flags.tx = TRUE;
        }
    }
    TraceVerbose(
        "q[%u]: datapath1_rx:%d datapath1_tx:%d datapath2_rx:%d datapath2_tx:%d sharedUmem:%d xdpMode:%s",
        queue->queueId, queue->datapath1.flags.rx, queue->datapath1.flags.tx,
        queue->datapath2.flags.rx, queue->datapath2.flags.tx,
        (queue->datapath1.sock != queue->datapath2.sock && queue->datapath2.sock != NULL),
        XdpModeToString[queue->xdpMode]);

    *Queue = queue;
    ASSERT_FRE(SUCCEEDED(res));

Exit:

    if (FAILED(res)) {
        if (queue != NULL) {
            CleanupQueue(queue);
        }
    }

    return res;
}

VOID
FuzzSocketUmemSetup(
    _Inout_ QUEUE_CONTEXT *Queue,
    _In_ HANDLE Sock,
    _Inout_ BOOLEAN *WasUmemRegistered
    )
{
    HRESULT res;

    if (RandUlong() % 2) {
        XSK_UMEM_REG umemReg = {0};

        if (RandUlong() % 6) {
            umemReg.totalSize = RandUlong() % 0x100000;
        } else {
            umemReg.totalSize = RandUlong();
        }

        if (RandUlong() % 6) {
            umemReg.chunkSize = RandUlong() % 4096;
        } else {
            umemReg.chunkSize = RandUlong();
        }

        if (RandUlong() % 6) {
            umemReg.headroom = RandUlong() % ((umemReg.chunkSize / 4) + 1);
        } else {
            umemReg.headroom = RandUlong();
        }

        umemReg.address =
            VirtualAlloc(
                NULL, umemReg.totalSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (umemReg.address == NULL) {
            return;
        }

        res =
            XskSetSockopt(
                Sock, XSK_SOCKOPT_UMEM_REG, &umemReg, sizeof(umemReg));
        if (SUCCEEDED(res)) {
            Queue->umemReg = umemReg;
            WriteBooleanRelease(WasUmemRegistered, TRUE);
            TraceVerbose("q[%u]: umem totalSize:%llu chunkSize:%u headroom:%u",
                Queue->queueId, umemReg.totalSize, umemReg.chunkSize, umemReg.headroom);
        } else {
            BOOL success = VirtualFree(umemReg.address, 0, MEM_RELEASE);
            ASSERT_FRE(success);
        }
    }
}

VOID
FuzzRingSize(
    _In_ QUEUE_CONTEXT *Queue,
    _Out_ UINT32 *Size
    )
{
    UINT32 numUmemDescriptors;

    if (ReadBooleanAcquire(&Queue->scenarioConfig.isUmemRegistered)) {
        numUmemDescriptors = (UINT32)(Queue->umemReg.totalSize / Queue->umemReg.chunkSize);
    } else {
        numUmemDescriptors = (RandUlong() % 16) + 1;
    }

    if (RandUlong() % 2) {
        *Size = 1ui32 << (RandUlong() % (RtlFindMostSignificantBit(numUmemDescriptors) + 1));
    } else {
        *Size = RandUlong();
    }
}

VOID
FuzzSocketRxTxSetup(
    _In_ QUEUE_CONTEXT *Queue,
    _In_ HANDLE Sock,
    _In_ BOOLEAN RequiresRx,
    _In_ BOOLEAN RequiresTx,
    _Inout_ BOOLEAN *WasRxSet,
    _Inout_ BOOLEAN *WasTxSet
    )
{
    HRESULT res;
    UINT32 ringSize;

    if (RequiresRx) {
        if (RandUlong() % 2) {
            FuzzRingSize(Queue, &ringSize);
            res =
                XskSetSockopt(
                    Sock, XSK_SOCKOPT_RX_RING_SIZE, &ringSize, sizeof(ringSize));
            if (SUCCEEDED(res)) {
                WriteBooleanRelease(WasRxSet, TRUE);
            }
        }
    }

    if (RequiresTx) {
        if (RandUlong() % 2) {
            FuzzRingSize(Queue, &ringSize);
            res =
                XskSetSockopt(
                    Sock, XSK_SOCKOPT_TX_RING_SIZE, &ringSize, sizeof(ringSize));
            if (SUCCEEDED(res)) {
                WriteBooleanRelease(WasTxSet, TRUE);
            }
        }
    }

    if (RandUlong() % 2) {
        FuzzRingSize(Queue, &ringSize);
        res =
            XskSetSockopt(
                Sock, XSK_SOCKOPT_RX_FILL_RING_SIZE, &ringSize, sizeof(ringSize));
    }

    if (RandUlong() % 2) {
        FuzzRingSize(Queue, &ringSize);
        res =
            XskSetSockopt(
                Sock, XSK_SOCKOPT_TX_COMPLETION_RING_SIZE, &ringSize, sizeof(ringSize));
    }
}

VOID
FuzzSocketMisc(
    _In_ QUEUE_CONTEXT *Queue,
    _In_ HANDLE Sock,
    _Inout_ HANDLE *RxProgramHandle
    )
{
    UINT32 optSize;

    if (RandUlong() % 2) {
        XSK_RING_INFO_SET ringInfo;
        optSize = sizeof(ringInfo);
        XskGetSockopt(Sock, XSK_SOCKOPT_RING_INFO, &ringInfo, &optSize);
    }

    if (RandUlong() % 2) {
        XSK_STATISTICS stats;
        optSize = sizeof(stats);
        XskGetSockopt(Sock, XSK_SOCKOPT_STATISTICS, &stats, &optSize);
    }

    if (RandUlong() % 2) {
        XDP_HOOK_ID hookId = {
            XDP_HOOK_L2,
            XDP_HOOK_RX,
            XDP_HOOK_INSPECT,
        };
        FuzzHookId(&hookId);
        XskSetSockopt(Sock, XSK_SOCKOPT_RX_HOOK_ID, &hookId, sizeof(hookId));
    }

    if (RandUlong() % 2) {
        XDP_HOOK_ID hookId = {
            XDP_HOOK_L2,
            XDP_HOOK_TX,
            XDP_HOOK_INJECT,
        };
        FuzzHookId(&hookId);
        XskSetSockopt(Sock, XSK_SOCKOPT_TX_HOOK_ID, &hookId, sizeof(hookId));
    }

    if (RandUlong() % 2) {
        UINT32 notifyFlags = 0;
        UINT32 timeoutMs = 0;
        UINT32 notifyResult;

        if (RandUlong() % 2) {
            notifyFlags |= XSK_NOTIFY_POKE_RX;
        }
        if (RandUlong() % 2) {
            notifyFlags |= XSK_NOTIFY_POKE_TX;
        }
        if (RandUlong() % 2) {
            notifyFlags |= XSK_NOTIFY_WAIT_RX;
        }
        if (RandUlong() % 2) {
            notifyFlags |= XSK_NOTIFY_WAIT_TX;
        }

        if (RandUlong() % 2) {
            timeoutMs = RandUlong() % 1000;
        }

        XskNotifySocket(Sock, notifyFlags, timeoutMs, &notifyResult);
    }

    if (!cleanDatapath && !(RandUlong() % 3)) {
        DetachXdpProgram(RxProgramHandle);

        WaitForSingleObject(stopEvent, 20);

        AttachXdpProgram(Queue, Sock, FALSE, RxProgramHandle);
    }
}

VOID
FuzzSocketBind(
    _In_ QUEUE_CONTEXT *Queue,
    _In_ HANDLE Sock,
    _In_opt_ HANDLE SharedUmemSock,
    _Inout_ BOOLEAN *WasSockBound
    )
{
    HRESULT res;
    UINT32 bindFlags = 0;

    if (RandUlong() % 2) {

        if (Queue->xdpMode == XdpModeGeneric) {
            bindFlags |= XSK_BIND_GENERIC;
        } else if (Queue->xdpMode == XdpModeNative) {
            bindFlags |= XSK_BIND_NATIVE;
        }

        if (!(RandUlong() % 10)) {
            bindFlags |= 0x1 << (RandUlong() % 32);
        }

        res = XskBind(Sock, ifindex, Queue->queueId, bindFlags, SharedUmemSock);
        if (SUCCEEDED(res)) {
            WriteBooleanRelease(WasSockBound, TRUE);
        }
    }
}

VOID
CleanupDatapath(
    _Inout_ XSK_DATAPATH_WORKER *Datapath
    )
{
    DetachXdpProgram(Datapath->rxProgram);

    if (Datapath->rxFreeRingBase) {
        free(Datapath->rxFreeRingBase);
    }
    if (Datapath->txFreeRingBase) {
        free(Datapath->txFreeRingBase);
    }
}

HRESULT
InitializeDatapath(
    _Inout_ XSK_DATAPATH_WORKER *Datapath
    )
{
    HRESULT res;
    XSK_RING_INFO_SET ringInfo;
    UINT32 ringInfoSize = sizeof(ringInfo);
    QUEUE_CONTEXT *queue = Datapath->shared->queue;
    UINT32 umemDescriptorCount =
        (UINT32)(queue->umemReg.totalSize / queue->umemReg.chunkSize);
    UINT32 descriptorCount = umemDescriptorCount / 2;
    UINT32 descriptorStart;

    Datapath->iobatchsize = DEFAULT_IO_BATCH;
    Datapath->pollMode = XSK_POLL_MODE_DEFAULT;
    Datapath->flags.wait = FALSE;
    Datapath->txiosize = queue->umemReg.chunkSize - queue->umemReg.headroom;

    res = XskGetSockopt(Datapath->sock, XSK_SOCKOPT_RING_INFO, &ringInfo, &ringInfoSize);
    if (FAILED(res)) {
        goto Exit;
    }
    ASSERT_FRE(ringInfoSize == sizeof(ringInfo));

    //
    // Create separate free rings for RX and TX, each getting half of the UMEM
    // descriptor space.
    //

    if (Datapath->flags.rx) {
        TraceVerbose("q[%u]d[0x%p]: rx_size:%u fill_size:%u",
            queue->queueId, Datapath->threadHandle, ringInfo.rx.size, ringInfo.fill.size);
        XskRingInitialize(&Datapath->rxRing, &ringInfo.rx);
        XskRingInitialize(&Datapath->fillRing, &ringInfo.fill);

        ASSERT_FRE(Datapath->rxRing.size > 0 && Datapath->fillRing.size > 0);

        descriptorStart = 0;
        res =
            FreeRingInitialize(
                &Datapath->rxFreeRing, &Datapath->rxFreeRingBase, descriptorStart,
                queue->umemReg.chunkSize, descriptorCount);
        if (FAILED(res)) {
            goto Exit;
        }

    }
    if (Datapath->flags.tx) {
        TraceVerbose("q[%u]d[0x%p]: tx_size:%u comp_size:%u",
            queue->queueId, Datapath->threadHandle, ringInfo.tx.size, ringInfo.completion.size);
        XskRingInitialize(&Datapath->txRing, &ringInfo.tx);
        XskRingInitialize(&Datapath->compRing, &ringInfo.completion);

        ASSERT_FRE(Datapath->txRing.size > 0 && Datapath->compRing.size > 0);

        descriptorStart = (umemDescriptorCount / 2) * queue->umemReg.chunkSize;
        res =
            FreeRingInitialize(
                &Datapath->txFreeRing, &Datapath->txFreeRingBase, descriptorStart,
                queue->umemReg.chunkSize, descriptorCount);
        if (FAILED(res)) {
            goto Exit;
        }
    }

    res =
        AttachXdpProgram(
            Datapath->shared->queue, Datapath->sock, Datapath->flags.rx, Datapath->rxProgram);
    if (FAILED(res)) {
        goto Exit;
    }

    ASSERT_FRE(SUCCEEDED(res));

Exit:

    if (FAILED(res)) {
        CleanupDatapath(Datapath);
    }

    return res;
}

VOID
NotifyDriver(
    _In_ XSK_DATAPATH_WORKER *Datapath,
    _In_ UINT32 DirectionFlags
    )
{
    UINT32 notifyResult;

    //
    // Ensure poke flags are read after writing producer/consumer indices.
    //
    MemoryBarrier();

    if ((DirectionFlags & XSK_NOTIFY_POKE_RX) && !XskRingProducerNeedPoke(&Datapath->fillRing)) {
        DirectionFlags &= ~XSK_NOTIFY_POKE_RX;
    }
    if ((DirectionFlags & XSK_NOTIFY_POKE_TX) && !XskRingProducerNeedPoke(&Datapath->txRing)) {
        DirectionFlags &= ~XSK_NOTIFY_POKE_TX;
    }

    if (DirectionFlags != 0) {
        XskNotifySocket(Datapath->sock, DirectionFlags, WAIT_DRIVER_TIMEOUT_MS, &notifyResult);
    }
}

BOOLEAN
ProcessPkts(
    _Inout_ XSK_DATAPATH_WORKER *Datapath
    )
{
    UINT32 notifyFlags = 0;
    UINT32 available;
    UINT32 consumerIndex;
    UINT32 producerIndex;

    if (Datapath->flags.rx) {
        //
        // Move packets from the RX ring to the RX free ring.
        //
        available =
            RingPairReserve(
                &Datapath->rxRing, &consumerIndex, &Datapath->rxFreeRing, &producerIndex, Datapath->iobatchsize);
        if (available > 0) {
            for (UINT32 i = 0; i < available; i++) {
                XSK_BUFFER_DESCRIPTOR *rxDesc = XskRingGetElement(&Datapath->rxRing, consumerIndex++);
                UINT64 *freeDesc = XskRingGetElement(&Datapath->rxFreeRing, producerIndex++);

                *freeDesc = XskDescriptorGetAddress(rxDesc->address);
            }

            XskRingConsumerRelease(&Datapath->rxRing, available);
            XskRingProducerSubmit(&Datapath->rxFreeRing, available);

            Datapath->rxPacketCount += available;
            QueryPerformanceCounter((LARGE_INTEGER*)&Datapath->rxWatchdogPerfCount);
        }

        //
        // Move packets from the RX free ring to the fill ring.
        //
        available =
            RingPairReserve(
                &Datapath->rxFreeRing, &consumerIndex, &Datapath->fillRing, &producerIndex, Datapath->iobatchsize);
        if (available > 0) {
            for (UINT32 i = 0; i < available; i++) {
                UINT64 *freeDesc = XskRingGetElement(&Datapath->rxFreeRing, consumerIndex++);
                UINT64 *fillDesc = XskRingGetElement(&Datapath->fillRing, producerIndex++);

                *fillDesc = *freeDesc;
            }

            XskRingConsumerRelease(&Datapath->rxFreeRing, available);
            XskRingProducerSubmit(&Datapath->fillRing, available);

            notifyFlags |= XSK_NOTIFY_POKE_RX;
        }

        if (Datapath->flags.wait &&
            XskRingConsumerReserve(&Datapath->rxRing, 1, &consumerIndex) == 0 &&
            XskRingConsumerReserve(&Datapath->rxFreeRing, 1, &consumerIndex) == 0) {
            notifyFlags |= XSK_NOTIFY_WAIT_RX;
        }

        if (Datapath->pollMode == XSK_POLL_MODE_SOCKET) {
            //
            // If socket poll mode is supported by the program, always enable pokes.
            //
            notifyFlags |= XSK_NOTIFY_POKE_RX;
        }
    }

    if (Datapath->flags.tx) {
        //
        // Move packets from the completion ring to the TX free ring.
        //
        available =
            RingPairReserve(
                &Datapath->compRing, &consumerIndex, &Datapath->txFreeRing, &producerIndex, Datapath->iobatchsize);
        if (available > 0) {
            for (UINT32 i = 0; i < available; i++) {
                UINT64 *compDesc = XskRingGetElement(&Datapath->compRing, consumerIndex++);
                UINT64 *freeDesc = XskRingGetElement(&Datapath->txFreeRing, producerIndex++);

                *freeDesc = *compDesc;
            }

            XskRingConsumerRelease(&Datapath->compRing, available);
            XskRingProducerSubmit(&Datapath->txFreeRing, available);

            Datapath->txPacketCount += available;
            QueryPerformanceCounter((LARGE_INTEGER*)&Datapath->txWatchdogPerfCount);
        }

        //
        // Move packets from the TX free ring to the tx ring.
        //
        available =
            RingPairReserve(
                &Datapath->txFreeRing, &consumerIndex, &Datapath->txRing, &producerIndex, Datapath->iobatchsize);
        if (available > 0) {
            for (UINT32 i = 0; i < available; i++) {
                UINT64 *freeDesc = XskRingGetElement(&Datapath->txFreeRing, consumerIndex++);
                XSK_BUFFER_DESCRIPTOR *txDesc = XskRingGetElement(&Datapath->txRing, producerIndex++);

                txDesc->address = *freeDesc;
                txDesc->length = Datapath->txiosize;
            }

            XskRingConsumerRelease(&Datapath->txFreeRing, available);
            XskRingProducerSubmit(&Datapath->txRing, available);

            notifyFlags |= XSK_NOTIFY_POKE_TX;
        }

        if (Datapath->flags.wait &&
            XskRingConsumerReserve(&Datapath->compRing, 1, &consumerIndex) == 0 &&
            XskRingConsumerReserve(&Datapath->txFreeRing, 1, &consumerIndex) == 0) {
            notifyFlags |= XSK_NOTIFY_WAIT_TX;
        }

        if (Datapath->pollMode == XSK_POLL_MODE_SOCKET) {
            //
            // If socket poll mode is supported by the program, always enable pokes.
            //
            notifyFlags |= XSK_NOTIFY_POKE_TX;
        }
    }

    if (notifyFlags != 0) {
        NotifyDriver(Datapath, notifyFlags);
    }

    //
    // TODO: Return FALSE when datapath can no longer process packets (RX/TX ring is not valid).
    //
    return TRUE;
}

VOID
PrintDatapathStats(
    _In_ CONST XSK_DATAPATH_WORKER *Datapath
    )
{
    XSK_STATISTICS stats;
    UINT32 optSize = sizeof(stats);
    CHAR rxPacketCount[64] = { 0 };
    CHAR txPacketCount[64] = { 0 };
    HRESULT res;

    res = XskGetSockopt(Datapath->sock, XSK_SOCKOPT_STATISTICS, &stats, &optSize);
    if (FAILED(res)) {
        return;
    }
    ASSERT_FRE(optSize == sizeof(stats));

    if (Datapath->flags.rx) {
        sprintf_s(rxPacketCount, sizeof(rxPacketCount), "%llu", Datapath->rxPacketCount);
    } else {
        sprintf_s(rxPacketCount, sizeof(rxPacketCount), "n/a");
    }

    if (Datapath->flags.tx) {
        sprintf_s(txPacketCount, sizeof(txPacketCount), "%llu", Datapath->txPacketCount);
    } else {
        sprintf_s(txPacketCount, sizeof(txPacketCount), "n/a");
    }

    printf("q[%u]d[0x%p]: rx:%s tx:%s rxDrop:%llu rxTrunc:%llu "
        "rxInvalidDesc:%llu txInvalidDesc:%llu xdpMode:%s\n",
        Datapath->shared->queue->queueId, Datapath->threadHandle,
        rxPacketCount, txPacketCount, stats.rxDropped, stats.rxTruncated,
        stats.rxInvalidDescriptors, stats.txInvalidDescriptors,
        XdpModeToString[Datapath->shared->queue->xdpMode]);
}

DWORD
WINAPI
XskDatapathWorkerFn(
    _In_ VOID *ThreadParameter
    )
{
    XSK_DATAPATH_WORKER *datapath = ThreadParameter;
    QUEUE_CONTEXT *queue = datapath->shared->queue;

    TraceEnter("q[%u]d[0x%p]", queue->queueId, datapath->threadHandle);

    if (SUCCEEDED(InitializeDatapath(datapath))) {
        while (!ReadBooleanNoFence(&done)) {
            if (ReadNoFence((LONG *)&datapath->shared->state) == ThreadStateReturn) {
                break;
            }

            if (!ProcessPkts(datapath)) {
                break;
            }
        }

        if (extraStats) {
            PrintDatapathStats(datapath);
        }
        CleanupDatapath(datapath);
    }

    TraceExit("q[%u]d[0x%p]", queue->queueId, datapath->threadHandle);
    return 0;
}

DWORD
WINAPI
XskFuzzerWorkerFn(
    _In_ VOID *ThreadParameter
    )
{
    XSK_FUZZER_WORKER *fuzzer = ThreadParameter;
    QUEUE_CONTEXT *queue = fuzzer->shared->queue;
    SCENARIO_CONFIG *scenarioConfig = &queue->scenarioConfig;

    TraceEnter("q[%u]f[0x%p]", queue->queueId, fuzzer->threadHandle);

    while (!ReadBooleanNoFence(&done)) {
        if (ReadNoFence((LONG *)&fuzzer->shared->state) == ThreadStateReturn) {
            break;
        }

        if (ReadNoFence((LONG *)&fuzzer->shared->state) == ThreadStatePause) {
            WaitForSingleObject(fuzzer->shared->pauseEvent, INFINITE);
            continue;
        }

        FuzzSocketUmemSetup(queue, queue->sock, &scenarioConfig->isUmemRegistered);

        FuzzSocketRxTxSetup(
            queue, queue->sock,
            scenarioConfig->sockRx, scenarioConfig->sockTx,
            &scenarioConfig->isSockRxSet, &scenarioConfig->isSockTxSet);
        FuzzSocketMisc(queue, queue->sock, &queue->rxProgram);

        if (queue->sharedUmemSock != NULL) {
            FuzzSocketRxTxSetup(
                queue, queue->sharedUmemSock,
                scenarioConfig->sharedUmemSockRx, scenarioConfig->sharedUmemSockTx,
                &scenarioConfig->isSharedUmemSockRxSet, &scenarioConfig->isSharedUmemSockTxSet);
            FuzzSocketMisc(queue, queue->sharedUmemSock, &queue->sharedUmemRxProgram);
        }

        if (ScenarioConfigBindReady(scenarioConfig)) {
            FuzzSocketBind(queue, queue->sock, NULL, &scenarioConfig->isSockBound);
            if (queue->sharedUmemSock != NULL) {
                FuzzSocketBind(
                    queue, queue->sharedUmemSock, queue->sock,
                    &scenarioConfig->isSharedUmemSockBound);
            }
        }

        if (ScenarioConfigComplete(scenarioConfig)) {
            if (WaitForSingleObject(scenarioConfig->completeEvent, 0) != WAIT_OBJECT_0) {
                TraceVerbose("q[%u]f[0x%p]: marking socket setup complete",
                    queue->queueId, fuzzer->threadHandle);
                SetEvent(scenarioConfig->completeEvent);
            }
            WaitForSingleObject(stopEvent, 50);
        }
    }

    TraceExit("q[%u]f[0x%p]", queue->queueId, fuzzer->threadHandle);
    return 0;
}

VOID
UpdateSetupStats(
    _In_ QUEUE_WORKER *QueueWorker,
    _In_ QUEUE_CONTEXT *Queue
    )
{
    //
    // Every scenario requires at least one UMEM registered and one socket bound.
    //
    ++QueueWorker->setupStats.initSuccess;
    ++QueueWorker->setupStats.umemTotal;
    ++QueueWorker->setupStats.bindTotal;

    if (Queue->scenarioConfig.isUmemRegistered) {
        ++QueueWorker->setupStats.umemSuccess;
    }
    if (Queue->scenarioConfig.sockRx) {
        ++QueueWorker->setupStats.rxTotal;
        if (Queue->scenarioConfig.isSockRxSet) {
            ++QueueWorker->setupStats.rxSuccess;
        }
    }
    if (Queue->scenarioConfig.sockTx) {
        ++QueueWorker->setupStats.txTotal;
        if (Queue->scenarioConfig.isSockTxSet) {
            ++QueueWorker->setupStats.txSuccess;
        }
    }
    if (Queue->scenarioConfig.isSockBound) {
        ++QueueWorker->setupStats.bindSuccess;
    }
    if (Queue->scenarioConfig.sharedUmemSockRx) {
        ++QueueWorker->setupStats.sharedRxTotal;
        if (Queue->scenarioConfig.isSharedUmemSockRxSet) {
            ++QueueWorker->setupStats.sharedRxSuccess;
        }
    }
    if (Queue->scenarioConfig.sharedUmemSockTx) {
        ++QueueWorker->setupStats.sharedTxTotal;
        if (Queue->scenarioConfig.isSharedUmemSockTxSet) {
            ++QueueWorker->setupStats.sharedTxSuccess;
        }
    }
    if (Queue->scenarioConfig.sharedUmemSockRx || Queue->scenarioConfig.sharedUmemSockTx) {
        ++QueueWorker->setupStats.sharedBindTotal;
        if (Queue->scenarioConfig.isSharedUmemSockBound) {
            ++QueueWorker->setupStats.sharedBindSuccess;
        }
    }
    if (ScenarioConfigComplete(&Queue->scenarioConfig)) {
        ++QueueWorker->setupStats.setupSuccess;
    }
}

VOID
PrintSetupStats(
    _In_ QUEUE_WORKER *QueueWorker,
    _In_ ULONG NumIterations
    )
{
    SETUP_STATS *setupStats = &QueueWorker->setupStats;

    printf(
        "\tbreakdown\n"
        "\tinit:       (%lu / %lu) %lu%%\n"
        "\tumem:       (%lu / %lu) %lu%%\n"
        "\trx:         (%lu / %lu) %lu%%\n"
        "\ttx:         (%lu / %lu) %lu%%\n"
        "\tbind:       (%lu / %lu) %lu%%\n"
        "\tsharedRx:   (%lu / %lu) %lu%%\n"
        "\tsharedTx:   (%lu / %lu) %lu%%\n"
        "\tsharedBind: (%lu / %lu) %lu%%\n",
        setupStats->initSuccess, NumIterations, Pct(setupStats->initSuccess, NumIterations),
        setupStats->umemSuccess, setupStats->umemTotal, Pct(setupStats->umemSuccess, setupStats->umemTotal),
        setupStats->rxSuccess, setupStats->rxTotal, Pct(setupStats->rxSuccess, setupStats->rxTotal),
        setupStats->txSuccess, setupStats->txTotal, Pct(setupStats->txSuccess, setupStats->txTotal),
        setupStats->bindSuccess, setupStats->bindTotal, Pct(setupStats->bindSuccess, setupStats->bindTotal),
        setupStats->sharedRxSuccess, setupStats->sharedRxTotal, Pct(setupStats->sharedRxSuccess, setupStats->sharedRxTotal),
        setupStats->sharedTxSuccess, setupStats->sharedTxTotal, Pct(setupStats->sharedTxSuccess, setupStats->sharedTxTotal),
        setupStats->sharedBindSuccess, setupStats->sharedBindTotal, Pct(setupStats->sharedBindSuccess, setupStats->sharedBindTotal));

}

DWORD
WINAPI
QueueWorkerFn(
    _In_ VOID *ThreadParameter
    )
{
    QUEUE_WORKER *queueWorker = ThreadParameter;
    ULONG numIterations = 0;
    ULONG numSuccessfulSetups = 0;
    ULONG successPct;

    TraceEnter("q[%u]", queueWorker->queueId);

    while (!ReadBooleanNoFence(&done)) {
        QUEUE_CONTEXT *queue;
        DWORD ret;

        ++numIterations;
        TraceVerbose("q[%u]: iter %lu", queueWorker->queueId, numIterations);

        QueryPerformanceCounter((LARGE_INTEGER*)&queueWorker->watchdogPerfCount);

        if (!SUCCEEDED(InitializeQueue(queueWorker->queueId, &queue))) {
            TraceVerbose("q[%u]: failed to setup queue", queueWorker->queueId);
            continue;
        }

        //
        // Hand off socket/s to fuzzer threads.
        //
        for (UINT32 i = 0; i < queue->fuzzerCount; i++) {
            queue->fuzzers[i].threadHandle =
                CreateThread(NULL, 0, XskFuzzerWorkerFn, &queue->fuzzers[i], 0, NULL);
        }

        //
        // Wait until fuzzers have successfully configured the socket/s.
        //
        TraceVerbose("q[%u]: waiting for sockets to be configured", queue->queueId);
        ret = WaitForSingleObject(queue->scenarioConfig.completeEvent, 500);

        if (ret == WAIT_OBJECT_0) {
            ++numSuccessfulSetups;

            //
            // Hand off configured socket/s to datapath thread/s.
            //
            if (queue->datapath1.sock != NULL) {
                queue->datapath1.threadHandle =
                    CreateThread(NULL, 0, XskDatapathWorkerFn, &queue->datapath1, 0, NULL);
            }
            if (queue->datapath2.sock != NULL) {
                queue->datapath2.threadHandle =
                    CreateThread(NULL, 0, XskDatapathWorkerFn, &queue->datapath2, 0, NULL);
            }

            //
            // Let datapath thread/s pump datapath for set duration.
            //
            TraceVerbose("q[%u]: letting datapath pump", queue->queueId);
            WaitForSingleObject(stopEvent, 500);

            //
            // Signal and wait for datapath thread/s to return.
            //
            TraceVerbose("q[%u]: waiting for datapath threads", queue->queueId);
            WriteNoFence((LONG *)&queue->datapathShared.state, ThreadStateReturn);
            if (queue->datapath1.threadHandle != NULL) {
                WaitForSingleObject(queue->datapath1.threadHandle, INFINITE);
            }
            if (queue->datapath2.threadHandle != NULL) {
                WaitForSingleObject(queue->datapath2.threadHandle, INFINITE);
            }
        }

        //
        // Signal and wait for fuzzer threads to return.
        //
        TraceVerbose("q[%u]: waiting for fuzzer threads", queue->queueId);
        WriteNoFence((LONG*)&queue->fuzzerShared.state, ThreadStateReturn);
        for (UINT32 i = 0; i < queue->fuzzerCount; i++) {
            WaitForSingleObject(queue->fuzzers[i].threadHandle, INFINITE);
        }

        if (extraStats) {
            UpdateSetupStats(queueWorker, queue);
        }

        CleanupQueue(queue);
    }

    successPct = Pct(numSuccessfulSetups, numIterations);
    printf("q[%u]: socket setup success rate: (%lu / %lu) %lu%%\n",
        queueWorker->queueId, numSuccessfulSetups, numIterations, successPct);

    if (extraStats) {
        PrintSetupStats(queueWorker, numIterations);
    }

    //
    // Require a certain percentage of sockets to complete the setup phase as a
    // proxy for ensuring effective code coverage.
    //
    ASSERT_FRE(successPct >= 50);

    TraceExit("q[%u]", queueWorker->queueId);
    return 0;
}

DWORD
WINAPI
AdminFn(
    _In_ VOID *ThreadParameter
    )
{
    DWORD res;
    CHAR cmdBuff[256];
    HKEY xdpParametersKey;
    CONST CHAR *delayDetachTimeoutRegName = "GenericDelayDetachTimeoutSec";

    UNREFERENCED_PARAMETER(ThreadParameter);

    TraceEnter("-");

    res =
        RegCreateKeyExA(
            HKEY_LOCAL_MACHINE,
            "System\\CurrentControlSet\\Services\\Xdp\\Parameters",
            0, NULL, REG_OPTION_VOLATILE, KEY_WRITE, NULL, &xdpParametersKey, NULL);
    ASSERT_FRE(res == ERROR_SUCCESS);

    while (TRUE) {
        res = WaitForSingleObject(workersDoneEvent, ADMIN_THREAD_TIMEOUT_SEC * 1000);
        if (res == WAIT_OBJECT_0) {
            break;
        }

        TraceVerbose("admin iter");

        if (!cleanDatapath && !(RandUlong() % 10)) {
            TraceVerbose("admin: restart adapter");
            RtlZeroMemory(cmdBuff, sizeof(cmdBuff));
            sprintf_s(
                cmdBuff, sizeof(cmdBuff),
                "%s -Command \"(Get-NetAdapter | Where-Object {$_.IfIndex -eq %d}) | Restart-NetAdapter\"",
                powershellPrefix, ifindex);
            system(cmdBuff);
        }

        if (!(RandUlong() % 10)) {
            DWORD DelayDetachTimeout = RandUlong() % 10;
            RegSetValueExA(
                xdpParametersKey, delayDetachTimeoutRegName, 0, REG_DWORD,
                (BYTE *)&DelayDetachTimeout, sizeof(DelayDetachTimeout));
        }
    }

    //
    // Clean up fuzzed registry state.
    //
    RegDeleteValueA(xdpParametersKey, delayDetachTimeoutRegName);
    RegCloseKey(xdpParametersKey);

    TraceExit("-");

    return 0;
}

DWORD
WINAPI
WatchdogFn(
    _In_ VOID *ThreadParameter
    )
{
    DWORD res;
    ULONGLONG perfCount;
    ULONGLONG watchdogTimeoutInCounts = perfFreq * WATCHDOG_THREAD_TIMEOUT_SEC;

    UNREFERENCED_PARAMETER(ThreadParameter);

    TraceEnter("-");

    while (TRUE) {
        res = WaitForSingleObject(workersDoneEvent, WATCHDOG_THREAD_TIMEOUT_SEC * 1000);
        if (res == WAIT_OBJECT_0) {
            break;
        }

        TraceVerbose("watchdog iter");

        QueryPerformanceCounter((LARGE_INTEGER*)&perfCount);
        for (UINT32 i = 0; i < queueCount; i++) {
            if ((LONGLONG)(queueWorkers[i].watchdogPerfCount + watchdogTimeoutInCounts - perfCount) < 0) {
                TraceError( "WATCHDOG exceeded on queue %d", i);
                printf("WATCHDOG exceeded on queue %d\n", i);
                if (strlen(watchdogCmd) > 0) {
                    TraceInfo("watchdogCmd=%s", watchdogCmd);
                    system(watchdogCmd);
                }
                DebugBreak();
                DbgRaiseAssertionFailure();
                exit(ERROR_TIMEOUT);
            }
        }
    }

    TraceExit("-");
    return 0;
}

VOID
PrintUsage(
    _In_ INT Line
    )
{
    printf("Line:%d\n", Line);
    printf(HELP);
    exit(1);
}

VOID
ParseArgs(
    INT argc,
    CHAR **argv
    )
{
    INT i = 1;

    if (argc < 3) {
        Usage();
    }

    if (strcmp(argv[i++], "-IfIndex")) {
        Usage();
    }
    ifindex = atoi(argv[i++]);

    while (i < argc) {
        if (!strcmp(argv[i], "-Minutes")) {
            if (++i > argc) {
                Usage();
            }
            duration = atoi(argv[i]) * 60;
            TraceVerbose("duration=%u", duration);
        } else if (!strcmp(argv[i], "-Stats")) {
            extraStats = TRUE;
        } else if (!strcmp(argv[i], "-Verbose")) {
            verbose = TRUE;
        } else if (!strcmp(argv[i], "-QueueCount")) {
            if (++i > argc) {
                Usage();
            }
            queueCount = atoi(argv[i]);
            TraceVerbose("queueCount=%u", queueCount);
        } else if (!strcmp(argv[i], "-FuzzerCount")) {
            if (++i > argc) {
                Usage();
            }
            fuzzerCount = atoi(argv[i]);
            TraceVerbose("fuzzerCount=%u", fuzzerCount);
        } else if (!strcmp(argv[i], "-CleanDatapath")) {
            cleanDatapath = TRUE;
            TraceVerbose("cleanDatapath=%!BOOLEAN!", cleanDatapath);
        } else if (!strcmp(argv[i], "-WatchdogCmd")) {
            if (++i > argc) {
                Usage();
            }
            watchdogCmd = argv[i];
            TraceVerbose("watchdogCmd=%s", watchdogCmd);
        } else {
            Usage();
        }

        ++i;
    }

    if (ifindex == -1) {
        Usage();
    }
}

BOOL
WINAPI
ConsoleCtrlHandler(
    _In_ DWORD CtrlType
    )
{
    UNREFERENCED_PARAMETER(CtrlType);

    TraceVerbose("CTRL-C");

    //
    // Gracefully initiate a stop.
    //
    SetEvent(stopEvent);

    return TRUE;
}

INT
__cdecl
main(
    INT argc,
    CHAR **argv
    )
{
    HANDLE adminThread;
    HANDLE watchdogThread;

    WPP_INIT_TRACING(NULL);

    ParseArgs(argc, argv);

    powershellPrefix = GetPowershellPrefix();

    QueryPerformanceFrequency((LARGE_INTEGER*)&perfFreq);

    stopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    ASSERT_FRE(stopEvent != NULL);

    workersDoneEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    ASSERT_FRE(workersDoneEvent != NULL);

    ASSERT_FRE(SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE));

    //
    // Allocate and initialize queue workers.
    //

    queueWorkers = calloc(queueCount, sizeof(*queueWorkers));
    ASSERT_FRE(queueWorkers != NULL);

    for (UINT32 i = 0; i < queueCount; i++) {
        QUEUE_WORKER *queueWorker = &queueWorkers[i];
        queueWorker->queueId = i;
        QueryPerformanceCounter((LARGE_INTEGER*)&queueWorker->watchdogPerfCount);
    }

    //
    // Create admin and watchdog thread for queue workers.
    //
    adminThread = CreateThread(NULL, 0, AdminFn, NULL, 0, NULL);
    ASSERT_FRE(adminThread);
    watchdogThread = CreateThread(NULL, 0, WatchdogFn, NULL, 0, NULL);
    ASSERT_FRE(watchdogThread);

    //
    // Kick off the queue workers.
    //
    for (UINT32 i = 0; i < queueCount; i++) {
        QUEUE_WORKER *queueWorker = &queueWorkers[i];
        queueWorker->threadHandle =
            CreateThread(NULL, 0, QueueWorkerFn, queueWorker, 0, NULL);
        ASSERT_FRE(queueWorker->threadHandle != NULL);
    }

    //
    // Wait for test duration.
    //
    TraceVerbose("main: running test...");
    WaitForSingleObject(stopEvent, (duration == ULONG_MAX) ? INFINITE : duration * 1000);
    WriteBooleanNoFence(&done, TRUE);

    //
    // Wait on each queue worker to return.
    //
    TraceVerbose("main: waiting for workers...");
    for (UINT32 i = 0; i < queueCount; i++) {
        QUEUE_WORKER *queueWorker = &queueWorkers[i];
        WaitForSingleObject(queueWorker->threadHandle, INFINITE);
    }

    //
    // Cleanup the admin and watchdog threads after all workers have exited.
    //

    SetEvent(workersDoneEvent);

    TraceVerbose("main: waiting for admin...");
    WaitForSingleObject(adminThread, INFINITE);

    TraceVerbose("main: waiting for watchdog...");
    WaitForSingleObject(watchdogThread, INFINITE);

    free(queueWorkers);

    printf("done\n");

    WPP_CLEANUP();

    return 0;
}
