//
// Copyright (C) Microsoft Corporation. All rights reserved.
//

#include "precomp.h"

//
// For rudimentary fairness between components running at dispatch, limit the
// time in each DPC.
//
#define MAX_ITERATIONS_PER_DPC 8

static
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
XdpEcNotifyEx(
    _In_ XDP_EC *Ec,
    _In_ BOOLEAN CanInline
    );

_IRQL_requires_(PASSIVE_LEVEL)
static
VOID
XdpEcPassiveWorker(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_opt_ PVOID Context
    )
{
    XDP_EC *Ec = Context;
    GROUP_AFFINITY Affinity = {0};
    GROUP_AFFINITY OldAffinity;
    PROCESSOR_NUMBER ProcessorNumber;

    UNREFERENCED_PARAMETER(DeviceObject);
    ASSERT(Ec != NULL);

    KeGetProcessorNumberFromIndex(Ec->OwningProcessor, &ProcessorNumber);
    Affinity.Group = ProcessorNumber.Group;
    Affinity.Mask = AFFINITY_MASK(ProcessorNumber.Number);
    KeSetSystemGroupAffinityThread(&Affinity, &OldAffinity);

    KeInsertQueueDpc(&Ec->Dpc, NULL, NULL);

    KeRevertToUserGroupAffinityThread(&OldAffinity);
}

static
_IRQL_requires_(DISPATCH_LEVEL)
BOOLEAN
XdpEcInvokePoll(
    _In_ XDP_EC *Ec
    )
{
    BOOLEAN NeedPoll;

    ASSERT(!Ec->InPoll);
    Ec->InPoll = TRUE;

    NeedPoll = Ec->Poll(Ec->PollContext);

    ASSERT(Ec->InPoll);
    Ec->InPoll = FALSE;

    return NeedPoll;
}

static
_IRQL_requires_(DISPATCH_LEVEL)
VOID
XdpEcPoll(
    _In_ XDP_EC *Ec
    )
{
    BOOLEAN NeedPoll = FALSE;
    BOOLEAN NeedYieldCheck;
    LARGE_INTEGER CurrentTick;
    UINT32 Iteration = 0;

    ASSERT(Ec->OwningProcessor == KeGetCurrentProcessorIndex());

    NeedYieldCheck = !Ec->SkipYieldCheck;
    Ec->SkipYieldCheck = FALSE;

    if (Ec->OwningProcessor != *Ec->IdealProcessor) {
        PROCESSOR_NUMBER ProcessorNumber;
        //
        // The target processor has changed. Invalidate this processor's claim
        // to the EC. The target processor will update the owning processor.
        //
        Ec->OwningProcessor = ULONG_MAX;
        KeGetProcessorNumberFromIndex(*Ec->IdealProcessor, &ProcessorNumber);
        KeSetTargetProcessorDpcEx(&Ec->Dpc, &ProcessorNumber);
        KeInsertQueueDpc(&Ec->Dpc, NULL, NULL);
        return;
    }

    if (NeedYieldCheck) {
        KeQueryTickCount(&CurrentTick);
        if (Ec->LastYieldTick.QuadPart < CurrentTick.QuadPart) {
            Ec->LastYieldTick.QuadPart = CurrentTick.QuadPart;
            if (KeShouldYieldProcessor()) {
                //
                // The EC should yield the processor, so queue a passive work
                // item.
                //
                // Allow the DPC to skip the next yield check: on older systems
                // configured with DPC watchdog timeouts disabled,
                // KeShouldYieldProcessor may return true even if the CPU had
                // recently dropped below dispatch level, which causes this EC
                // to yield the processor prematurely, i.e. without ever polling
                // the callback. If there's a lot of DPC activity on the system,
                // starvation can occur in the degenerate case.
                //
                Ec->SkipYieldCheck = TRUE;
                IoQueueWorkItem(Ec->WorkItem, XdpEcPassiveWorker, DelayedWorkQueue, Ec);
                return;
            }
        }
    }

    do {
        NeedPoll = XdpEcInvokePoll(Ec);
    } while (NeedPoll && ++Iteration < MAX_ITERATIONS_PER_DPC);

    if (NeedPoll) {
        KeInsertQueueDpc(&Ec->Dpc, NULL, NULL);
    } else {
        //
        // No more work. Re-arm the EC and re-check the poll callback.
        //
        InterlockedExchange8((CHAR *)&Ec->Armed, TRUE);

        if (XdpEcInvokePoll(Ec)) {
            //
            // There is more work, after all.
            //
            XdpEcNotifyEx(Ec, FALSE);
        } else if (Ec->CleanupComplete != NULL) {
            KeSetEvent(Ec->CleanupComplete, 0, FALSE);
        }
    }
}

static
_Function_class_(KDEFERRED_ROUTINE)
_IRQL_requires_max_(DISPATCH_LEVEL)
_IRQL_requires_min_(DISPATCH_LEVEL)
_IRQL_requires_(DISPATCH_LEVEL)
_IRQL_requires_same_
VOID
XdpEcDpcThunk(
    _In_ struct _KDPC *Dpc,
    _In_opt_ VOID *DeferredContext,
    _In_opt_ VOID *SystemArgument1,
    _In_opt_ VOID *SystemArgument2
    )
{
    XDP_EC *Ec = DeferredContext;
    ULONG CurrentProcessor = KeGetCurrentProcessorIndex();

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);
    ASSERT(DeferredContext != NULL);

    Ec->OwningProcessor = CurrentProcessor;
    XdpEcPoll(DeferredContext);
}

static
_IRQL_requires_(DISPATCH_LEVEL)
BOOLEAN
XdpEcIsSerializable(
    _In_ XDP_EC *Ec,
    _In_ ULONG CurrentProcessor
    )
{
    return CurrentProcessor == Ec->OwningProcessor && !Ec->InPoll;
}

static
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
XdpEcNotifyEx(
    _In_ XDP_EC *Ec,
    _In_ BOOLEAN CanInline
    )
{
    if (InterlockedExchange8((CHAR *)&Ec->Armed, FALSE)) {
        KIRQL OldIrql = KeRaiseIrqlToDpcLevel();
        ULONG CurrentProcessor = KeGetCurrentProcessorIndex();

        if (CanInline && XdpEcIsSerializable(Ec, CurrentProcessor)) {
            XdpEcPoll(Ec);
        } else {
            KeInsertQueueDpc(&Ec->Dpc, NULL, NULL);
        }

        if (OldIrql != DISPATCH_LEVEL) {
            KeLowerIrql(OldIrql);
        }
    }
}

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
XdpEcNotify(
    _In_ XDP_EC *Ec
    )
{
    XdpEcNotifyEx(Ec, TRUE);
}

_IRQL_requires_(DISPATCH_LEVEL)
BOOLEAN
XdpEcEnterInline(
    _In_ XDP_EC *Ec,
    _In_ ULONG CurrentProcessor
    )
{
    if (XdpEcIsSerializable(Ec, CurrentProcessor)) {
        Ec->InPoll = TRUE;
        return TRUE;
    }

    return FALSE;
}

_IRQL_requires_(DISPATCH_LEVEL)
VOID
XdpEcExitInline(
    _In_ XDP_EC *Ec
    )
{
    ASSERT(KeGetCurrentProcessorIndex() == Ec->OwningProcessor);
    ASSERT(Ec->InPoll);
    Ec->InPoll = FALSE;
}

_IRQL_requires_(PASSIVE_LEVEL)
VOID
XdpEcCleanup(
    _In_ XDP_EC *Ec
    )
{
    if (Ec->WorkItem != NULL) {
        KEVENT CleanupEvent;

        KeInitializeEvent(&CleanupEvent, NotificationEvent, FALSE);
        WritePointerRelease(&Ec->CleanupComplete, &CleanupEvent);
        XdpEcNotify(Ec);
        KeWaitForSingleObject(&CleanupEvent, Executive, KernelMode, FALSE, NULL);
        Ec->CleanupComplete = NULL;

        IoFreeWorkItem(Ec->WorkItem);
    }
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
XdpEcInitialize(
    _Inout_ XDP_EC *Ec,
    _In_ XDP_EC_POLL_ROUTINE *Poll,
    _In_ VOID *PollContext,
    _In_ ULONG *IdealProcessor
    )
{
    NTSTATUS Status;
    PROCESSOR_NUMBER ProcessorNumber;

    RtlZeroMemory(Ec, sizeof(*Ec));
    Ec->Poll = Poll;
    Ec->PollContext = PollContext;
    Ec->IdealProcessor = IdealProcessor;
    Ec->Armed = TRUE;

    //
    // Either a driver or device object is acceptable to this routine.
    //
    Ec->WorkItem = IoAllocateWorkItem((PDEVICE_OBJECT)XdpLwfDriverObject);
    if (Ec->WorkItem == NULL) {
        Status = STATUS_NO_MEMORY;
        goto Exit;
    }

    KeInitializeDpc(&Ec->Dpc, XdpEcDpcThunk, Ec);
    KeGetProcessorNumberFromIndex(*Ec->IdealProcessor, &ProcessorNumber);
    KeSetTargetProcessorDpcEx(&Ec->Dpc, &ProcessorNumber);

    Status = STATUS_SUCCESS;

Exit:

    return Status;
}
