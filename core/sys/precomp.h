//
// Copyright (C) Microsoft Corporation. All rights reserved.
//

#pragma once

#pragma warning(disable:4201)  // nonstandard extension used: nameless struct/union

#include <ntdef.h>
#include <ntstatus.h>
#include <ntifs.h>
#include <ntintsafe.h>
#include <ndis.h>
#include <stdlib.h>

#define XDPAPI
#define XDPEXPORT(RoutineName) RoutineName##Thunk

#include <xdp/bufferinterfacecontext.h>
#include <xdp/bufferlogicaladdress.h>
#include <xdp/buffermdl.h>
#include <xdp/buffervirtualaddress.h>
#include <xdp/control.h>
#include <xdp/datapath.h>
#include <xdp/framefragment.h>
#include <xdp/frameinterfacecontext.h>
#include <xdp/framerxaction.h>
#include <xdp/frametxcompletioncontext.h>

#include <msxdp.h>
#include <xdpassert.h>
#include <xdpif.h>
#include <xdpioctl.h>
#include <xdplwf.h>
#include <xdpnmrprovider.h>
#include <xdppollshim.h>
#include <xdprefcount.h>
#include <xdpregistry.h>
#include <xdprtl.h>
#include <xdprxqueue_internal.h>
#include <xdptrace.h>
#include <xdptxqueue_internal.h>
#include <xdpworkqueue.h>

#pragma warning(disable:4200) // nonstandard extension used: zero-sized array in struct/union

#include "xdpp.h"
#include "bind.h"
#include "dispatch.h"
#include "extensionset.h"
#include "program.h"
#include "queue.h"
#include "redirect.h"
#include "ring.h"
#include "rx.h"
#include "tx.h"
#include "version.h"
#include "xsk.h"
