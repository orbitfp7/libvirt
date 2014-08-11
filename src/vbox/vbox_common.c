/*
 * Copyright 2014, Taowei Luo (uaedante@gmail.com)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include <config.h>

#include <unistd.h>

#include "internal.h"
#include "datatypes.h"
#include "domain_conf.h"
#include "domain_event.h"
#include "virlog.h"
#include "viralloc.h"
#include "nodeinfo.h"
#include "virstring.h"

#include "vbox_common.h"
#include "vbox_uniformed_api.h"
#include "vbox_install_api.h"

/* Common codes for vbox driver. With the definitions in vbox_common.h,
 * it treats vbox structs as a void*. Though vboxUniformedAPI
 * it call vbox functions. This file is a high level implement about
 * the vbox driver.
 */

#define VIR_FROM_THIS VIR_FROM_VBOX

VIR_LOG_INIT("vbox.vbox_common");

#define RC_SUCCEEDED(rc) NS_SUCCEEDED(rc.resultCode)
#define RC_FAILED(rc) NS_FAILED(rc.resultCode)

#define VBOX_UTF16_FREE(arg)                                            \
    do {                                                                \
        if (arg) {                                                      \
            gVBoxAPI.UPFN.Utf16Free(data->pFuncs, arg);                 \
            (arg) = NULL;                                               \
        }                                                               \
    } while (0)

#define VBOX_UTF8_FREE(arg)                                             \
    do {                                                                \
        if (arg) {                                                      \
            gVBoxAPI.UPFN.Utf8Free(data->pFuncs, arg);                  \
            (arg) = NULL;                                               \
        }                                                               \
    } while (0)

#define VBOX_COM_UNALLOC_MEM(arg)                                       \
    do {                                                                \
        if (arg) {                                                      \
            gVBoxAPI.UPFN.ComUnallocMem(data->pFuncs, arg);             \
            (arg) = NULL;                                               \
        }                                                               \
    } while (0)

#define VBOX_UTF16_TO_UTF8(arg1, arg2)  gVBoxAPI.UPFN.Utf16ToUtf8(data->pFuncs, arg1, arg2)
#define VBOX_UTF8_TO_UTF16(arg1, arg2)  gVBoxAPI.UPFN.Utf8ToUtf16(data->pFuncs, arg1, arg2)

#define VBOX_RELEASE(arg)                                                     \
    do {                                                                      \
        if (arg) {                                                            \
            gVBoxAPI.nsUISupports.Release((void *)arg);                        \
            (arg) = NULL;                                                     \
        }                                                                     \
    } while (0)

#define VBOX_MEDIUM_RELEASE(arg)                                              \
    do {                                                                      \
        if (arg) {                                                            \
            gVBoxAPI.UIMedium.Release(arg);                                   \
            (arg) = NULL;                                                     \
        }                                                                     \
    } while (0)

#define VBOX_OBJECT_CHECK(conn, type, value) \
vboxGlobalData *data = conn->privateData;\
type ret = value;\
if (!data->vboxObj) {\
    return ret;\
}

#define vboxIIDUnalloc(iid)                     gVBoxAPI.UIID.vboxIIDUnalloc(data, iid)
#define vboxIIDToUUID(iid, uuid)                gVBoxAPI.UIID.vboxIIDToUUID(data, iid, uuid)
#define vboxIIDFromUUID(iid, uuid)              gVBoxAPI.UIID.vboxIIDFromUUID(data, iid, uuid)
#define vboxIIDIsEqual(iid1, iid2)              gVBoxAPI.UIID.vboxIIDIsEqual(data, iid1, iid2)
#define DEBUGIID(msg, iid)                      gVBoxAPI.UIID.DEBUGIID(msg, iid)
#define vboxIIDFromArrayItem(iid, array, idx) \
    gVBoxAPI.UIID.vboxIIDFromArrayItem(data, iid, array, idx)

#define VBOX_IID_INITIALIZE(iid)                gVBoxAPI.UIID.vboxIIDInitialize(iid)

#define ARRAY_GET_MACHINES \
    (gVBoxAPI.UArray.handleGetMachines(data->vboxObj))

/* global vbox API, used for all common codes. */
static vboxUniformedAPI gVBoxAPI;

int vboxRegisterUniformedAPI(uint32_t uVersion)
{
    /* Install gVBoxAPI according to the vbox API version.
     * Return -1 for unsupported version.
     */
    if (uVersion >= 2001052 && uVersion < 2002051) {
        vbox22InstallUniformedAPI(&gVBoxAPI);
    } else if (uVersion >= 2002051 && uVersion < 3000051) {
        vbox30InstallUniformedAPI(&gVBoxAPI);
    } else if (uVersion >= 3000051 && uVersion < 3001051) {
        vbox31InstallUniformedAPI(&gVBoxAPI);
    } else if (uVersion >= 3001051 && uVersion < 3002051) {
        vbox32InstallUniformedAPI(&gVBoxAPI);
    } else if (uVersion >= 3002051 && uVersion < 4000051) {
        vbox40InstallUniformedAPI(&gVBoxAPI);
    } else if (uVersion >= 4000051 && uVersion < 4001051) {
        vbox41InstallUniformedAPI(&gVBoxAPI);
    } else if (uVersion >= 4001051 && uVersion < 4002020) {
        vbox42InstallUniformedAPI(&gVBoxAPI);
    } else if (uVersion >= 4002020 && uVersion < 4002051) {
        vbox42_20InstallUniformedAPI(&gVBoxAPI);
    } else if (uVersion >= 4002051 && uVersion < 4003004) {
        vbox43InstallUniformedAPI(&gVBoxAPI);
    } else if (uVersion >= 4003004 && uVersion < 4003051) {
        vbox43_4InstallUniformedAPI(&gVBoxAPI);
    } else {
        return -1;
    }
    return 0;
}

static int openSessionForMachine(vboxGlobalData *data, const unsigned char *dom_uuid, vboxIIDUnion *iid,
                                 IMachine **machine, bool checkflag)
{
    VBOX_IID_INITIALIZE(iid);
    vboxIIDFromUUID(iid, dom_uuid);
    if (!checkflag || gVBoxAPI.getMachineForSession) {
        /* Get machine for the call to VBOX_SESSION_OPEN_EXISTING */
        if (NS_FAILED(gVBoxAPI.UIVirtualBox.GetMachine(data->vboxObj, iid, machine))) {
            virReportError(VIR_ERR_NO_DOMAIN, "%s",
                           _("no domain with matching uuid"));
            return -1;
        }
    }
    return 0;
}

/**
 * function to get the values for max port per
 * instance and max slots per port for the devices
 *
 * @returns     true on Success, false on failure.
 * @param       vbox            Input IVirtualBox pointer
 * @param       maxPortPerInst  Output array of max port per instance
 * @param       maxSlotPerPort  Output array of max slot per port
 *
 */

static bool vboxGetMaxPortSlotValues(IVirtualBox *vbox,
                                     PRUint32 *maxPortPerInst,
                                     PRUint32 *maxSlotPerPort)
{
    ISystemProperties *sysProps = NULL;

    if (!vbox)
        return false;

    gVBoxAPI.UIVirtualBox.GetSystemProperties(vbox, &sysProps);

    if (!sysProps)
        return false;

    gVBoxAPI.UISystemProperties.GetMaxPortCountForStorageBus(sysProps,
                                                             StorageBus_IDE,
                                                             &maxPortPerInst[StorageBus_IDE]);
    gVBoxAPI.UISystemProperties.GetMaxPortCountForStorageBus(sysProps,
                                                             StorageBus_SATA,
                                                             &maxPortPerInst[StorageBus_SATA]);
    gVBoxAPI.UISystemProperties.GetMaxPortCountForStorageBus(sysProps,
                                                             StorageBus_SCSI,
                                                             &maxPortPerInst[StorageBus_SCSI]);
    gVBoxAPI.UISystemProperties.GetMaxPortCountForStorageBus(sysProps,
                                                             StorageBus_Floppy,
                                                             &maxPortPerInst[StorageBus_Floppy]);

    gVBoxAPI.UISystemProperties.GetMaxDevicesPerPortForStorageBus(sysProps,
                                                                  StorageBus_IDE,
                                                                  &maxSlotPerPort[StorageBus_IDE]);
    gVBoxAPI.UISystemProperties.GetMaxDevicesPerPortForStorageBus(sysProps,
                                                                  StorageBus_SATA,
                                                                  &maxSlotPerPort[StorageBus_SATA]);
    gVBoxAPI.UISystemProperties.GetMaxDevicesPerPortForStorageBus(sysProps,
                                                                  StorageBus_SCSI,
                                                                  &maxSlotPerPort[StorageBus_SCSI]);
    gVBoxAPI.UISystemProperties.GetMaxDevicesPerPortForStorageBus(sysProps,
                                                                  StorageBus_Floppy,
                                                                  &maxSlotPerPort[StorageBus_Floppy]);

    VBOX_RELEASE(sysProps);

    return true;
}

/**
 * function to get the StorageBus, Port number
 * and Device number for the given devicename
 * e.g: hda has StorageBus = IDE, port = 0,
 *      device = 0
 *
 * @returns     true on Success, false on failure.
 * @param       deviceName      Input device name
 * @param       aMaxPortPerInst Input array of max port per device instance
 * @param       aMaxSlotPerPort Input array of max slot per device port
 * @param       storageBus      Input storage bus type
 * @param       deviceInst      Output device instance number
 * @param       devicePort      Output port number
 * @param       deviceSlot      Output slot number
 *
 */
static bool vboxGetDeviceDetails(const char *deviceName,
                                 PRUint32   *aMaxPortPerInst,
                                 PRUint32   *aMaxSlotPerPort,
                                 PRUint32    storageBus,
                                 PRInt32    *deviceInst,
                                 PRInt32    *devicePort,
                                 PRInt32    *deviceSlot) {
    int total = 0;
    PRUint32 maxPortPerInst = 0;
    PRUint32 maxSlotPerPort = 0;

    if (!deviceName ||
        !deviceInst ||
        !devicePort ||
        !deviceSlot ||
        !aMaxPortPerInst ||
        !aMaxSlotPerPort)
        return false;

    if ((storageBus < StorageBus_IDE) ||
        (storageBus > StorageBus_Floppy))
        return false;

    total = virDiskNameToIndex(deviceName);

    maxPortPerInst = aMaxPortPerInst[storageBus];
    maxSlotPerPort = aMaxSlotPerPort[storageBus];

    if (!maxPortPerInst ||
        !maxSlotPerPort ||
        (total < 0))
        return false;

    *deviceInst = total / (maxPortPerInst * maxSlotPerPort);
    *devicePort = (total % (maxPortPerInst * maxSlotPerPort)) / maxSlotPerPort;
    *deviceSlot = (total % (maxPortPerInst * maxSlotPerPort)) % maxSlotPerPort;

    VIR_DEBUG("name=%s, total=%d, storageBus=%u, deviceInst=%d, "
          "devicePort=%d deviceSlot=%d, maxPortPerInst=%u maxSlotPerPort=%u",
          deviceName, total, storageBus, *deviceInst, *devicePort,
          *deviceSlot, maxPortPerInst, maxSlotPerPort);

    return true;
}

/**
 * function to generate the name for medium,
 * for e.g: hda, sda, etc
 *
 * @returns     null terminated string with device name or NULL
 *              for failures
 * @param       conn            Input Connection Pointer
 * @param       storageBus      Input storage bus type
 * @param       deviceInst      Input device instance number
 * @param       devicePort      Input port number
 * @param       deviceSlot      Input slot number
 * @param       aMaxPortPerInst Input array of max port per device instance
 * @param       aMaxSlotPerPort Input array of max slot per device port
 *
 */
static char *vboxGenerateMediumName(PRUint32  storageBus,
                                    PRInt32   deviceInst,
                                    PRInt32   devicePort,
                                    PRInt32   deviceSlot,
                                    PRUint32 *aMaxPortPerInst,
                                    PRUint32 *aMaxSlotPerPort)
{
    const char *prefix = NULL;
    char *name  = NULL;
    int   total = 0;
    PRUint32 maxPortPerInst = 0;
    PRUint32 maxSlotPerPort = 0;

    if (!aMaxPortPerInst ||
        !aMaxSlotPerPort)
        return NULL;

    if ((storageBus < StorageBus_IDE) ||
        (storageBus > StorageBus_Floppy))
        return NULL;

    maxPortPerInst = aMaxPortPerInst[storageBus];
    maxSlotPerPort = aMaxSlotPerPort[storageBus];
    total =   (deviceInst * maxPortPerInst * maxSlotPerPort)
            + (devicePort * maxSlotPerPort)
            + deviceSlot;

    if (storageBus == StorageBus_IDE) {
        prefix = "hd";
    } else if ((storageBus == StorageBus_SATA) ||
               (storageBus == StorageBus_SCSI)) {
        prefix = "sd";
    } else if (storageBus == StorageBus_Floppy) {
        prefix = "fd";
    }

    name = virIndexToDiskName(total, prefix);

    VIR_DEBUG("name=%s, total=%d, storageBus=%u, deviceInst=%d, "
          "devicePort=%d deviceSlot=%d, maxPortPerInst=%u maxSlotPerPort=%u",
          NULLSTR(name), total, storageBus, deviceInst, devicePort,
          deviceSlot, maxPortPerInst, maxSlotPerPort);
    return name;
}

static virDomainDefParserConfig vboxDomainDefParserConfig = {
    .macPrefix = { 0x08, 0x00, 0x27 },
};

static virDomainXMLOptionPtr
vboxXMLConfInit(void)
{
    return virDomainXMLOptionNew(&vboxDomainDefParserConfig,
                                 NULL, NULL);
}

static int vboxInitialize(vboxGlobalData *data)
{
    if (gVBoxAPI.UPFN.Initialize(data) != 0)
        goto cleanup;

    if (gVBoxAPI.domainEventCallbacks && gVBoxAPI.initializeDomainEvent(data) != 0)
        goto cleanup;

    if (data->vboxObj == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("IVirtualBox object is null"));
        goto cleanup;
    }

    if (data->vboxSession == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("ISession object is null"));
        goto cleanup;
    }

    return 0;

 cleanup:
    return -1;
}

static virCapsPtr vboxCapsInit(void)
{
    virCapsPtr caps;
    virCapsGuestPtr guest;

    if ((caps = virCapabilitiesNew(virArchFromHost(),
                                   false, false)) == NULL)
        goto no_memory;

    if (nodeCapsInitNUMA(caps) < 0)
        goto no_memory;

    if ((guest = virCapabilitiesAddGuest(caps,
                                         "hvm",
                                         caps->host.arch,
                                         NULL,
                                         NULL,
                                         0,
                                         NULL)) == NULL)
        goto no_memory;

    if (virCapabilitiesAddGuestDomain(guest,
                                      "vbox",
                                      NULL,
                                      NULL,
                                      0,
                                      NULL) == NULL)
        goto no_memory;

    return caps;

 no_memory:
    virObjectUnref(caps);
    return NULL;
}

static int vboxExtractVersion(vboxGlobalData *data)
{
    int ret = -1;
    PRUnichar *versionUtf16 = NULL;
    char *vboxVersion = NULL;
    nsresult rc;

    if (data->version > 0)
        return 0;

    rc = gVBoxAPI.UIVirtualBox.GetVersion(data->vboxObj, &versionUtf16);
    if (NS_FAILED(rc))
        goto failed;

    VBOX_UTF16_TO_UTF8(versionUtf16, &vboxVersion);

    if (virParseVersionString(vboxVersion, &data->version, false) >= 0)
        ret = 0;

    VBOX_UTF8_FREE(vboxVersion);
    VBOX_COM_UNALLOC_MEM(versionUtf16);
 failed:
    if (ret != 0)
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Could not extract VirtualBox version"));

    return ret;
}

static void vboxUninitialize(vboxGlobalData *data)
{
    if (!data)
        return;

    gVBoxAPI.UPFN.Uninitialize(data);

    virObjectUnref(data->caps);
    virObjectUnref(data->xmlopt);
    if (gVBoxAPI.domainEventCallbacks)
        virObjectEventStateFree(data->domainEvents);
    VIR_FREE(data);
}

virDrvOpenStatus vboxConnectOpen(virConnectPtr conn,
                                 virConnectAuthPtr auth ATTRIBUTE_UNUSED,
                                 unsigned int flags)
{
    vboxGlobalData *data = NULL;
    uid_t uid = geteuid();

    virCheckFlags(VIR_CONNECT_RO, VIR_DRV_OPEN_ERROR);

    if (conn->uri == NULL &&
        !(conn->uri = virURIParse(uid ? "vbox:///session" : "vbox:///system")))
        return VIR_DRV_OPEN_ERROR;

    if (conn->uri->scheme == NULL ||
        STRNEQ(conn->uri->scheme, "vbox"))
        return VIR_DRV_OPEN_DECLINED;

    /* Leave for remote driver */
    if (conn->uri->server != NULL)
        return VIR_DRV_OPEN_DECLINED;

    if (conn->uri->path == NULL || STREQ(conn->uri->path, "")) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("no VirtualBox driver path specified (try vbox:///session)"));
        return VIR_DRV_OPEN_ERROR;
    }

    if (uid != 0) {
        if (STRNEQ(conn->uri->path, "/session")) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("unknown driver path '%s' specified (try vbox:///session)"), conn->uri->path);
            return VIR_DRV_OPEN_ERROR;
        }
    } else { /* root */
        if (STRNEQ(conn->uri->path, "/system") &&
            STRNEQ(conn->uri->path, "/session")) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("unknown driver path '%s' specified (try vbox:///system)"), conn->uri->path);
            return VIR_DRV_OPEN_ERROR;
        }
    }

    if (VIR_ALLOC(data) < 0)
        return VIR_DRV_OPEN_ERROR;

    if (!(data->caps = vboxCapsInit()) ||
        vboxInitialize(data) < 0 ||
        vboxExtractVersion(data) < 0 ||
        !(data->xmlopt = vboxXMLConfInit())) {
        vboxUninitialize(data);
        return VIR_DRV_OPEN_ERROR;
    }

    if (gVBoxAPI.domainEventCallbacks) {
        if (!(data->domainEvents = virObjectEventStateNew())) {
            vboxUninitialize(data);
            return VIR_DRV_OPEN_ERROR;
        }

        data->conn = conn;
    }

    if (gVBoxAPI.hasStaticGlobalData)
        gVBoxAPI.registerGlobalData(data);

    conn->privateData = data;
    VIR_DEBUG("in vboxOpen");

    return VIR_DRV_OPEN_SUCCESS;
}

int vboxConnectClose(virConnectPtr conn)
{
    vboxGlobalData *data = conn->privateData;
    VIR_DEBUG("%s: in vboxClose", conn->driver->name);

    vboxUninitialize(data);
    conn->privateData = NULL;

    return 0;
}

int
vboxDomainSave(virDomainPtr dom, const char *path ATTRIBUTE_UNUSED)
{
    VBOX_OBJECT_CHECK(dom->conn, int, -1);
    IConsole *console    = NULL;
    vboxIIDUnion iid;
    IMachine *machine = NULL;
    IProgress *progress = NULL;
    resultCodeUnion resultCode;
    nsresult rc;

    /* VirtualBox currently doesn't support saving to a file
     * at a location other then the machine folder and thus
     * setting path to ATTRIBUTE_UNUSED for now, will change
     * this behaviour once get the VirtualBox API in right
     * shape to do this
     */

    /* Open a Session for the machine */
    if (openSessionForMachine(data, dom->uuid, &iid, &machine, true) < 0)
        goto cleanup;

    rc = gVBoxAPI.UISession.OpenExisting(data, &iid, machine);
    if (NS_FAILED(rc))
        goto cleanup;

    rc = gVBoxAPI.UISession.GetConsole(data->vboxSession, &console);
    if (NS_FAILED(rc) || !console)
        goto freeSession;

    rc = gVBoxAPI.UIConsole.SaveState(console, &progress);
    if (!progress)
        goto freeSession;

    gVBoxAPI.UIProgress.WaitForCompletion(progress, -1);
    gVBoxAPI.UIProgress.GetResultCode(progress, &resultCode);
    if (RC_SUCCEEDED(resultCode))
        ret = 0;

 freeSession:
    gVBoxAPI.UISession.Close(data->vboxSession);

 cleanup:
    DEBUGIID("UUID of machine being saved:", &iid);
    VBOX_RELEASE(machine);
    VBOX_RELEASE(console);
    VBOX_RELEASE(progress);
    vboxIIDUnalloc(&iid);
    return ret;
}

static void vboxDriverLock(vboxGlobalData *data)
{
    virMutexLock(&data->lock);
}

static void vboxDriverUnlock(vboxGlobalData *data)
{
    virMutexUnlock(&data->lock);
}

int vboxConnectGetVersion(virConnectPtr conn, unsigned long *version)
{
    vboxGlobalData *data = conn->privateData;
    VIR_DEBUG("%s: in vboxGetVersion", conn->driver->name);

    vboxDriverLock(data);
    *version = data->version;
    vboxDriverUnlock(data);

    return 0;
}

char *vboxConnectGetHostname(virConnectPtr conn ATTRIBUTE_UNUSED)
{
    return virGetHostname();
}

int vboxConnectIsSecure(virConnectPtr conn ATTRIBUTE_UNUSED)
{
    /* Driver is using local, non-network based transport */
    return 1;
}

int vboxConnectIsEncrypted(virConnectPtr conn ATTRIBUTE_UNUSED)
{
    /* No encryption is needed, or used on the local transport*/
    return 0;
}

int vboxConnectIsAlive(virConnectPtr conn ATTRIBUTE_UNUSED)
{
    return 1;
}

int
vboxConnectGetMaxVcpus(virConnectPtr conn, const char *type ATTRIBUTE_UNUSED)
{
    VBOX_OBJECT_CHECK(conn, int, -1);
    PRUint32 maxCPUCount = 0;

    /* VirtualBox Supports only hvm and thus the type passed to it
     * has no meaning, setting it to ATTRIBUTE_UNUSED
     */
    ISystemProperties *systemProperties = NULL;

    gVBoxAPI.UIVirtualBox.GetSystemProperties(data->vboxObj, &systemProperties);
    if (!systemProperties)
        goto cleanup;
    gVBoxAPI.UISystemProperties.GetMaxGuestCPUCount(systemProperties, &maxCPUCount);

    if (maxCPUCount > 0)
        ret = maxCPUCount;

 cleanup:
    VBOX_RELEASE(systemProperties);
    return ret;
}

char *vboxConnectGetCapabilities(virConnectPtr conn)
{
    VBOX_OBJECT_CHECK(conn, char *, NULL);

    vboxDriverLock(data);
    ret = virCapabilitiesFormatXML(data->caps);
    vboxDriverUnlock(data);

    return ret;
}

int vboxConnectListDomains(virConnectPtr conn, int *ids, int nids)
{
    VBOX_OBJECT_CHECK(conn, int, -1);
    vboxArray machines = VBOX_ARRAY_INITIALIZER;
    PRUint32 state;
    nsresult rc;
    size_t i, j;

    rc = gVBoxAPI.UArray.vboxArrayGet(&machines, data->vboxObj, ARRAY_GET_MACHINES);
    if (NS_FAILED(rc)) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Could not get list of Domains, rc=%08x"),
                       (unsigned)rc);
        goto cleanup;
    }

    ret = 0;
    for (i = 0, j = 0; (i < machines.count) && (j < nids); ++i) {
        IMachine *machine = machines.items[i];

        if (machine) {
            PRBool isAccessible = PR_FALSE;
            gVBoxAPI.UIMachine.GetAccessible(machine, &isAccessible);
            if (isAccessible) {
                gVBoxAPI.UIMachine.GetState(machine, &state);
                if (gVBoxAPI.machineStateChecker.Online(state)) {
                    ret++;
                    ids[j++] = i + 1;
                }
            }
        }
    }

 cleanup:
    gVBoxAPI.UArray.vboxArrayRelease(&machines);
    return ret;
}

int vboxConnectNumOfDomains(virConnectPtr conn)
{
    VBOX_OBJECT_CHECK(conn, int, -1);
    vboxArray machines = VBOX_ARRAY_INITIALIZER;
    PRUint32 state;
    nsresult rc;
    size_t i;

    rc = gVBoxAPI.UArray.vboxArrayGet(&machines, data->vboxObj, ARRAY_GET_MACHINES);
    if (NS_FAILED(rc)) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Could not get number of Domains, rc=%08x"), (unsigned)rc);
        goto cleanup;
    }

    ret = 0;
    for (i = 0; i < machines.count; ++i) {
        IMachine *machine = machines.items[i];

        if (machine) {
            PRBool isAccessible = PR_FALSE;
            gVBoxAPI.UIMachine.GetAccessible(machine, &isAccessible);
            if (isAccessible) {
                gVBoxAPI.UIMachine.GetState(machine, &state);
                if (gVBoxAPI.machineStateChecker.Online(state))
                    ret++;
            }
        }
    }

 cleanup:
    gVBoxAPI.UArray.vboxArrayRelease(&machines);
    return ret;
}

virDomainPtr vboxDomainLookupByID(virConnectPtr conn, int id)
{
    VBOX_OBJECT_CHECK(conn, virDomainPtr, NULL);
    vboxArray machines = VBOX_ARRAY_INITIALIZER;
    IMachine *machine;
    PRBool isAccessible = PR_FALSE;
    PRUnichar *machineNameUtf16 = NULL;
    char *machineNameUtf8  = NULL;
    vboxIIDUnion iid;
    unsigned char uuid[VIR_UUID_BUFLEN];
    PRUint32 state;
    nsresult rc;

    VBOX_IID_INITIALIZE(&iid);
    /* Internal vbox IDs start from 0, the public libvirt ID
     * starts from 1, so refuse id == 0, and adjust the rest*/
    if (id == 0) {
        virReportError(VIR_ERR_NO_DOMAIN,
                       _("no domain with matching id %d"), id);
        return NULL;
    }
    id = id - 1;

    rc = gVBoxAPI.UArray.vboxArrayGet(&machines, data->vboxObj, ARRAY_GET_MACHINES);
    if (NS_FAILED(rc)) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Could not get list of machines, rc=%08x"), (unsigned)rc);
        return NULL;
    }

    if (id >= machines.count)
        goto cleanup;

    machine = machines.items[id];

    if (!machine)
        goto cleanup;

    isAccessible = PR_FALSE;
    gVBoxAPI.UIMachine.GetAccessible(machine, &isAccessible);
    if (!isAccessible)
        goto cleanup;

    gVBoxAPI.UIMachine.GetState(machine, &state);
    if (!gVBoxAPI.machineStateChecker.Online(state))
        goto cleanup;

    gVBoxAPI.UIMachine.GetName(machine, &machineNameUtf16);
    VBOX_UTF16_TO_UTF8(machineNameUtf16, &machineNameUtf8);

    gVBoxAPI.UIMachine.GetId(machine, &iid);
    vboxIIDToUUID(&iid, uuid);
    vboxIIDUnalloc(&iid);

    /* get a new domain pointer from virGetDomain, if it fails
     * then no need to assign the id, else assign the id, cause
     * it is -1 by default. rest is taken care by virGetDomain
     * itself, so need not worry.
     */

    ret = virGetDomain(conn, machineNameUtf8, uuid);
    if (ret)
        ret->id = id + 1;

    /* Cleanup all the XPCOM allocated stuff here */
    VBOX_UTF8_FREE(machineNameUtf8);
    VBOX_UTF16_FREE(machineNameUtf16);

 cleanup:
    gVBoxAPI.UArray.vboxArrayRelease(&machines);
    return ret;
}

virDomainPtr vboxDomainLookupByUUID(virConnectPtr conn,
                                    const unsigned char *uuid)
{
    VBOX_OBJECT_CHECK(conn, virDomainPtr, NULL);
    vboxArray machines = VBOX_ARRAY_INITIALIZER;
    vboxIIDUnion iid;
    char *machineNameUtf8  = NULL;
    PRUnichar *machineNameUtf16 = NULL;
    unsigned char iid_as_uuid[VIR_UUID_BUFLEN];
    size_t i;
    bool matched = false;
    nsresult rc;

    VBOX_IID_INITIALIZE(&iid);
    rc = gVBoxAPI.UArray.vboxArrayGet(&machines, data->vboxObj, ARRAY_GET_MACHINES);
    if (NS_FAILED(rc)) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Could not get list of machines, rc=%08x"), (unsigned)rc);
        return NULL;
    }

    for (i = 0; i < machines.count; ++i) {
        IMachine *machine = machines.items[i];
        PRBool isAccessible = PR_FALSE;

        if (!machine)
            continue;

        gVBoxAPI.UIMachine.GetAccessible(machine, &isAccessible);
        if (!isAccessible)
            continue;

        rc = gVBoxAPI.UIMachine.GetId(machine, &iid);
        if (NS_FAILED(rc))
            continue;
        vboxIIDToUUID(&iid, iid_as_uuid);
        vboxIIDUnalloc(&iid);

        if (memcmp(uuid, iid_as_uuid, VIR_UUID_BUFLEN) == 0) {

            PRUint32 state;

            matched = true;

            gVBoxAPI.UIMachine.GetName(machine, &machineNameUtf16);
            VBOX_UTF16_TO_UTF8(machineNameUtf16, &machineNameUtf8);

            gVBoxAPI.UIMachine.GetState(machine, &state);

            /* get a new domain pointer from virGetDomain, if it fails
             * then no need to assign the id, else assign the id, cause
             * it is -1 by default. rest is taken care by virGetDomain
             * itself, so need not worry.
             */

            ret = virGetDomain(conn, machineNameUtf8, iid_as_uuid);
            if (ret &&
                gVBoxAPI.machineStateChecker.Online(state))
                ret->id = i + 1;
         }

         if (matched)
             break;
    }

    /* Do the cleanup and take care you dont leak any memory */
    VBOX_UTF8_FREE(machineNameUtf8);
    VBOX_COM_UNALLOC_MEM(machineNameUtf16);
    gVBoxAPI.UArray.vboxArrayRelease(&machines);

    return ret;
}

virDomainPtr
vboxDomainLookupByName(virConnectPtr conn, const char *name)
{
    VBOX_OBJECT_CHECK(conn, virDomainPtr, NULL);
    vboxArray machines = VBOX_ARRAY_INITIALIZER;
    vboxIIDUnion iid;
    char *machineNameUtf8  = NULL;
    PRUnichar *machineNameUtf16 = NULL;
    unsigned char uuid[VIR_UUID_BUFLEN];
    size_t i;
    bool matched = false;
    nsresult rc;

    VBOX_IID_INITIALIZE(&iid);
    rc = gVBoxAPI.UArray.vboxArrayGet(&machines, data->vboxObj, ARRAY_GET_MACHINES);
    if (NS_FAILED(rc)) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Could not get list of machines, rc=%08x"), (unsigned)rc);
        return NULL;
    }

    for (i = 0; i < machines.count; ++i) {
        IMachine *machine = machines.items[i];
        PRBool isAccessible = PR_FALSE;

        if (!machine)
            continue;

        gVBoxAPI.UIMachine.GetAccessible(machine, &isAccessible);
        if (!isAccessible)
            continue;

        gVBoxAPI.UIMachine.GetName(machine, &machineNameUtf16);
        VBOX_UTF16_TO_UTF8(machineNameUtf16, &machineNameUtf8);

        if (STREQ(name, machineNameUtf8)) {

            PRUint32 state;

            matched = true;

            gVBoxAPI.UIMachine.GetId(machine, &iid);
            vboxIIDToUUID(&iid, uuid);
            vboxIIDUnalloc(&iid);

            gVBoxAPI.UIMachine.GetState(machine, &state);

            /* get a new domain pointer from virGetDomain, if it fails
             * then no need to assign the id, else assign the id, cause
             * it is -1 by default. rest is taken care by virGetDomain
             * itself, so need not worry.
             */

            ret = virGetDomain(conn, machineNameUtf8, uuid);
            if (ret &&
                gVBoxAPI.machineStateChecker.Online(state))
                ret->id = i + 1;
        }

        VBOX_UTF8_FREE(machineNameUtf8);
        VBOX_COM_UNALLOC_MEM(machineNameUtf16);
        if (matched)
            break;
    }

    gVBoxAPI.UArray.vboxArrayRelease(&machines);

    return ret;
}

static void
vboxSetBootDeviceOrder(virDomainDefPtr def, vboxGlobalData *data,
                       IMachine *machine)
{
    ISystemProperties *systemProperties = NULL;
    PRUint32 maxBootPosition            = 0;
    size_t i = 0;

    VIR_DEBUG("def->os.type             %s", def->os.type);
    VIR_DEBUG("def->os.arch             %s", virArchToString(def->os.arch));
    VIR_DEBUG("def->os.machine          %s", def->os.machine);
    VIR_DEBUG("def->os.nBootDevs        %zu", def->os.nBootDevs);
    VIR_DEBUG("def->os.bootDevs[0]      %d", def->os.bootDevs[0]);
    VIR_DEBUG("def->os.bootDevs[1]      %d", def->os.bootDevs[1]);
    VIR_DEBUG("def->os.bootDevs[2]      %d", def->os.bootDevs[2]);
    VIR_DEBUG("def->os.bootDevs[3]      %d", def->os.bootDevs[3]);
    VIR_DEBUG("def->os.init             %s", def->os.init);
    VIR_DEBUG("def->os.kernel           %s", def->os.kernel);
    VIR_DEBUG("def->os.initrd           %s", def->os.initrd);
    VIR_DEBUG("def->os.cmdline          %s", def->os.cmdline);
    VIR_DEBUG("def->os.root             %s", def->os.root);
    VIR_DEBUG("def->os.loader           %s", def->os.loader);
    VIR_DEBUG("def->os.bootloader       %s", def->os.bootloader);
    VIR_DEBUG("def->os.bootloaderArgs   %s", def->os.bootloaderArgs);

    gVBoxAPI.UIVirtualBox.GetSystemProperties(data->vboxObj, &systemProperties);
    if (systemProperties) {
        gVBoxAPI.UISystemProperties.GetMaxBootPosition(systemProperties,
                                                       &maxBootPosition);
        VBOX_RELEASE(systemProperties);
    }

    /* Clear the defaults first */
    for (i = 0; i < maxBootPosition; i++) {
        gVBoxAPI.UIMachine.SetBootOrder(machine, i+1, DeviceType_Null);
    }

    for (i = 0; (i < def->os.nBootDevs) && (i < maxBootPosition); i++) {
        PRUint32 device = DeviceType_Null;

        if (def->os.bootDevs[i] == VIR_DOMAIN_BOOT_FLOPPY) {
            device = DeviceType_Floppy;
        } else if (def->os.bootDevs[i] == VIR_DOMAIN_BOOT_CDROM) {
            device = DeviceType_DVD;
        } else if (def->os.bootDevs[i] == VIR_DOMAIN_BOOT_DISK) {
            device = DeviceType_HardDisk;
        } else if (def->os.bootDevs[i] == VIR_DOMAIN_BOOT_NET) {
            device = DeviceType_Network;
        }
        gVBoxAPI.UIMachine.SetBootOrder(machine, i+1, device);
    }
}

static void
vboxAttachDrivesNew(virDomainDefPtr def, vboxGlobalData *data, IMachine *machine)
{
    /* AttachDrives for 3.0 and later */
    size_t i;
    nsresult rc;
    PRUint32 maxPortPerInst[StorageBus_Floppy + 1] = {};
    PRUint32 maxSlotPerPort[StorageBus_Floppy + 1] = {};
    PRUnichar *storageCtlName = NULL;
    bool error = false;

    if (gVBoxAPI.vboxAttachDrivesUseOld)
        VIR_WARN("This function may not work in current vbox version");

    /* get the max port/slots/etc for the given storage bus */
    error = !vboxGetMaxPortSlotValues(data->vboxObj, maxPortPerInst,
                                      maxSlotPerPort);

    /* add a storage controller for the mediums to be attached */
    /* this needs to change when multiple controller are supported for
     * ver > 3.1 */
    {
        IStorageController *storageCtl = NULL;
        PRUnichar *sName = NULL;

        VBOX_UTF8_TO_UTF16("IDE Controller", &sName);
        gVBoxAPI.UIMachine.AddStorageController(machine,
                                                sName,
                                                StorageBus_IDE,
                                                &storageCtl);
        VBOX_UTF16_FREE(sName);
        VBOX_RELEASE(storageCtl);

        VBOX_UTF8_TO_UTF16("SATA Controller", &sName);
        gVBoxAPI.UIMachine.AddStorageController(machine,
                                                sName,
                                                StorageBus_SATA,
                                                &storageCtl);
        VBOX_UTF16_FREE(sName);
        VBOX_RELEASE(storageCtl);

        VBOX_UTF8_TO_UTF16("SCSI Controller", &sName);
        gVBoxAPI.UIMachine.AddStorageController(machine,
                                                sName,
                                                StorageBus_SCSI,
                                                &storageCtl);
        VBOX_UTF16_FREE(sName);
        VBOX_RELEASE(storageCtl);

        VBOX_UTF8_TO_UTF16("Floppy Controller", &sName);
        gVBoxAPI.UIMachine.AddStorageController(machine,
                                                sName,
                                                StorageBus_Floppy,
                                                &storageCtl);
        VBOX_UTF16_FREE(sName);
        VBOX_RELEASE(storageCtl);
    }

    for (i = 0; i < def->ndisks && !error; i++) {
        const char *src = virDomainDiskGetSource(def->disks[i]);
        int type = virDomainDiskGetType(def->disks[i]);
        int format = virDomainDiskGetFormat(def->disks[i]);

        VIR_DEBUG("disk(%zu) type:       %d", i, type);
        VIR_DEBUG("disk(%zu) device:     %d", i, def->disks[i]->device);
        VIR_DEBUG("disk(%zu) bus:        %d", i, def->disks[i]->bus);
        VIR_DEBUG("disk(%zu) src:        %s", i, src);
        VIR_DEBUG("disk(%zu) dst:        %s", i, def->disks[i]->dst);
        VIR_DEBUG("disk(%zu) driverName: %s", i,
                  virDomainDiskGetDriver(def->disks[i]));
        VIR_DEBUG("disk(%zu) driverType: %s", i,
                  virStorageFileFormatTypeToString(format));
        VIR_DEBUG("disk(%zu) cachemode:  %d", i, def->disks[i]->cachemode);
        VIR_DEBUG("disk(%zu) readonly:   %s", i, (def->disks[i]->src->readonly
                                             ? "True" : "False"));
        VIR_DEBUG("disk(%zu) shared:     %s", i, (def->disks[i]->src->shared
                                             ? "True" : "False"));

        if (type == VIR_STORAGE_TYPE_FILE && src) {
            IMedium   *medium          = NULL;
            vboxIIDUnion mediumUUID;
            PRUnichar *mediumFileUtf16 = NULL;
            PRUint32   storageBus      = StorageBus_Null;
            PRUint32   deviceType      = DeviceType_Null;
            PRUint32   accessMode      = AccessMode_ReadOnly;
            PRInt32    deviceInst      = 0;
            PRInt32    devicePort      = 0;
            PRInt32    deviceSlot      = 0;

            VBOX_IID_INITIALIZE(&mediumUUID);
            VBOX_UTF8_TO_UTF16(src, &mediumFileUtf16);

            if (def->disks[i]->device == VIR_DOMAIN_DISK_DEVICE_DISK) {
                deviceType = DeviceType_HardDisk;
                accessMode = AccessMode_ReadWrite;
            } else if (def->disks[i]->device == VIR_DOMAIN_DISK_DEVICE_CDROM) {
                deviceType = DeviceType_DVD;
                accessMode = AccessMode_ReadOnly;
            } else if (def->disks[i]->device == VIR_DOMAIN_DISK_DEVICE_FLOPPY) {
                deviceType = DeviceType_Floppy;
                accessMode = AccessMode_ReadWrite;
            } else {
                VBOX_UTF16_FREE(mediumFileUtf16);
                continue;
            }

            gVBoxAPI.UIVirtualBox.FindMedium(data->vboxObj, mediumFileUtf16,
                                             deviceType, accessMode, &medium);

            if (!medium) {
                PRUnichar *mediumEmpty = NULL;

                VBOX_UTF8_TO_UTF16("", &mediumEmpty);

                rc = gVBoxAPI.UIVirtualBox.OpenMedium(data->vboxObj,
                                                      mediumFileUtf16,
                                                      deviceType, accessMode,
                                                      &medium);
                VBOX_UTF16_FREE(mediumEmpty);
            }

            if (!medium) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("Failed to attach the following disk/dvd/floppy "
                                 "to the machine: %s, rc=%08x"),
                               src, (unsigned)rc);
                VBOX_UTF16_FREE(mediumFileUtf16);
                continue;
            }

            rc = gVBoxAPI.UIMedium.GetId(medium, &mediumUUID);
            if (NS_FAILED(rc)) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("can't get the uuid of the file to be attached "
                                 "as harddisk/dvd/floppy: %s, rc=%08x"),
                               src, (unsigned)rc);
                VBOX_MEDIUM_RELEASE(medium);
                VBOX_UTF16_FREE(mediumFileUtf16);
                continue;
            }

            if (def->disks[i]->device == VIR_DOMAIN_DISK_DEVICE_DISK) {
                if (def->disks[i]->src->readonly) {
                    gVBoxAPI.UIMedium.SetType(medium, MediumType_Immutable);
                    VIR_DEBUG("setting harddisk to immutable");
                } else if (!def->disks[i]->src->readonly) {
                    gVBoxAPI.UIMedium.SetType(medium, MediumType_Normal);
                    VIR_DEBUG("setting harddisk type to normal");
                }
            }

            if (def->disks[i]->bus == VIR_DOMAIN_DISK_BUS_IDE) {
                VBOX_UTF8_TO_UTF16("IDE Controller", &storageCtlName);
                storageBus = StorageBus_IDE;
            } else if (def->disks[i]->bus == VIR_DOMAIN_DISK_BUS_SATA) {
                VBOX_UTF8_TO_UTF16("SATA Controller", &storageCtlName);
                storageBus = StorageBus_SATA;
            } else if (def->disks[i]->bus == VIR_DOMAIN_DISK_BUS_SCSI) {
                VBOX_UTF8_TO_UTF16("SCSI Controller", &storageCtlName);
                storageBus = StorageBus_SCSI;
            } else if (def->disks[i]->bus == VIR_DOMAIN_DISK_BUS_FDC) {
                VBOX_UTF8_TO_UTF16("Floppy Controller", &storageCtlName);
                storageBus = StorageBus_Floppy;
            }

            /* get the device details i.e instance, port and slot */
            if (!vboxGetDeviceDetails(def->disks[i]->dst,
                                      maxPortPerInst,
                                      maxSlotPerPort,
                                      storageBus,
                                      &deviceInst,
                                      &devicePort,
                                      &deviceSlot)) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("can't get the port/slot number of "
                                 "harddisk/dvd/floppy to be attached: "
                                 "%s, rc=%08x"),
                               src, (unsigned)rc);
                VBOX_MEDIUM_RELEASE(medium);
                vboxIIDUnalloc(&mediumUUID);
                VBOX_UTF16_FREE(mediumFileUtf16);
                continue;
            }

            /* attach the harddisk/dvd/Floppy to the storage controller */
            rc = gVBoxAPI.UIMachine.AttachDevice(machine,
                                                 storageCtlName,
                                                 devicePort,
                                                 deviceSlot,
                                                 deviceType,
                                                 medium);

            if (NS_FAILED(rc)) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("could not attach the file as "
                                 "harddisk/dvd/floppy: %s, rc=%08x"),
                               src, (unsigned)rc);
            } else {
                DEBUGIID("Attached HDD/DVD/Floppy with UUID", &mediumUUID);
            }

            VBOX_MEDIUM_RELEASE(medium);
            vboxIIDUnalloc(&mediumUUID);
            VBOX_UTF16_FREE(mediumFileUtf16);
            VBOX_UTF16_FREE(storageCtlName);
        }
    }
}

static void
vboxAttachDrives(virDomainDefPtr def, vboxGlobalData *data, IMachine *machine)
{
    /* Here, About the vboxAttachDrives. In fact,there is
     * three different implementations. We name it as
     * v1, v2 and v3.
     *
     * The first version(v1) is only used in vbox 2.2 and 3.0,
     * v2 is used by 3.1 and 3.2, and v3 is used for later
     * vbox versions. In sight of implementation, the v1 is
     * totally different with v2 and v3. The v2 shares the same
     * outline with v3, meanwhile the API they used has much
     * difference.
     *
     * It seems we have no thing to do with old versions such as
     * v1 and v2 when developing new vbox drivers. What's more,
     * most of the vbox APIs used in v1 and v2 is incompatible with
     * new vbox versions. It is a burden to put these APIs into
     * vboxUniformedAPI, I prefer not to do that.
     *
     * After balancing the code size and the complied code size,
     * I put my solution here. The v1 and v2 is a version specified
     * code, which only be generated for first four version. The v3
     * will be put in vbox_common.c, it be complied only once, then
     * be used by all next vbox drivers.
     *
     * Check the flag vboxAttachDrivesUseOld can tell you which
     * implementation to use. When the flag is set, we need use
     * the old version though gVBoxAPI.vboxAttachDrivesOld. It
     * will automatically point to v1 or v2 deponds on you version.
     * If the flag is clear, just call vboxAttachDrivesNew, which
     * is the v3 implementation.
     */
    if (gVBoxAPI.vboxAttachDrivesUseOld)
        gVBoxAPI.vboxAttachDrivesOld(def, data, machine);
    else
        vboxAttachDrivesNew(def, data, machine);
}

static void
vboxAttachSound(virDomainDefPtr def, IMachine *machine)
{
    nsresult rc;
    IAudioAdapter *audioAdapter = NULL;

    /* Check if def->nsounds is one as VirtualBox currently supports
     * only one sound card
     */
    if (def->nsounds != 1)
        return;

    gVBoxAPI.UIMachine.GetAudioAdapter(machine, &audioAdapter);
    if (!audioAdapter)
        return;

    rc = gVBoxAPI.UIAudioAdapter.SetEnabled(audioAdapter, 1);
    if (NS_FAILED(rc))
        goto cleanup;

    if (def->sounds[0]->model == VIR_DOMAIN_SOUND_MODEL_SB16) {
        gVBoxAPI.UIAudioAdapter.SetAudioController(audioAdapter,
                                                   AudioControllerType_SB16);
    } else
    if (def->sounds[0]->model == VIR_DOMAIN_SOUND_MODEL_AC97) {
        gVBoxAPI.UIAudioAdapter.SetAudioController(audioAdapter,
                                                   AudioControllerType_AC97);
    }

 cleanup:
    VBOX_RELEASE(audioAdapter);
}

static void
vboxAttachNetwork(virDomainDefPtr def, vboxGlobalData *data, IMachine *machine)
{
    ISystemProperties *systemProperties = NULL;
    PRUint32 chipsetType                = ChipsetType_Null;
    PRUint32 networkAdapterCount        = 0;
    size_t i = 0;

    if (gVBoxAPI.chipsetType)
        gVBoxAPI.UIMachine.GetChipsetType(machine, &chipsetType);

    gVBoxAPI.UIVirtualBox.GetSystemProperties(data->vboxObj, &systemProperties);
    if (systemProperties) {
        gVBoxAPI.UISystemProperties.GetMaxNetworkAdapters(systemProperties, chipsetType,
                                                          &networkAdapterCount);
        VBOX_RELEASE(systemProperties);
    }

    VIR_DEBUG("Number of Network Cards to be connected: %zu", def->nnets);
    VIR_DEBUG("Number of Network Cards available: %d", networkAdapterCount);

    for (i = 0; (i < def->nnets) && (i < networkAdapterCount); i++) {
        INetworkAdapter *adapter = NULL;
        PRUint32 adapterType     = NetworkAdapterType_Null;
        char macaddr[VIR_MAC_STRING_BUFLEN] = {0};
        char macaddrvbox[VIR_MAC_STRING_BUFLEN - 5] = {0};
        PRUnichar *MACAddress = NULL;

        virMacAddrFormat(&def->nets[i]->mac, macaddr);
        snprintf(macaddrvbox, VIR_MAC_STRING_BUFLEN - 5,
                 "%02X%02X%02X%02X%02X%02X",
                 def->nets[i]->mac.addr[0],
                 def->nets[i]->mac.addr[1],
                 def->nets[i]->mac.addr[2],
                 def->nets[i]->mac.addr[3],
                 def->nets[i]->mac.addr[4],
                 def->nets[i]->mac.addr[5]);
        macaddrvbox[VIR_MAC_STRING_BUFLEN - 6] = '\0';

        VIR_DEBUG("NIC(%zu): Type:   %d", i, def->nets[i]->type);
        VIR_DEBUG("NIC(%zu): Model:  %s", i, def->nets[i]->model);
        VIR_DEBUG("NIC(%zu): Mac:    %s", i, macaddr);
        VIR_DEBUG("NIC(%zu): ifname: %s", i, def->nets[i]->ifname);
        if (def->nets[i]->type == VIR_DOMAIN_NET_TYPE_NETWORK) {
            VIR_DEBUG("NIC(%zu): name:    %s", i, def->nets[i]->data.network.name);
        } else if (def->nets[i]->type == VIR_DOMAIN_NET_TYPE_INTERNAL) {
            VIR_DEBUG("NIC(%zu): name:   %s", i, def->nets[i]->data.internal.name);
        } else if (def->nets[i]->type == VIR_DOMAIN_NET_TYPE_USER) {
            VIR_DEBUG("NIC(%zu): NAT.", i);
        } else if (def->nets[i]->type == VIR_DOMAIN_NET_TYPE_BRIDGE) {
            VIR_DEBUG("NIC(%zu): brname: %s", i, def->nets[i]->data.bridge.brname);
            VIR_DEBUG("NIC(%zu): script: %s", i, def->nets[i]->script);
            VIR_DEBUG("NIC(%zu): ipaddr: %s", i, def->nets[i]->data.bridge.ipaddr);
        }

        gVBoxAPI.UIMachine.GetNetworkAdapter(machine, i, &adapter);
        if (!adapter)
            continue;

        gVBoxAPI.UINetworkAdapter.SetEnabled(adapter, 1);

        if (def->nets[i]->model) {
            if (STRCASEEQ(def->nets[i]->model, "Am79C970A")) {
                adapterType = NetworkAdapterType_Am79C970A;
            } else if (STRCASEEQ(def->nets[i]->model, "Am79C973")) {
                adapterType = NetworkAdapterType_Am79C973;
            } else if (STRCASEEQ(def->nets[i]->model, "82540EM")) {
                adapterType = NetworkAdapterType_I82540EM;
            } else if (STRCASEEQ(def->nets[i]->model, "82545EM")) {
                adapterType = NetworkAdapterType_I82545EM;
            } else if (STRCASEEQ(def->nets[i]->model, "82543GC")) {
                adapterType = NetworkAdapterType_I82543GC;
            } else if (gVBoxAPI.APIVersion >= 3000051 &&
                       STRCASEEQ(def->nets[i]->model, "virtio")) {
                /* Only vbox 3.1 and later support NetworkAdapterType_Virto */
                adapterType = NetworkAdapterType_Virtio;
            }
        } else {
            adapterType = NetworkAdapterType_Am79C973;
        }

        gVBoxAPI.UINetworkAdapter.SetAdapterType(adapter, adapterType);

        if (def->nets[i]->type == VIR_DOMAIN_NET_TYPE_BRIDGE) {
            PRUnichar *hostInterface = NULL;
            /* Bridged Network */

            gVBoxAPI.UINetworkAdapter.AttachToBridgedInterface(adapter);

            if (def->nets[i]->data.bridge.brname) {
                VBOX_UTF8_TO_UTF16(def->nets[i]->data.bridge.brname,
                                   &hostInterface);
                gVBoxAPI.UINetworkAdapter.SetBridgedInterface(adapter, hostInterface);
                VBOX_UTF16_FREE(hostInterface);
            }
        } else if (def->nets[i]->type == VIR_DOMAIN_NET_TYPE_INTERNAL) {
            PRUnichar *internalNetwork = NULL;
            /* Internal Network */

            gVBoxAPI.UINetworkAdapter.AttachToInternalNetwork(adapter);

            if (def->nets[i]->data.internal.name) {
                VBOX_UTF8_TO_UTF16(def->nets[i]->data.internal.name,
                                   &internalNetwork);
                gVBoxAPI.UINetworkAdapter.SetInternalNetwork(adapter, internalNetwork);
                VBOX_UTF16_FREE(internalNetwork);
            }
        } else if (def->nets[i]->type == VIR_DOMAIN_NET_TYPE_NETWORK) {
            PRUnichar *hostInterface = NULL;
            /* Host Only Networking (currently only vboxnet0 available
             * on *nix and mac, on windows you can create and configure
             * as many as you want)
             */
            gVBoxAPI.UINetworkAdapter.AttachToHostOnlyInterface(adapter);

            if (def->nets[i]->data.network.name) {
                VBOX_UTF8_TO_UTF16(def->nets[i]->data.network.name,
                                   &hostInterface);
                gVBoxAPI.UINetworkAdapter.SetHostOnlyInterface(adapter, hostInterface);
                VBOX_UTF16_FREE(hostInterface);
            }
        } else if (def->nets[i]->type == VIR_DOMAIN_NET_TYPE_USER) {
            /* NAT */
            gVBoxAPI.UINetworkAdapter.AttachToNAT(adapter);
        } else {
            /* else always default to NAT if we don't understand
             * what option is been passed to us
             */
            gVBoxAPI.UINetworkAdapter.AttachToNAT(adapter);
        }

        VBOX_UTF8_TO_UTF16(macaddrvbox, &MACAddress);
        gVBoxAPI.UINetworkAdapter.SetMACAddress(adapter, MACAddress);
        VBOX_UTF16_FREE(MACAddress);
    }
}

static void
vboxAttachSerial(virDomainDefPtr def, vboxGlobalData *data, IMachine *machine)
{
    ISystemProperties *systemProperties = NULL;
    PRUint32 serialPortCount            = 0;
    size_t i = 0;

    gVBoxAPI.UIVirtualBox.GetSystemProperties(data->vboxObj, &systemProperties);
    if (systemProperties) {
        gVBoxAPI.UISystemProperties.GetSerialPortCount(systemProperties,
                                                       &serialPortCount);
        VBOX_RELEASE(systemProperties);
    }

    VIR_DEBUG("Number of Serial Ports to be connected: %zu", def->nserials);
    VIR_DEBUG("Number of Serial Ports available: %d", serialPortCount);

    for (i = 0; (i < def->nserials) && (i < serialPortCount); i++) {
        ISerialPort *serialPort = NULL;
        PRUnichar *pathUtf16 = NULL;

        VIR_DEBUG("SerialPort(%zu): Type: %d", i, def->serials[i]->source.type);
        VIR_DEBUG("SerialPort(%zu): target.port: %d", i,
              def->serials[i]->target.port);

        gVBoxAPI.UIMachine.GetSerialPort(machine, i, &serialPort);
        if (!serialPort)
            continue;

        gVBoxAPI.UISerialPort.SetEnabled(serialPort, 1);

        if (def->serials[i]->source.data.file.path) {
            VBOX_UTF8_TO_UTF16(def->serials[i]->source.data.file.path,
                               &pathUtf16);
            gVBoxAPI.UISerialPort.SetPath(serialPort, pathUtf16);
        }

        /* For now hard code the serial ports to COM1 and COM2,
         * COM1 (Base Addr: 0x3F8 (decimal: 1016), IRQ: 4)
         * COM2 (Base Addr: 0x2F8 (decimal:  760), IRQ: 3)
         * TODO: make this more flexible
         */
        /* TODO: to improve the libvirt XMl handling so
         * that def->serials[i]->target.port shows real port
         * and not always start at 0
         */
        if (def->serials[i]->target.port == 0) {
            gVBoxAPI.UISerialPort.SetIRQ(serialPort, 4);
            gVBoxAPI.UISerialPort.SetIOBase(serialPort, 1016);
            VIR_DEBUG(" serialPort-%zu irq: %d, iobase 0x%x, path: %s",
                  i, 4, 1016, def->serials[i]->source.data.file.path);
        } else if (def->serials[i]->target.port == 1) {
            gVBoxAPI.UISerialPort.SetIRQ(serialPort, 3);
            gVBoxAPI.UISerialPort.SetIOBase(serialPort, 760);
            VIR_DEBUG(" serialPort-%zu irq: %d, iobase 0x%x, path: %s",
                  i, 3, 760, def->serials[i]->source.data.file.path);
        }

        if (def->serials[i]->source.type == VIR_DOMAIN_CHR_TYPE_DEV) {
            gVBoxAPI.UISerialPort.SetHostMode(serialPort, PortMode_HostDevice);
        } else if (def->serials[i]->source.type == VIR_DOMAIN_CHR_TYPE_PIPE) {
            gVBoxAPI.UISerialPort.SetHostMode(serialPort, PortMode_HostPipe);
        } else if (gVBoxAPI.APIVersion >= 2002051 &&
                   def->serials[i]->source.type == VIR_DOMAIN_CHR_TYPE_FILE) {
            /* PortMode RawFile is used for vbox 3.0 or later */
            gVBoxAPI.UISerialPort.SetHostMode(serialPort, PortMode_RawFile);
        } else {
            gVBoxAPI.UISerialPort.SetHostMode(serialPort,
                                              PortMode_Disconnected);
        }

        VBOX_RELEASE(serialPort);
        VBOX_UTF16_FREE(pathUtf16);
    }
}

static void
vboxAttachParallel(virDomainDefPtr def, vboxGlobalData *data, IMachine *machine)
{
    ISystemProperties *systemProperties = NULL;
    PRUint32 parallelPortCount          = 0;
    size_t i = 0;

    gVBoxAPI.UIVirtualBox.GetSystemProperties(data->vboxObj, &systemProperties);
    if (systemProperties) {
        gVBoxAPI.UISystemProperties.GetParallelPortCount(systemProperties,
                                                         &parallelPortCount);
        VBOX_RELEASE(systemProperties);
    }

    VIR_DEBUG("Number of Parallel Ports to be connected: %zu", def->nparallels);
    VIR_DEBUG("Number of Parallel Ports available: %d", parallelPortCount);
    for (i = 0; (i < def->nparallels) && (i < parallelPortCount); i++) {
        IParallelPort *parallelPort = NULL;
        PRUnichar *pathUtf16 = NULL;

        VIR_DEBUG("ParallelPort(%zu): Type: %d", i, def->parallels[i]->source.type);
        VIR_DEBUG("ParallelPort(%zu): target.port: %d", i,
              def->parallels[i]->target.port);

        gVBoxAPI.UIMachine.GetParallelPort(machine, i, &parallelPort);
        if (!parallelPort)
            continue;

        VBOX_UTF8_TO_UTF16(def->parallels[i]->source.data.file.path, &pathUtf16);

        /* For now hard code the parallel ports to
         * LPT1 (Base Addr: 0x378 (decimal: 888), IRQ: 7)
         * LPT2 (Base Addr: 0x278 (decimal: 632), IRQ: 5)
         * TODO: make this more flexible
         */
        if ((def->parallels[i]->source.type == VIR_DOMAIN_CHR_TYPE_DEV)  ||
            (def->parallels[i]->source.type == VIR_DOMAIN_CHR_TYPE_PTY)  ||
            (def->parallels[i]->source.type == VIR_DOMAIN_CHR_TYPE_FILE) ||
            (def->parallels[i]->source.type == VIR_DOMAIN_CHR_TYPE_PIPE)) {
            gVBoxAPI.UIParallelPort.SetPath(parallelPort, pathUtf16);
            if (i == 0) {
                gVBoxAPI.UIParallelPort.SetIRQ(parallelPort, 7);
                gVBoxAPI.UIParallelPort.SetIOBase(parallelPort, 888);
                VIR_DEBUG(" parallePort-%zu irq: %d, iobase 0x%x, path: %s",
                      i, 7, 888, def->parallels[i]->source.data.file.path);
            } else if (i == 1) {
                gVBoxAPI.UIParallelPort.SetIRQ(parallelPort, 5);
                gVBoxAPI.UIParallelPort.SetIOBase(parallelPort, 632);
                VIR_DEBUG(" parallePort-%zu irq: %d, iobase 0x%x, path: %s",
                      i, 5, 632, def->parallels[i]->source.data.file.path);
            }
        }

        /* like serial port, parallel port can't be enabled unless
         * correct IRQ and IOBase values are specified.
         */
        gVBoxAPI.UIParallelPort.SetEnabled(parallelPort, 1);

        VBOX_RELEASE(parallelPort);
        VBOX_UTF16_FREE(pathUtf16);
    }
}

static void
vboxAttachVideo(virDomainDefPtr def, IMachine *machine)
{
    if ((def->nvideos == 1) &&
        (def->videos[0]->type == VIR_DOMAIN_VIDEO_TYPE_VBOX)) {
        gVBoxAPI.UIMachine.SetVRAMSize(machine,
                                       VIR_DIV_UP(def->videos[0]->vram, 1024));
        gVBoxAPI.UIMachine.SetMonitorCount(machine, def->videos[0]->heads);
        if (def->videos[0]->accel) {
            gVBoxAPI.UIMachine.SetAccelerate3DEnabled(machine,
                                                      def->videos[0]->accel->support3d);
            if (gVBoxAPI.accelerate2DVideo)
                gVBoxAPI.UIMachine.SetAccelerate2DVideoEnabled(machine,
                                                               def->videos[0]->accel->support2d);
        } else {
            gVBoxAPI.UIMachine.SetAccelerate3DEnabled(machine, 0);
            if (gVBoxAPI.accelerate2DVideo)
                gVBoxAPI.UIMachine.SetAccelerate2DVideoEnabled(machine, 0);
        }
    }
}

static void
vboxAttachDisplay(virDomainDefPtr def, vboxGlobalData *data, IMachine *machine)
{
    int vrdpPresent  = 0;
    int sdlPresent   = 0;
    int guiPresent   = 0;
    char *guiDisplay = NULL;
    char *sdlDisplay = NULL;
    size_t i = 0;

    for (i = 0; i < def->ngraphics; i++) {
        IVRDxServer *VRDxServer = NULL;

        if ((def->graphics[i]->type == VIR_DOMAIN_GRAPHICS_TYPE_RDP) &&
            (vrdpPresent == 0)) {

            vrdpPresent = 1;
            gVBoxAPI.UIMachine.GetVRDxServer(machine, &VRDxServer);
            if (VRDxServer) {
                const char *listenAddr
                    = virDomainGraphicsListenGetAddress(def->graphics[i], 0);

                gVBoxAPI.UIVRDxServer.SetEnabled(VRDxServer, PR_TRUE);
                VIR_DEBUG("VRDP Support turned ON.");

                gVBoxAPI.UIVRDxServer.SetPorts(data, VRDxServer, def->graphics[i]);

                if (def->graphics[i]->data.rdp.replaceUser) {
                    gVBoxAPI.UIVRDxServer.SetReuseSingleConnection(VRDxServer,
                                                                   PR_TRUE);
                    VIR_DEBUG("VRDP set to reuse single connection");
                }

                if (def->graphics[i]->data.rdp.multiUser) {
                    gVBoxAPI.UIVRDxServer.SetAllowMultiConnection(VRDxServer,
                                                                  PR_TRUE);
                    VIR_DEBUG("VRDP set to allow multiple connection");
                }

                if (listenAddr) {
                    PRUnichar *netAddressUtf16 = NULL;

                    VBOX_UTF8_TO_UTF16(listenAddr, &netAddressUtf16);
                    gVBoxAPI.UIVRDxServer.SetNetAddress(data, VRDxServer,
                                                        netAddressUtf16);
                    VIR_DEBUG("VRDP listen address is set to: %s",
                              listenAddr);

                    VBOX_UTF16_FREE(netAddressUtf16);
                }

                VBOX_RELEASE(VRDxServer);
            }
        }

        if ((def->graphics[i]->type == VIR_DOMAIN_GRAPHICS_TYPE_DESKTOP) &&
            (guiPresent == 0)) {
            guiPresent = 1;
            if (VIR_STRDUP(guiDisplay, def->graphics[i]->data.desktop.display) < 0) {
                /* just don't go to cleanup yet as it is ok to have
                 * guiDisplay as NULL and we check it below if it
                 * exist and then only use it there
                 */
            }
        }

        if ((def->graphics[i]->type == VIR_DOMAIN_GRAPHICS_TYPE_SDL) &&
            (sdlPresent == 0)) {
            sdlPresent = 1;
            if (VIR_STRDUP(sdlDisplay, def->graphics[i]->data.sdl.display) < 0) {
                /* just don't go to cleanup yet as it is ok to have
                 * sdlDisplay as NULL and we check it below if it
                 * exist and then only use it there
                 */
            }
        }
    }

    if ((vrdpPresent == 1) && (guiPresent == 0) && (sdlPresent == 0)) {
        /* store extradata key that frontend is set to vrdp */
        PRUnichar *keyTypeUtf16   = NULL;
        PRUnichar *valueTypeUtf16 = NULL;

        VBOX_UTF8_TO_UTF16("FRONTEND/Type", &keyTypeUtf16);
        VBOX_UTF8_TO_UTF16("vrdp", &valueTypeUtf16);

        gVBoxAPI.UIMachine.SetExtraData(machine, keyTypeUtf16, valueTypeUtf16);

        VBOX_UTF16_FREE(keyTypeUtf16);
        VBOX_UTF16_FREE(valueTypeUtf16);

    } else if ((guiPresent == 0) && (sdlPresent == 1)) {
        /* store extradata key that frontend is set to sdl */
        PRUnichar *keyTypeUtf16      = NULL;
        PRUnichar *valueTypeUtf16    = NULL;
        PRUnichar *keyDislpayUtf16   = NULL;
        PRUnichar *valueDisplayUtf16 = NULL;

        VBOX_UTF8_TO_UTF16("FRONTEND/Type", &keyTypeUtf16);
        VBOX_UTF8_TO_UTF16("sdl", &valueTypeUtf16);

        gVBoxAPI.UIMachine.SetExtraData(machine, keyTypeUtf16, valueTypeUtf16);

        VBOX_UTF16_FREE(keyTypeUtf16);
        VBOX_UTF16_FREE(valueTypeUtf16);

        if (sdlDisplay) {
            VBOX_UTF8_TO_UTF16("FRONTEND/Display", &keyDislpayUtf16);
            VBOX_UTF8_TO_UTF16(sdlDisplay, &valueDisplayUtf16);

            gVBoxAPI.UIMachine.SetExtraData(machine, keyDislpayUtf16,
                                            valueDisplayUtf16);

            VBOX_UTF16_FREE(keyDislpayUtf16);
            VBOX_UTF16_FREE(valueDisplayUtf16);
        }

    } else {
        /* if all are set then default is gui, with vrdp turned on */
        PRUnichar *keyTypeUtf16      = NULL;
        PRUnichar *valueTypeUtf16    = NULL;
        PRUnichar *keyDislpayUtf16   = NULL;
        PRUnichar *valueDisplayUtf16 = NULL;

        VBOX_UTF8_TO_UTF16("FRONTEND/Type", &keyTypeUtf16);
        VBOX_UTF8_TO_UTF16("gui", &valueTypeUtf16);

        gVBoxAPI.UIMachine.SetExtraData(machine, keyTypeUtf16, valueTypeUtf16);

        VBOX_UTF16_FREE(keyTypeUtf16);
        VBOX_UTF16_FREE(valueTypeUtf16);

        if (guiDisplay) {
            VBOX_UTF8_TO_UTF16("FRONTEND/Display", &keyDislpayUtf16);
            VBOX_UTF8_TO_UTF16(guiDisplay, &valueDisplayUtf16);

            gVBoxAPI.UIMachine.SetExtraData(machine, keyDislpayUtf16,
                                            valueDisplayUtf16);

            VBOX_UTF16_FREE(keyDislpayUtf16);
            VBOX_UTF16_FREE(valueDisplayUtf16);
        }
    }

    VIR_FREE(guiDisplay);
    VIR_FREE(sdlDisplay);
}

static void
vboxAttachUSB(virDomainDefPtr def, vboxGlobalData *data, IMachine *machine)
{
    IUSBCommon *USBCommon = NULL;
    size_t i = 0;
    bool isUSB = false;
    nsresult rc;

    if (def->nhostdevs == 0)
        return;

    /* Loop through the devices first and see if you
     * have a USB Device, only if you have one then
     * start the USB controller else just proceed as
     * usual
     */
    for (i = 0; i < def->nhostdevs; i++) {
        if (def->hostdevs[i]->mode != VIR_DOMAIN_HOSTDEV_MODE_SUBSYS)
            continue;

        if (def->hostdevs[i]->source.subsys.type !=
            VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_USB)
            continue;

        if (!def->hostdevs[i]->source.subsys.u.usb.vendor &&
            !def->hostdevs[i]->source.subsys.u.usb.product)
            continue;

        VIR_DEBUG("USB Device detected, VendorId:0x%x, ProductId:0x%x",
                  def->hostdevs[i]->source.subsys.u.usb.vendor,
                  def->hostdevs[i]->source.subsys.u.usb.product);
        isUSB = true;
        break;
    }

    if (!isUSB)
        return;

    /* First Start the USB Controller and then loop
     * to attach USB Devices to it
     */
    rc = gVBoxAPI.UIMachine.GetUSBCommon(machine, &USBCommon);
    if (NS_FAILED(rc) || !USBCommon)
        return;
    gVBoxAPI.UIUSBCommon.Enable(USBCommon);

    for (i = 0; i < def->nhostdevs; i++) {
        char *filtername           = NULL;
        PRUnichar *filternameUtf16 = NULL;
        IUSBDeviceFilter *filter   = NULL;
        PRUnichar *vendorIdUtf16  = NULL;
        char vendorId[40]         = {0};
        PRUnichar *productIdUtf16 = NULL;
        char productId[40]        = {0};

        if (def->hostdevs[i]->mode != VIR_DOMAIN_HOSTDEV_MODE_SUBSYS)
            continue;

        if (def->hostdevs[i]->source.subsys.type !=
            VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_USB)
            continue;

        /* Zero pad for nice alignment when fewer than 9999
         * devices.
         */
        if (virAsprintf(&filtername, "filter%04zu", i) >= 0) {
            VBOX_UTF8_TO_UTF16(filtername, &filternameUtf16);
            VIR_FREE(filtername);
            gVBoxAPI.UIUSBCommon.CreateDeviceFilter(USBCommon,
                                                    filternameUtf16,
                                                    &filter);
        }
        VBOX_UTF16_FREE(filternameUtf16);

        if (!filter)
            continue;

        if (!def->hostdevs[i]->source.subsys.u.usb.vendor &&
            !def->hostdevs[i]->source.subsys.u.usb.product)
            continue;

        if (def->hostdevs[i]->source.subsys.u.usb.vendor) {
            snprintf(vendorId, sizeof(vendorId), "%x",
                     def->hostdevs[i]->source.subsys.u.usb.vendor);
            VBOX_UTF8_TO_UTF16(vendorId, &vendorIdUtf16);
            gVBoxAPI.UIUSBDeviceFilter.SetVendorId(filter, vendorIdUtf16);
            VBOX_UTF16_FREE(vendorIdUtf16);
        }
        if (def->hostdevs[i]->source.subsys.u.usb.product) {
            snprintf(productId, sizeof(productId), "%x",
                     def->hostdevs[i]->source.subsys.u.usb.product);
            VBOX_UTF8_TO_UTF16(productId, &productIdUtf16);
            gVBoxAPI.UIUSBDeviceFilter.SetProductId(filter,
                                                    productIdUtf16);
            VBOX_UTF16_FREE(productIdUtf16);
        }
        gVBoxAPI.UIUSBDeviceFilter.SetActive(filter, 1);
        gVBoxAPI.UIUSBCommon.InsertDeviceFilter(USBCommon, i, filter);
        VBOX_RELEASE(filter);
    }

    VBOX_RELEASE(USBCommon);
}

static void
vboxAttachSharedFolder(virDomainDefPtr def, vboxGlobalData *data, IMachine *machine)
{
    size_t i;
    PRUnichar *nameUtf16;
    PRUnichar *hostPathUtf16;
    PRBool writable;

    if (def->nfss == 0)
        return;

    for (i = 0; i < def->nfss; i++) {
        if (def->fss[i]->type != VIR_DOMAIN_FS_TYPE_MOUNT)
            continue;

        VBOX_UTF8_TO_UTF16(def->fss[i]->dst, &nameUtf16);
        VBOX_UTF8_TO_UTF16(def->fss[i]->src, &hostPathUtf16);
        writable = !def->fss[i]->readonly;

        gVBoxAPI.UIMachine.CreateSharedFolder(machine, nameUtf16, hostPathUtf16,
                                              writable, PR_FALSE);

        VBOX_UTF16_FREE(nameUtf16);
        VBOX_UTF16_FREE(hostPathUtf16);
    }
}

virDomainPtr vboxDomainDefineXML(virConnectPtr conn, const char *xml)
{
    VBOX_OBJECT_CHECK(conn, virDomainPtr, NULL);
    IMachine       *machine     = NULL;
    IBIOSSettings  *bios        = NULL;
    vboxIIDUnion mchiid;
    virDomainDefPtr def         = NULL;
    nsresult rc;
    char uuidstr[VIR_UUID_STRING_BUFLEN];

    VBOX_IID_INITIALIZE(&mchiid);
    if (!(def = virDomainDefParseString(xml, data->caps, data->xmlopt,
                                        1 << VIR_DOMAIN_VIRT_VBOX,
                                        VIR_DOMAIN_XML_INACTIVE))) {
        goto cleanup;
    }

    virUUIDFormat(def->uuid, uuidstr);

    rc = gVBoxAPI.UIVirtualBox.CreateMachine(data, def, &machine, uuidstr);

    if (NS_FAILED(rc)) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("could not define a domain, rc=%08x"), (unsigned)rc);
        goto cleanup;
    }

    rc = gVBoxAPI.UIMachine.SetMemorySize(machine,
                                          VIR_DIV_UP(def->mem.cur_balloon, 1024));
    if (NS_FAILED(rc)) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("could not set the memory size of the domain to: %llu Kb, "
                         "rc=%08x"),
                       def->mem.cur_balloon, (unsigned)rc);
    }

    if (def->vcpus != def->maxvcpus) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("current vcpu count must equal maximum"));
    }
    rc = gVBoxAPI.UIMachine.SetCPUCount(machine, def->maxvcpus);
    if (NS_FAILED(rc)) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("could not set the number of virtual CPUs to: %u, rc=%08x"),
                       def->maxvcpus, (unsigned)rc);
    }

    rc = gVBoxAPI.UIMachine.SetCPUProperty(machine, CPUPropertyType_PAE,
                                           def->features[VIR_DOMAIN_FEATURE_PAE] ==
                                           VIR_TRISTATE_SWITCH_ON);
    if (NS_FAILED(rc)) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("could not change PAE status to: %s, rc=%08x"),
                       (def->features[VIR_DOMAIN_FEATURE_PAE] == VIR_TRISTATE_SWITCH_ON)
                       ? _("Enabled") : _("Disabled"), (unsigned)rc);
    }

    gVBoxAPI.UIMachine.GetBIOSSettings(machine, &bios);
    if (bios) {
        rc = gVBoxAPI.UIBIOSSettings.SetACPIEnabled(bios,
                                                    def->features[VIR_DOMAIN_FEATURE_ACPI] ==
                                                    VIR_TRISTATE_SWITCH_ON);
        if (NS_FAILED(rc)) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("could not change ACPI status to: %s, rc=%08x"),
                           (def->features[VIR_DOMAIN_FEATURE_ACPI] == VIR_TRISTATE_SWITCH_ON)
                           ? _("Enabled") : _("Disabled"), (unsigned)rc);
        }
        rc = gVBoxAPI.UIBIOSSettings.SetIOAPICEnabled(bios,
                                                      def->features[VIR_DOMAIN_FEATURE_APIC] ==
                                                      VIR_TRISTATE_SWITCH_ON);
        if (NS_FAILED(rc)) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("could not change APIC status to: %s, rc=%08x"),
                           (def->features[VIR_DOMAIN_FEATURE_APIC] == VIR_TRISTATE_SWITCH_ON)
                           ? _("Enabled") : _("Disabled"), (unsigned)rc);
        }
        VBOX_RELEASE(bios);
    }

    /* Register the machine before attaching other devices to it */
    rc = gVBoxAPI.UIVirtualBox.RegisterMachine(data->vboxObj, machine);
    if (NS_FAILED(rc)) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("could not define a domain, rc=%08x"), (unsigned)rc);
        goto cleanup;
    }

    /* Get the uuid of the machine, currently it is immutable
     * object so open a session to it and get it back, so that
     * you can make changes to the machine setting
     */
    gVBoxAPI.UIMachine.GetId(machine, &mchiid);
    gVBoxAPI.UISession.Open(data, &mchiid, machine);
    gVBoxAPI.UISession.GetMachine(data->vboxSession, &machine);

    vboxSetBootDeviceOrder(def, data, machine);
    vboxAttachDrives(def, data, machine);
    vboxAttachSound(def, machine);
    vboxAttachNetwork(def, data, machine);
    vboxAttachSerial(def, data, machine);
    vboxAttachParallel(def, data, machine);
    vboxAttachVideo(def, machine);
    vboxAttachDisplay(def, data, machine);
    vboxAttachUSB(def, data, machine);
    vboxAttachSharedFolder(def, data, machine);

    /* Save the machine settings made till now and close the
     * session. also free up the mchiid variable used.
     */
    rc = gVBoxAPI.UIMachine.SaveSettings(machine);
    if (NS_FAILED(rc)) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("failed no saving settings, rc=%08x"), (unsigned)rc);
        goto cleanup;
    }

    gVBoxAPI.UISession.Close(data->vboxSession);
    vboxIIDUnalloc(&mchiid);

    ret = virGetDomain(conn, def->name, def->uuid);
    VBOX_RELEASE(machine);

    virDomainDefFree(def);

    return ret;

 cleanup:
    VBOX_RELEASE(machine);
    virDomainDefFree(def);
    return NULL;
}

static void
detachDevices_common(vboxGlobalData *data, vboxIIDUnion *iidu)
{
    /* Block for checking if HDD's are attched to VM.
     * considering just IDE bus for now. Also skipped
     * chanel=1 and device=0 (Secondary Master) as currenlty
     * it is allocated to CD/DVD Drive by default.
     *
     * Only do this for VirtualBox 3.x and before. Since
     * VirtualBox 4.0 the Unregister method can do this for use.
     */
    IMachine *machine = NULL;
    PRUnichar *hddcnameUtf16 = NULL;
    nsresult rc;
    char *hddcname;

    if (!gVBoxAPI.detachDevicesExplicitly)
        VIR_WARN("This function may not work in current vbox version");

    ignore_value(VIR_STRDUP(hddcname, "IDE"));
    VBOX_UTF8_TO_UTF16(hddcname, &hddcnameUtf16);
    VIR_FREE(hddcname);

    /* Open a Session for the machine */
    rc = gVBoxAPI.UISession.Open(data, iidu, machine);
    if (NS_SUCCEEDED(rc)) {
        rc = gVBoxAPI.UISession.GetMachine(data->vboxSession, &machine);
        if (NS_SUCCEEDED(rc) && machine) {
            gVBoxAPI.detachDevices(data, machine, hddcnameUtf16);
            gVBoxAPI.UIMachine.SaveSettings(machine);
        }
        gVBoxAPI.UISession.Close(data->vboxSession);
    }
    VBOX_UTF16_FREE(hddcnameUtf16);
}

int vboxDomainUndefineFlags(virDomainPtr dom, unsigned int flags)
{
    VBOX_OBJECT_CHECK(dom->conn, int, -1);
    IMachine *machine    = NULL;
    vboxIIDUnion iid;
    nsresult rc;

    gVBoxAPI.UIID.vboxIIDInitialize(&iid);
    /* No managed save, so we explicitly reject
     * VIR_DOMAIN_UNDEFINE_MANAGED_SAVE.  No snapshot metadata for
     * VBox, so we can trivially ignore that flag.  */
    virCheckFlags(VIR_DOMAIN_UNDEFINE_SNAPSHOTS_METADATA, -1);
    vboxIIDFromUUID(&iid, dom->uuid);
    if (gVBoxAPI.detachDevicesExplicitly)
        detachDevices_common(data, &iid);
    rc = gVBoxAPI.unregisterMachine(data, &iid, &machine);

    DEBUGIID("UUID of machine being undefined", &iid);

    if (NS_SUCCEEDED(rc)) {
        gVBoxAPI.deleteConfig(machine);
        ret = 0;
    } else {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("could not delete the domain, rc=%08x"), (unsigned)rc);
    }

    vboxIIDUnalloc(&iid);
    VBOX_RELEASE(machine);

    return ret;
}

static int
vboxStartMachine(virDomainPtr dom, int maxDomID, IMachine *machine, vboxIIDUnion *iid)
{
    VBOX_OBJECT_CHECK(dom->conn, int, -1);
    int vrdpPresent              = 0;
    int sdlPresent               = 0;
    int guiPresent               = 0;
    char *guiDisplay             = NULL;
    char *sdlDisplay             = NULL;
    PRUnichar *keyTypeUtf16      = NULL;
    PRUnichar *valueTypeUtf16    = NULL;
    char      *valueTypeUtf8     = NULL;
    PRUnichar *keyDislpayUtf16   = NULL;
    PRUnichar *valueDisplayUtf16 = NULL;
    char      *valueDisplayUtf8  = NULL;
    IProgress *progress          = NULL;
    PRUnichar *env               = NULL;
    PRUnichar *sessionType       = NULL;
    nsresult rc;

    VBOX_UTF8_TO_UTF16("FRONTEND/Type", &keyTypeUtf16);
    gVBoxAPI.UIMachine.GetExtraData(machine, keyTypeUtf16, &valueTypeUtf16);
    VBOX_UTF16_FREE(keyTypeUtf16);

    if (valueTypeUtf16) {
        VBOX_UTF16_TO_UTF8(valueTypeUtf16, &valueTypeUtf8);
        VBOX_UTF16_FREE(valueTypeUtf16);

        if (STREQ(valueTypeUtf8, "sdl") || STREQ(valueTypeUtf8, "gui")) {

            VBOX_UTF8_TO_UTF16("FRONTEND/Display", &keyDislpayUtf16);
            gVBoxAPI.UIMachine.GetExtraData(machine, keyDislpayUtf16,
                                            &valueDisplayUtf16);
            VBOX_UTF16_FREE(keyDislpayUtf16);

            if (valueDisplayUtf16) {
                VBOX_UTF16_TO_UTF8(valueDisplayUtf16, &valueDisplayUtf8);
                VBOX_UTF16_FREE(valueDisplayUtf16);

                if (strlen(valueDisplayUtf8) <= 0)
                    VBOX_UTF8_FREE(valueDisplayUtf8);
            }

            if (STREQ(valueTypeUtf8, "sdl")) {
                sdlPresent = 1;
                if (VIR_STRDUP(sdlDisplay, valueDisplayUtf8) < 0) {
                    /* just don't go to cleanup yet as it is ok to have
                     * sdlDisplay as NULL and we check it below if it
                     * exist and then only use it there
                     */
                }
            }

            if (STREQ(valueTypeUtf8, "gui")) {
                guiPresent = 1;
                if (VIR_STRDUP(guiDisplay, valueDisplayUtf8) < 0) {
                    /* just don't go to cleanup yet as it is ok to have
                     * guiDisplay as NULL and we check it below if it
                     * exist and then only use it there
                     */
                }
            }
        }

        if (STREQ(valueTypeUtf8, "vrdp")) {
            vrdpPresent = 1;
        }

        if (!vrdpPresent && !sdlPresent && !guiPresent) {
            /* if nothing is selected it means either the machine xml
             * file is really old or some values are missing so fallback
             */
            guiPresent = 1;
        }

        VBOX_UTF8_FREE(valueTypeUtf8);

    } else {
        guiPresent = 1;
    }
    VBOX_UTF8_FREE(valueDisplayUtf8);

    if (guiPresent) {
        if (guiDisplay) {
            char *displayutf8;
            if (virAsprintf(&displayutf8, "DISPLAY=%s", guiDisplay) >= 0) {
                VBOX_UTF8_TO_UTF16(displayutf8, &env);
                VIR_FREE(displayutf8);
            }
            VIR_FREE(guiDisplay);
        }

        VBOX_UTF8_TO_UTF16("gui", &sessionType);
    }

    if (sdlPresent) {
        if (sdlDisplay) {
            char *displayutf8;
            if (virAsprintf(&displayutf8, "DISPLAY=%s", sdlDisplay) >= 0) {
                VBOX_UTF8_TO_UTF16(displayutf8, &env);
                VIR_FREE(displayutf8);
            }
            VIR_FREE(sdlDisplay);
        }

        VBOX_UTF8_TO_UTF16("sdl", &sessionType);
    }

    if (vrdpPresent) {
        VBOX_UTF8_TO_UTF16("vrdp", &sessionType);
    }

    rc = gVBoxAPI.UIMachine.LaunchVMProcess(data, machine, iid,
                                            sessionType, env,
                                            &progress);

    if (NS_FAILED(rc)) {
        virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                       _("OpenRemoteSession/LaunchVMProcess failed, domain can't be started"));
        ret = -1;
    } else {
        PRBool completed = 0;
        resultCodeUnion resultCode;

        gVBoxAPI.UIProgress.WaitForCompletion(progress, -1);
        rc = gVBoxAPI.UIProgress.GetCompleted(progress, &completed);
        if (NS_FAILED(rc)) {
            /* error */
            ret = -1;
        }
        gVBoxAPI.UIProgress.GetResultCode(progress, &resultCode);
        if (RC_FAILED(resultCode)) {
            /* error */
            ret = -1;
        } else {
            /* all ok set the domid */
            dom->id = maxDomID + 1;
            ret = 0;
        }
    }

    VBOX_RELEASE(progress);

    gVBoxAPI.UISession.Close(data->vboxSession);

    VBOX_UTF16_FREE(env);
    VBOX_UTF16_FREE(sessionType);

    return ret;
}

int vboxDomainCreateWithFlags(virDomainPtr dom, unsigned int flags)
{
    VBOX_OBJECT_CHECK(dom->conn, int, -1);
    vboxArray machines = VBOX_ARRAY_INITIALIZER;
    unsigned char uuid[VIR_UUID_BUFLEN] = {0};
    nsresult rc;
    size_t i = 0;

    virCheckFlags(0, -1);

    if (!dom->name) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Error while reading the domain name"));
        goto cleanup;
    }

    rc = gVBoxAPI.UArray.vboxArrayGet(&machines, data->vboxObj, ARRAY_GET_MACHINES);
    if (NS_FAILED(rc)) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Could not get list of machines, rc=%08x"), (unsigned)rc);
        goto cleanup;
    }

    for (i = 0; i < machines.count; ++i) {
        IMachine *machine = machines.items[i];
        PRBool isAccessible = PR_FALSE;

        if (!machine)
            continue;

        gVBoxAPI.UIMachine.GetAccessible(machine, &isAccessible);
        if (isAccessible) {
            vboxIIDUnion iid;

            VBOX_IID_INITIALIZE(&iid);

            rc = gVBoxAPI.UIMachine.GetId(machine, &iid);
            if (NS_FAILED(rc))
                continue;
            vboxIIDToUUID(&iid, uuid);

            if (memcmp(dom->uuid, uuid, VIR_UUID_BUFLEN) == 0) {
                PRUint32 state;
                gVBoxAPI.UIMachine.GetState(machine, &state);

                if (gVBoxAPI.machineStateChecker.NotStart(state)) {
                    ret = vboxStartMachine(dom, i, machine, &iid);
                } else {
                    virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                                   _("machine is not in "
                                     "poweroff|saved|aborted state, so "
                                     "couldn't start it"));
                    ret = -1;
                }
            }
            vboxIIDUnalloc(&iid);
            if (ret != -1)
                break;
        }
    }

    /* Do the cleanup and take care you dont leak any memory */
    gVBoxAPI.UArray.vboxArrayRelease(&machines);

 cleanup:
    return ret;
}

int vboxDomainCreate(virDomainPtr dom)
{
    return vboxDomainCreateWithFlags(dom, 0);
}

virDomainPtr vboxDomainCreateXML(virConnectPtr conn, const char *xml,
                                 unsigned int flags)
{
    /* VirtualBox currently doesn't have support for running
     * virtual machines without actually defining them and thus
     * for time being just define new machine and start it.
     *
     * TODO: After the appropriate API's are added in VirtualBox
     * change this behaviour to the expected one.
     */

    virDomainPtr dom;

    virCheckFlags(0, NULL);

    dom = vboxDomainDefineXML(conn, xml);
    if (dom == NULL)
        return NULL;

    if (vboxDomainCreate(dom) < 0) {
        vboxDomainUndefineFlags(dom, 0);
        virObjectUnref(dom);
        return NULL;
    }

    return dom;
}

int vboxDomainIsActive(virDomainPtr dom)
{
    VBOX_OBJECT_CHECK(dom->conn, int, -1);
    vboxArray machines = VBOX_ARRAY_INITIALIZER;
    vboxIIDUnion iid;
    char *machineNameUtf8  = NULL;
    PRUnichar *machineNameUtf16 = NULL;
    unsigned char uuid[VIR_UUID_BUFLEN];
    size_t i;
    bool matched = false;
    nsresult rc;

    VBOX_IID_INITIALIZE(&iid);
    rc = gVBoxAPI.UArray.vboxArrayGet(&machines, data->vboxObj, ARRAY_GET_MACHINES);
    if (NS_FAILED(rc)) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Could not get list of machines, rc=%08x"), (unsigned)rc);
        return ret;
    }

    for (i = 0; i < machines.count; ++i) {
        IMachine *machine = machines.items[i];
        PRBool isAccessible = PR_FALSE;

        if (!machine)
            continue;

        gVBoxAPI.UIMachine.GetAccessible(machine, &isAccessible);
        if (!isAccessible)
            continue;

        gVBoxAPI.UIMachine.GetId(machine, &iid);
        if (NS_FAILED(rc))
            continue;
        vboxIIDToUUID(&iid, uuid);
        vboxIIDUnalloc(&iid);

        if (memcmp(dom->uuid, uuid, VIR_UUID_BUFLEN) == 0) {

            PRUint32 state;

            matched = true;

            gVBoxAPI.UIMachine.GetName(machine, &machineNameUtf16);
            VBOX_UTF16_TO_UTF8(machineNameUtf16, &machineNameUtf8);

            gVBoxAPI.UIMachine.GetState(machine, &state);

            if (gVBoxAPI.machineStateChecker.Online(state))
                ret = 1;
            else
                ret = 0;
        }

        if (matched)
            break;
    }

    /* Do the cleanup and take care you dont leak any memory */
    VBOX_UTF8_FREE(machineNameUtf8);
    VBOX_COM_UNALLOC_MEM(machineNameUtf16);
    gVBoxAPI.UArray.vboxArrayRelease(&machines);

    return ret;
}

int vboxDomainIsPersistent(virDomainPtr dom)
{
    /* All domains are persistent.  However, we do want to check for
     * existence. */
    VBOX_OBJECT_CHECK(dom->conn, int, -1);
    vboxIIDUnion iid;
    IMachine *machine = NULL;

    if (openSessionForMachine(data, dom->uuid, &iid, &machine, false) < 0)
        goto cleanup;

    ret = 1;

 cleanup:
    VBOX_RELEASE(machine);
    vboxIIDUnalloc(&iid);
    return ret;
}

int vboxDomainIsUpdated(virDomainPtr dom)
{
    /* VBox domains never have a persistent state that differs from
     * current state.  However, we do want to check for existence.  */
    VBOX_OBJECT_CHECK(dom->conn, int, -1);
    vboxIIDUnion iid;
    IMachine *machine = NULL;

    if (openSessionForMachine(data, dom->uuid, &iid, &machine, false) < 0)
        goto cleanup;

    ret = 0;

 cleanup:
    VBOX_RELEASE(machine);
    vboxIIDUnalloc(&iid);
    return ret;
}

int vboxDomainSuspend(virDomainPtr dom)
{
    VBOX_OBJECT_CHECK(dom->conn, int, -1);
    IMachine *machine    = NULL;
    vboxIIDUnion iid;
    IConsole *console    = NULL;
    PRBool isAccessible  = PR_FALSE;
    PRUint32 state;

    if (openSessionForMachine(data, dom->uuid, &iid, &machine, false) < 0)
        goto cleanup;

    if (!machine)
        goto cleanup;

    gVBoxAPI.UIMachine.GetAccessible(machine, &isAccessible);
    if (!isAccessible)
        goto cleanup;

    gVBoxAPI.UIMachine.GetState(machine, &state);

    if (gVBoxAPI.machineStateChecker.Running(state)) {
        /* set state pause */
        gVBoxAPI.UISession.OpenExisting(data, &iid, machine);
        gVBoxAPI.UISession.GetConsole(data->vboxSession, &console);
        if (console) {
            gVBoxAPI.UIConsole.Pause(console);
            VBOX_RELEASE(console);
            ret = 0;
        } else {
            virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                           _("error while suspending the domain"));
            goto cleanup;
        }
        gVBoxAPI.UISession.Close(data->vboxSession);
    } else {
        virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                       _("machine not in running state to suspend it"));
        goto cleanup;
    }

 cleanup:
    VBOX_RELEASE(machine);
    vboxIIDUnalloc(&iid);
    return ret;
}

int vboxDomainResume(virDomainPtr dom)
{
    VBOX_OBJECT_CHECK(dom->conn, int, -1);
    IMachine *machine    = NULL;
    vboxIIDUnion iid;
    IConsole *console    = NULL;
    PRUint32 state;
    PRBool isAccessible = PR_FALSE;

    if (openSessionForMachine(data, dom->uuid, &iid, &machine, false) < 0)
        goto cleanup;

    if (!machine)
        goto cleanup;

    gVBoxAPI.UIMachine.GetAccessible(machine, &isAccessible);
    if (!isAccessible)
        goto cleanup;

    gVBoxAPI.UIMachine.GetState(machine, &state);

    if (gVBoxAPI.machineStateChecker.Paused(state)) {
        /* resume the machine here */
        gVBoxAPI.UISession.OpenExisting(data, &iid, machine);
        gVBoxAPI.UISession.GetConsole(data->vboxSession, &console);
        if (console) {
            gVBoxAPI.UIConsole.Resume(console);
            VBOX_RELEASE(console);
            ret = 0;
        } else {
            virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                           _("error while resuming the domain"));
            goto cleanup;
        }
        gVBoxAPI.UISession.Close(data->vboxSession);
    } else {
        virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                       _("machine not paused, so can't resume it"));
        goto cleanup;
    }

 cleanup:
    VBOX_RELEASE(machine);
    vboxIIDUnalloc(&iid);
    return ret;
}

int vboxDomainShutdownFlags(virDomainPtr dom, unsigned int flags)
{
    VBOX_OBJECT_CHECK(dom->conn, int, -1);
    IMachine *machine    = NULL;
    vboxIIDUnion iid;
    IConsole *console    = NULL;
    PRUint32 state;
    PRBool isAccessible  = PR_FALSE;

    virCheckFlags(0, -1);

    if (openSessionForMachine(data, dom->uuid, &iid, &machine, false) < 0)
        goto cleanup;

    if (!machine)
        goto cleanup;

    gVBoxAPI.UIMachine.GetAccessible(machine, &isAccessible);
    if (!isAccessible)
        goto cleanup;

    gVBoxAPI.UIMachine.GetState(machine, &state);

    if (gVBoxAPI.machineStateChecker.Paused(state)) {
        virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                       _("machine paused, so can't power it down"));
        goto cleanup;
    } else if (gVBoxAPI.machineStateChecker.PoweredOff(state)) {
        virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                       _("machine already powered down"));
        goto cleanup;
    }

    gVBoxAPI.UISession.OpenExisting(data, &iid, machine);
    gVBoxAPI.UISession.GetConsole(data->vboxSession, &console);
    if (console) {
        gVBoxAPI.UIConsole.PowerButton(console);
        VBOX_RELEASE(console);
        ret = 0;
    }
    gVBoxAPI.UISession.Close(data->vboxSession);

 cleanup:
    VBOX_RELEASE(machine);
    vboxIIDUnalloc(&iid);
    return ret;
}

int vboxDomainShutdown(virDomainPtr dom)
{
    return vboxDomainShutdownFlags(dom, 0);
}

int vboxDomainReboot(virDomainPtr dom, unsigned int flags)
{
    VBOX_OBJECT_CHECK(dom->conn, int, -1);
    IMachine *machine    = NULL;
    vboxIIDUnion iid;
    IConsole *console    = NULL;
    PRUint32 state;
    PRBool isAccessible  = PR_FALSE;

    virCheckFlags(0, -1);

    if (openSessionForMachine(data, dom->uuid, &iid, &machine, false) < 0)
        goto cleanup;

    if (!machine)
        goto cleanup;

    gVBoxAPI.UIMachine.GetAccessible(machine, &isAccessible);
    if (!isAccessible)
        goto cleanup;

    gVBoxAPI.UIMachine.GetState(machine, &state);

    if (gVBoxAPI.machineStateChecker.Running(state)) {
        gVBoxAPI.UISession.OpenExisting(data, &iid, machine);
        gVBoxAPI.UISession.GetConsole(data->vboxSession, &console);
        if (console) {
            gVBoxAPI.UIConsole.Reset(console);
            VBOX_RELEASE(console);
            ret = 0;
        }
        gVBoxAPI.UISession.Close(data->vboxSession);
    } else {
        virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                       _("machine not running, so can't reboot it"));
        goto cleanup;
    }

 cleanup:
    VBOX_RELEASE(machine);
    vboxIIDUnalloc(&iid);
    return ret;
}

int vboxDomainDestroyFlags(virDomainPtr dom, unsigned int flags)
{
    VBOX_OBJECT_CHECK(dom->conn, int, -1);
    IMachine *machine    = NULL;
    vboxIIDUnion iid;
    IConsole *console    = NULL;
    PRUint32 state;
    PRBool isAccessible  = PR_FALSE;

    virCheckFlags(0, -1);

    if (openSessionForMachine(data, dom->uuid, &iid, &machine, false) < 0)
        goto cleanup;

    if (!machine)
        goto cleanup;

    gVBoxAPI.UIMachine.GetAccessible(machine, &isAccessible);
    if (!isAccessible)
        goto cleanup;

    gVBoxAPI.UIMachine.GetState(machine, &state);

    if (gVBoxAPI.machineStateChecker.PoweredOff(state)) {
        virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                       _("machine already powered down"));
        goto cleanup;
    }

    gVBoxAPI.UISession.OpenExisting(data, &iid, machine);
    gVBoxAPI.UISession.GetConsole(data->vboxSession, &console);
    if (console) {
        gVBoxAPI.UIConsole.PowerDown(console);
        VBOX_RELEASE(console);
        dom->id = -1;
        ret = 0;
    }
    gVBoxAPI.UISession.Close(data->vboxSession);

 cleanup:
    VBOX_RELEASE(machine);
    vboxIIDUnalloc(&iid);
    return ret;
}

int vboxDomainDestroy(virDomainPtr dom)
{
    return vboxDomainDestroyFlags(dom, 0);
}

char *vboxDomainGetOSType(virDomainPtr dom ATTRIBUTE_UNUSED) {
    /* Returning "hvm" always as suggested on list, cause
     * this functions seems to be badly named and it
     * is supposed to pass the ABI name and not the domain
     * operating system driver as I had imagined ;)
     */
    char *osType;

    ignore_value(VIR_STRDUP(osType, "hvm"));
    return osType;
}

int vboxDomainSetMemory(virDomainPtr dom, unsigned long memory)
{
    VBOX_OBJECT_CHECK(dom->conn, int, -1);
    IMachine *machine    = NULL;
    vboxIIDUnion iid;
    PRUint32 state;
    PRBool isAccessible  = PR_FALSE;
    nsresult rc;

    if (openSessionForMachine(data, dom->uuid, &iid, &machine, false) < 0)
        goto cleanup;

    if (!machine)
        goto cleanup;

    gVBoxAPI.UIMachine.GetAccessible(machine, &isAccessible);
    if (!isAccessible)
        goto cleanup;

    gVBoxAPI.UIMachine.GetState(machine, &state);

    if (!gVBoxAPI.machineStateChecker.PoweredOff(state)) {
        virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                       _("memory size can't be changed unless domain is powered down"));
        goto cleanup;
    }

    rc = gVBoxAPI.UISession.Open(data, &iid, machine);
    if (NS_FAILED(rc))
        goto cleanup;

    rc = gVBoxAPI.UISession.GetMachine(data->vboxSession, &machine);
    if (NS_SUCCEEDED(rc) && machine) {

        rc = gVBoxAPI.UIMachine.SetMemorySize(machine,
                                              VIR_DIV_UP(memory, 1024));
        if (NS_SUCCEEDED(rc)) {
            gVBoxAPI.UIMachine.SaveSettings(machine);
            ret = 0;
        } else {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("could not set the memory size of the "
                             "domain to: %lu Kb, rc=%08x"),
                           memory, (unsigned)rc);
        }
    }
    gVBoxAPI.UISession.Close(data->vboxSession);

 cleanup:
    VBOX_RELEASE(machine);
    vboxIIDUnalloc(&iid);
    return ret;
}

int vboxDomainGetInfo(virDomainPtr dom, virDomainInfoPtr info)
{
    VBOX_OBJECT_CHECK(dom->conn, int, -1);
    vboxArray machines = VBOX_ARRAY_INITIALIZER;
    char *machineName    = NULL;
    PRUnichar *machineNameUtf16 = NULL;
    nsresult rc;
    size_t i = 0;

    rc = gVBoxAPI.UArray.vboxArrayGet(&machines, data->vboxObj, ARRAY_GET_MACHINES);
    if (NS_FAILED(rc)) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Could not get list of machines, rc=%08x"), (unsigned)rc);
        goto cleanup;
    }

    info->nrVirtCpu = 0;
    for (i = 0; i < machines.count; ++i) {
        IMachine *machine = machines.items[i];
        PRBool isAccessible = PR_FALSE;

        if (!machine)
            continue;

        gVBoxAPI.UIMachine.GetAccessible(machine, &isAccessible);
        if (!isAccessible)
            continue;

        gVBoxAPI.UIMachine.GetName(machine, &machineNameUtf16);
        VBOX_UTF16_TO_UTF8(machineNameUtf16, &machineName);

        if (STREQ(dom->name, machineName)) {
            /* Get the Machine State (also match it with
            * virDomainState). Get the Machine memory and
            * for time being set max_balloon and cur_balloon to same
            * Also since there is no direct way of checking
            * the cputime required (one condition being the
            * VM is remote), return zero for cputime. Get the
            * number of CPU.
            */
            PRUint32 CPUCount   = 0;
            PRUint32 memorySize = 0;
            PRUint32 state;
            PRUint32 maxMemorySize = 4 * 1024;
            ISystemProperties *systemProperties = NULL;

            gVBoxAPI.UIVirtualBox.GetSystemProperties(data->vboxObj, &systemProperties);
            if (systemProperties) {
                gVBoxAPI.UISystemProperties.GetMaxGuestRAM(systemProperties, &maxMemorySize);
                VBOX_RELEASE(systemProperties);
                systemProperties = NULL;
            }

            gVBoxAPI.UIMachine.GetCPUCount(machine, &CPUCount);
            gVBoxAPI.UIMachine.GetMemorySize(machine, &memorySize);
            gVBoxAPI.UIMachine.GetState(machine, &state);

            info->cpuTime = 0;
            info->nrVirtCpu = CPUCount;
            info->memory = memorySize * 1024;
            info->maxMem = maxMemorySize * 1024;
            info->state = gVBoxAPI.vboxConvertState(state);

            ret = 0;
        }

        VBOX_UTF8_FREE(machineName);
        VBOX_COM_UNALLOC_MEM(machineNameUtf16);
        if (info->nrVirtCpu)
            break;

    }

    gVBoxAPI.UArray.vboxArrayRelease(&machines);

 cleanup:
    return ret;
}

int vboxDomainGetState(virDomainPtr dom, int *state,
                       int *reason, unsigned int flags)
{
    VBOX_OBJECT_CHECK(dom->conn, int, -1);
    vboxIIDUnion domiid;
    IMachine *machine = NULL;
    PRUint32 mstate;

    virCheckFlags(0, -1);

    if (openSessionForMachine(data, dom->uuid, &domiid, &machine, false) < 0)
        goto cleanup;

    gVBoxAPI.UIMachine.GetState(machine, &mstate);

    *state = gVBoxAPI.vboxConvertState(mstate);

    if (reason)
        *reason = 0;

    ret = 0;

 cleanup:
    vboxIIDUnalloc(&domiid);
    return ret;
}

int vboxDomainSetVcpusFlags(virDomainPtr dom, unsigned int nvcpus,
                            unsigned int flags)
{
    VBOX_OBJECT_CHECK(dom->conn, int, -1);
    IMachine *machine    = NULL;
    vboxIIDUnion iid;
    PRUint32  CPUCount   = nvcpus;
    nsresult rc;

    if (flags != VIR_DOMAIN_AFFECT_LIVE) {
        virReportError(VIR_ERR_INVALID_ARG, _("unsupported flags: (0x%x)"), flags);
        return -1;
    }

    if (openSessionForMachine(data, dom->uuid, &iid, &machine, true) < 0)
        return -1;

    rc = gVBoxAPI.UISession.Open(data, &iid, machine);
    if (NS_SUCCEEDED(rc)) {
        gVBoxAPI.UISession.GetMachine(data->vboxSession, &machine);
        if (machine) {
            rc = gVBoxAPI.UIMachine.SetCPUCount(machine, CPUCount);
            if (NS_SUCCEEDED(rc)) {
                gVBoxAPI.UIMachine.SaveSettings(machine);
                ret = 0;
            } else {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("could not set the number of cpus of the domain "
                                 "to: %u, rc=%08x"),
                               CPUCount, (unsigned)rc);
            }
            VBOX_RELEASE(machine);
        } else {
            virReportError(VIR_ERR_NO_DOMAIN,
                           _("no domain with matching id %d"), dom->id);
        }
    } else {
        virReportError(VIR_ERR_NO_DOMAIN,
                       _("can't open session to the domain with id %d"), dom->id);
    }
    gVBoxAPI.UISession.Close(data->vboxSession);

    vboxIIDUnalloc(&iid);
    return ret;
}

int vboxDomainSetVcpus(virDomainPtr dom, unsigned int nvcpus)
{
    return vboxDomainSetVcpusFlags(dom, nvcpus, VIR_DOMAIN_AFFECT_LIVE);
}

int vboxDomainGetVcpusFlags(virDomainPtr dom, unsigned int flags)
{
    VBOX_OBJECT_CHECK(dom->conn, int, -1);
    ISystemProperties *systemProperties = NULL;
    PRUint32 maxCPUCount = 0;

    if (flags != (VIR_DOMAIN_AFFECT_LIVE | VIR_DOMAIN_VCPU_MAXIMUM)) {
        virReportError(VIR_ERR_INVALID_ARG, _("unsupported flags: (0x%x)"), flags);
        return -1;
    }

    /* Currently every domain supports the same number of max cpus
     * as that supported by vbox and thus take it directly from
     * the systemproperties.
     */

    gVBoxAPI.UIVirtualBox.GetSystemProperties(data->vboxObj, &systemProperties);
    if (systemProperties) {
        gVBoxAPI.UISystemProperties.GetMaxGuestCPUCount(systemProperties, &maxCPUCount);
        VBOX_RELEASE(systemProperties);
    }

    if (maxCPUCount > 0)
        ret = maxCPUCount;

    return ret;
}

int vboxDomainGetMaxVcpus(virDomainPtr dom)
{
    return vboxDomainGetVcpusFlags(dom, (VIR_DOMAIN_AFFECT_LIVE |
                                         VIR_DOMAIN_VCPU_MAXIMUM));
}

static void
vboxHostDeviceGetXMLDesc(vboxGlobalData *data, virDomainDefPtr def, IMachine *machine)
{
    IUSBCommon *USBCommon = NULL;
    PRBool enabled = PR_FALSE;
    vboxArray deviceFilters = VBOX_ARRAY_INITIALIZER;
    size_t i;
    PRUint32 USBFilterCount = 0;

    def->nhostdevs = 0;

    gVBoxAPI.UIMachine.GetUSBCommon(machine, &USBCommon);
    if (!USBCommon)
        return;

    gVBoxAPI.UIUSBCommon.GetEnabled(USBCommon, &enabled);
    if (!enabled)
        goto release_controller;

    gVBoxAPI.UArray.vboxArrayGet(&deviceFilters, USBCommon,
                                 gVBoxAPI.UArray.handleUSBGetDeviceFilters(USBCommon));

    if (deviceFilters.count <= 0)
        goto release_filters;

    /* check if the filters are active and then only
     * alloc mem and set def->nhostdevs
     */

    for (i = 0; i < deviceFilters.count; i++) {
        PRBool active = PR_FALSE;
        IUSBDeviceFilter *deviceFilter = deviceFilters.items[i];

        gVBoxAPI.UIUSBDeviceFilter.GetActive(deviceFilter, &active);
        if (active) {
            def->nhostdevs++;
        }
    }

    if (def->nhostdevs == 0)
        goto release_filters;

    /* Alloc mem needed for the filters now */
    if (VIR_ALLOC_N(def->hostdevs, def->nhostdevs) < 0)
        goto release_filters;

    for (i = 0; i < def->nhostdevs; i++) {
        def->hostdevs[i] = virDomainHostdevDefAlloc();
        if (!def->hostdevs[i])
            goto release_hostdevs;
    }

    for (i = 0; i < deviceFilters.count; i++) {
        PRBool active                  = PR_FALSE;
        IUSBDeviceFilter *deviceFilter = deviceFilters.items[i];
        PRUnichar *vendorIdUtf16       = NULL;
        char *vendorIdUtf8             = NULL;
        unsigned vendorId              = 0;
        PRUnichar *productIdUtf16      = NULL;
        char *productIdUtf8            = NULL;
        unsigned productId             = 0;
        char *endptr                   = NULL;

        gVBoxAPI.UIUSBDeviceFilter.GetActive(deviceFilter, &active);
        if (!active)
            continue;

        def->hostdevs[USBFilterCount]->mode =
            VIR_DOMAIN_HOSTDEV_MODE_SUBSYS;
        def->hostdevs[USBFilterCount]->source.subsys.type =
            VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_USB;

        gVBoxAPI.UIUSBDeviceFilter.GetVendorId(deviceFilter, &vendorIdUtf16);
        gVBoxAPI.UIUSBDeviceFilter.GetProductId(deviceFilter, &productIdUtf16);

        VBOX_UTF16_TO_UTF8(vendorIdUtf16, &vendorIdUtf8);
        VBOX_UTF16_TO_UTF8(productIdUtf16, &productIdUtf8);

        ignore_value(virStrToLong_ui(vendorIdUtf8, &endptr, 16, &vendorId));
        ignore_value(virStrToLong_ui(productIdUtf8, &endptr, 16, &productId));

        def->hostdevs[USBFilterCount]->source.subsys.u.usb.vendor  = vendorId;
        def->hostdevs[USBFilterCount]->source.subsys.u.usb.product = productId;

        VBOX_UTF16_FREE(vendorIdUtf16);
        VBOX_UTF8_FREE(vendorIdUtf8);

        VBOX_UTF16_FREE(productIdUtf16);
        VBOX_UTF8_FREE(productIdUtf8);

        USBFilterCount++;
    }

 release_filters:
    gVBoxAPI.UArray.vboxArrayRelease(&deviceFilters);
 release_controller:
    VBOX_RELEASE(USBCommon);
    return;

 release_hostdevs:
    for (i = 0; i < def->nhostdevs; i++)
        virDomainHostdevDefFree(def->hostdevs[i]);
    VIR_FREE(def->hostdevs);

    goto release_filters;
}

static void
vboxDumpIDEHDDsNew(virDomainDefPtr def, vboxGlobalData *data, IMachine *machine)
{
    /* dump IDE hdds if present */
    vboxArray mediumAttachments         = VBOX_ARRAY_INITIALIZER;
    bool error = false;
    int diskCount = 0;
    size_t i;
    PRUint32   maxPortPerInst[StorageBus_Floppy + 1] = {};
    PRUint32   maxSlotPerPort[StorageBus_Floppy + 1] = {};

    if (gVBoxAPI.oldMediumInterface)
        VIR_WARN("This function may not work in current vbox version");

    def->ndisks = 0;
    gVBoxAPI.UArray.vboxArrayGet(&mediumAttachments, machine,
                                 gVBoxAPI.UArray.handleMachineGetMediumAttachments(machine));

    /* get the number of attachments */
    for (i = 0; i < mediumAttachments.count; i++) {
        IMediumAttachment *imediumattach = mediumAttachments.items[i];
        if (imediumattach) {
            IMedium *medium = NULL;

            gVBoxAPI.UIMediumAttachment.GetMedium(imediumattach, &medium);
            if (medium) {
                def->ndisks++;
                VBOX_RELEASE(medium);
            }
        }
    }

    /* Allocate mem, if fails return error */
    if (VIR_ALLOC_N(def->disks, def->ndisks) >= 0) {
        for (i = 0; i < def->ndisks; i++) {
            virDomainDiskDefPtr disk = virDomainDiskDefNew();
            if (!disk) {
                error = true;
                break;
            }
            def->disks[i] = disk;
        }
    } else {
        error = true;
    }

    if (!error)
        error = !vboxGetMaxPortSlotValues(data->vboxObj, maxPortPerInst, maxSlotPerPort);

    /* get the attachment details here */
    for (i = 0; i < mediumAttachments.count && diskCount < def->ndisks && !error; i++) {
        IMediumAttachment *imediumattach = mediumAttachments.items[i];
        IStorageController *storageController = NULL;
        PRUnichar *storageControllerName = NULL;
        PRUint32   deviceType     = DeviceType_Null;
        PRUint32   storageBus     = StorageBus_Null;
        PRBool     readOnly       = PR_FALSE;
        IMedium   *medium         = NULL;
        PRUnichar *mediumLocUtf16 = NULL;
        char      *mediumLocUtf8  = NULL;
        PRUint32   deviceInst     = 0;
        PRInt32    devicePort     = 0;
        PRInt32    deviceSlot     = 0;

        if (!imediumattach)
            continue;

        gVBoxAPI.UIMediumAttachment.GetMedium(imediumattach, &medium);
        if (!medium)
            continue;

        gVBoxAPI.UIMediumAttachment.GetController(imediumattach, &storageControllerName);
        if (!storageControllerName) {
            VBOX_RELEASE(medium);
            continue;
        }

        gVBoxAPI.UIMachine.GetStorageControllerByName(machine,
                                                      storageControllerName,
                                                      &storageController);
        VBOX_UTF16_FREE(storageControllerName);
        if (!storageController) {
            VBOX_RELEASE(medium);
            continue;
        }

        gVBoxAPI.UIMedium.GetLocation(medium, &mediumLocUtf16);
        VBOX_UTF16_TO_UTF8(mediumLocUtf16, &mediumLocUtf8);
        VBOX_UTF16_FREE(mediumLocUtf16);
        ignore_value(virDomainDiskSetSource(def->disks[diskCount],
                                            mediumLocUtf8));
        VBOX_UTF8_FREE(mediumLocUtf8);

        if (!virDomainDiskGetSource(def->disks[diskCount])) {
            VBOX_RELEASE(medium);
            VBOX_RELEASE(storageController);
            error = true;
            break;
        }

        gVBoxAPI.UIStorageController.GetBus(storageController, &storageBus);
        if (storageBus == StorageBus_IDE) {
            def->disks[diskCount]->bus = VIR_DOMAIN_DISK_BUS_IDE;
        } else if (storageBus == StorageBus_SATA) {
            def->disks[diskCount]->bus = VIR_DOMAIN_DISK_BUS_SATA;
        } else if (storageBus == StorageBus_SCSI) {
            def->disks[diskCount]->bus = VIR_DOMAIN_DISK_BUS_SCSI;
        } else if (storageBus == StorageBus_Floppy) {
            def->disks[diskCount]->bus = VIR_DOMAIN_DISK_BUS_FDC;
        }

        gVBoxAPI.UIMediumAttachment.GetType(imediumattach, &deviceType);
        if (deviceType == DeviceType_HardDisk)
            def->disks[diskCount]->device = VIR_DOMAIN_DISK_DEVICE_DISK;
        else if (deviceType == DeviceType_Floppy)
            def->disks[diskCount]->device = VIR_DOMAIN_DISK_DEVICE_FLOPPY;
        else if (deviceType == DeviceType_DVD)
            def->disks[diskCount]->device = VIR_DOMAIN_DISK_DEVICE_CDROM;

        gVBoxAPI.UIMediumAttachment.GetPort(imediumattach, &devicePort);
        gVBoxAPI.UIMediumAttachment.GetDevice(imediumattach, &deviceSlot);
        def->disks[diskCount]->dst = vboxGenerateMediumName(storageBus,
                                                            deviceInst,
                                                            devicePort,
                                                            deviceSlot,
                                                            maxPortPerInst,
                                                            maxSlotPerPort);
        if (!def->disks[diskCount]->dst) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Could not generate medium name for the disk "
                             "at: controller instance:%u, port:%d, slot:%d"),
                           deviceInst, devicePort, deviceSlot);
            VBOX_RELEASE(medium);
            VBOX_RELEASE(storageController);
            error = true;
            break;
        }

        gVBoxAPI.UIMedium.GetReadOnly(medium, &readOnly);
        if (readOnly == PR_TRUE)
            def->disks[diskCount]->src->readonly = true;

        virDomainDiskSetType(def->disks[diskCount],
                             VIR_STORAGE_TYPE_FILE);

        VBOX_RELEASE(medium);
        VBOX_RELEASE(storageController);
        diskCount++;
    }

    gVBoxAPI.UArray.vboxArrayRelease(&mediumAttachments);

    /* cleanup on error */
    if (error) {
        for (i = 0; i < def->ndisks; i++) {
            VIR_FREE(def->disks[i]);
        }
        VIR_FREE(def->disks);
        def->ndisks = 0;
    }
}

static void
vboxDumpVideo(virDomainDefPtr def, vboxGlobalData *data ATTRIBUTE_UNUSED,
              IMachine *machine)
{
    /* dump video options vram/2d/3d/directx/etc. */
    /* Currently supports only one graphics card */
    def->nvideos = 1;
    if (VIR_ALLOC_N(def->videos, def->nvideos) >= 0) {
        if (VIR_ALLOC(def->videos[0]) >= 0) {
            /* the default is: vram is 8MB, One monitor, 3dAccel Off */
            PRUint32 VRAMSize          = 8;
            PRUint32 monitorCount      = 1;
            PRBool accelerate3DEnabled = PR_FALSE;
            PRBool accelerate2DEnabled = PR_FALSE;

            gVBoxAPI.UIMachine.GetVRAMSize(machine, &VRAMSize);
            gVBoxAPI.UIMachine.GetMonitorCount(machine, &monitorCount);
            gVBoxAPI.UIMachine.GetAccelerate3DEnabled(machine, &accelerate3DEnabled);
            if (gVBoxAPI.accelerate2DVideo)
                gVBoxAPI.UIMachine.GetAccelerate2DVideoEnabled(machine, &accelerate2DEnabled);

            def->videos[0]->type            = VIR_DOMAIN_VIDEO_TYPE_VBOX;
            def->videos[0]->vram            = VRAMSize * 1024;
            def->videos[0]->heads           = monitorCount;
            if (VIR_ALLOC(def->videos[0]->accel) >= 0) {
                def->videos[0]->accel->support3d = accelerate3DEnabled;
                def->videos[0]->accel->support2d = accelerate2DEnabled;
            }
        }
    }
}

static void
vboxDumpDisplay(virDomainDefPtr def, vboxGlobalData *data, IMachine *machine)
{
    /* dump display options vrdp/gui/sdl */
    int vrdpPresent           = 0;
    int sdlPresent            = 0;
    int guiPresent            = 0;
    int totalPresent          = 0;
    char *guiDisplay          = NULL;
    char *sdlDisplay          = NULL;
    PRUnichar *keyTypeUtf16   = NULL;
    PRUnichar *valueTypeUtf16 = NULL;
    char      *valueTypeUtf8  = NULL;
    IVRDxServer *VRDxServer   = NULL;
    PRBool VRDxEnabled        = PR_FALSE;

    def->ngraphics = 0;

    VBOX_UTF8_TO_UTF16("FRONTEND/Type", &keyTypeUtf16);
    gVBoxAPI.UIMachine.GetExtraData(machine, keyTypeUtf16, &valueTypeUtf16);
    VBOX_UTF16_FREE(keyTypeUtf16);

    if (valueTypeUtf16) {
        VBOX_UTF16_TO_UTF8(valueTypeUtf16, &valueTypeUtf8);
        VBOX_UTF16_FREE(valueTypeUtf16);

        if (STREQ(valueTypeUtf8, "sdl") || STREQ(valueTypeUtf8, "gui")) {
            PRUnichar *keyDislpayUtf16   = NULL;
            PRUnichar *valueDisplayUtf16 = NULL;
            char      *valueDisplayUtf8  = NULL;

            VBOX_UTF8_TO_UTF16("FRONTEND/Display", &keyDislpayUtf16);
            gVBoxAPI.UIMachine.GetExtraData(machine, keyDislpayUtf16, &valueDisplayUtf16);
            VBOX_UTF16_FREE(keyDislpayUtf16);

            if (valueDisplayUtf16) {
                VBOX_UTF16_TO_UTF8(valueDisplayUtf16, &valueDisplayUtf8);
                VBOX_UTF16_FREE(valueDisplayUtf16);

                if (strlen(valueDisplayUtf8) <= 0)
                    VBOX_UTF8_FREE(valueDisplayUtf8);
            }

            if (STREQ(valueTypeUtf8, "sdl")) {
                sdlPresent = 1;
                if (VIR_STRDUP(sdlDisplay, valueDisplayUtf8) < 0) {
                    /* just don't go to cleanup yet as it is ok to have
                     * sdlDisplay as NULL and we check it below if it
                     * exist and then only use it there
                     */
                }
                totalPresent++;
            }

            if (STREQ(valueTypeUtf8, "gui")) {
                guiPresent = 1;
                if (VIR_STRDUP(guiDisplay, valueDisplayUtf8) < 0) {
                    /* just don't go to cleanup yet as it is ok to have
                     * guiDisplay as NULL and we check it below if it
                     * exist and then only use it there
                     */
                }
                totalPresent++;
            }
            VBOX_UTF8_FREE(valueDisplayUtf8);
        }

        if (STREQ(valueTypeUtf8, "vrdp"))
            vrdpPresent = 1;

        VBOX_UTF8_FREE(valueTypeUtf8);
    }

    if ((totalPresent > 0) && (VIR_ALLOC_N(def->graphics, totalPresent) >= 0)) {
        if ((guiPresent) && (VIR_ALLOC(def->graphics[def->ngraphics]) >= 0)) {
            def->graphics[def->ngraphics]->type = VIR_DOMAIN_GRAPHICS_TYPE_DESKTOP;
            if (guiDisplay)
                def->graphics[def->ngraphics]->data.desktop.display = guiDisplay;
            def->ngraphics++;
        }

        if ((sdlPresent) && (VIR_ALLOC(def->graphics[def->ngraphics]) >= 0)) {
            def->graphics[def->ngraphics]->type = VIR_DOMAIN_GRAPHICS_TYPE_SDL;
            if (sdlDisplay)
                def->graphics[def->ngraphics]->data.sdl.display = sdlDisplay;
            def->ngraphics++;
        }
    } else if ((vrdpPresent != 1) && (totalPresent == 0) && (VIR_ALLOC_N(def->graphics, 1) >= 0)) {
        if (VIR_ALLOC(def->graphics[def->ngraphics]) >= 0) {
            const char *tmp;
            def->graphics[def->ngraphics]->type = VIR_DOMAIN_GRAPHICS_TYPE_DESKTOP;
            tmp = virGetEnvBlockSUID("DISPLAY");
            if (VIR_STRDUP(def->graphics[def->ngraphics]->data.desktop.display, tmp) < 0) {
                /* just don't go to cleanup yet as it is ok to have
                 * display as NULL
                 */
            }
            totalPresent++;
            def->ngraphics++;
        }
    }

    gVBoxAPI.UIMachine.GetVRDxServer(machine, &VRDxServer);
    if (VRDxServer) {
        gVBoxAPI.UIVRDxServer.GetEnabled(VRDxServer, &VRDxEnabled);
        if (VRDxEnabled) {

            totalPresent++;

            if ((VIR_REALLOC_N(def->graphics, totalPresent) >= 0) &&
                (VIR_ALLOC(def->graphics[def->ngraphics]) >= 0)) {
                PRUnichar *netAddressUtf16   = NULL;
                char      *netAddressUtf8    = NULL;
                PRBool allowMultiConnection  = PR_FALSE;
                PRBool reuseSingleConnection = PR_FALSE;

                gVBoxAPI.UIVRDxServer.GetPorts(data, VRDxServer, def->graphics[def->ngraphics]);

                def->graphics[def->ngraphics]->type = VIR_DOMAIN_GRAPHICS_TYPE_RDP;

                gVBoxAPI.UIVRDxServer.GetNetAddress(data, VRDxServer, &netAddressUtf16);
                if (netAddressUtf16) {
                    VBOX_UTF16_TO_UTF8(netAddressUtf16, &netAddressUtf8);
                    if (STRNEQ(netAddressUtf8, ""))
                        virDomainGraphicsListenSetAddress(def->graphics[def->ngraphics], 0,
                                                          netAddressUtf8, -1, true);
                    VBOX_UTF16_FREE(netAddressUtf16);
                    VBOX_UTF8_FREE(netAddressUtf8);
                }

                gVBoxAPI.UIVRDxServer.GetAllowMultiConnection(VRDxServer, &allowMultiConnection);
                if (allowMultiConnection) {
                    def->graphics[def->ngraphics]->data.rdp.multiUser = true;
                }

                gVBoxAPI.UIVRDxServer.GetReuseSingleConnection(VRDxServer, &reuseSingleConnection);
                if (reuseSingleConnection) {
                    def->graphics[def->ngraphics]->data.rdp.replaceUser = true;
                }

                def->ngraphics++;
            } else
                virReportOOMError();
        }
        VBOX_RELEASE(VRDxServer);
    }
}

static void
vboxDumpSharedFolders(virDomainDefPtr def, vboxGlobalData *data, IMachine *machine)
{
    /* shared folders */
    vboxArray sharedFolders = VBOX_ARRAY_INITIALIZER;
    size_t i = 0;

    def->nfss = 0;

    gVBoxAPI.UArray.vboxArrayGet(&sharedFolders, machine,
                                 gVBoxAPI.UArray.handleMachineGetSharedFolders(machine));

    if (sharedFolders.count <= 0)
        goto sharedFoldersCleanup;

    if (VIR_ALLOC_N(def->fss, sharedFolders.count) < 0)
        goto sharedFoldersCleanup;

    for (i = 0; i < sharedFolders.count; i++) {
        ISharedFolder *sharedFolder = sharedFolders.items[i];
        PRUnichar *nameUtf16 = NULL;
        char *name = NULL;
        PRUnichar *hostPathUtf16 = NULL;
        char *hostPath = NULL;
        PRBool writable = PR_FALSE;

        if (VIR_ALLOC(def->fss[i]) < 0)
            goto sharedFoldersCleanup;

        def->fss[i]->type = VIR_DOMAIN_FS_TYPE_MOUNT;

        gVBoxAPI.UISharedFolder.GetHostPath(sharedFolder, &hostPathUtf16);
        VBOX_UTF16_TO_UTF8(hostPathUtf16, &hostPath);
        if (VIR_STRDUP(def->fss[i]->src, hostPath) < 0) {
            VBOX_UTF8_FREE(hostPath);
            VBOX_UTF16_FREE(hostPathUtf16);
            goto sharedFoldersCleanup;
        }
        VBOX_UTF8_FREE(hostPath);
        VBOX_UTF16_FREE(hostPathUtf16);

        gVBoxAPI.UISharedFolder.GetName(sharedFolder, &nameUtf16);
        VBOX_UTF16_TO_UTF8(nameUtf16, &name);
        if (VIR_STRDUP(def->fss[i]->dst, name) < 0) {
            VBOX_UTF8_FREE(name);
            VBOX_UTF16_FREE(nameUtf16);
            goto sharedFoldersCleanup;
        }
        VBOX_UTF8_FREE(name);
        VBOX_UTF16_FREE(nameUtf16);

        gVBoxAPI.UISharedFolder.GetWritable(sharedFolder, &writable);
        def->fss[i]->readonly = !writable;

        ++def->nfss;
    }

 sharedFoldersCleanup:
    gVBoxAPI.UArray.vboxArrayRelease(&sharedFolders);
}

static void
vboxDumpNetwork(virDomainDefPtr def, vboxGlobalData *data, IMachine *machine, PRUint32 networkAdapterCount)
{
    PRUint32 netAdpIncCnt = 0;
    size_t i = 0;
    /* dump network cards if present */
    def->nnets = 0;
    /* Get which network cards are enabled */
    for (i = 0; i < networkAdapterCount; i++) {
        INetworkAdapter *adapter = NULL;

        gVBoxAPI.UIMachine.GetNetworkAdapter(machine, i, &adapter);
        if (adapter) {
            PRBool enabled = PR_FALSE;

            gVBoxAPI.UINetworkAdapter.GetEnabled(adapter, &enabled);
            if (enabled) {
                def->nnets++;
            }

            VBOX_RELEASE(adapter);
        }
    }

    /* Allocate memory for the networkcards which are enabled */
    if ((def->nnets > 0) && (VIR_ALLOC_N(def->nets, def->nnets) >= 0)) {
        for (i = 0; i < def->nnets; i++) {
            ignore_value(VIR_ALLOC(def->nets[i]));
        }
    }

    /* Now get the details about the network cards here */
    for (i = 0; netAdpIncCnt < def->nnets && i < networkAdapterCount; i++) {
        INetworkAdapter *adapter = NULL;

        gVBoxAPI.UIMachine.GetNetworkAdapter(machine, i, &adapter);
        if (adapter) {
            PRBool enabled = PR_FALSE;

            gVBoxAPI.UINetworkAdapter.GetEnabled(adapter, &enabled);
            if (enabled) {
                PRUint32 attachmentType    = NetworkAttachmentType_Null;
                PRUint32 adapterType       = NetworkAdapterType_Null;
                PRUnichar *MACAddressUtf16 = NULL;
                char *MACAddress           = NULL;
                char macaddr[VIR_MAC_STRING_BUFLEN] = {0};

                gVBoxAPI.UINetworkAdapter.GetAttachmentType(adapter, &attachmentType);
                if (attachmentType == NetworkAttachmentType_NAT) {

                    def->nets[netAdpIncCnt]->type = VIR_DOMAIN_NET_TYPE_USER;

                } else if (attachmentType == NetworkAttachmentType_Bridged) {
                    PRUnichar *hostIntUtf16 = NULL;
                    char *hostInt           = NULL;

                    def->nets[netAdpIncCnt]->type = VIR_DOMAIN_NET_TYPE_BRIDGE;

                    gVBoxAPI.UINetworkAdapter.GetBridgedInterface(adapter, &hostIntUtf16);

                    VBOX_UTF16_TO_UTF8(hostIntUtf16, &hostInt);
                    ignore_value(VIR_STRDUP(def->nets[netAdpIncCnt]->data.bridge.brname, hostInt));

                    VBOX_UTF8_FREE(hostInt);
                    VBOX_UTF16_FREE(hostIntUtf16);

                } else if (attachmentType == NetworkAttachmentType_Internal) {
                    PRUnichar *intNetUtf16 = NULL;
                    char *intNet           = NULL;

                    def->nets[netAdpIncCnt]->type = VIR_DOMAIN_NET_TYPE_INTERNAL;

                    gVBoxAPI.UINetworkAdapter.GetInternalNetwork(adapter, &intNetUtf16);

                    VBOX_UTF16_TO_UTF8(intNetUtf16, &intNet);
                    ignore_value(VIR_STRDUP(def->nets[netAdpIncCnt]->data.internal.name, intNet));

                    VBOX_UTF8_FREE(intNet);
                    VBOX_UTF16_FREE(intNetUtf16);

                } else if (attachmentType == NetworkAttachmentType_HostOnly) {
                    PRUnichar *hostIntUtf16 = NULL;
                    char *hostInt           = NULL;

                    def->nets[netAdpIncCnt]->type = VIR_DOMAIN_NET_TYPE_NETWORK;

                    gVBoxAPI.UINetworkAdapter.GetHostOnlyInterface(adapter, &hostIntUtf16);

                    VBOX_UTF16_TO_UTF8(hostIntUtf16, &hostInt);
                    ignore_value(VIR_STRDUP(def->nets[netAdpIncCnt]->data.network.name, hostInt));

                    VBOX_UTF8_FREE(hostInt);
                    VBOX_UTF16_FREE(hostIntUtf16);

                } else {
                    /* default to user type i.e. NAT in VirtualBox if this
                     * dump is ever used to create a machine.
                     */
                    def->nets[netAdpIncCnt]->type = VIR_DOMAIN_NET_TYPE_USER;
                }

                gVBoxAPI.UINetworkAdapter.GetAdapterType(adapter, &adapterType);
                if (adapterType == NetworkAdapterType_Am79C970A) {
                    ignore_value(VIR_STRDUP(def->nets[netAdpIncCnt]->model, "Am79C970A"));
                } else if (adapterType == NetworkAdapterType_Am79C973) {
                    ignore_value(VIR_STRDUP(def->nets[netAdpIncCnt]->model, "Am79C973"));
                } else if (adapterType == NetworkAdapterType_I82540EM) {
                    ignore_value(VIR_STRDUP(def->nets[netAdpIncCnt]->model, "82540EM"));
                } else if (adapterType == NetworkAdapterType_I82545EM) {
                    ignore_value(VIR_STRDUP(def->nets[netAdpIncCnt]->model, "82545EM"));
                } else if (adapterType == NetworkAdapterType_I82543GC) {
                    ignore_value(VIR_STRDUP(def->nets[netAdpIncCnt]->model, "82543GC"));
                } else if (gVBoxAPI.APIVersion >= 3000051 &&
                           adapterType == NetworkAdapterType_Virtio) {
                    /* Only vbox 3.1 and later support NetworkAdapterType_Virto */
                    ignore_value(VIR_STRDUP(def->nets[netAdpIncCnt]->model, "virtio"));
                }

                gVBoxAPI.UINetworkAdapter.GetMACAddress(adapter, &MACAddressUtf16);
                VBOX_UTF16_TO_UTF8(MACAddressUtf16, &MACAddress);
                snprintf(macaddr, VIR_MAC_STRING_BUFLEN,
                         "%c%c:%c%c:%c%c:%c%c:%c%c:%c%c",
                         MACAddress[0], MACAddress[1], MACAddress[2], MACAddress[3],
                         MACAddress[4], MACAddress[5], MACAddress[6], MACAddress[7],
                         MACAddress[8], MACAddress[9], MACAddress[10], MACAddress[11]);

                /* XXX some real error handling here some day ... */
                if (virMacAddrParse(macaddr, &def->nets[netAdpIncCnt]->mac) < 0)
                {}

                netAdpIncCnt++;

                VBOX_UTF16_FREE(MACAddressUtf16);
                VBOX_UTF8_FREE(MACAddress);
            }

            VBOX_RELEASE(adapter);
        }
    }
}

static void
vboxDumpAudio(virDomainDefPtr def, vboxGlobalData *data ATTRIBUTE_UNUSED,
              IMachine *machine)
{
    /* dump sound card if active */

    /* Set def->nsounds to one as VirtualBox currently supports
     * only one sound card
     */
    IAudioAdapter *audioAdapter = NULL;

    gVBoxAPI.UIMachine.GetAudioAdapter(machine, &audioAdapter);
    if (audioAdapter) {
        PRBool enabled = PR_FALSE;

        gVBoxAPI.UIAudioAdapter.GetEnabled(audioAdapter, &enabled);
        if (enabled) {
            PRUint32 audioController = AudioControllerType_AC97;

            def->nsounds = 1;
            if (VIR_ALLOC_N(def->sounds, def->nsounds) >= 0) {
                if (VIR_ALLOC(def->sounds[0]) >= 0) {
                    gVBoxAPI.UIAudioAdapter.GetAudioController(audioAdapter, &audioController);
                    if (audioController == AudioControllerType_SB16) {
                        def->sounds[0]->model = VIR_DOMAIN_SOUND_MODEL_SB16;
                    } else if (audioController == AudioControllerType_AC97) {
                        def->sounds[0]->model = VIR_DOMAIN_SOUND_MODEL_AC97;
                    }
                } else {
                    VIR_FREE(def->sounds);
                    def->nsounds = 0;
                }
            } else {
                def->nsounds = 0;
            }
        }
        VBOX_RELEASE(audioAdapter);
    }
}

static void
vboxDumpSerial(virDomainDefPtr def, vboxGlobalData *data, IMachine *machine, PRUint32 serialPortCount)
{
    PRUint32 serialPortIncCount = 0;
    size_t i = 0;
    /* dump serial port if active */
    def->nserials = 0;
    /* Get which serial ports are enabled/active */
    for (i = 0; i < serialPortCount; i++) {
        ISerialPort *serialPort = NULL;

        gVBoxAPI.UIMachine.GetSerialPort(machine, i, &serialPort);
        if (serialPort) {
            PRBool enabled = PR_FALSE;

            gVBoxAPI.UISerialPort.GetEnabled(serialPort, &enabled);
            if (enabled) {
                def->nserials++;
            }

            VBOX_RELEASE(serialPort);
        }
    }

    /* Allocate memory for the serial ports which are enabled */
    if ((def->nserials > 0) && (VIR_ALLOC_N(def->serials, def->nserials) >= 0)) {
        for (i = 0; i < def->nserials; i++) {
            ignore_value(VIR_ALLOC(def->serials[i]));
        }
    }

    /* Now get the details about the serial ports here */
    for (i = 0;
         serialPortIncCount < def->nserials && i < serialPortCount;
         i++) {
        ISerialPort *serialPort = NULL;

        gVBoxAPI.UIMachine.GetSerialPort(machine, i, &serialPort);
        if (serialPort) {
            PRBool enabled = PR_FALSE;

            gVBoxAPI.UISerialPort.GetEnabled(serialPort, &enabled);
            if (enabled) {
                PRUint32 hostMode    = PortMode_Disconnected;
                PRUint32 IOBase      = 0;
                PRUint32 IRQ         = 0;
                PRUnichar *pathUtf16 = NULL;
                char *path           = NULL;

                gVBoxAPI.UISerialPort.GetHostMode(serialPort, &hostMode);
                if (hostMode == PortMode_HostPipe) {
                    def->serials[serialPortIncCount]->source.type = VIR_DOMAIN_CHR_TYPE_PIPE;
                } else if (hostMode == PortMode_HostDevice) {
                    def->serials[serialPortIncCount]->source.type = VIR_DOMAIN_CHR_TYPE_DEV;
                } else if (gVBoxAPI.APIVersion >= 2002051 &&
                           hostMode == PortMode_RawFile) {
                    /* PortMode RawFile is used for vbox 3.0 or later */
                    def->serials[serialPortIncCount]->source.type = VIR_DOMAIN_CHR_TYPE_FILE;
                } else {
                    def->serials[serialPortIncCount]->source.type = VIR_DOMAIN_CHR_TYPE_NULL;
                }

                def->serials[serialPortIncCount]->deviceType = VIR_DOMAIN_CHR_DEVICE_TYPE_SERIAL;

                gVBoxAPI.UISerialPort.GetIRQ(serialPort, &IRQ);
                gVBoxAPI.UISerialPort.GetIOBase(serialPort, &IOBase);
                if ((IRQ == 4) && (IOBase == 1016)) {
                    def->serials[serialPortIncCount]->target.port = 0;
                } else if ((IRQ == 3) && (IOBase == 760)) {
                    def->serials[serialPortIncCount]->target.port = 1;
                }

                gVBoxAPI.UISerialPort.GetPath(serialPort, &pathUtf16);

                if (pathUtf16) {
                    VBOX_UTF16_TO_UTF8(pathUtf16, &path);
                    ignore_value(VIR_STRDUP(def->serials[serialPortIncCount]->source.data.file.path, path));
                }

                serialPortIncCount++;

                VBOX_UTF16_FREE(pathUtf16);
                VBOX_UTF8_FREE(path);
            }

            VBOX_RELEASE(serialPort);
        }
    }
}

static void
vboxDumpParallel(virDomainDefPtr def, vboxGlobalData *data, IMachine *machine, PRUint32 parallelPortCount)
{
    PRUint32 parallelPortIncCount = 0;
    size_t i = 0;
    /* dump parallel ports if active */
    def->nparallels = 0;
    /* Get which parallel ports are enabled/active */
    for (i = 0; i < parallelPortCount; i++) {
        IParallelPort *parallelPort = NULL;

        gVBoxAPI.UIMachine.GetParallelPort(machine, i, &parallelPort);
        if (parallelPort) {
            PRBool enabled = PR_FALSE;

            gVBoxAPI.UIParallelPort.GetEnabled(parallelPort, &enabled);
            if (enabled) {
                def->nparallels++;
            }

            VBOX_RELEASE(parallelPort);
        }
    }

    /* Allocate memory for the parallel ports which are enabled */
    if ((def->nparallels > 0) && (VIR_ALLOC_N(def->parallels, def->nparallels) >= 0)) {
        for (i = 0; i < def->nparallels; i++) {
            ignore_value(VIR_ALLOC(def->parallels[i]));
        }
    }

    /* Now get the details about the parallel ports here */
    for (i = 0;
         parallelPortIncCount < def->nparallels &&
             i < parallelPortCount;
         i++) {
        IParallelPort *parallelPort = NULL;

        gVBoxAPI.UIMachine.GetParallelPort(machine, i, &parallelPort);
        if (parallelPort) {
            PRBool enabled = PR_FALSE;

            gVBoxAPI.UIParallelPort.GetEnabled(parallelPort, &enabled);
            if (enabled) {
                PRUint32 IOBase      = 0;
                PRUint32 IRQ         = 0;
                PRUnichar *pathUtf16 = NULL;
                char *path           = NULL;

                gVBoxAPI.UIParallelPort.GetIRQ(parallelPort, &IRQ);
                gVBoxAPI.UIParallelPort.GetIOBase(parallelPort, &IOBase);
                if ((IRQ == 7) && (IOBase == 888)) {
                    def->parallels[parallelPortIncCount]->target.port = 0;
                } else if ((IRQ == 5) && (IOBase == 632)) {
                    def->parallels[parallelPortIncCount]->target.port = 1;
                }

                def->parallels[parallelPortIncCount]->source.type = VIR_DOMAIN_CHR_TYPE_FILE;
                def->parallels[parallelPortIncCount]->deviceType = VIR_DOMAIN_CHR_DEVICE_TYPE_PARALLEL;

                gVBoxAPI.UIParallelPort.GetPath(parallelPort, &pathUtf16);

                VBOX_UTF16_TO_UTF8(pathUtf16, &path);
                ignore_value(VIR_STRDUP(def->parallels[parallelPortIncCount]->source.data.file.path, path));

                parallelPortIncCount++;

                VBOX_UTF16_FREE(pathUtf16);
                VBOX_UTF8_FREE(path);
            }

            VBOX_RELEASE(parallelPort);
        }
    }
}

char *vboxDomainGetXMLDesc(virDomainPtr dom, unsigned int flags)
{
    VBOX_OBJECT_CHECK(dom->conn, char *, NULL);
    virDomainDefPtr def  = NULL;
    IMachine *machine    = NULL;
    vboxIIDUnion iid;
    PRBool accessible = PR_FALSE;
    size_t i = 0;
    PRBool PAEEnabled                   = PR_FALSE;
    PRBool ACPIEnabled                  = PR_FALSE;
    PRBool IOAPICEnabled                = PR_FALSE;
    PRUint32 CPUCount                   = 0;
    PRUint32 memorySize                 = 0;
    PRUint32 networkAdapterCount        = 0;
    PRUint32 maxMemorySize              = 4 * 1024;
    PRUint32 maxBootPosition            = 0;
    PRUint32 serialPortCount            = 0;
    PRUint32 parallelPortCount          = 0;
    IBIOSSettings *bios                 = NULL;
    PRUint32 chipsetType                = ChipsetType_Null;
    ISystemProperties *systemProperties = NULL;

    /* Flags checked by virDomainDefFormat */

    if (openSessionForMachine(data, dom->uuid, &iid, &machine, false) < 0)
        goto cleanup;

    if (VIR_ALLOC(def) < 0)
        goto cleanup;

    gVBoxAPI.UIMachine.GetAccessible(machine, &accessible);
    if (!accessible)
        goto cleanup;

    def->virtType = VIR_DOMAIN_VIRT_VBOX;
    def->id = dom->id;
    memcpy(def->uuid, dom->uuid, VIR_UUID_BUFLEN);
    if (VIR_STRDUP(def->name, dom->name) < 0)
        goto cleanup;

    gVBoxAPI.UIMachine.GetMemorySize(machine, &memorySize);
    def->mem.cur_balloon = memorySize * 1024;

    if (gVBoxAPI.chipsetType)
        gVBoxAPI.UIMachine.GetChipsetType(machine, &chipsetType);

    gVBoxAPI.UIVirtualBox.GetSystemProperties(data->vboxObj, &systemProperties);
    if (systemProperties) {
        gVBoxAPI.UISystemProperties.GetMaxGuestRAM(systemProperties, &maxMemorySize);
        gVBoxAPI.UISystemProperties.GetMaxBootPosition(systemProperties, &maxBootPosition);
        gVBoxAPI.UISystemProperties.GetMaxNetworkAdapters(systemProperties, chipsetType, &networkAdapterCount);
        gVBoxAPI.UISystemProperties.GetSerialPortCount(systemProperties, &serialPortCount);
        gVBoxAPI.UISystemProperties.GetParallelPortCount(systemProperties, &parallelPortCount);
        VBOX_RELEASE(systemProperties);
        systemProperties = NULL;
    }
    /* Currently setting memory and maxMemory as same, cause
     * the notation here seems to be inconsistent while
     * reading and while dumping xml
     */
    /* def->mem.max_balloon = maxMemorySize * 1024; */
    def->mem.max_balloon = memorySize * 1024;

    gVBoxAPI.UIMachine.GetCPUCount(machine, &CPUCount);
    def->maxvcpus = def->vcpus = CPUCount;

    /* Skip cpumasklen, cpumask, onReboot, onPoweroff, onCrash */

    if (VIR_STRDUP(def->os.type, "hvm") < 0)
        goto cleanup;

    def->os.arch = virArchFromHost();

    def->os.nBootDevs = 0;
    for (i = 0; (i < VIR_DOMAIN_BOOT_LAST) && (i < maxBootPosition); i++) {
        PRUint32 device = DeviceType_Null;

        gVBoxAPI.UIMachine.GetBootOrder(machine, i+1, &device);

        if (device == DeviceType_Floppy) {
            def->os.bootDevs[i] = VIR_DOMAIN_BOOT_FLOPPY;
            def->os.nBootDevs++;
        } else if (device == DeviceType_DVD) {
            def->os.bootDevs[i] = VIR_DOMAIN_BOOT_CDROM;
            def->os.nBootDevs++;
        } else if (device == DeviceType_HardDisk) {
            def->os.bootDevs[i] = VIR_DOMAIN_BOOT_DISK;
            def->os.nBootDevs++;
        } else if (device == DeviceType_Network) {
            def->os.bootDevs[i] = VIR_DOMAIN_BOOT_NET;
            def->os.nBootDevs++;
        } else if (device == DeviceType_USB) {
            /* Not supported by libvirt yet */
        } else if (device == DeviceType_SharedFolder) {
            /* Not supported by libvirt yet */
            /* Can VirtualBox really boot from a shared folder? */
        }
    }

    gVBoxAPI.UIMachine.GetCPUProperty(machine, CPUPropertyType_PAE, &PAEEnabled);
    if (PAEEnabled)
        def->features[VIR_DOMAIN_FEATURE_PAE] = VIR_TRISTATE_SWITCH_ON;

    gVBoxAPI.UIMachine.GetBIOSSettings(machine, &bios);
    if (bios) {
        gVBoxAPI.UIBIOSSettings.GetACPIEnabled(bios, &ACPIEnabled);
        if (ACPIEnabled)
            def->features[VIR_DOMAIN_FEATURE_ACPI] = VIR_TRISTATE_SWITCH_ON;

        gVBoxAPI.UIBIOSSettings.GetIOAPICEnabled(bios, &IOAPICEnabled);
        if (IOAPICEnabled)
            def->features[VIR_DOMAIN_FEATURE_APIC] = VIR_TRISTATE_SWITCH_ON;

        VBOX_RELEASE(bios);
    }

    /* Currently VirtualBox always uses locatime
     * so locatime is always true here */
    def->clock.offset = VIR_DOMAIN_CLOCK_OFFSET_LOCALTIME;

    vboxDumpVideo(def, data, machine);
    vboxDumpDisplay(def, data, machine);

    /* As the medium interface changed from 3.0 to 3.1.
     * There are two totally different implementations.
     * The old one would be version specified, while the
     * new one is using the vboxUniformedAPI and be put
     * into the common code.
     */
    if (gVBoxAPI.oldMediumInterface)
        gVBoxAPI.dumpIDEHDDsOld(def, data, machine);
    else
        vboxDumpIDEHDDsNew(def, data, machine);

    vboxDumpSharedFolders(def, data, machine);
    vboxDumpNetwork(def, data, machine, networkAdapterCount);
    vboxDumpAudio(def, data, machine);

    if (gVBoxAPI.oldMediumInterface) {
        gVBoxAPI.dumpDVD(def, data, machine);
        gVBoxAPI.dumpFloppy(def, data, machine);
    }

    vboxDumpSerial(def, data, machine, serialPortCount);
    vboxDumpParallel(def, data, machine, parallelPortCount);

    /* dump USB devices/filters if active */
    vboxHostDeviceGetXMLDesc(data, def, machine);

    ret = virDomainDefFormat(def, flags);

 cleanup:
    VBOX_RELEASE(machine);
    vboxIIDUnalloc(&iid);
    virDomainDefFree(def);
    return ret;
}

int vboxConnectListDefinedDomains(virConnectPtr conn,
                                  char ** const names, int maxnames)
{
    VBOX_OBJECT_CHECK(conn, int, -1);
    vboxArray machines = VBOX_ARRAY_INITIALIZER;
    char *machineName    = NULL;
    PRUnichar *machineNameUtf16 = NULL;
    PRUint32 state;
    nsresult rc;
    size_t i, j;

    rc = gVBoxAPI.UArray.vboxArrayGet(&machines, data->vboxObj,
                                      ARRAY_GET_MACHINES);
    if (NS_FAILED(rc)) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Could not get list of Defined Domains, rc=%08x"),
                       (unsigned)rc);
        goto cleanup;
    }

    memset(names, 0, sizeof(names[i]) * maxnames);

    ret = 0;
    for (i = 0, j = 0; (i < machines.count) && (j < maxnames); i++) {
        PRBool isAccessible = PR_FALSE;
        IMachine *machine = machines.items[i];

        if (!machine)
            continue;

        gVBoxAPI.UIMachine.GetAccessible(machine, &isAccessible);
        if (!isAccessible)
            continue;

        gVBoxAPI.UIMachine.GetState(machine, &state);
        if (!gVBoxAPI.machineStateChecker.Inactive(state))
            continue;

        gVBoxAPI.UIMachine.GetName(machine, &machineNameUtf16);
        VBOX_UTF16_TO_UTF8(machineNameUtf16, &machineName);
        if (VIR_STRDUP(names[j], machineName) < 0) {
            VBOX_UTF16_FREE(machineNameUtf16);
            VBOX_UTF8_FREE(machineName);
            for (j = 0; j < maxnames; j++)
                VIR_FREE(names[j]);
            ret = -1;
            goto cleanup;
        }
        VBOX_UTF16_FREE(machineNameUtf16);
        VBOX_UTF8_FREE(machineName);
        j++;
        ret++;
    }

 cleanup:
    gVBoxAPI.UArray.vboxArrayRelease(&machines);
    return ret;
}

int vboxConnectNumOfDefinedDomains(virConnectPtr conn)
{
    VBOX_OBJECT_CHECK(conn, int, -1);
    vboxArray machines = VBOX_ARRAY_INITIALIZER;
    PRUint32 state;
    nsresult rc;
    size_t i;

    rc = gVBoxAPI.UArray.vboxArrayGet(&machines, data->vboxObj,
                                      ARRAY_GET_MACHINES);
    if (NS_FAILED(rc)) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Could not get number of Defined Domains, rc=%08x"),
                       (unsigned)rc);
        goto cleanup;
    }

    ret = 0;
    for (i = 0; i < machines.count; ++i) {
        PRBool isAccessible = PR_FALSE;
        IMachine *machine = machines.items[i];

        if (!machine)
            continue;

        gVBoxAPI.UIMachine.GetAccessible(machine, &isAccessible);
        if (!isAccessible)
            continue;

        gVBoxAPI.UIMachine.GetState(machine, &state);
        if (gVBoxAPI.machineStateChecker.Inactive(state)) {
            ret++;
        }
    }

 cleanup:
    gVBoxAPI.UArray.vboxArrayRelease(&machines);
    return ret;
}