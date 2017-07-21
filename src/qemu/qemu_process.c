/*
 * qemu_process.c: QEMU process management
 *
 * Copyright (C) 2006-2016 Red Hat, Inc.
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
 *
 */

#include <config.h>

#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#if defined(__linux__)
# include <linux/capability.h>
#elif defined(__FreeBSD__)
# include <sys/param.h>
# include <sys/cpuset.h>
#endif

#include "qemu_process.h"
#include "qemu_processpriv.h"
#include "qemu_domain.h"
#include "qemu_cgroup.h"
#include "qemu_capabilities.h"
#include "qemu_monitor.h"
#include "qemu_command.h"
#include "qemu_hostdev.h"
#include "qemu_hotplug.h"
#include "qemu_migration.h"
#include "qemu_interface.h"

#include "cpu/cpu.h"
#include "datatypes.h"
#include "virlog.h"
#include "virerror.h"
#include "viralloc.h"
#include "virhook.h"
#include "virfile.h"
#include "virpidfile.h"
#include "nodeinfo.h"
#include "domain_audit.h"
#include "domain_nwfilter.h"
#include "locking/domain_lock.h"
#include "network/bridge_driver.h"
#include "viruuid.h"
#include "virprocess.h"
#include "virtime.h"
#include "virnetdevtap.h"
#include "virnetdevopenvswitch.h"
#include "virnetdevmidonet.h"
#include "virbitmap.h"
#include "viratomic.h"
#include "virnuma.h"
#include "virstring.h"
#include "virhostdev.h"
#include "storage/storage_driver.h"
#include "configmake.h"
#include "nwfilter_conf.h"
#include "netdev_bandwidth_conf.h"

#define VIR_FROM_THIS VIR_FROM_QEMU

VIR_LOG_INIT("qemu.qemu_process");

/**
 * qemuProcessRemoveDomainStatus
 *
 * remove all state files of a domain from statedir
 *
 * Returns 0 on success
 */
static int
qemuProcessRemoveDomainStatus(virQEMUDriverPtr driver,
                              virDomainObjPtr vm)
{
    char ebuf[1024];
    char *file = NULL;
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virQEMUDriverConfigPtr cfg = virQEMUDriverGetConfig(driver);
    int ret = -1;

    if (virAsprintf(&file, "%s/%s.xml", cfg->stateDir, vm->def->name) < 0)
        goto cleanup;

    if (unlink(file) < 0 && errno != ENOENT && errno != ENOTDIR)
        VIR_WARN("Failed to remove domain XML for %s: %s",
                 vm->def->name, virStrerror(errno, ebuf, sizeof(ebuf)));
    VIR_FREE(file);

    if (priv->pidfile &&
        unlink(priv->pidfile) < 0 &&
        errno != ENOENT)
        VIR_WARN("Failed to remove PID file for %s: %s",
                 vm->def->name, virStrerror(errno, ebuf, sizeof(ebuf)));

    ret = 0;
 cleanup:
    virObjectUnref(cfg);
    return ret;
}


/* XXX figure out how to remove this */
extern virQEMUDriverPtr qemu_driver;

/*
 * This is a callback registered with a qemuAgentPtr instance,
 * and to be invoked when the agent console hits an end of file
 * condition, or error, thus indicating VM shutdown should be
 * performed
 */
static void
qemuProcessHandleAgentEOF(qemuAgentPtr agent,
                          virDomainObjPtr vm)
{
    qemuDomainObjPrivatePtr priv;

    VIR_DEBUG("Received EOF from agent on %p '%s'", vm, vm->def->name);

    virObjectLock(vm);

    priv = vm->privateData;

    if (!priv->agent) {
        VIR_DEBUG("Agent freed already");
        goto unlock;
    }

    if (priv->beingDestroyed) {
        VIR_DEBUG("Domain is being destroyed, agent EOF is expected");
        goto unlock;
    }

    qemuAgentClose(agent);
    priv->agent = NULL;

    virObjectUnlock(vm);
    return;

 unlock:
    virObjectUnlock(vm);
    return;
}


/*
 * This is invoked when there is some kind of error
 * parsing data to/from the agent. The VM can continue
 * to run, but no further agent commands will be
 * allowed
 */
static void
qemuProcessHandleAgentError(qemuAgentPtr agent ATTRIBUTE_UNUSED,
                            virDomainObjPtr vm)
{
    qemuDomainObjPrivatePtr priv;

    VIR_DEBUG("Received error from agent on %p '%s'", vm, vm->def->name);

    virObjectLock(vm);

    priv = vm->privateData;

    priv->agentError = true;

    virObjectUnlock(vm);
}

static void qemuProcessHandleAgentDestroy(qemuAgentPtr agent,
                                          virDomainObjPtr vm)
{
    VIR_DEBUG("Received destroy agent=%p vm=%p", agent, vm);

    virObjectUnref(vm);
}


static qemuAgentCallbacks agentCallbacks = {
    .destroy = qemuProcessHandleAgentDestroy,
    .eofNotify = qemuProcessHandleAgentEOF,
    .errorNotify = qemuProcessHandleAgentError,
};


int
qemuConnectAgent(virQEMUDriverPtr driver, virDomainObjPtr vm)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    int ret = -1;
    qemuAgentPtr agent = NULL;
    virDomainChrDefPtr config = qemuFindAgentConfig(vm->def);

    if (!config)
        return 0;

    if (priv->agent)
        return 0;

    if (virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_VSERPORT_CHANGE) &&
        config->state != VIR_DOMAIN_CHR_DEVICE_STATE_CONNECTED) {
        VIR_DEBUG("Deferring connecting to guest agent");
        return 0;
    }

    if (virSecurityManagerSetDaemonSocketLabel(driver->securityManager,
                                               vm->def) < 0) {
        VIR_ERROR(_("Failed to set security context for agent for %s"),
                  vm->def->name);
        goto cleanup;
    }

    /* Hold an extra reference because we can't allow 'vm' to be
     * deleted while the agent is active */
    virObjectRef(vm);

    ignore_value(virTimeMillisNow(&priv->agentStart));
    virObjectUnlock(vm);

    agent = qemuAgentOpen(vm,
                          &config->source,
                          &agentCallbacks);

    virObjectLock(vm);
    priv->agentStart = 0;

    if (agent == NULL)
        virObjectUnref(vm);

    if (!virDomainObjIsActive(vm)) {
        qemuAgentClose(agent);
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("guest crashed while connecting to the guest agent"));
        ret = -2;
        goto cleanup;
    }

    if (virSecurityManagerClearSocketLabel(driver->securityManager,
                                           vm->def) < 0) {
        VIR_ERROR(_("Failed to clear security context for agent for %s"),
                  vm->def->name);
        qemuAgentClose(agent);
        goto cleanup;
    }


    priv->agent = agent;

    if (priv->agent == NULL) {
        VIR_INFO("Failed to connect agent for %s", vm->def->name);
        goto cleanup;
    }

    ret = 0;

 cleanup:
    return ret;
}


/*
 * This is a callback registered with a qemuMonitorPtr instance,
 * and to be invoked when the monitor console hits an end of file
 * condition, or error, thus indicating VM shutdown should be
 * performed
 */
static void
qemuProcessHandleMonitorEOF(qemuMonitorPtr mon ATTRIBUTE_UNUSED,
                            virDomainObjPtr vm,
                            void *opaque)
{
    virQEMUDriverPtr driver = opaque;
    virObjectEventPtr event = NULL;
    qemuDomainObjPrivatePtr priv;
    int eventReason = VIR_DOMAIN_EVENT_STOPPED_SHUTDOWN;
    int stopReason = VIR_DOMAIN_SHUTOFF_SHUTDOWN;
    const char *auditReason = "shutdown";
    unsigned int stopFlags = 0;

    VIR_DEBUG("Received EOF on %p '%s'", vm, vm->def->name);

    virObjectLock(vm);

    priv = vm->privateData;

    if (priv->beingDestroyed) {
        VIR_DEBUG("Domain is being destroyed, EOF is expected");
        goto cleanup;
    }

    if (!virDomainObjIsActive(vm)) {
        VIR_DEBUG("Domain %p is not active, ignoring EOF", vm);
        goto cleanup;
    }

    if (priv->monJSON && !priv->gotShutdown) {
        VIR_DEBUG("Monitor connection to '%s' closed without SHUTDOWN event; "
                  "assuming the domain crashed", vm->def->name);
        eventReason = VIR_DOMAIN_EVENT_STOPPED_FAILED;
        stopReason = VIR_DOMAIN_SHUTOFF_CRASHED;
        auditReason = "failed";
    }

    if (priv->job.asyncJob == QEMU_ASYNC_JOB_MIGRATION_IN) {
        stopFlags |= VIR_QEMU_PROCESS_STOP_MIGRATED;
        qemuMigrationErrorSave(driver, vm->def->name,
                               qemuMonitorLastError(priv->mon));
    }

    event = virDomainEventLifecycleNewFromObj(vm,
                                     VIR_DOMAIN_EVENT_STOPPED,
                                     eventReason);
    qemuProcessStop(driver, vm, stopReason, stopFlags);
    virDomainAuditStop(vm, auditReason);

    qemuDomainRemoveInactive(driver, vm);

 cleanup:
    virObjectUnlock(vm);
    qemuDomainEventQueue(driver, event);
}


/*
 * This is invoked when there is some kind of error
 * parsing data to/from the monitor. The VM can continue
 * to run, but no further monitor commands will be
 * allowed
 */
static void
qemuProcessHandleMonitorError(qemuMonitorPtr mon ATTRIBUTE_UNUSED,
                              virDomainObjPtr vm,
                              void *opaque)
{
    virQEMUDriverPtr driver = opaque;
    virObjectEventPtr event = NULL;

    VIR_DEBUG("Received error on %p '%s'", vm, vm->def->name);

    virObjectLock(vm);

    ((qemuDomainObjPrivatePtr) vm->privateData)->monError = true;
    event = virDomainEventControlErrorNewFromObj(vm);
    qemuDomainEventQueue(driver, event);

    virObjectUnlock(vm);
}


virDomainDiskDefPtr
qemuProcessFindDomainDiskByAlias(virDomainObjPtr vm,
                                 const char *alias)
{
    size_t i;

    if (STRPREFIX(alias, QEMU_DRIVE_HOST_PREFIX))
        alias += strlen(QEMU_DRIVE_HOST_PREFIX);

    for (i = 0; i < vm->def->ndisks; i++) {
        virDomainDiskDefPtr disk;

        disk = vm->def->disks[i];
        if (disk->info.alias != NULL && STREQ(disk->info.alias, alias))
            return disk;
    }

    virReportError(VIR_ERR_INTERNAL_ERROR,
                   _("no disk found with alias %s"),
                   alias);
    return NULL;
}

static int
qemuProcessGetVolumeQcowPassphrase(virConnectPtr conn,
                                   virDomainDiskDefPtr disk,
                                   char **secretRet,
                                   size_t *secretLen)
{
    virSecretPtr secret;
    char *passphrase;
    unsigned char *data;
    size_t size;
    int ret = -1;
    virStorageEncryptionPtr enc;

    if (!disk->src->encryption) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("disk %s does not have any encryption information"),
                       disk->src->path);
        return -1;
    }
    enc = disk->src->encryption;

    if (!conn) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("cannot find secrets without a connection"));
        goto cleanup;
    }

    if (conn->secretDriver == NULL ||
        conn->secretDriver->secretLookupByUUID == NULL ||
        conn->secretDriver->secretGetValue == NULL) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("secret storage not supported"));
        goto cleanup;
    }

    if (enc->format != VIR_STORAGE_ENCRYPTION_FORMAT_QCOW ||
        enc->nsecrets != 1 ||
        enc->secrets[0]->type !=
        VIR_STORAGE_ENCRYPTION_SECRET_TYPE_PASSPHRASE) {
        virReportError(VIR_ERR_XML_ERROR,
                       _("invalid <encryption> for volume %s"),
                       virDomainDiskGetSource(disk));
        goto cleanup;
    }

    secret = conn->secretDriver->secretLookupByUUID(conn,
                                                    enc->secrets[0]->uuid);
    if (secret == NULL)
        goto cleanup;
    data = conn->secretDriver->secretGetValue(secret, &size, 0,
                                              VIR_SECRET_GET_VALUE_INTERNAL_CALL);
    virObjectUnref(secret);
    if (data == NULL)
        goto cleanup;

    if (memchr(data, '\0', size) != NULL) {
        memset(data, 0, size);
        VIR_FREE(data);
        virReportError(VIR_ERR_XML_ERROR,
                       _("format='qcow' passphrase for %s must not contain a "
                         "'\\0'"), virDomainDiskGetSource(disk));
        goto cleanup;
    }

    if (VIR_ALLOC_N(passphrase, size + 1) < 0) {
        memset(data, 0, size);
        VIR_FREE(data);
        goto cleanup;
    }
    memcpy(passphrase, data, size);
    passphrase[size] = '\0';

    memset(data, 0, size);
    VIR_FREE(data);

    *secretRet = passphrase;
    *secretLen = size;

    ret = 0;

 cleanup:
    return ret;
}

static int
qemuProcessFindVolumeQcowPassphrase(qemuMonitorPtr mon ATTRIBUTE_UNUSED,
                                    virConnectPtr conn,
                                    virDomainObjPtr vm,
                                    const char *path,
                                    char **secretRet,
                                    size_t *secretLen,
                                    void *opaque ATTRIBUTE_UNUSED)
{
    virDomainDiskDefPtr disk;
    int ret = -1;

    virObjectLock(vm);
    if (!(disk = virDomainDiskByName(vm->def, path, true))) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("no disk found with path %s"),
                       path);
        goto cleanup;
    }

    ret = qemuProcessGetVolumeQcowPassphrase(conn, disk, secretRet, secretLen);

 cleanup:
    virObjectUnlock(vm);
    return ret;
}


static int
qemuProcessHandleReset(qemuMonitorPtr mon ATTRIBUTE_UNUSED,
                       virDomainObjPtr vm,
                       void *opaque)
{
    virQEMUDriverPtr driver = opaque;
    virObjectEventPtr event;
    qemuDomainObjPrivatePtr priv;
    virQEMUDriverConfigPtr cfg = virQEMUDriverGetConfig(driver);

    virObjectLock(vm);

    event = virDomainEventRebootNewFromObj(vm);
    priv = vm->privateData;
    if (priv->agent)
        qemuAgentNotifyEvent(priv->agent, QEMU_AGENT_EVENT_RESET);

    if (virDomainSaveStatus(driver->xmlopt, cfg->stateDir, vm, driver->caps) < 0)
        VIR_WARN("Failed to save status on vm %s", vm->def->name);

    virObjectUnlock(vm);

    qemuDomainEventQueue(driver, event);

    virObjectUnref(cfg);
    return 0;
}


/*
 * Since we have the '-no-shutdown' flag set, the
 * QEMU process will currently have guest OS shutdown
 * and the CPUS stopped. To fake the reboot, we thus
 * want todo a reset of the virtual hardware, followed
 * by restart of the CPUs. This should result in the
 * guest OS booting up again
 */
static void
qemuProcessFakeReboot(void *opaque)
{
    virQEMUDriverPtr driver = qemu_driver;
    virDomainObjPtr vm = opaque;
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virObjectEventPtr event = NULL;
    virQEMUDriverConfigPtr cfg = virQEMUDriverGetConfig(driver);
    virDomainRunningReason reason = VIR_DOMAIN_RUNNING_BOOTED;
    int ret = -1, rc;

    VIR_DEBUG("vm=%p", vm);
    virObjectLock(vm);
    if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_MODIFY) < 0)
        goto cleanup;

    if (!virDomainObjIsActive(vm)) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("guest unexpectedly quit"));
        goto endjob;
    }

    qemuDomainObjEnterMonitor(driver, vm);
    rc = qemuMonitorSystemReset(priv->mon);

    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        goto endjob;

    if (rc < 0)
        goto endjob;

    if (virDomainObjGetState(vm, NULL) == VIR_DOMAIN_CRASHED)
        reason = VIR_DOMAIN_RUNNING_CRASHED;

    if (qemuProcessStartCPUs(driver, vm, NULL,
                             reason,
                             QEMU_ASYNC_JOB_NONE) < 0) {
        if (virGetLastError() == NULL)
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           "%s", _("resume operation failed"));
        goto endjob;
    }
    priv->gotShutdown = false;
    event = virDomainEventLifecycleNewFromObj(vm,
                                     VIR_DOMAIN_EVENT_RESUMED,
                                     VIR_DOMAIN_EVENT_RESUMED_UNPAUSED);

    if (virDomainSaveStatus(driver->xmlopt, cfg->stateDir, vm, driver->caps) < 0) {
        VIR_WARN("Unable to save status on vm %s after state change",
                 vm->def->name);
    }

    ret = 0;

 endjob:
    qemuDomainObjEndJob(driver, vm);

 cleanup:
    if (ret == -1)
        ignore_value(qemuProcessKill(vm, VIR_QEMU_PROCESS_KILL_FORCE));
    virDomainObjEndAPI(&vm);
    qemuDomainEventQueue(driver, event);
    virObjectUnref(cfg);
}


void
qemuProcessShutdownOrReboot(virQEMUDriverPtr driver,
                            virDomainObjPtr vm)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;

    if (priv->fakeReboot) {
        qemuDomainSetFakeReboot(driver, vm, false);
        virObjectRef(vm);
        virThread th;
        if (virThreadCreate(&th,
                            false,
                            qemuProcessFakeReboot,
                            vm) < 0) {
            VIR_ERROR(_("Failed to create reboot thread, killing domain"));
            ignore_value(qemuProcessKill(vm, VIR_QEMU_PROCESS_KILL_NOWAIT));
            virObjectUnref(vm);
        }
    } else {
        ignore_value(qemuProcessKill(vm, VIR_QEMU_PROCESS_KILL_NOWAIT));
    }
}


static int
qemuProcessHandleEvent(qemuMonitorPtr mon ATTRIBUTE_UNUSED,
                       virDomainObjPtr vm,
                       const char *eventName,
                       long long seconds,
                       unsigned int micros,
                       const char *details,
                       void *opaque)
{
    virQEMUDriverPtr driver = opaque;
    virObjectEventPtr event = NULL;

    VIR_DEBUG("vm=%p", vm);

    virObjectLock(vm);
    event = virDomainQemuMonitorEventNew(vm->def->id, vm->def->name,
                                         vm->def->uuid, eventName,
                                         seconds, micros, details);

    virObjectUnlock(vm);
    qemuDomainEventQueue(driver, event);

    return 0;
}


static int
qemuProcessHandleShutdown(qemuMonitorPtr mon ATTRIBUTE_UNUSED,
                          virDomainObjPtr vm,
                          void *opaque)
{
    virQEMUDriverPtr driver = opaque;
    qemuDomainObjPrivatePtr priv;
    virObjectEventPtr event = NULL;
    virQEMUDriverConfigPtr cfg = virQEMUDriverGetConfig(driver);

    VIR_DEBUG("vm=%p", vm);

    virObjectLock(vm);

    priv = vm->privateData;
    if (priv->gotShutdown) {
        VIR_DEBUG("Ignoring repeated SHUTDOWN event from domain %s",
                  vm->def->name);
        goto unlock;
    } else if (!virDomainObjIsActive(vm)) {
        VIR_DEBUG("Ignoring SHUTDOWN event from inactive domain %s",
                  vm->def->name);
        goto unlock;
    }
    priv->gotShutdown = true;

    VIR_DEBUG("Transitioned guest %s to shutdown state",
              vm->def->name);
    virDomainObjSetState(vm,
                         VIR_DOMAIN_SHUTDOWN,
                         VIR_DOMAIN_SHUTDOWN_UNKNOWN);
    event = virDomainEventLifecycleNewFromObj(vm,
                                     VIR_DOMAIN_EVENT_SHUTDOWN,
                                     VIR_DOMAIN_EVENT_SHUTDOWN_FINISHED);

    if (virDomainSaveStatus(driver->xmlopt, cfg->stateDir, vm, driver->caps) < 0) {
        VIR_WARN("Unable to save status on vm %s after state change",
                 vm->def->name);
    }

    if (priv->agent)
        qemuAgentNotifyEvent(priv->agent, QEMU_AGENT_EVENT_SHUTDOWN);

    qemuProcessShutdownOrReboot(driver, vm);

 unlock:
    virObjectUnlock(vm);
    qemuDomainEventQueue(driver, event);
    virObjectUnref(cfg);

    return 0;
}


static int
qemuProcessHandleStop(qemuMonitorPtr mon ATTRIBUTE_UNUSED,
                      virDomainObjPtr vm,
                      void *opaque)
{
    virQEMUDriverPtr driver = opaque;
    virObjectEventPtr event = NULL;
    virDomainPausedReason reason = VIR_DOMAIN_PAUSED_UNKNOWN;
    virDomainEventSuspendedDetailType detail = VIR_DOMAIN_EVENT_SUSPENDED_PAUSED;
    virQEMUDriverConfigPtr cfg = virQEMUDriverGetConfig(driver);

    virObjectLock(vm);
    if (virDomainObjGetState(vm, NULL) == VIR_DOMAIN_RUNNING) {
        qemuDomainObjPrivatePtr priv = vm->privateData;

        if (priv->gotShutdown) {
            VIR_DEBUG("Ignoring STOP event after SHUTDOWN");
            goto unlock;
        }

        if (priv->job.asyncJob == QEMU_ASYNC_JOB_MIGRATION_OUT) {
            if (priv->job.current->stats.status ==
                        QEMU_MONITOR_MIGRATION_STATUS_POSTCOPY) {
                reason = VIR_DOMAIN_PAUSED_POSTCOPY;
                detail = VIR_DOMAIN_EVENT_SUSPENDED_POSTCOPY;
            } else {
                reason = VIR_DOMAIN_PAUSED_MIGRATION;
                detail = VIR_DOMAIN_EVENT_SUSPENDED_MIGRATED;
            }
        }

        VIR_DEBUG("Transitioned guest %s to paused state, reason %s",
                  vm->def->name, virDomainPausedReasonTypeToString(reason));

        if (priv->job.current)
            ignore_value(virTimeMillisNow(&priv->job.current->stopped));

        if (priv->signalStop)
            virDomainObjBroadcast(vm);

        virDomainObjSetState(vm, VIR_DOMAIN_PAUSED, reason);
        event = virDomainEventLifecycleNewFromObj(vm,
                                                  VIR_DOMAIN_EVENT_SUSPENDED,
                                                  detail);

        VIR_FREE(priv->lockState);
        if (virDomainLockProcessPause(driver->lockManager, vm, &priv->lockState) < 0)
            VIR_WARN("Unable to release lease on %s", vm->def->name);
        VIR_DEBUG("Preserving lock state '%s'", NULLSTR(priv->lockState));

        if (virDomainSaveStatus(driver->xmlopt, cfg->stateDir, vm, driver->caps) < 0) {
            VIR_WARN("Unable to save status on vm %s after state change",
                     vm->def->name);
        }
    }

 unlock:
    virObjectUnlock(vm);
    qemuDomainEventQueue(driver, event);
    virObjectUnref(cfg);

    return 0;
}


static int
qemuProcessHandleResume(qemuMonitorPtr mon ATTRIBUTE_UNUSED,
                        virDomainObjPtr vm,
                        void *opaque)
{
    virQEMUDriverPtr driver = opaque;
    virObjectEventPtr event = NULL;
    virQEMUDriverConfigPtr cfg = virQEMUDriverGetConfig(driver);

    virObjectLock(vm);
    if (virDomainObjGetState(vm, NULL) == VIR_DOMAIN_PAUSED) {
        qemuDomainObjPrivatePtr priv = vm->privateData;

        if (priv->gotShutdown) {
            VIR_DEBUG("Ignoring RESUME event after SHUTDOWN");
            goto unlock;
        }

        VIR_DEBUG("Transitioned guest %s out of paused into resumed state",
                  vm->def->name);

        virDomainObjSetState(vm, VIR_DOMAIN_RUNNING,
                                 VIR_DOMAIN_RUNNING_UNPAUSED);
        event = virDomainEventLifecycleNewFromObj(vm,
                                         VIR_DOMAIN_EVENT_RESUMED,
                                         VIR_DOMAIN_EVENT_RESUMED_UNPAUSED);

        VIR_DEBUG("Using lock state '%s' on resume event", NULLSTR(priv->lockState));
        if (virDomainLockProcessResume(driver->lockManager, cfg->uri,
                                       vm, priv->lockState) < 0) {
            /* Don't free priv->lockState on error, because we need
             * to make sure we have state still present if the user
             * tries to resume again
             */
            goto unlock;
        }
        VIR_FREE(priv->lockState);

        if (virDomainSaveStatus(driver->xmlopt, cfg->stateDir, vm, driver->caps) < 0) {
            VIR_WARN("Unable to save status on vm %s after state change",
                     vm->def->name);
        }
    }

 unlock:
    virObjectUnlock(vm);
    qemuDomainEventQueue(driver, event);
    virObjectUnref(cfg);
    return 0;
}

static int
qemuProcessHandleRTCChange(qemuMonitorPtr mon ATTRIBUTE_UNUSED,
                           virDomainObjPtr vm,
                           long long offset,
                           void *opaque)
{
    virQEMUDriverPtr driver = opaque;
    virObjectEventPtr event = NULL;
    virQEMUDriverConfigPtr cfg = virQEMUDriverGetConfig(driver);

    virObjectLock(vm);

    if (vm->def->clock.offset == VIR_DOMAIN_CLOCK_OFFSET_VARIABLE) {
        /* when a basedate is manually given on the qemu commandline
         * rather than simply "-rtc base=utc", the offset sent by qemu
         * in this event is *not* the new offset from UTC, but is
         * instead the new offset from the *original basedate* +
         * uptime. For example, if the original offset was 3600 and
         * the guest clock has been advanced by 10 seconds, qemu will
         * send "10" in the event - this means that the new offset
         * from UTC is 3610, *not* 10. If the guest clock is advanced
         * by another 10 seconds, qemu will now send "20" - i.e. each
         * event is the sum of the most recent change and all previous
         * changes since the domain was started. Fortunately, we have
         * saved the initial offset in "adjustment0", so to arrive at
         * the proper new "adjustment", we just add the most recent
         * offset to adjustment0.
         */
        offset += vm->def->clock.data.variable.adjustment0;
        vm->def->clock.data.variable.adjustment = offset;

        if (virDomainSaveStatus(driver->xmlopt, cfg->stateDir, vm, driver->caps) < 0)
           VIR_WARN("unable to save domain status with RTC change");
    }

    event = virDomainEventRTCChangeNewFromObj(vm, offset);

    virObjectUnlock(vm);

    qemuDomainEventQueue(driver, event);
    virObjectUnref(cfg);
    return 0;
}


static int
qemuProcessHandleWatchdog(qemuMonitorPtr mon ATTRIBUTE_UNUSED,
                          virDomainObjPtr vm,
                          int action,
                          void *opaque)
{
    virQEMUDriverPtr driver = opaque;
    virObjectEventPtr watchdogEvent = NULL;
    virObjectEventPtr lifecycleEvent = NULL;
    virQEMUDriverConfigPtr cfg = virQEMUDriverGetConfig(driver);

    virObjectLock(vm);
    watchdogEvent = virDomainEventWatchdogNewFromObj(vm, action);

    if (action == VIR_DOMAIN_EVENT_WATCHDOG_PAUSE &&
        virDomainObjGetState(vm, NULL) == VIR_DOMAIN_RUNNING) {
        qemuDomainObjPrivatePtr priv = vm->privateData;
        VIR_DEBUG("Transitioned guest %s to paused state due to watchdog", vm->def->name);

        virDomainObjSetState(vm, VIR_DOMAIN_PAUSED, VIR_DOMAIN_PAUSED_WATCHDOG);
        lifecycleEvent = virDomainEventLifecycleNewFromObj(vm,
                                                  VIR_DOMAIN_EVENT_SUSPENDED,
                                                  VIR_DOMAIN_EVENT_SUSPENDED_WATCHDOG);

        VIR_FREE(priv->lockState);
        if (virDomainLockProcessPause(driver->lockManager, vm, &priv->lockState) < 0)
            VIR_WARN("Unable to release lease on %s", vm->def->name);
        VIR_DEBUG("Preserving lock state '%s'", NULLSTR(priv->lockState));

        if (virDomainSaveStatus(driver->xmlopt, cfg->stateDir, vm, driver->caps) < 0) {
            VIR_WARN("Unable to save status on vm %s after watchdog event",
                     vm->def->name);
        }
    }

    if (vm->def->watchdog->action == VIR_DOMAIN_WATCHDOG_ACTION_DUMP) {
        struct qemuProcessEvent *processEvent;
        if (VIR_ALLOC(processEvent) == 0) {
            processEvent->eventType = QEMU_PROCESS_EVENT_WATCHDOG;
            processEvent->action = VIR_DOMAIN_WATCHDOG_ACTION_DUMP;
            processEvent->vm = vm;
            /* Hold an extra reference because we can't allow 'vm' to be
             * deleted before handling watchdog event is finished.
             */
            virObjectRef(vm);
            if (virThreadPoolSendJob(driver->workerPool, 0, processEvent) < 0) {
                if (!virObjectUnref(vm))
                    vm = NULL;
                VIR_FREE(processEvent);
            }
        }
    }

    if (vm)
        virObjectUnlock(vm);
    qemuDomainEventQueue(driver, watchdogEvent);
    qemuDomainEventQueue(driver, lifecycleEvent);

    virObjectUnref(cfg);
    return 0;
}


static int
qemuProcessHandleIOError(qemuMonitorPtr mon ATTRIBUTE_UNUSED,
                         virDomainObjPtr vm,
                         const char *diskAlias,
                         int action,
                         const char *reason,
                         void *opaque)
{
    virQEMUDriverPtr driver = opaque;
    virObjectEventPtr ioErrorEvent = NULL;
    virObjectEventPtr ioErrorEvent2 = NULL;
    virObjectEventPtr lifecycleEvent = NULL;
    const char *srcPath;
    const char *devAlias;
    virDomainDiskDefPtr disk;
    virQEMUDriverConfigPtr cfg = virQEMUDriverGetConfig(driver);

    virObjectLock(vm);
    disk = qemuProcessFindDomainDiskByAlias(vm, diskAlias);

    if (disk) {
        srcPath = virDomainDiskGetSource(disk);
        devAlias = disk->info.alias;
    } else {
        srcPath = "";
        devAlias = "";
    }

    ioErrorEvent = virDomainEventIOErrorNewFromObj(vm, srcPath, devAlias, action);
    ioErrorEvent2 = virDomainEventIOErrorReasonNewFromObj(vm, srcPath, devAlias, action, reason);

    if (action == VIR_DOMAIN_EVENT_IO_ERROR_PAUSE &&
        virDomainObjGetState(vm, NULL) == VIR_DOMAIN_RUNNING) {
        qemuDomainObjPrivatePtr priv = vm->privateData;
        VIR_DEBUG("Transitioned guest %s to paused state due to IO error", vm->def->name);

        if (priv->signalIOError)
            virDomainObjBroadcast(vm);

        virDomainObjSetState(vm, VIR_DOMAIN_PAUSED, VIR_DOMAIN_PAUSED_IOERROR);
        lifecycleEvent = virDomainEventLifecycleNewFromObj(vm,
                                                  VIR_DOMAIN_EVENT_SUSPENDED,
                                                  VIR_DOMAIN_EVENT_SUSPENDED_IOERROR);

        VIR_FREE(priv->lockState);
        if (virDomainLockProcessPause(driver->lockManager, vm, &priv->lockState) < 0)
            VIR_WARN("Unable to release lease on %s", vm->def->name);
        VIR_DEBUG("Preserving lock state '%s'", NULLSTR(priv->lockState));

        if (virDomainSaveStatus(driver->xmlopt, cfg->stateDir, vm, driver->caps) < 0)
            VIR_WARN("Unable to save status on vm %s after IO error", vm->def->name);
    }
    virObjectUnlock(vm);

    qemuDomainEventQueue(driver, ioErrorEvent);
    qemuDomainEventQueue(driver, ioErrorEvent2);
    qemuDomainEventQueue(driver, lifecycleEvent);
    virObjectUnref(cfg);
    return 0;
}

static int
qemuProcessHandleBlockJob(qemuMonitorPtr mon ATTRIBUTE_UNUSED,
                          virDomainObjPtr vm,
                          const char *diskAlias,
                          int type,
                          int status,
                          void *opaque)
{
    virQEMUDriverPtr driver = opaque;
    struct qemuProcessEvent *processEvent = NULL;
    virDomainDiskDefPtr disk;
    qemuDomainDiskPrivatePtr diskPriv;
    char *data = NULL;

    virObjectLock(vm);

    VIR_DEBUG("Block job for device %s (domain: %p,%s) type %d status %d",
              diskAlias, vm, vm->def->name, type, status);

    if (!(disk = qemuProcessFindDomainDiskByAlias(vm, diskAlias)))
        goto error;
    diskPriv = QEMU_DOMAIN_DISK_PRIVATE(disk);

    if (diskPriv->blockJobSync) {
        /* We have a SYNC API waiting for this event, dispatch it back */
        diskPriv->blockJobType = type;
        diskPriv->blockJobStatus = status;
        virDomainObjBroadcast(vm);
    } else {
        /* there is no waiting SYNC API, dispatch the update to a thread */
        if (VIR_ALLOC(processEvent) < 0)
            goto error;

        processEvent->eventType = QEMU_PROCESS_EVENT_BLOCK_JOB;
        if (VIR_STRDUP(data, diskAlias) < 0)
            goto error;
        processEvent->data = data;
        processEvent->vm = vm;
        processEvent->action = type;
        processEvent->status = status;

        virObjectRef(vm);
        if (virThreadPoolSendJob(driver->workerPool, 0, processEvent) < 0) {
            ignore_value(virObjectUnref(vm));
            goto error;
        }
    }

 cleanup:
    virObjectUnlock(vm);
    return 0;
 error:
    if (processEvent)
        VIR_FREE(processEvent->data);
    VIR_FREE(processEvent);
    goto cleanup;
}


static int
qemuProcessHandleGraphics(qemuMonitorPtr mon ATTRIBUTE_UNUSED,
                          virDomainObjPtr vm,
                          int phase,
                          int localFamily,
                          const char *localNode,
                          const char *localService,
                          int remoteFamily,
                          const char *remoteNode,
                          const char *remoteService,
                          const char *authScheme,
                          const char *x509dname,
                          const char *saslUsername,
                          void *opaque)
{
    virQEMUDriverPtr driver = opaque;
    virObjectEventPtr event;
    virDomainEventGraphicsAddressPtr localAddr = NULL;
    virDomainEventGraphicsAddressPtr remoteAddr = NULL;
    virDomainEventGraphicsSubjectPtr subject = NULL;
    size_t i;

    if (VIR_ALLOC(localAddr) < 0)
        goto error;
    localAddr->family = localFamily;
    if (VIR_STRDUP(localAddr->service, localService) < 0 ||
        VIR_STRDUP(localAddr->node, localNode) < 0)
        goto error;

    if (VIR_ALLOC(remoteAddr) < 0)
        goto error;
    remoteAddr->family = remoteFamily;
    if (VIR_STRDUP(remoteAddr->service, remoteService) < 0 ||
        VIR_STRDUP(remoteAddr->node, remoteNode) < 0)
        goto error;

    if (VIR_ALLOC(subject) < 0)
        goto error;
    if (x509dname) {
        if (VIR_REALLOC_N(subject->identities, subject->nidentity+1) < 0)
            goto error;
        subject->nidentity++;
        if (VIR_STRDUP(subject->identities[subject->nidentity-1].type, "x509dname") < 0 ||
            VIR_STRDUP(subject->identities[subject->nidentity-1].name, x509dname) < 0)
            goto error;
    }
    if (saslUsername) {
        if (VIR_REALLOC_N(subject->identities, subject->nidentity+1) < 0)
            goto error;
        subject->nidentity++;
        if (VIR_STRDUP(subject->identities[subject->nidentity-1].type, "saslUsername") < 0 ||
            VIR_STRDUP(subject->identities[subject->nidentity-1].name, saslUsername) < 0)
            goto error;
    }

    virObjectLock(vm);
    event = virDomainEventGraphicsNewFromObj(vm, phase, localAddr, remoteAddr, authScheme, subject);
    virObjectUnlock(vm);

    qemuDomainEventQueue(driver, event);

    return 0;

 error:
    if (localAddr) {
        VIR_FREE(localAddr->service);
        VIR_FREE(localAddr->node);
        VIR_FREE(localAddr);
    }
    if (remoteAddr) {
        VIR_FREE(remoteAddr->service);
        VIR_FREE(remoteAddr->node);
        VIR_FREE(remoteAddr);
    }
    if (subject) {
        for (i = 0; i < subject->nidentity; i++) {
            VIR_FREE(subject->identities[i].type);
            VIR_FREE(subject->identities[i].name);
        }
        VIR_FREE(subject->identities);
        VIR_FREE(subject);
    }

    return -1;
}

static int
qemuProcessHandleTrayChange(qemuMonitorPtr mon ATTRIBUTE_UNUSED,
                            virDomainObjPtr vm,
                            const char *devAlias,
                            int reason,
                            void *opaque)
{
    virQEMUDriverPtr driver = opaque;
    virObjectEventPtr event = NULL;
    virDomainDiskDefPtr disk;
    virQEMUDriverConfigPtr cfg = virQEMUDriverGetConfig(driver);

    virObjectLock(vm);
    disk = qemuProcessFindDomainDiskByAlias(vm, devAlias);

    if (disk) {
        event = virDomainEventTrayChangeNewFromObj(vm,
                                                   devAlias,
                                                   reason);
        /* Update disk tray status */
        if (reason == VIR_DOMAIN_EVENT_TRAY_CHANGE_OPEN)
            disk->tray_status = VIR_DOMAIN_DISK_TRAY_OPEN;
        else if (reason == VIR_DOMAIN_EVENT_TRAY_CHANGE_CLOSE)
            disk->tray_status = VIR_DOMAIN_DISK_TRAY_CLOSED;

        if (virDomainSaveStatus(driver->xmlopt, cfg->stateDir, vm, driver->caps) < 0) {
            VIR_WARN("Unable to save status on vm %s after tray moved event",
                     vm->def->name);
        }

        virDomainObjBroadcast(vm);
    }

    virObjectUnlock(vm);
    qemuDomainEventQueue(driver, event);
    virObjectUnref(cfg);
    return 0;
}

static int
qemuProcessHandlePMWakeup(qemuMonitorPtr mon ATTRIBUTE_UNUSED,
                          virDomainObjPtr vm,
                          void *opaque)
{
    virQEMUDriverPtr driver = opaque;
    virObjectEventPtr event = NULL;
    virObjectEventPtr lifecycleEvent = NULL;
    virQEMUDriverConfigPtr cfg = virQEMUDriverGetConfig(driver);

    virObjectLock(vm);
    event = virDomainEventPMWakeupNewFromObj(vm);

    /* Don't set domain status back to running if it wasn't paused
     * from guest side, otherwise it can just cause confusion.
     */
    if (virDomainObjGetState(vm, NULL) == VIR_DOMAIN_PMSUSPENDED) {
        VIR_DEBUG("Transitioned guest %s from pmsuspended to running "
                  "state due to QMP wakeup event", vm->def->name);

        virDomainObjSetState(vm, VIR_DOMAIN_RUNNING,
                             VIR_DOMAIN_RUNNING_WAKEUP);
        lifecycleEvent = virDomainEventLifecycleNewFromObj(vm,
                                                  VIR_DOMAIN_EVENT_STARTED,
                                                  VIR_DOMAIN_EVENT_STARTED_WAKEUP);

        if (virDomainSaveStatus(driver->xmlopt, cfg->stateDir, vm, driver->caps) < 0) {
            VIR_WARN("Unable to save status on vm %s after wakeup event",
                     vm->def->name);
        }
    }

    virObjectUnlock(vm);
    qemuDomainEventQueue(driver, event);
    qemuDomainEventQueue(driver, lifecycleEvent);
    virObjectUnref(cfg);
    return 0;
}

static int
qemuProcessHandlePMSuspend(qemuMonitorPtr mon ATTRIBUTE_UNUSED,
                           virDomainObjPtr vm,
                           void *opaque)
{
    virQEMUDriverPtr driver = opaque;
    virObjectEventPtr event = NULL;
    virObjectEventPtr lifecycleEvent = NULL;
    virQEMUDriverConfigPtr cfg = virQEMUDriverGetConfig(driver);

    virObjectLock(vm);
    event = virDomainEventPMSuspendNewFromObj(vm);

    if (virDomainObjGetState(vm, NULL) == VIR_DOMAIN_RUNNING) {
        qemuDomainObjPrivatePtr priv = vm->privateData;
        VIR_DEBUG("Transitioned guest %s to pmsuspended state due to "
                  "QMP suspend event", vm->def->name);

        virDomainObjSetState(vm, VIR_DOMAIN_PMSUSPENDED,
                             VIR_DOMAIN_PMSUSPENDED_UNKNOWN);
        lifecycleEvent =
            virDomainEventLifecycleNewFromObj(vm,
                                     VIR_DOMAIN_EVENT_PMSUSPENDED,
                                     VIR_DOMAIN_EVENT_PMSUSPENDED_MEMORY);

        if (virDomainSaveStatus(driver->xmlopt, cfg->stateDir, vm, driver->caps) < 0) {
            VIR_WARN("Unable to save status on vm %s after suspend event",
                     vm->def->name);
        }

        if (priv->agent)
            qemuAgentNotifyEvent(priv->agent, QEMU_AGENT_EVENT_SUSPEND);
    }

    virObjectUnlock(vm);

    qemuDomainEventQueue(driver, event);
    qemuDomainEventQueue(driver, lifecycleEvent);
    virObjectUnref(cfg);
    return 0;
}

static int
qemuProcessHandleBalloonChange(qemuMonitorPtr mon ATTRIBUTE_UNUSED,
                               virDomainObjPtr vm,
                               unsigned long long actual,
                               void *opaque)
{
    virQEMUDriverPtr driver = opaque;
    virObjectEventPtr event = NULL;
    virQEMUDriverConfigPtr cfg = virQEMUDriverGetConfig(driver);

    virObjectLock(vm);
    event = virDomainEventBalloonChangeNewFromObj(vm, actual);

    VIR_DEBUG("Updating balloon from %lld to %lld kb",
              vm->def->mem.cur_balloon, actual);
    vm->def->mem.cur_balloon = actual;

    if (virDomainSaveStatus(driver->xmlopt, cfg->stateDir, vm, driver->caps) < 0)
        VIR_WARN("unable to save domain status with balloon change");

    virObjectUnlock(vm);

    qemuDomainEventQueue(driver, event);
    virObjectUnref(cfg);
    return 0;
}

static int
qemuProcessHandlePMSuspendDisk(qemuMonitorPtr mon ATTRIBUTE_UNUSED,
                               virDomainObjPtr vm,
                               void *opaque)
{
    virQEMUDriverPtr driver = opaque;
    virObjectEventPtr event = NULL;
    virObjectEventPtr lifecycleEvent = NULL;
    virQEMUDriverConfigPtr cfg = virQEMUDriverGetConfig(driver);

    virObjectLock(vm);
    event = virDomainEventPMSuspendDiskNewFromObj(vm);

    if (virDomainObjGetState(vm, NULL) == VIR_DOMAIN_RUNNING) {
        qemuDomainObjPrivatePtr priv = vm->privateData;
        VIR_DEBUG("Transitioned guest %s to pmsuspended state due to "
                  "QMP suspend_disk event", vm->def->name);

        virDomainObjSetState(vm, VIR_DOMAIN_PMSUSPENDED,
                             VIR_DOMAIN_PMSUSPENDED_UNKNOWN);
        lifecycleEvent =
            virDomainEventLifecycleNewFromObj(vm,
                                     VIR_DOMAIN_EVENT_PMSUSPENDED,
                                     VIR_DOMAIN_EVENT_PMSUSPENDED_DISK);

        if (virDomainSaveStatus(driver->xmlopt, cfg->stateDir, vm, driver->caps) < 0) {
            VIR_WARN("Unable to save status on vm %s after suspend event",
                     vm->def->name);
        }

        if (priv->agent)
            qemuAgentNotifyEvent(priv->agent, QEMU_AGENT_EVENT_SUSPEND);
    }

    virObjectUnlock(vm);

    qemuDomainEventQueue(driver, event);
    qemuDomainEventQueue(driver, lifecycleEvent);
    virObjectUnref(cfg);

    return 0;
}


static int
qemuProcessHandleGuestPanic(qemuMonitorPtr mon ATTRIBUTE_UNUSED,
                            virDomainObjPtr vm,
                            void *opaque)
{
    virQEMUDriverPtr driver = opaque;
    struct qemuProcessEvent *processEvent;

    virObjectLock(vm);
    if (VIR_ALLOC(processEvent) < 0)
        goto cleanup;

    processEvent->eventType = QEMU_PROCESS_EVENT_GUESTPANIC;
    processEvent->action = vm->def->onCrash;
    processEvent->vm = vm;
    /* Hold an extra reference because we can't allow 'vm' to be
     * deleted before handling guest panic event is finished.
     */
    virObjectRef(vm);
    if (virThreadPoolSendJob(driver->workerPool, 0, processEvent) < 0) {
        if (!virObjectUnref(vm))
            vm = NULL;
        VIR_FREE(processEvent);
    }

 cleanup:
    if (vm)
        virObjectUnlock(vm);

    return 0;
}


int
qemuProcessHandleDeviceDeleted(qemuMonitorPtr mon ATTRIBUTE_UNUSED,
                               virDomainObjPtr vm,
                               const char *devAlias,
                               void *opaque)
{
    virQEMUDriverPtr driver = opaque;
    struct qemuProcessEvent *processEvent = NULL;
    char *data;

    virObjectLock(vm);

    VIR_DEBUG("Device %s removed from domain %p %s",
              devAlias, vm, vm->def->name);

    if (qemuDomainSignalDeviceRemoval(vm, devAlias))
        goto cleanup;

    if (VIR_ALLOC(processEvent) < 0)
        goto error;

    processEvent->eventType = QEMU_PROCESS_EVENT_DEVICE_DELETED;
    if (VIR_STRDUP(data, devAlias) < 0)
        goto error;
    processEvent->data = data;
    processEvent->vm = vm;

    virObjectRef(vm);
    if (virThreadPoolSendJob(driver->workerPool, 0, processEvent) < 0) {
        ignore_value(virObjectUnref(vm));
        goto error;
    }

 cleanup:
    virObjectUnlock(vm);
    return 0;
 error:
    if (processEvent)
        VIR_FREE(processEvent->data);
    VIR_FREE(processEvent);
    goto cleanup;
}


static int
qemuProcessHandleNicRxFilterChanged(qemuMonitorPtr mon ATTRIBUTE_UNUSED,
                                    virDomainObjPtr vm,
                                    const char *devAlias,
                                    void *opaque)
{
    virQEMUDriverPtr driver = opaque;
    struct qemuProcessEvent *processEvent = NULL;
    char *data;

    virObjectLock(vm);

    VIR_DEBUG("Device %s RX Filter changed in domain %p %s",
              devAlias, vm, vm->def->name);

    if (VIR_ALLOC(processEvent) < 0)
        goto error;

    processEvent->eventType = QEMU_PROCESS_EVENT_NIC_RX_FILTER_CHANGED;
    if (VIR_STRDUP(data, devAlias) < 0)
        goto error;
    processEvent->data = data;
    processEvent->vm = vm;

    virObjectRef(vm);
    if (virThreadPoolSendJob(driver->workerPool, 0, processEvent) < 0) {
        ignore_value(virObjectUnref(vm));
        goto error;
    }

 cleanup:
    virObjectUnlock(vm);
    return 0;
 error:
    if (processEvent)
        VIR_FREE(processEvent->data);
    VIR_FREE(processEvent);
    goto cleanup;
}


static int
qemuProcessHandleSerialChanged(qemuMonitorPtr mon ATTRIBUTE_UNUSED,
                               virDomainObjPtr vm,
                               const char *devAlias,
                               bool connected,
                               void *opaque)
{
    virQEMUDriverPtr driver = opaque;
    struct qemuProcessEvent *processEvent = NULL;
    char *data;

    virObjectLock(vm);

    VIR_DEBUG("Serial port %s state changed to '%d' in domain %p %s",
              devAlias, connected, vm, vm->def->name);

    if (VIR_ALLOC(processEvent) < 0)
        goto error;

    processEvent->eventType = QEMU_PROCESS_EVENT_SERIAL_CHANGED;
    if (VIR_STRDUP(data, devAlias) < 0)
        goto error;
    processEvent->data = data;
    processEvent->action = connected;
    processEvent->vm = vm;

    virObjectRef(vm);
    if (virThreadPoolSendJob(driver->workerPool, 0, processEvent) < 0) {
        ignore_value(virObjectUnref(vm));
        goto error;
    }

 cleanup:
    virObjectUnlock(vm);
    return 0;
 error:
    if (processEvent)
        VIR_FREE(processEvent->data);
    VIR_FREE(processEvent);
    goto cleanup;
}


static int
qemuProcessHandleSpiceMigrated(qemuMonitorPtr mon ATTRIBUTE_UNUSED,
                               virDomainObjPtr vm,
                               void *opaque ATTRIBUTE_UNUSED)
{
    qemuDomainObjPrivatePtr priv;

    virObjectLock(vm);

    VIR_DEBUG("Spice migration completed for domain %p %s",
              vm, vm->def->name);

    priv = vm->privateData;
    if (priv->job.asyncJob != QEMU_ASYNC_JOB_MIGRATION_OUT) {
        VIR_DEBUG("got SPICE_MIGRATE_COMPLETED event without a migration job");
        goto cleanup;
    }

    priv->job.spiceMigrated = true;
    virDomainObjBroadcast(vm);

 cleanup:
    virObjectUnlock(vm);
    return 0;
}


static int
qemuProcessHandleMigrationStatus(qemuMonitorPtr mon ATTRIBUTE_UNUSED,
                                 virDomainObjPtr vm,
                                 int status,
                                 void *opaque ATTRIBUTE_UNUSED)
{
    qemuDomainObjPrivatePtr priv;

    virObjectLock(vm);

    VIR_DEBUG("Migration of domain %p %s changed state to %s",
              vm, vm->def->name,
              qemuMonitorMigrationStatusTypeToString(status));

    priv = vm->privateData;
    if (priv->job.asyncJob == QEMU_ASYNC_JOB_NONE) {
        VIR_DEBUG("got MIGRATION event without a migration job");
        goto cleanup;
    }

    priv->job.current->stats.status = status;
    virDomainObjBroadcast(vm);

 cleanup:
    virObjectUnlock(vm);
    return 0;
}


static int
qemuProcessHandleMigrationPass(qemuMonitorPtr mon ATTRIBUTE_UNUSED,
                               virDomainObjPtr vm,
                               int pass,
                               void *opaque)
{
    virQEMUDriverPtr driver = opaque;
    qemuDomainObjPrivatePtr priv;

    virObjectLock(vm);

    VIR_DEBUG("Migrating domain %p %s, iteration %d",
              vm, vm->def->name, pass);

    priv = vm->privateData;
    if (priv->job.asyncJob == QEMU_ASYNC_JOB_NONE) {
        VIR_DEBUG("got MIGRATION_PASS event without a migration job");
        goto cleanup;
    }

    priv->job.current->stats.ram_iteration = pass;
    virDomainObjBroadcast(vm);

    qemuDomainEventQueue(driver,
                         virDomainEventMigrationIterationNewFromObj(vm, pass));

 cleanup:
    virObjectUnlock(vm);
    return 0;
}


static qemuMonitorCallbacks monitorCallbacks = {
    .eofNotify = qemuProcessHandleMonitorEOF,
    .errorNotify = qemuProcessHandleMonitorError,
    .diskSecretLookup = qemuProcessFindVolumeQcowPassphrase,
    .domainEvent = qemuProcessHandleEvent,
    .domainShutdown = qemuProcessHandleShutdown,
    .domainStop = qemuProcessHandleStop,
    .domainResume = qemuProcessHandleResume,
    .domainReset = qemuProcessHandleReset,
    .domainRTCChange = qemuProcessHandleRTCChange,
    .domainWatchdog = qemuProcessHandleWatchdog,
    .domainIOError = qemuProcessHandleIOError,
    .domainGraphics = qemuProcessHandleGraphics,
    .domainBlockJob = qemuProcessHandleBlockJob,
    .domainTrayChange = qemuProcessHandleTrayChange,
    .domainPMWakeup = qemuProcessHandlePMWakeup,
    .domainPMSuspend = qemuProcessHandlePMSuspend,
    .domainBalloonChange = qemuProcessHandleBalloonChange,
    .domainPMSuspendDisk = qemuProcessHandlePMSuspendDisk,
    .domainGuestPanic = qemuProcessHandleGuestPanic,
    .domainDeviceDeleted = qemuProcessHandleDeviceDeleted,
    .domainNicRxFilterChanged = qemuProcessHandleNicRxFilterChanged,
    .domainSerialChange = qemuProcessHandleSerialChanged,
    .domainSpiceMigrated = qemuProcessHandleSpiceMigrated,
    .domainMigrationStatus = qemuProcessHandleMigrationStatus,
    .domainMigrationPass = qemuProcessHandleMigrationPass,
};

static void
qemuProcessMonitorReportLogError(qemuMonitorPtr mon,
                                 const char *msg,
                                 void *opaque);


static void
qemuProcessMonitorLogFree(void *opaque)
{
    qemuDomainLogContextPtr logCtxt = opaque;
    qemuDomainLogContextFree(logCtxt);
}

static int
qemuConnectMonitor(virQEMUDriverPtr driver, virDomainObjPtr vm, int asyncJob,
                   qemuDomainLogContextPtr logCtxt)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    int ret = -1;
    qemuMonitorPtr mon = NULL;

    if (virSecurityManagerSetDaemonSocketLabel(driver->securityManager,
                                               vm->def) < 0) {
        VIR_ERROR(_("Failed to set security context for monitor for %s"),
                  vm->def->name);
        return -1;
    }

    /* Hold an extra reference because we can't allow 'vm' to be
     * deleted unitl the monitor gets its own reference. */
    virObjectRef(vm);

    ignore_value(virTimeMillisNow(&priv->monStart));
    virObjectUnlock(vm);

    mon = qemuMonitorOpen(vm,
                          priv->monConfig,
                          priv->monJSON,
                          &monitorCallbacks,
                          driver);

    if (mon && logCtxt) {
        qemuDomainLogContextRef(logCtxt);
        qemuMonitorSetDomainLog(mon,
                                qemuProcessMonitorReportLogError,
                                logCtxt,
                                qemuProcessMonitorLogFree);
    }

    virObjectLock(vm);
    virObjectUnref(vm);
    priv->monStart = 0;

    if (!virDomainObjIsActive(vm)) {
        qemuMonitorClose(mon);
        mon = NULL;
    }
    priv->mon = mon;

    if (virSecurityManagerClearSocketLabel(driver->securityManager, vm->def) < 0) {
        VIR_ERROR(_("Failed to clear security context for monitor for %s"),
                  vm->def->name);
        return -1;
    }

    if (priv->mon == NULL) {
        VIR_INFO("Failed to connect monitor for %s", vm->def->name);
        return -1;
    }


    if (qemuDomainObjEnterMonitorAsync(driver, vm, asyncJob) < 0)
        return -1;

    if (qemuMonitorSetCapabilities(priv->mon) < 0)
        goto cleanup;

    if (virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_MONITOR_JSON) &&
        virQEMUCapsProbeQMP(priv->qemuCaps, priv->mon) < 0)
        goto cleanup;

    if (virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_MIGRATION_EVENT) &&
        qemuMonitorSetMigrationCapability(priv->mon,
                                          QEMU_MONITOR_MIGRATION_CAPS_EVENTS,
                                          true) < 0) {
        VIR_DEBUG("Cannot enable migration events; clearing capability");
        virQEMUCapsClear(priv->qemuCaps, QEMU_CAPS_MIGRATION_EVENT);
    }

    ret = 0;

 cleanup:
    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        ret = -1;
    return ret;
}


/**
 * qemuProcessReadLog: Read log file of a qemu VM
 * @logCtxt: the domain log context
 * @msg: pointer to buffer to store the read messages in
 *
 * Reads log of a qemu VM. Skips messages not produced by qemu or irrelevant
 * messages. Returns returns 0 on success or -1 on error
 */
static int
qemuProcessReadLog(qemuDomainLogContextPtr logCtxt, char **msg)
{
    char *buf;
    ssize_t got;
    char *eol;
    char *filter_next;

    if ((got = qemuDomainLogContextRead(logCtxt, &buf)) < 0)
        return -1;

    /* Filter out debug messages from intermediate libvirt process */
    filter_next = buf;
    while ((eol = strchr(filter_next, '\n'))) {
        *eol = '\0';
        if (virLogProbablyLogMessage(filter_next) ||
            STRPREFIX(filter_next, "char device redirected to")) {
            size_t skip = (eol + 1) - filter_next;
            memmove(filter_next, eol + 1, buf + got - eol);
            got -= skip;
        } else {
            filter_next = eol + 1;
            *eol = '\n';
        }
    }
    filter_next = NULL; /* silence false coverity warning */

    if (got > 0 &&
        buf[got - 1] == '\n') {
        buf[got - 1] = '\0';
        got--;
    }
    ignore_value(VIR_REALLOC_N_QUIET(buf, got + 1));
    *msg = buf;
    return 0;
}


static int
qemuProcessReportLogError(qemuDomainLogContextPtr logCtxt,
                          const char *msgprefix)
{
    char *logmsg = NULL;

    if (qemuProcessReadLog(logCtxt, &logmsg) < 0)
        return -1;

    virResetLastError();
    virReportError(VIR_ERR_INTERNAL_ERROR,
                   _("%s: %s"), msgprefix, logmsg);
    VIR_FREE(logmsg);
    return 0;
}


static void
qemuProcessMonitorReportLogError(qemuMonitorPtr mon ATTRIBUTE_UNUSED,
                                 const char *msg,
                                 void *opaque)
{
    qemuDomainLogContextPtr logCtxt = opaque;
    qemuProcessReportLogError(logCtxt, msg);
}


static int
qemuProcessLookupPTYs(virDomainDefPtr def,
                      virQEMUCapsPtr qemuCaps,
                      virDomainChrDefPtr *devices,
                      int count,
                      virHashTablePtr info)
{
    size_t i;

    for (i = 0; i < count; i++) {
        virDomainChrDefPtr chr = devices[i];
        bool chardevfmt = virQEMUCapsSupportsChardev(def, qemuCaps, chr);

        if (chr->source.type == VIR_DOMAIN_CHR_TYPE_PTY) {
            char id[32];
            qemuMonitorChardevInfoPtr entry;

            if (snprintf(id, sizeof(id), "%s%s",
                         chardevfmt ? "char" : "",
                         chr->info.alias) >= sizeof(id)) {
                virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                               _("failed to format device alias "
                                 "for PTY retrieval"));
                return -1;
            }

            entry = virHashLookup(info, id);
            if (!entry || !entry->ptyPath) {
                if (chr->source.data.file.path == NULL) {
                    /* neither the log output nor 'info chardev' had a
                     * pty path for this chardev, report an error
                     */
                    virReportError(VIR_ERR_INTERNAL_ERROR,
                                   _("no assigned pty for device %s"), id);
                    return -1;
                } else {
                    /* 'info chardev' had no pty path for this chardev,
                     * but the log output had, so we're fine
                     */
                    continue;
                }
            }

            VIR_FREE(chr->source.data.file.path);
            if (VIR_STRDUP(chr->source.data.file.path, entry->ptyPath) < 0)
                return -1;
        }
    }

    return 0;
}

static int
qemuProcessFindCharDevicePTYsMonitor(virDomainObjPtr vm,
                                     virQEMUCapsPtr qemuCaps,
                                     virHashTablePtr info)
{
    size_t i = 0;

    if (qemuProcessLookupPTYs(vm->def, qemuCaps,
                              vm->def->serials, vm->def->nserials,
                              info) < 0)
        return -1;

    if (qemuProcessLookupPTYs(vm->def, qemuCaps,
                              vm->def->parallels, vm->def->nparallels,
                              info) < 0)
        return -1;

    if (qemuProcessLookupPTYs(vm->def, qemuCaps,
                              vm->def->channels, vm->def->nchannels,
                              info) < 0)
        return -1;
    /* For historical reasons, console[0] can be just an alias
     * for serial[0]. That's why we need to update it as well. */
    if (vm->def->nconsoles) {
        virDomainChrDefPtr chr = vm->def->consoles[0];

        if (vm->def->nserials &&
            chr->deviceType == VIR_DOMAIN_CHR_DEVICE_TYPE_CONSOLE &&
            chr->targetType == VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_SERIAL) {
            /* yes, the first console is just an alias for serials[0] */
            i = 1;
            if (virDomainChrSourceDefCopy(&chr->source,
                                          &((vm->def->serials[0])->source)) < 0)
                return -1;
        }
    }

    if (qemuProcessLookupPTYs(vm->def, qemuCaps,
                              vm->def->consoles + i, vm->def->nconsoles - i,
                              info) < 0)
        return -1;

    return 0;
}


static int
qemuProcessRefreshChannelVirtioState(virQEMUDriverPtr driver,
                                     virDomainObjPtr vm,
                                     virHashTablePtr info,
                                     int booted)
{
    size_t i;
    int agentReason = VIR_CONNECT_DOMAIN_EVENT_AGENT_LIFECYCLE_REASON_CHANNEL;
    qemuMonitorChardevInfoPtr entry;
    virObjectEventPtr event = NULL;
    char id[32];

    if (booted)
        agentReason = VIR_CONNECT_DOMAIN_EVENT_AGENT_LIFECYCLE_REASON_DOMAIN_STARTED;

    for (i = 0; i < vm->def->nchannels; i++) {
        virDomainChrDefPtr chr = vm->def->channels[i];
        if (chr->targetType == VIR_DOMAIN_CHR_CHANNEL_TARGET_TYPE_VIRTIO) {
            if (snprintf(id, sizeof(id), "char%s",
                         chr->info.alias) >= sizeof(id)) {
                virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                               _("failed to format device alias "
                                 "for PTY retrieval"));
                return -1;
            }

            /* port state not reported */
            if (!(entry = virHashLookup(info, id)) ||
                !entry->state)
                continue;

            if (entry->state != VIR_DOMAIN_CHR_DEVICE_STATE_DEFAULT &&
                STREQ_NULLABLE(chr->target.name, "org.qemu.guest_agent.0") &&
                (event = virDomainEventAgentLifecycleNewFromObj(vm, entry->state,
                                                                agentReason)))
                qemuDomainEventQueue(driver, event);

            chr->state = entry->state;
        }
    }

    return 0;
}


int
qemuRefreshVirtioChannelState(virQEMUDriverPtr driver,
                              virDomainObjPtr vm)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virHashTablePtr info = NULL;
    int ret = -1;

    qemuDomainObjEnterMonitor(driver, vm);
    ret = qemuMonitorGetChardevInfo(priv->mon, &info);
    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        ret = -1;

    if (ret < 0)
        goto cleanup;

    ret = qemuProcessRefreshChannelVirtioState(driver, vm, info, false);

 cleanup:
    virHashFree(info);
    return ret;
}


static int
qemuProcessRefreshBalloonState(virQEMUDriverPtr driver,
                               virDomainObjPtr vm,
                               int asyncJob)
{
    unsigned long long balloon;
    int rc;

    /* if no ballooning is available, the current size equals to the current
     * full memory size */
    if (!vm->def->memballoon ||
        vm->def->memballoon->model == VIR_DOMAIN_MEMBALLOON_MODEL_NONE) {
        vm->def->mem.cur_balloon = virDomainDefGetMemoryActual(vm->def);
        return 0;
    }

    if (qemuDomainObjEnterMonitorAsync(driver, vm, asyncJob) < 0)
        return -1;

    rc = qemuMonitorGetBalloonInfo(qemuDomainGetMonitor(vm), &balloon);
    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        rc = -1;

    if (rc < 0)
        return -1;

    vm->def->mem.cur_balloon = balloon;

    return 0;
}


static int
qemuProcessWaitForMonitor(virQEMUDriverPtr driver,
                          virDomainObjPtr vm,
                          int asyncJob,
                          virQEMUCapsPtr qemuCaps,
                          qemuDomainLogContextPtr logCtxt)
{
    int ret = -1;
    virHashTablePtr info = NULL;
    qemuDomainObjPrivatePtr priv;

    VIR_DEBUG("Connect monitor to %p '%s'", vm, vm->def->name);
    if (qemuConnectMonitor(driver, vm, asyncJob, logCtxt) < 0)
        goto cleanup;

    /* Try to get the pty path mappings again via the monitor. This is much more
     * reliable if it's available.
     * Note that the monitor itself can be on a pty, so we still need to try the
     * log output method. */
    priv = vm->privateData;
    if (qemuDomainObjEnterMonitorAsync(driver, vm, asyncJob) < 0)
        goto cleanup;
    ret = qemuMonitorGetChardevInfo(priv->mon, &info);
    VIR_DEBUG("qemuMonitorGetChardevInfo returned %i", ret);
    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        ret = -1;

    if (ret == 0) {
        if ((ret = qemuProcessFindCharDevicePTYsMonitor(vm, qemuCaps,
                                                        info)) < 0)
            goto cleanup;

        if ((ret = qemuProcessRefreshChannelVirtioState(driver, vm, info,
                                                        true)) < 0)
            goto cleanup;
    }

 cleanup:
    virHashFree(info);

    if (logCtxt && kill(vm->pid, 0) == -1 && errno == ESRCH) {
        qemuProcessReportLogError(logCtxt,
                                  _("process exited while connecting to monitor"));
        ret = -1;
    }

    return ret;
}


static int
qemuProcessDetectIOThreadPIDs(virQEMUDriverPtr driver,
                              virDomainObjPtr vm,
                              int asyncJob)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    qemuMonitorIOThreadInfoPtr *iothreads = NULL;
    int niothreads = 0;
    int ret = -1;
    size_t i;

    if (!virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_OBJECT_IOTHREAD)) {
        /* The following check is because at one time a domain could
         * define iothreadids and start the domain - only failing the
         * capability check when attempting to add a disk. Because the
         * iothreads and [n]iothreadids were left untouched other code
         * assumed it could use the ->thread_id value to make thread_id
         * based adjustments (e.g. pinning, scheduling) which while
         * succeeding would execute on the calling thread.
         */
        if (vm->def->niothreadids) {
            for (i = 0; i < vm->def->niothreadids; i++) {
                /* Check if the domain had defined any iothreadid elements
                 * and supply a VIR_INFO indicating that it's being removed.
                 */
                if (!vm->def->iothreadids[i]->autofill)
                    VIR_INFO("IOThreads not supported, remove iothread id '%u'",
                             vm->def->iothreadids[i]->iothread_id);
                virDomainIOThreadIDDefFree(vm->def->iothreadids[i]);
            }
            /* Remove any trace */
            VIR_FREE(vm->def->iothreadids);
            vm->def->niothreadids = 0;
            vm->def->iothreads = 0;
        }
        return 0;
    }

    /* Get the list of IOThreads from qemu */
    if (qemuDomainObjEnterMonitorAsync(driver, vm, asyncJob) < 0)
        goto cleanup;
    niothreads = qemuMonitorGetIOThreads(priv->mon, &iothreads);
    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        goto cleanup;
    if (niothreads < 0)
        goto cleanup;

    if (niothreads != vm->def->niothreadids) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("got wrong number of IOThread pids from QEMU monitor. "
                         "got %d, wanted %zu"),
                       niothreads, vm->def->niothreadids);
        goto cleanup;
    }

    /* Nothing to do */
    if (niothreads == 0) {
        ret = 0;
        goto cleanup;
    }

    for (i = 0; i < niothreads; i++) {
        virDomainIOThreadIDDefPtr iothrid;

        if (!(iothrid = virDomainIOThreadIDFind(vm->def,
                                                iothreads[i]->iothread_id))) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("iothread %d not found"),
                           iothreads[i]->iothread_id);
            goto cleanup;
        }
        iothrid->thread_id = iothreads[i]->thread_id;
    }

    ret = 0;

 cleanup:
    if (iothreads) {
        for (i = 0; i < niothreads; i++)
            VIR_FREE(iothreads[i]);
        VIR_FREE(iothreads);
    }
    return ret;
}


/*
 * To be run between fork/exec of QEMU only
 */
static int
qemuProcessInitCpuAffinity(virDomainObjPtr vm)
{
    int ret = -1;
    virBitmapPtr cpumap = NULL;
    virBitmapPtr cpumapToSet = NULL;
    qemuDomainObjPrivatePtr priv = vm->privateData;

    if (!vm->pid) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Cannot setup CPU affinity until process is started"));
        return -1;
    }

    if (vm->def->placement_mode == VIR_DOMAIN_CPU_PLACEMENT_MODE_AUTO) {
        VIR_DEBUG("Set CPU affinity with advisory nodeset from numad");
        cpumapToSet = priv->autoCpuset;
    } else {
        VIR_DEBUG("Set CPU affinity with specified cpuset");
        if (vm->def->cpumask) {
            cpumapToSet = vm->def->cpumask;
        } else {
            /* You may think this is redundant, but we can't assume libvirtd
             * itself is running on all pCPUs, so we need to explicitly set
             * the spawned QEMU instance to all pCPUs if no map is given in
             * its config file */
            int hostcpus;

            /* setaffinity fails if you set bits for CPUs which
             * aren't present, so we have to limit ourselves */
            if ((hostcpus = nodeGetCPUCount(NULL)) < 0)
                goto cleanup;

            if (hostcpus > QEMUD_CPUMASK_LEN)
                hostcpus = QEMUD_CPUMASK_LEN;

            if (!(cpumap = virBitmapNew(hostcpus)))
                goto cleanup;

            virBitmapSetAll(cpumap);

            cpumapToSet = cpumap;
        }
    }

    if (virProcessSetAffinity(vm->pid, cpumapToSet) < 0)
        goto cleanup;

    ret = 0;

 cleanup:
    virBitmapFree(cpumap);
    return ret;
}

/* set link states to down on interfaces at qemu start */
static int
qemuProcessSetLinkStates(virQEMUDriverPtr driver,
                         virDomainObjPtr vm,
                         qemuDomainAsyncJob asyncJob)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virDomainDefPtr def = vm->def;
    size_t i;
    int ret = -1;
    int rv;

    if (qemuDomainObjEnterMonitorAsync(driver, vm, asyncJob) < 0)
        return -1;

    for (i = 0; i < def->nnets; i++) {
        if (def->nets[i]->linkstate == VIR_DOMAIN_NET_INTERFACE_LINK_STATE_DOWN) {
            if (!def->nets[i]->info.alias) {
                virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                               _("missing alias for network device"));
                goto cleanup;
            }

            VIR_DEBUG("Setting link state: %s", def->nets[i]->info.alias);

            if (!virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_NETDEV)) {
                virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                               _("Setting of link state is not supported by this qemu"));
                goto cleanup;
            }

            rv = qemuMonitorSetLink(priv->mon,
                                    def->nets[i]->info.alias,
                                    VIR_DOMAIN_NET_INTERFACE_LINK_STATE_DOWN);
            if (rv < 0) {
                virReportError(VIR_ERR_OPERATION_FAILED,
                               _("Couldn't set link state on interface: %s"),
                               def->nets[i]->info.alias);
                goto cleanup;
            }
        }
    }

    ret = 0;

 cleanup:
    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        ret = -1;
    return ret;
}


/* Set CPU affinities for emulator threads. */
static int
qemuProcessSetEmulatorAffinity(virDomainObjPtr vm)
{
    virBitmapPtr cpumask;
    virDomainDefPtr def = vm->def;
    int ret = -1;

    if (def->cputune.emulatorpin)
        cpumask = def->cputune.emulatorpin;
    else if (def->cpumask)
        cpumask = def->cpumask;
    else
        return 0;

    ret = virProcessSetAffinity(vm->pid, cpumask);
    return ret;
}


static int
qemuProcessInitPasswords(virConnectPtr conn,
                         virQEMUDriverPtr driver,
                         virDomainObjPtr vm,
                         int asyncJob)
{
    int ret = 0;
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virQEMUDriverConfigPtr cfg = virQEMUDriverGetConfig(driver);
    size_t i;
    char *alias = NULL;
    char *secret = NULL;

    for (i = 0; i < vm->def->ngraphics; ++i) {
        virDomainGraphicsDefPtr graphics = vm->def->graphics[i];
        if (graphics->type == VIR_DOMAIN_GRAPHICS_TYPE_VNC) {
            ret = qemuDomainChangeGraphicsPasswords(driver, vm,
                                                    VIR_DOMAIN_GRAPHICS_TYPE_VNC,
                                                    &graphics->data.vnc.auth,
                                                    cfg->vncPassword,
                                                    asyncJob);
        } else if (graphics->type == VIR_DOMAIN_GRAPHICS_TYPE_SPICE) {
            ret = qemuDomainChangeGraphicsPasswords(driver, vm,
                                                    VIR_DOMAIN_GRAPHICS_TYPE_SPICE,
                                                    &graphics->data.spice.auth,
                                                    cfg->spicePassword,
                                                    asyncJob);
        }

        if (ret < 0)
            goto cleanup;
    }

    if (virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_DEVICE)) {
        for (i = 0; i < vm->def->ndisks; i++) {
            size_t secretLen;

            if (!vm->def->disks[i]->src->encryption ||
                !virDomainDiskGetSource(vm->def->disks[i]))
                continue;

            VIR_FREE(secret);
            if (qemuProcessGetVolumeQcowPassphrase(conn,
                                                   vm->def->disks[i],
                                                   &secret, &secretLen) < 0)
                goto cleanup;

            VIR_FREE(alias);
            if (VIR_STRDUP(alias, vm->def->disks[i]->info.alias) < 0)
                goto cleanup;
            if (qemuDomainObjEnterMonitorAsync(driver, vm, asyncJob) < 0)
                goto cleanup;
            ret = qemuMonitorSetDrivePassphrase(priv->mon, alias, secret);
            if (qemuDomainObjExitMonitor(driver, vm) < 0)
                ret = -1;
            if (ret < 0)
                goto cleanup;
        }
    }

 cleanup:
    VIR_FREE(alias);
    VIR_FREE(secret);
    virObjectUnref(cfg);
    return ret;
}


#define QEMU_PCI_VENDOR_INTEL     0x8086
#define QEMU_PCI_VENDOR_LSI_LOGIC 0x1000
#define QEMU_PCI_VENDOR_REDHAT    0x1af4
#define QEMU_PCI_VENDOR_CIRRUS    0x1013
#define QEMU_PCI_VENDOR_REALTEK   0x10ec
#define QEMU_PCI_VENDOR_AMD       0x1022
#define QEMU_PCI_VENDOR_ENSONIQ   0x1274
#define QEMU_PCI_VENDOR_VMWARE    0x15ad
#define QEMU_PCI_VENDOR_QEMU      0x1234

#define QEMU_PCI_PRODUCT_DISK_VIRTIO 0x1001

#define QEMU_PCI_PRODUCT_BALLOON_VIRTIO 0x1002

#define QEMU_PCI_PRODUCT_NIC_NE2K     0x8029
#define QEMU_PCI_PRODUCT_NIC_PCNET    0x2000
#define QEMU_PCI_PRODUCT_NIC_RTL8139  0x8139
#define QEMU_PCI_PRODUCT_NIC_E1000    0x100E
#define QEMU_PCI_PRODUCT_NIC_VIRTIO   0x1000

#define QEMU_PCI_PRODUCT_VGA_CIRRUS 0x00b8
#define QEMU_PCI_PRODUCT_VGA_VMWARE 0x0405
#define QEMU_PCI_PRODUCT_VGA_STDVGA 0x1111

#define QEMU_PCI_PRODUCT_AUDIO_AC97    0x2415
#define QEMU_PCI_PRODUCT_AUDIO_ES1370  0x5000

#define QEMU_PCI_PRODUCT_CONTROLLER_PIIX 0x7010
#define QEMU_PCI_PRODUCT_CONTROLLER_LSI  0x0012

#define QEMU_PCI_PRODUCT_WATCHDOG_I63000ESB 0x25ab

static int
qemuProcessAssignNextPCIAddress(virDomainDeviceInfo *info,
                                int vendor,
                                int product,
                                qemuMonitorPCIAddress *addrs,
                                int naddrs)
{
    bool found = false;
    size_t i;

    VIR_DEBUG("Look for %x:%x out of %d", vendor, product, naddrs);

    for (i = 0; i < naddrs; i++) {
        VIR_DEBUG("Maybe %x:%x", addrs[i].vendor, addrs[i].product);
        if (addrs[i].vendor == vendor &&
            addrs[i].product == product) {
            VIR_DEBUG("Match %zu", i);
            found = true;
            break;
        }
    }
    if (!found)
        return -1;

    /* Blank it out so this device isn't matched again */
    addrs[i].vendor = 0;
    addrs[i].product = 0;

    if (info->type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE)
        info->type = VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI;

    if (info->type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI) {
        info->addr.pci.domain = addrs[i].addr.domain;
        info->addr.pci.bus = addrs[i].addr.bus;
        info->addr.pci.slot = addrs[i].addr.slot;
        info->addr.pci.function = addrs[i].addr.function;
    }

    return 0;
}

static int
qemuProcessGetPCIDiskVendorProduct(virDomainDiskDefPtr def,
                                   unsigned *vendor,
                                   unsigned *product)
{
    switch (def->bus) {
    case VIR_DOMAIN_DISK_BUS_VIRTIO:
        *vendor = QEMU_PCI_VENDOR_REDHAT;
        *product = QEMU_PCI_PRODUCT_DISK_VIRTIO;
        break;

    default:
        return -1;
    }

    return 0;
}

static int
qemuProcessGetPCINetVendorProduct(virDomainNetDefPtr def,
                                  unsigned *vendor,
                                  unsigned *product)
{
    if (!def->model)
        return -1;

    if (STREQ(def->model, "ne2k_pci")) {
        *vendor = QEMU_PCI_VENDOR_REALTEK;
        *product = QEMU_PCI_PRODUCT_NIC_NE2K;
    } else if (STREQ(def->model, "pcnet")) {
        *vendor = QEMU_PCI_VENDOR_AMD;
        *product = QEMU_PCI_PRODUCT_NIC_PCNET;
    } else if (STREQ(def->model, "rtl8139")) {
        *vendor = QEMU_PCI_VENDOR_REALTEK;
        *product = QEMU_PCI_PRODUCT_NIC_RTL8139;
    } else if (STREQ(def->model, "e1000")) {
        *vendor = QEMU_PCI_VENDOR_INTEL;
        *product = QEMU_PCI_PRODUCT_NIC_E1000;
    } else if (STREQ(def->model, "virtio")) {
        *vendor = QEMU_PCI_VENDOR_REDHAT;
        *product = QEMU_PCI_PRODUCT_NIC_VIRTIO;
    } else {
        VIR_INFO("Unexpected NIC model %s, cannot get PCI address",
                 def->model);
        return -1;
    }
    return 0;
}

static int
qemuProcessGetPCIControllerVendorProduct(virDomainControllerDefPtr def,
                                         unsigned *vendor,
                                         unsigned *product)
{
    switch (def->type) {
    case VIR_DOMAIN_CONTROLLER_TYPE_SCSI:
        *vendor = QEMU_PCI_VENDOR_LSI_LOGIC;
        *product = QEMU_PCI_PRODUCT_CONTROLLER_LSI;
        break;

    case VIR_DOMAIN_CONTROLLER_TYPE_FDC:
        /* XXX we could put in the ISA bridge address, but
           that's not technically the FDC's address */
        return -1;

    case VIR_DOMAIN_CONTROLLER_TYPE_IDE:
        *vendor = QEMU_PCI_VENDOR_INTEL;
        *product = QEMU_PCI_PRODUCT_CONTROLLER_PIIX;
        break;

    default:
        VIR_INFO("Unexpected controller type %s, cannot get PCI address",
                 virDomainControllerTypeToString(def->type));
        return -1;
    }

    return 0;
}

static int
qemuProcessGetPCIVideoVendorProduct(virDomainVideoDefPtr def,
                                    unsigned *vendor,
                                    unsigned *product)
{
    switch (def->type) {
    case VIR_DOMAIN_VIDEO_TYPE_CIRRUS:
        *vendor = QEMU_PCI_VENDOR_CIRRUS;
        *product = QEMU_PCI_PRODUCT_VGA_CIRRUS;
        break;

    case VIR_DOMAIN_VIDEO_TYPE_VGA:
        *vendor = QEMU_PCI_VENDOR_QEMU;
        *product = QEMU_PCI_PRODUCT_VGA_STDVGA;
        break;

    case VIR_DOMAIN_VIDEO_TYPE_VMVGA:
        *vendor = QEMU_PCI_VENDOR_VMWARE;
        *product = QEMU_PCI_PRODUCT_VGA_VMWARE;
        break;

    default:
        return -1;
    }
    return 0;
}

static int
qemuProcessGetPCISoundVendorProduct(virDomainSoundDefPtr def,
                                    unsigned *vendor,
                                    unsigned *product)
{
    switch (def->model) {
    case VIR_DOMAIN_SOUND_MODEL_ES1370:
        *vendor = QEMU_PCI_VENDOR_ENSONIQ;
        *product = QEMU_PCI_PRODUCT_AUDIO_ES1370;
        break;

    case VIR_DOMAIN_SOUND_MODEL_AC97:
        *vendor = QEMU_PCI_VENDOR_INTEL;
        *product = QEMU_PCI_PRODUCT_AUDIO_AC97;
        break;

    default:
        return -1;
    }

    return 0;
}

static int
qemuProcessGetPCIWatchdogVendorProduct(virDomainWatchdogDefPtr def,
                                       unsigned *vendor,
                                       unsigned *product)
{
    switch (def->model) {
    case VIR_DOMAIN_WATCHDOG_MODEL_I6300ESB:
        *vendor = QEMU_PCI_VENDOR_INTEL;
        *product = QEMU_PCI_PRODUCT_WATCHDOG_I63000ESB;
        break;

    default:
        return -1;
    }

    return 0;
}


static int
qemuProcessGetPCIMemballoonVendorProduct(virDomainMemballoonDefPtr def,
                                         unsigned *vendor,
                                         unsigned *product)
{
    switch (def->model) {
    case VIR_DOMAIN_MEMBALLOON_MODEL_VIRTIO:
        *vendor = QEMU_PCI_VENDOR_REDHAT;
        *product = QEMU_PCI_PRODUCT_BALLOON_VIRTIO;
        break;

    default:
        return -1;
    }

    return 0;
}


/*
 * This entire method assumes that PCI devices in 'info pci'
 * match ordering of devices specified on the command line
 * wrt to devices of matching vendor+product
 *
 * XXXX this might not be a valid assumption if we assign
 * some static addrs on CLI. Have to check that...
 */
static int
qemuProcessDetectPCIAddresses(virDomainObjPtr vm,
                              qemuMonitorPCIAddress *addrs,
                              int naddrs)
{
    unsigned int vendor = 0, product = 0;
    size_t i;

    /* XXX should all these vendor/product IDs be kept in the
     * actual device data structure instead ?
     */

    for (i = 0; i < vm->def->ndisks; i++) {
        if (qemuProcessGetPCIDiskVendorProduct(vm->def->disks[i], &vendor, &product) < 0)
            continue;

        if (qemuProcessAssignNextPCIAddress(&(vm->def->disks[i]->info),
                                            vendor, product,
                                            addrs, naddrs) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("cannot find PCI address for VirtIO disk %s"),
                           vm->def->disks[i]->dst);
            return -1;
        }
    }

    for (i = 0; i < vm->def->nnets; i++) {
        if (qemuProcessGetPCINetVendorProduct(vm->def->nets[i], &vendor, &product) < 0)
            continue;

        if (qemuProcessAssignNextPCIAddress(&(vm->def->nets[i]->info),
                                            vendor, product,
                                            addrs,  naddrs) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("cannot find PCI address for %s NIC"),
                           vm->def->nets[i]->model);
            return -1;
        }
    }

    for (i = 0; i < vm->def->ncontrollers; i++) {
        if (qemuProcessGetPCIControllerVendorProduct(vm->def->controllers[i], &vendor, &product) < 0)
            continue;

        if (qemuProcessAssignNextPCIAddress(&(vm->def->controllers[i]->info),
                                            vendor, product,
                                            addrs,  naddrs) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("cannot find PCI address for controller %s"),
                           virDomainControllerTypeToString(vm->def->controllers[i]->type));
            return -1;
        }
    }

    for (i = 0; i < vm->def->nvideos; i++) {
        if (qemuProcessGetPCIVideoVendorProduct(vm->def->videos[i], &vendor, &product) < 0)
            continue;

        if (qemuProcessAssignNextPCIAddress(&(vm->def->videos[i]->info),
                                            vendor, product,
                                            addrs,  naddrs) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("cannot find PCI address for video adapter %s"),
                           virDomainVideoTypeToString(vm->def->videos[i]->type));
            return -1;
        }
    }

    for (i = 0; i < vm->def->nsounds; i++) {
        if (qemuProcessGetPCISoundVendorProduct(vm->def->sounds[i], &vendor, &product) < 0)
            continue;

        if (qemuProcessAssignNextPCIAddress(&(vm->def->sounds[i]->info),
                                    vendor, product,
                                     addrs,  naddrs) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("cannot find PCI address for sound adapter %s"),
                           virDomainSoundModelTypeToString(vm->def->sounds[i]->model));
            return -1;
        }
    }


    if (vm->def->watchdog &&
        qemuProcessGetPCIWatchdogVendorProduct(vm->def->watchdog, &vendor, &product) == 0) {
        if (qemuProcessAssignNextPCIAddress(&(vm->def->watchdog->info),
                                            vendor, product,
                                            addrs,  naddrs) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("cannot find PCI address for watchdog %s"),
                           virDomainWatchdogModelTypeToString(vm->def->watchdog->model));
            return -1;
        }
    }

    if (vm->def->memballoon &&
        qemuProcessGetPCIMemballoonVendorProduct(vm->def->memballoon, &vendor, &product) == 0) {
        if (qemuProcessAssignNextPCIAddress(&(vm->def->memballoon->info),
                                            vendor, product,
                                            addrs, naddrs) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("cannot find PCI address for balloon %s"),
                           virDomainMemballoonModelTypeToString(vm->def->memballoon->model));
            return -1;
        }
    }

    /* XXX console (virtio) */


    /* ... and now things we don't have in our xml */

    /* XXX USB controller ? */

    /* XXX what about other PCI devices (ie bridges) */

    return 0;
}

static int
qemuProcessInitPCIAddresses(virQEMUDriverPtr driver,
                            virDomainObjPtr vm,
                            int asyncJob)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    int naddrs;
    int ret = -1;
    qemuMonitorPCIAddress *addrs = NULL;

    if (qemuDomainObjEnterMonitorAsync(driver, vm, asyncJob) < 0)
        return -1;
    naddrs = qemuMonitorGetAllPCIAddresses(priv->mon,
                                           &addrs);
    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        goto cleanup;

    if (naddrs > 0)
        ret = qemuProcessDetectPCIAddresses(vm, addrs, naddrs);

 cleanup:
    VIR_FREE(addrs);

    return ret;
}


static int
qemuProcessPrepareChardevDevice(virDomainDefPtr def ATTRIBUTE_UNUSED,
                                virDomainChrDefPtr dev,
                                void *opaque ATTRIBUTE_UNUSED)
{
    int fd;
    if (dev->source.type != VIR_DOMAIN_CHR_TYPE_FILE)
        return 0;

    if ((fd = open(dev->source.data.file.path,
                   O_CREAT | O_APPEND, S_IRUSR|S_IWUSR)) < 0) {
        virReportSystemError(errno,
                             _("Unable to pre-create chardev file '%s'"),
                             dev->source.data.file.path);
        return -1;
    }

    VIR_FORCE_CLOSE(fd);

    return 0;
}


static int
qemuProcessCleanupChardevDevice(virDomainDefPtr def ATTRIBUTE_UNUSED,
                                virDomainChrDefPtr dev,
                                void *opaque ATTRIBUTE_UNUSED)
{
    if (dev->source.type == VIR_DOMAIN_CHR_TYPE_UNIX &&
        dev->source.data.nix.listen &&
        dev->source.data.nix.path)
        unlink(dev->source.data.nix.path);

    return 0;
}


/**
 * Loads and update video memory size for video devices according to QEMU
 * process as the QEMU will silently update the values that we pass to QEMU
 * through command line.  We need to load these updated values and store them
 * into the status XML.
 *
 * We will fail if for some reason the values cannot be loaded from QEMU because
 * its mandatory to get the correct video memory size to status XML to not break
 * migration.
 */
static int
qemuProcessUpdateVideoRamSize(virQEMUDriverPtr driver,
                              virDomainObjPtr vm,
                              int asyncJob)
{
    int ret = -1;
    ssize_t i;
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virDomainVideoDefPtr video = NULL;
    virQEMUDriverConfigPtr cfg = NULL;

    if (qemuDomainObjEnterMonitorAsync(driver, vm, asyncJob) < 0)
        return -1;

    for (i = 0; i < vm->def->nvideos; i++) {
        video = vm->def->videos[i];

        switch (video->type) {
        case VIR_DOMAIN_VIDEO_TYPE_VGA:
            if (virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_VGA_VGAMEM)) {
                if (qemuMonitorUpdateVideoMemorySize(priv->mon, video, "VGA") < 0)
                    goto error;
            }
            break;
        case VIR_DOMAIN_VIDEO_TYPE_QXL:
            if (i == 0) {
                if (virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_QXL_VGA_VGAMEM)) {
                    if (qemuMonitorUpdateVideoMemorySize(priv->mon, video,
                                                         "qxl-vga") < 0)
                        goto error;
                }
            } else {
                if (virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_QXL_VGAMEM)) {
                    if (qemuMonitorUpdateVideoMemorySize(priv->mon, video,
                                                         "qxl") < 0)
                        goto error;
                }
            }
            break;
        case VIR_DOMAIN_VIDEO_TYPE_VMVGA:
            if (virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_VMWARE_SVGA_VGAMEM)) {
                if (qemuMonitorUpdateVideoMemorySize(priv->mon, video,
                                                     "vmware-svga") < 0)
                    goto error;
            }
            break;
        case VIR_DOMAIN_VIDEO_TYPE_CIRRUS:
        case VIR_DOMAIN_VIDEO_TYPE_XEN:
        case VIR_DOMAIN_VIDEO_TYPE_VBOX:
        case VIR_DOMAIN_VIDEO_TYPE_LAST:
            break;
        }

    }

    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        return -1;

    cfg = virQEMUDriverGetConfig(driver);
    ret = virDomainSaveStatus(driver->xmlopt, cfg->stateDir, vm, driver->caps);
    virObjectUnref(cfg);

    return ret;

 error:
    ignore_value(qemuDomainObjExitMonitor(driver, vm));
    return -1;
}


struct qemuProcessHookData {
    virConnectPtr conn;
    virDomainObjPtr vm;
    virQEMUDriverPtr driver;
    virQEMUDriverConfigPtr cfg;
};

static int qemuProcessHook(void *data)
{
    struct qemuProcessHookData *h = data;
    qemuDomainObjPrivatePtr priv = h->vm->privateData;
    int ret = -1;
    int fd;
    virBitmapPtr nodeset = NULL;
    virDomainNumatuneMemMode mode;

    /* This method cannot use any mutexes, which are not
     * protected across fork()
     */

    virSecurityManagerPostFork(h->driver->securityManager);

    /* Some later calls want pid present */
    h->vm->pid = getpid();

    VIR_DEBUG("Obtaining domain lock");
    /*
     * Since we're going to leak the returned FD to QEMU,
     * we need to make sure it gets a sensible label.
     * This mildly sucks, because there could be other
     * sockets the lock driver opens that we don't want
     * labelled. So far we're ok though.
     */
    if (virSecurityManagerSetSocketLabel(h->driver->securityManager, h->vm->def) < 0)
        goto cleanup;
    if (virDomainLockProcessStart(h->driver->lockManager,
                                  h->cfg->uri,
                                  h->vm,
                                  /* QEMU is always paused initially */
                                  true,
                                  &fd) < 0)
        goto cleanup;
    if (virSecurityManagerClearSocketLabel(h->driver->securityManager, h->vm->def) < 0)
        goto cleanup;

    if (virDomainNumatuneGetMode(h->vm->def->numa, -1, &mode) == 0) {
        if (mode == VIR_DOMAIN_NUMATUNE_MEM_STRICT &&
            h->cfg->cgroupControllers & (1 << VIR_CGROUP_CONTROLLER_CPUSET) &&
            virCgroupControllerAvailable(VIR_CGROUP_CONTROLLER_CPUSET)) {
            /* Use virNuma* API iff necessary. Once set and child is exec()-ed,
             * there's no way for us to change it. Rely on cgroups (if available
             * and enabled in the config) rather than virNuma*. */
            VIR_DEBUG("Relying on CGroups for memory binding");
        } else {
            nodeset = virDomainNumatuneGetNodeset(h->vm->def->numa,
                                                  priv->autoNodeset, -1);

            if (virNumaSetupMemoryPolicy(mode, nodeset) < 0)
                goto cleanup;
        }
    }

    ret = 0;

 cleanup:
    virObjectUnref(h->cfg);
    VIR_DEBUG("Hook complete ret=%d", ret);
    return ret;
}

int
qemuProcessPrepareMonitorChr(virQEMUDriverConfigPtr cfg,
                             virDomainChrSourceDefPtr monConfig,
                             const char *vm)
{
    monConfig->type = VIR_DOMAIN_CHR_TYPE_UNIX;
    monConfig->data.nix.listen = true;

    if (virAsprintf(&monConfig->data.nix.path, "%s/domain-%s/monitor.sock",
                    cfg->libDir, vm) < 0)
        return -1;
    return 0;
}


/*
 * Precondition: vm must be locked, and a job must be active.
 * This method will call {Enter,Exit}Monitor
 */
int
qemuProcessStartCPUs(virQEMUDriverPtr driver, virDomainObjPtr vm,
                     virConnectPtr conn, virDomainRunningReason reason,
                     qemuDomainAsyncJob asyncJob)
{
    int ret = -1;
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virQEMUDriverConfigPtr cfg = virQEMUDriverGetConfig(driver);

    /* Bring up netdevs before starting CPUs */
    if (qemuInterfaceStartDevices(vm->def) < 0)
       goto cleanup;

    VIR_DEBUG("Using lock state '%s'", NULLSTR(priv->lockState));
    if (virDomainLockProcessResume(driver->lockManager, cfg->uri,
                                   vm, priv->lockState) < 0) {
        /* Don't free priv->lockState on error, because we need
         * to make sure we have state still present if the user
         * tries to resume again
         */
        goto cleanup;
    }
    VIR_FREE(priv->lockState);

    if (qemuDomainObjEnterMonitorAsync(driver, vm, asyncJob) < 0)
        goto release;

    ret = qemuMonitorStartCPUs(priv->mon, conn);
    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        ret = -1;

    if (ret < 0)
        goto release;

    virDomainObjSetState(vm, VIR_DOMAIN_RUNNING, reason);

 cleanup:
    virObjectUnref(cfg);
    return ret;

 release:
    if (virDomainLockProcessPause(driver->lockManager, vm, &priv->lockState) < 0)
        VIR_WARN("Unable to release lease on %s", vm->def->name);
    VIR_DEBUG("Preserving lock state '%s'", NULLSTR(priv->lockState));
    goto cleanup;
}


int qemuProcessStopCPUs(virQEMUDriverPtr driver,
                        virDomainObjPtr vm,
                        virDomainPausedReason reason,
                        qemuDomainAsyncJob asyncJob)
{
    int ret = -1;
    qemuDomainObjPrivatePtr priv = vm->privateData;

    VIR_FREE(priv->lockState);

    if (qemuDomainObjEnterMonitorAsync(driver, vm, asyncJob) < 0)
        goto cleanup;

    ret = qemuMonitorStopCPUs(priv->mon);
    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        ret = -1;

    if (ret < 0)
        goto cleanup;

    /* de-activate netdevs after stopping CPUs */
    ignore_value(qemuInterfaceStopDevices(vm->def));

    if (priv->job.current)
        ignore_value(virTimeMillisNow(&priv->job.current->stopped));

    virDomainObjSetState(vm, VIR_DOMAIN_PAUSED, reason);
    if (virDomainLockProcessPause(driver->lockManager, vm, &priv->lockState) < 0)
        VIR_WARN("Unable to release lease on %s", vm->def->name);
    VIR_DEBUG("Preserving lock state '%s'", NULLSTR(priv->lockState));

 cleanup:
    return ret;
}



static int
qemuProcessNotifyNets(virDomainDefPtr def)
{
    size_t i;

    for (i = 0; i < def->nnets; i++) {
        virDomainNetDefPtr net = def->nets[i];
        /* keep others from trying to use the macvtap device name, but
         * don't return error if this happens, since that causes the
         * domain to be unceremoniously killed, which would be *very*
         * impolite.
         */
        if (virDomainNetGetActualType(net) == VIR_DOMAIN_NET_TYPE_DIRECT)
           ignore_value(virNetDevMacVLanReserveName(net->ifname, false));

        if (networkNotifyActualDevice(def, net) < 0)
            return -1;
    }
    return 0;
}

static int
qemuProcessFiltersInstantiate(virDomainDefPtr def)
{
    size_t i;

    for (i = 0; i < def->nnets; i++) {
        virDomainNetDefPtr net = def->nets[i];
        if ((net->filter) && (net->ifname)) {
            if (virDomainConfNWFilterInstantiate(def->uuid, net) < 0)
                return 1;
        }
    }

    return 0;
}

static int
qemuProcessUpdateState(virQEMUDriverPtr driver, virDomainObjPtr vm)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virDomainState state;
    virDomainPausedReason reason;
    virDomainState newState = VIR_DOMAIN_NOSTATE;
    int oldReason;
    int newReason;
    bool running;
    char *msg = NULL;
    int ret;

    qemuDomainObjEnterMonitor(driver, vm);
    ret = qemuMonitorGetStatus(priv->mon, &running, &reason);
    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        return -1;

    if (ret < 0)
        return -1;

    state = virDomainObjGetState(vm, &oldReason);

    if (running &&
        (state == VIR_DOMAIN_SHUTOFF ||
         (state == VIR_DOMAIN_PAUSED &&
          oldReason == VIR_DOMAIN_PAUSED_STARTING_UP))) {
        newState = VIR_DOMAIN_RUNNING;
        newReason = VIR_DOMAIN_RUNNING_BOOTED;
        ignore_value(VIR_STRDUP_QUIET(msg, "finished booting"));
    } else if (state == VIR_DOMAIN_PAUSED && running) {
        newState = VIR_DOMAIN_RUNNING;
        newReason = VIR_DOMAIN_RUNNING_UNPAUSED;
        ignore_value(VIR_STRDUP_QUIET(msg, "was unpaused"));
    } else if (state == VIR_DOMAIN_RUNNING && !running) {
        if (reason == VIR_DOMAIN_PAUSED_SHUTTING_DOWN) {
            newState = VIR_DOMAIN_SHUTDOWN;
            newReason = VIR_DOMAIN_SHUTDOWN_UNKNOWN;
            ignore_value(VIR_STRDUP_QUIET(msg, "shutdown"));
        } else if (reason == VIR_DOMAIN_PAUSED_CRASHED) {
            newState = VIR_DOMAIN_CRASHED;
            newReason = VIR_DOMAIN_CRASHED_PANICKED;
            ignore_value(VIR_STRDUP_QUIET(msg, "crashed"));
        } else {
            newState = VIR_DOMAIN_PAUSED;
            newReason = reason;
            ignore_value(virAsprintf(&msg, "was paused (%s)",
                                 virDomainPausedReasonTypeToString(reason)));
        }
    }

    if (newState != VIR_DOMAIN_NOSTATE) {
        VIR_DEBUG("Domain %s %s while its monitor was disconnected;"
                  " changing state to %s (%s)",
                  vm->def->name,
                  NULLSTR(msg),
                  virDomainStateTypeToString(newState),
                  virDomainStateReasonToString(newState, newReason));
        VIR_FREE(msg);
        virDomainObjSetState(vm, newState, newReason);
    }

    return 0;
}

static int
qemuProcessRecoverMigrationIn(virQEMUDriverPtr driver,
                              virDomainObjPtr vm,
                              virConnectPtr conn,
                              qemuMigrationJobPhase phase,
                              virDomainState state,
                              int reason)
{
    bool postcopy = (state == VIR_DOMAIN_PAUSED &&
                     reason == VIR_DOMAIN_PAUSED_POSTCOPY_FAILED) ||
                    (state == VIR_DOMAIN_RUNNING &&
                     reason == VIR_DOMAIN_RUNNING_POSTCOPY);

    switch (phase) {
    case QEMU_MIGRATION_PHASE_NONE:
    case QEMU_MIGRATION_PHASE_PERFORM2:
    case QEMU_MIGRATION_PHASE_BEGIN3:
    case QEMU_MIGRATION_PHASE_PERFORM3:
    case QEMU_MIGRATION_PHASE_PERFORM3_DONE:
    case QEMU_MIGRATION_PHASE_CONFIRM3_CANCELLED:
    case QEMU_MIGRATION_PHASE_CONFIRM3:
    case QEMU_MIGRATION_PHASE_LAST:
        /* N/A for incoming migration */
        break;

    case QEMU_MIGRATION_PHASE_PREPARE:
        VIR_DEBUG("Killing unfinished incoming migration for domain %s",
                  vm->def->name);
        return -1;

    case QEMU_MIGRATION_PHASE_FINISH2:
        /* source domain is already killed so let's just resume the domain
         * and hope we are all set */
        VIR_DEBUG("Incoming migration finished, resuming domain %s",
                  vm->def->name);
        if (qemuProcessStartCPUs(driver, vm, conn,
                                 VIR_DOMAIN_RUNNING_UNPAUSED,
                                 QEMU_ASYNC_JOB_NONE) < 0) {
            VIR_WARN("Could not resume domain %s", vm->def->name);
        }
        break;

    case QEMU_MIGRATION_PHASE_FINISH3:
        /* migration finished, we started resuming the domain but didn't
         * confirm success or failure yet; killing it seems safest unless
         * we already started guest CPUs or we were in post-copy mode */
        if (postcopy) {
            qemuMigrationPostcopyFailed(driver, vm);
        } else if (state != VIR_DOMAIN_RUNNING) {
            VIR_DEBUG("Killing migrated domain %s", vm->def->name);
            return -1;
        }
        break;
    }

    return 0;
}

static int
qemuProcessRecoverMigrationOut(virQEMUDriverPtr driver,
                               virDomainObjPtr vm,
                               virConnectPtr conn,
                               qemuMigrationJobPhase phase,
                               virDomainState state,
                               int reason)
{
    bool postcopy = state == VIR_DOMAIN_PAUSED &&
                    (reason == VIR_DOMAIN_PAUSED_POSTCOPY ||
                     reason == VIR_DOMAIN_PAUSED_POSTCOPY_FAILED);

    switch (phase) {
    case QEMU_MIGRATION_PHASE_NONE:
    case QEMU_MIGRATION_PHASE_PREPARE:
    case QEMU_MIGRATION_PHASE_FINISH2:
    case QEMU_MIGRATION_PHASE_FINISH3:
    case QEMU_MIGRATION_PHASE_LAST:
        /* N/A for outgoing migration */
        break;

    case QEMU_MIGRATION_PHASE_BEGIN3:
        /* nothing happened so far, just forget we were about to migrate the
         * domain */
        break;

    case QEMU_MIGRATION_PHASE_PERFORM2:
    case QEMU_MIGRATION_PHASE_PERFORM3:
        /* migration is still in progress, let's cancel it and resume the
         * domain; however we can only do that before migration enters
         * post-copy mode
         */
        if (postcopy) {
            qemuMigrationPostcopyFailed(driver, vm);
        } else {
            VIR_DEBUG("Cancelling unfinished migration of domain %s",
                      vm->def->name);
            if (qemuMigrationCancel(driver, vm) < 0) {
                VIR_WARN("Could not cancel ongoing migration of domain %s",
                         vm->def->name);
            }
            goto resume;
        }
        break;

    case QEMU_MIGRATION_PHASE_PERFORM3_DONE:
        /* migration finished but we didn't have a chance to get the result
         * of Finish3 step; third party needs to check what to do next; in
         * post-copy mode we can use PAUSED_POSTCOPY_FAILED state for this
         */
        if (postcopy)
            qemuMigrationPostcopyFailed(driver, vm);
        break;

    case QEMU_MIGRATION_PHASE_CONFIRM3_CANCELLED:
        /* Finish3 failed, we need to resume the domain, but once we enter
         * post-copy mode there's no way back, so let's just mark the domain
         * as broken in that case
         */
        if (postcopy) {
            qemuMigrationPostcopyFailed(driver, vm);
        } else {
            VIR_DEBUG("Resuming domain %s after failed migration",
                      vm->def->name);
            goto resume;
        }
        break;

    case QEMU_MIGRATION_PHASE_CONFIRM3:
        /* migration completed, we need to kill the domain here */
        return -1;
    }

    return 0;

 resume:
    /* resume the domain but only if it was paused as a result of
     * migration
     */
    if (state == VIR_DOMAIN_PAUSED &&
        (reason == VIR_DOMAIN_PAUSED_MIGRATION ||
         reason == VIR_DOMAIN_PAUSED_UNKNOWN)) {
        if (qemuProcessStartCPUs(driver, vm, conn,
                                 VIR_DOMAIN_RUNNING_UNPAUSED,
                                 QEMU_ASYNC_JOB_NONE) < 0) {
            VIR_WARN("Could not resume domain %s", vm->def->name);
        }
    }
    return 0;
}

static int
qemuProcessRecoverJob(virQEMUDriverPtr driver,
                      virDomainObjPtr vm,
                      virConnectPtr conn,
                      const struct qemuDomainJobObj *job)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virDomainState state;
    int reason;

    state = virDomainObjGetState(vm, &reason);

    switch (job->asyncJob) {
    case QEMU_ASYNC_JOB_MIGRATION_OUT:
        if (qemuProcessRecoverMigrationOut(driver, vm, conn, job->phase,
                                           state, reason) < 0)
            return -1;
        break;

    case QEMU_ASYNC_JOB_MIGRATION_IN:
        if (qemuProcessRecoverMigrationIn(driver, vm, conn, job->phase,
                                          state, reason) < 0)
            return -1;
        break;

    case QEMU_ASYNC_JOB_SAVE:
    case QEMU_ASYNC_JOB_DUMP:
    case QEMU_ASYNC_JOB_SNAPSHOT:
        qemuDomainObjEnterMonitor(driver, vm);
        ignore_value(qemuMonitorMigrateCancel(priv->mon));
        if (qemuDomainObjExitMonitor(driver, vm) < 0)
            return -1;
        /* resume the domain but only if it was paused as a result of
         * running a migration-to-file operation.  Although we are
         * recovering an async job, this function is run at startup
         * and must resume things using sync monitor connections.  */
         if (state == VIR_DOMAIN_PAUSED &&
             ((job->asyncJob == QEMU_ASYNC_JOB_DUMP &&
               reason == VIR_DOMAIN_PAUSED_DUMP) ||
              (job->asyncJob == QEMU_ASYNC_JOB_SAVE &&
               reason == VIR_DOMAIN_PAUSED_SAVE) ||
              (job->asyncJob == QEMU_ASYNC_JOB_SNAPSHOT &&
               reason == VIR_DOMAIN_PAUSED_SNAPSHOT) ||
              reason == VIR_DOMAIN_PAUSED_UNKNOWN)) {
             if (qemuProcessStartCPUs(driver, vm, conn,
                                      VIR_DOMAIN_RUNNING_UNPAUSED,
                                      QEMU_ASYNC_JOB_NONE) < 0) {
                 VIR_WARN("Could not resume domain '%s' after migration to file",
                          vm->def->name);
            }
        }
        break;

    case QEMU_ASYNC_JOB_START:
        /* Already handled in VIR_DOMAIN_PAUSED_STARTING_UP check. */
        break;

    case QEMU_ASYNC_JOB_NONE:
    case QEMU_ASYNC_JOB_LAST:
        break;
    }

    if (!virDomainObjIsActive(vm))
        return -1;

    /* In case any special handling is added for job type that has been ignored
     * before, QEMU_DOMAIN_TRACK_JOBS (from qemu_domain.h) needs to be updated
     * for the job to be properly tracked in domain state XML.
     */
    switch (job->active) {
    case QEMU_JOB_QUERY:
        /* harmless */
        break;

    case QEMU_JOB_DESTROY:
        VIR_DEBUG("Domain %s should have already been destroyed",
                  vm->def->name);
        return -1;

    case QEMU_JOB_SUSPEND:
        /* mostly harmless */
        break;

    case QEMU_JOB_MODIFY:
        /* XXX depending on the command we may be in an inconsistent state and
         * we should probably fall back to "monitor error" state and refuse to
         */
        break;

    case QEMU_JOB_MIGRATION_OP:
    case QEMU_JOB_ABORT:
    case QEMU_JOB_ASYNC:
    case QEMU_JOB_ASYNC_NESTED:
        /* async job was already handled above */
    case QEMU_JOB_NONE:
    case QEMU_JOB_LAST:
        break;
    }

    return 0;
}

static int
qemuProcessUpdateDevices(virQEMUDriverPtr driver,
                         virDomainObjPtr vm)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virDomainDeviceDef dev;
    char **old;
    char **tmp;
    int ret = -1;

    if (!virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_DEVICE_DEL_EVENT))
        return 0;

    old = priv->qemuDevices;
    priv->qemuDevices = NULL;
    if (qemuDomainUpdateDeviceList(driver, vm, QEMU_ASYNC_JOB_NONE) < 0)
        goto cleanup;

    if ((tmp = old)) {
        while (*tmp) {
            if (!virStringArrayHasString(priv->qemuDevices, *tmp) &&
                virDomainDefFindDevice(vm->def, *tmp, &dev, false) == 0 &&
                qemuDomainRemoveDevice(driver, vm, &dev) < 0) {
                goto cleanup;
            }
            tmp++;
        }
    }
    ret = 0;

 cleanup:
    virStringFreeList(old);
    return ret;
}

struct qemuProcessReconnectData {
    virConnectPtr conn;
    virQEMUDriverPtr driver;
    virDomainObjPtr obj;
};
/*
 * Open an existing VM's monitor, re-detect VCPU threads
 * and re-reserve the security labels in use
 *
 * We own the virConnectPtr we are passed here - whoever started
 * this thread function has increased the reference counter to it
 * so that we now have to close it.
 *
 * This function also inherits a locked and ref'd domain object.
 *
 * This function needs to:
 * 1. Enter job
 * 1. just before monitor reconnect do lightweight MonitorEnter
 *    (increase VM refcount and unlock VM)
 * 2. reconnect to monitor
 * 3. do lightweight MonitorExit (lock VM)
 * 4. continue reconnect process
 * 5. EndJob
 *
 * We can't do normal MonitorEnter & MonitorExit because these two lock the
 * monitor lock, which does not exists in this early phase.
 */
static void
qemuProcessReconnect(void *opaque)
{
    struct qemuProcessReconnectData *data = opaque;
    virQEMUDriverPtr driver = data->driver;
    virDomainObjPtr obj = data->obj;
    qemuDomainObjPrivatePtr priv;
    virConnectPtr conn = data->conn;
    struct qemuDomainJobObj oldjob;
    int state;
    int reason;
    virQEMUDriverConfigPtr cfg;
    size_t i;
    int ret;
    unsigned int stopFlags = 0;

    VIR_FREE(data);

    qemuDomainObjRestoreJob(obj, &oldjob);
    if (oldjob.asyncJob == QEMU_ASYNC_JOB_MIGRATION_IN)
        stopFlags |= VIR_QEMU_PROCESS_STOP_MIGRATED;

    cfg = virQEMUDriverGetConfig(driver);
    priv = obj->privateData;

    /* XXX If we ever gonna change pid file pattern, come up with
     * some intelligence here to deal with old paths. */
    if (!(priv->pidfile = virPidFileBuildPath(cfg->stateDir, obj->def->name)))
        goto killvm;

    if (qemuDomainObjBeginJob(driver, obj, QEMU_JOB_MODIFY) < 0)
        goto killvm;

    virNWFilterReadLockFilterUpdates();

    VIR_DEBUG("Reconnect monitor to %p '%s'", obj, obj->def->name);

    /* XXX check PID liveliness & EXE path */
    if (qemuConnectMonitor(driver, obj, QEMU_ASYNC_JOB_NONE, NULL) < 0)
        goto error;

    if (qemuHostdevUpdateActiveDomainDevices(driver, obj->def) < 0)
        goto error;

    if (qemuConnectCgroup(driver, obj) < 0)
        goto error;

    /* XXX: Need to change as long as lock is introduced for
     * qemu_driver->sharedDevices.
     */
    for (i = 0; i < obj->def->ndisks; i++) {
        virDomainDeviceDef dev;

        if (virStorageTranslateDiskSourcePool(conn, obj->def->disks[i]) < 0)
            goto error;

        /* XXX we should be able to restore all data from XML in the future.
         * This should be the only place that calls qemuDomainDetermineDiskChain
         * with @report_broken == false to guarantee best-effort domain
         * reconnect */
        if (qemuDomainDetermineDiskChain(driver, obj, obj->def->disks[i],
                                         true, false) < 0)
            goto error;

        dev.type = VIR_DOMAIN_DEVICE_DISK;
        dev.data.disk = obj->def->disks[i];
        if (qemuAddSharedDevice(driver, &dev, obj->def->name) < 0)
            goto error;
    }

    if (qemuProcessUpdateState(driver, obj) < 0)
        goto error;

    state = virDomainObjGetState(obj, &reason);
    if (state == VIR_DOMAIN_SHUTOFF ||
        (state == VIR_DOMAIN_PAUSED &&
         reason == VIR_DOMAIN_PAUSED_STARTING_UP)) {
        VIR_DEBUG("Domain '%s' wasn't fully started yet, killing it",
                  obj->def->name);
        goto error;
    }

    /* If upgrading from old libvirtd we won't have found any
     * caps in the domain status, so re-query them
     */
    if (!priv->qemuCaps &&
        !(priv->qemuCaps = virQEMUCapsCacheLookupCopy(driver->qemuCapsCache,
                                                      obj->def->emulator,
                                                      obj->def->os.machine)))
        goto error;

    /* In case the domain shutdown while we were not running,
     * we need to finish the shutdown process. And we need to do it after
     * we have virQEMUCaps filled in.
     */
    if (state == VIR_DOMAIN_SHUTDOWN ||
        (state == VIR_DOMAIN_PAUSED &&
         reason == VIR_DOMAIN_PAUSED_SHUTTING_DOWN)) {
        VIR_DEBUG("Finishing shutdown sequence for domain %s",
                  obj->def->name);
        qemuProcessShutdownOrReboot(driver, obj);
        qemuDomainObjEndJob(driver, obj);
        goto cleanup;
    }

    if (virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_DEVICE))
        if ((qemuDomainAssignAddresses(obj->def, priv->qemuCaps, obj)) < 0)
            goto error;

    /* if domain requests security driver we haven't loaded, report error, but
     * do not kill the domain
     */
    ignore_value(virSecurityManagerCheckAllLabel(driver->securityManager,
                                                 obj->def));

    if (virSecurityManagerReserveLabel(driver->securityManager, obj->def, obj->pid) < 0)
        goto error;

    if (qemuProcessNotifyNets(obj->def) < 0)
        goto error;

    if (qemuProcessFiltersInstantiate(obj->def))
        goto error;

    if (qemuDomainCheckEjectableMedia(driver, obj, QEMU_ASYNC_JOB_NONE) < 0)
        goto error;

    if (qemuRefreshVirtioChannelState(driver, obj) < 0)
        goto error;

    if (qemuProcessRefreshBalloonState(driver, obj, QEMU_ASYNC_JOB_NONE) < 0)
        goto error;

    if (qemuProcessRecoverJob(driver, obj, conn, &oldjob) < 0)
        goto error;

    if (qemuProcessUpdateDevices(driver, obj) < 0)
        goto error;

    /* Failure to connect to agent shouldn't be fatal */
    if ((ret = qemuConnectAgent(driver, obj)) < 0) {
        if (ret == -2)
            goto error;

        VIR_WARN("Cannot connect to QEMU guest agent for %s",
                 obj->def->name);
        virResetLastError();
        priv->agentError = true;
    }

    /* update domain state XML with possibly updated state in virDomainObj */
    if (virDomainSaveStatus(driver->xmlopt, cfg->stateDir, obj, driver->caps) < 0)
        goto error;

    /* Run an hook to allow admins to do some magic */
    if (virHookPresent(VIR_HOOK_DRIVER_QEMU)) {
        char *xml = qemuDomainDefFormatXML(driver, obj->def, 0);
        int hookret;

        hookret = virHookCall(VIR_HOOK_DRIVER_QEMU, obj->def->name,
                              VIR_HOOK_QEMU_OP_RECONNECT, VIR_HOOK_SUBOP_BEGIN,
                              NULL, xml, NULL);
        VIR_FREE(xml);

        /*
         * If the script raised an error abort the launch
         */
        if (hookret < 0)
            goto error;
    }

    if (virAtomicIntInc(&driver->nactive) == 1 && driver->inhibitCallback)
        driver->inhibitCallback(true, driver->inhibitOpaque);

    qemuDomainObjEndJob(driver, obj);
    goto cleanup;

 error:
    qemuDomainObjEndJob(driver, obj);
 killvm:
    if (virDomainObjIsActive(obj)) {
        /* We can't get the monitor back, so must kill the VM
         * to remove danger of it ending up running twice if
         * user tries to start it again later
         */
        if (virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_NO_SHUTDOWN)) {
            /* If we couldn't get the monitor and qemu supports
             * no-shutdown, we can safely say that the domain
             * crashed ... */
            state = VIR_DOMAIN_SHUTOFF_CRASHED;
        } else {
            /* ... but if it doesn't we can't say what the state
             * really is and FAILED means "failed to start" */
            state = VIR_DOMAIN_SHUTOFF_UNKNOWN;
        }
        qemuProcessStop(driver, obj, state, stopFlags);
    }

    qemuDomainRemoveInactive(driver, obj);

 cleanup:
    virDomainObjEndAPI(&obj);
    virObjectUnref(conn);
    virObjectUnref(cfg);
    virNWFilterUnlockFilterUpdates();
}

static int
qemuProcessReconnectHelper(virDomainObjPtr obj,
                           void *opaque)
{
    virThread thread;
    struct qemuProcessReconnectData *src = opaque;
    struct qemuProcessReconnectData *data;

    /* If the VM was inactive, we don't need to reconnect */
    if (!obj->pid)
        return 0;

    if (VIR_ALLOC(data) < 0)
        return -1;

    memcpy(data, src, sizeof(*data));
    data->obj = obj;

    /* this lock and reference will be eventually transferred to the thread
     * that handles the reconnect */
    virObjectLock(obj);
    virObjectRef(obj);

    /* Since we close the connection later on, we have to make sure that the
     * threads we start see a valid connection throughout their lifetime. We
     * simply increase the reference counter here.
     */
    virObjectRef(data->conn);

    if (virThreadCreate(&thread, false, qemuProcessReconnect, data) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Could not create thread. QEMU initialization "
                         "might be incomplete"));
       /* We can't spawn a thread and thus connect to monitor. Kill qemu. */
        qemuProcessStop(src->driver, obj, VIR_DOMAIN_SHUTOFF_FAILED, 0);
        qemuDomainRemoveInactive(src->driver, obj);

        virDomainObjEndAPI(&obj);
        virObjectUnref(data->conn);
        VIR_FREE(data);
        return -1;
    }

    return 0;
}

/**
 * qemuProcessReconnectAll
 *
 * Try to re-open the resources for live VMs that we care
 * about.
 */
void
qemuProcessReconnectAll(virConnectPtr conn, virQEMUDriverPtr driver)
{
    struct qemuProcessReconnectData data = {.conn = conn, .driver = driver};
    virDomainObjListForEach(driver->domains, qemuProcessReconnectHelper, &data);
}

static int
qemuProcessVNCAllocatePorts(virQEMUDriverPtr driver,
                            virDomainGraphicsDefPtr graphics)
{
    unsigned short port;

    if (graphics->data.vnc.socket)
        return 0;

    if (graphics->data.vnc.autoport) {
        if (virPortAllocatorAcquire(driver->remotePorts, &port) < 0)
            return -1;
        graphics->data.vnc.port = port;
    }

    if (graphics->data.vnc.websocket == -1) {
        if (virPortAllocatorAcquire(driver->webSocketPorts, &port) < 0)
            return -1;
        graphics->data.vnc.websocket = port;
    }

    return 0;
}

int
qemuProcessSPICEAllocatePorts(virQEMUDriverPtr driver,
                              virQEMUDriverConfigPtr cfg,
                              virDomainGraphicsDefPtr graphics,
                              bool allocate)
{
    unsigned short port = 0;
    unsigned short tlsPort;
    size_t i;
    int defaultMode = graphics->data.spice.defaultMode;

    bool needTLSPort = false;
    bool needPort = false;

    if (graphics->data.spice.autoport) {
        /* check if tlsPort or port need allocation */
        for (i = 0; i < VIR_DOMAIN_GRAPHICS_SPICE_CHANNEL_LAST; i++) {
            switch (graphics->data.spice.channels[i]) {
            case VIR_DOMAIN_GRAPHICS_SPICE_CHANNEL_MODE_SECURE:
                needTLSPort = true;
                break;

            case VIR_DOMAIN_GRAPHICS_SPICE_CHANNEL_MODE_INSECURE:
                needPort = true;
                break;

            case VIR_DOMAIN_GRAPHICS_SPICE_CHANNEL_MODE_ANY:
                /* default mode will be used */
                break;
            }
        }
        switch (defaultMode) {
        case VIR_DOMAIN_GRAPHICS_SPICE_CHANNEL_MODE_SECURE:
            needTLSPort = true;
            break;

        case VIR_DOMAIN_GRAPHICS_SPICE_CHANNEL_MODE_INSECURE:
            needPort = true;
            break;

        case VIR_DOMAIN_GRAPHICS_SPICE_CHANNEL_MODE_ANY:
            if (cfg->spiceTLS)
                needTLSPort = true;
            needPort = true;
            break;
        }
    }

    if (!allocate) {
        if (needPort || graphics->data.spice.port == -1)
            graphics->data.spice.port = 5901;

        if (needTLSPort || graphics->data.spice.tlsPort == -1)
            graphics->data.spice.tlsPort = 5902;

        return 0;
    }

    if (needPort || graphics->data.spice.port == -1) {
        if (virPortAllocatorAcquire(driver->remotePorts, &port) < 0)
            goto error;

        graphics->data.spice.port = port;
    }

    if (needTLSPort || graphics->data.spice.tlsPort == -1) {
        if (!cfg->spiceTLS) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("Auto allocation of spice TLS port requested "
                             "but spice TLS is disabled in qemu.conf"));
            goto error;
        }

        if (virPortAllocatorAcquire(driver->remotePorts, &tlsPort) < 0)
            goto error;

        graphics->data.spice.tlsPort = tlsPort;
    }

    return 0;

 error:
    virPortAllocatorRelease(driver->remotePorts, port);
    return -1;
}


static int
qemuValidateCpuCount(virDomainDefPtr def,
                     virQEMUCapsPtr qemuCaps)
{
    unsigned int maxCpus = virQEMUCapsGetMachineMaxCpus(qemuCaps, def->os.machine);

    if (virDomainDefGetVcpus(def) == 0) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Domain requires at least 1 vCPU"));
        return -1;
    }

    if (maxCpus > 0 && virDomainDefGetVcpusMax(def) > maxCpus) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Maximum CPUs greater than specified machine type limit"));
        return -1;
    }

    return 0;
}


static bool
qemuProcessVerifyGuestCPU(virQEMUDriverPtr driver,
                          virDomainObjPtr vm,
                          int asyncJob)
{
    virDomainDefPtr def = vm->def;
    virArch arch = def->os.arch;
    virCPUDataPtr guestcpu = NULL;
    qemuDomainObjPrivatePtr priv = vm->privateData;
    int rc;
    bool ret = false;
    size_t i;

    /* no features are passed to QEMU with -cpu host
     * so it makes no sense to verify them */
    if (def->cpu && def->cpu->mode == VIR_CPU_MODE_HOST_PASSTHROUGH)
        return true;

    switch (arch) {
    case VIR_ARCH_I686:
    case VIR_ARCH_X86_64:
        if (qemuDomainObjEnterMonitorAsync(driver, vm, asyncJob) < 0)
            return false;
        rc = qemuMonitorGetGuestCPU(priv->mon, arch, &guestcpu);
        if (qemuDomainObjExitMonitor(driver, vm) < 0)
            return false;

        if (rc < 0) {
            if (rc == -2)
                break;

            goto cleanup;
        }

        if (def->features[VIR_DOMAIN_FEATURE_PVSPINLOCK] == VIR_TRISTATE_SWITCH_ON) {
            if (!cpuHasFeature(guestcpu, VIR_CPU_x86_KVM_PV_UNHALT)) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("host doesn't support paravirtual spinlocks"));
                goto cleanup;
            }
        }

        for (i = 0; def->cpu && i < def->cpu->nfeatures; i++) {
            virCPUFeatureDefPtr feature = &def->cpu->features[i];

            if (feature->policy != VIR_CPU_FEATURE_REQUIRE)
                continue;

            if (STREQ(feature->name, "invtsc") &&
                !cpuHasFeature(guestcpu, feature->name)) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("host doesn't support invariant TSC"));
                goto cleanup;
            }
        }
        break;

    default:
        break;
    }

    ret = true;

 cleanup:
    cpuDataFree(guestcpu);
    return ret;
}


static int
qemuPrepareNVRAM(virQEMUDriverConfigPtr cfg,
                 virCapsPtr caps,
                 virDomainObjPtr vm,
                 bool migrated)
{
    int ret = -1;
    int srcFD = -1;
    int dstFD = -1;
    virDomainLoaderDefPtr loader = vm->def->os.loader;
    bool generated = false;
    bool created = false;

    /* Unless domain has RO loader of pflash type, we have
     * nothing to do here.  If the loader is RW then it's not
     * using split code and vars feature, so no nvram file needs
     * to be created. */
    if (!loader || loader->type != VIR_DOMAIN_LOADER_TYPE_PFLASH ||
        loader->readonly != VIR_TRISTATE_SWITCH_ON)
        return 0;

    /* If the nvram path is configured already, there's nothing
     * we need to do. Unless we are starting the destination side
     * of migration in which case nvram is configured in the
     * domain XML but the file doesn't exist yet. Moreover, after
     * the migration is completed, qemu will invoke a
     * synchronization write into the nvram file so we don't have
     * to take care about transmitting the real data on the other
     * side. */
    if (loader->nvram && !migrated)
        return 0;

    /* Autogenerate nvram path if needed.*/
    if (!loader->nvram) {
        if (virAsprintf(&loader->nvram,
                        "%s/%s_VARS.fd",
                        cfg->nvramDir, vm->def->name) < 0)
            goto cleanup;

        generated = true;

        if (vm->persistent &&
            virDomainSaveConfig(cfg->configDir, caps, vm->def) < 0)
            goto cleanup;
    }

    if (!virFileExists(loader->nvram)) {
        const char *master_nvram_path = loader->templt;
        ssize_t r;

        if (!loader->templt) {
            size_t i;
            for (i = 0; i < cfg->nloader; i++) {
                if (STREQ(cfg->loader[i], loader->path)) {
                    master_nvram_path = cfg->nvram[i];
                    break;
                }
            }
        }

        if (!master_nvram_path) {
            virReportError(VIR_ERR_OPERATION_FAILED,
                           _("unable to find any master var store for "
                             "loader: %s"), loader->path);
            goto cleanup;
        }

        if ((srcFD = virFileOpenAs(master_nvram_path, O_RDONLY,
                                   0, -1, -1, 0)) < 0) {
            virReportSystemError(-srcFD,
                                 _("Failed to open file '%s'"),
                                 master_nvram_path);
            goto cleanup;
        }
        if ((dstFD = virFileOpenAs(loader->nvram,
                                   O_WRONLY | O_CREAT | O_EXCL,
                                   S_IRUSR | S_IWUSR,
                                   cfg->user, cfg->group, 0)) < 0) {
            virReportSystemError(-dstFD,
                                 _("Failed to create file '%s'"),
                                 loader->nvram);
            goto cleanup;
        }
        created = true;

        do {
            char buf[1024];

            if ((r = saferead(srcFD, buf, sizeof(buf))) < 0) {
                virReportSystemError(errno,
                                     _("Unable to read from file '%s'"),
                                     master_nvram_path);
                goto cleanup;
            }

            if (safewrite(dstFD, buf, r) < 0) {
                virReportSystemError(errno,
                                     _("Unable to write to file '%s'"),
                                     loader->nvram);
                goto cleanup;
            }
        } while (r);

        if (VIR_CLOSE(srcFD) < 0) {
            virReportSystemError(errno,
                                 _("Unable to close file '%s'"),
                                 master_nvram_path);
            goto cleanup;
        }
        if (VIR_CLOSE(dstFD) < 0) {
            virReportSystemError(errno,
                                 _("Unable to close file '%s'"),
                                 loader->nvram);
            goto cleanup;
        }
    }

    ret = 0;
 cleanup:
    /* We successfully generated the nvram path, but failed to
     * copy the file content. Roll back. */
    if (ret < 0) {
        if (created)
            unlink(loader->nvram);
        if (generated)
            VIR_FREE(loader->nvram);
    }

    VIR_FORCE_CLOSE(srcFD);
    VIR_FORCE_CLOSE(dstFD);
    return ret;
}


static void
qemuLogOperation(virDomainObjPtr vm,
                 const char *msg,
                 virCommandPtr cmd,
                 qemuDomainLogContextPtr logCtxt)
{
    char *timestamp;
    qemuDomainObjPrivatePtr priv = vm->privateData;
    int qemuVersion = virQEMUCapsGetVersion(priv->qemuCaps);
    const char *package = virQEMUCapsGetPackage(priv->qemuCaps);
    char *hostname = virGetHostname();

    if ((timestamp = virTimeStringNow()) == NULL)
        goto cleanup;

    if (qemuDomainLogContextWrite(logCtxt,
                                  "%s: %s %s, qemu version: %d.%d.%d%s, hostname: %s\n",
                                  timestamp, msg, VIR_LOG_VERSION_STRING,
                                  (qemuVersion / 1000000) % 1000,
                                  (qemuVersion / 1000) % 1000,
                                  qemuVersion % 1000,
                                  package ? package : "",
                                  hostname ? hostname : "") < 0)
        goto cleanup;

    if (cmd) {
        char *args = virCommandToString(cmd);
        qemuDomainLogContextWrite(logCtxt, "%s\n", args);
        VIR_FREE(args);
    }

 cleanup:
    VIR_FREE(hostname);
    VIR_FREE(timestamp);
}


void
qemuProcessIncomingDefFree(qemuProcessIncomingDefPtr inc)
{
    if (!inc)
        return;

    VIR_FREE(inc->address);
    VIR_FREE(inc->launchURI);
    VIR_FREE(inc->deferredURI);
    VIR_FREE(inc);
}


/*
 * This function does not copy @path, the caller is responsible for keeping
 * the @path pointer valid during the lifetime of the allocated
 * qemuProcessIncomingDef structure.
 */
qemuProcessIncomingDefPtr
qemuProcessIncomingDefNew(virQEMUCapsPtr qemuCaps,
                          const char *listenAddress,
                          const char *migrateFrom,
                          int fd,
                          const char *path)
{
    qemuProcessIncomingDefPtr inc = NULL;

    if (qemuMigrationCheckIncoming(qemuCaps, migrateFrom) < 0)
        return NULL;

    if (VIR_ALLOC(inc) < 0)
        return NULL;

    if (VIR_STRDUP(inc->address, listenAddress) < 0)
        goto error;

    inc->launchURI = qemuMigrationIncomingURI(migrateFrom, fd);
    if (!inc->launchURI)
        goto error;

    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_INCOMING_DEFER)) {
        inc->deferredURI = inc->launchURI;
        if (VIR_STRDUP(inc->launchURI, "defer") < 0)
            goto error;
    }

    inc->fd = fd;
    inc->path = path;

    return inc;

 error:
    qemuProcessIncomingDefFree(inc);
    return NULL;
}


/*
 * This function starts a new QEMU_ASYNC_JOB_START async job. The user is
 * responsible for calling qemuProcessEndJob to stop this job and for passing
 * QEMU_ASYNC_JOB_START as @asyncJob argument to any function requiring this
 * parameter between qemuProcessBeginJob and qemuProcessEndJob.
 */
int
qemuProcessBeginJob(virQEMUDriverPtr driver,
                    virDomainObjPtr vm)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;

    if (qemuDomainObjBeginAsyncJob(driver, vm, QEMU_ASYNC_JOB_START) < 0)
        return -1;

    qemuDomainObjSetAsyncJobMask(vm, QEMU_JOB_NONE);
    priv->job.current->type = VIR_DOMAIN_JOB_UNBOUNDED;

    return 0;
}


void
qemuProcessEndJob(virQEMUDriverPtr driver,
                  virDomainObjPtr vm)
{
    qemuDomainObjEndAsyncJob(driver, vm);
}


static int
qemuProcessStartHook(virQEMUDriverPtr driver,
                     virDomainObjPtr vm,
                     virHookQemuOpType op,
                     virHookSubopType subop)
{
    char *xml;
    int ret;

    if (!virHookPresent(VIR_HOOK_DRIVER_QEMU))
        return 0;

    if (!(xml = qemuDomainDefFormatXML(driver, vm->def, 0)))
        return -1;

    ret = virHookCall(VIR_HOOK_DRIVER_QEMU, vm->def->name, op, subop,
                      NULL, xml, NULL);
    VIR_FREE(xml);

    return ret;
}


static int
qemuProcessSetupGraphics(virQEMUDriverPtr driver,
                         virDomainObjPtr vm)
{
    virQEMUDriverConfigPtr cfg = virQEMUDriverGetConfig(driver);
    size_t i;
    int ret = -1;

    for (i = 0; i < vm->def->ngraphics; ++i) {
        virDomainGraphicsDefPtr graphics = vm->def->graphics[i];
        if (graphics->type == VIR_DOMAIN_GRAPHICS_TYPE_VNC &&
            !graphics->data.vnc.autoport) {
            if (virPortAllocatorSetUsed(driver->remotePorts,
                                        graphics->data.vnc.port,
                                        true) < 0)
                goto cleanup;
            graphics->data.vnc.portReserved = true;

        } else if (graphics->type == VIR_DOMAIN_GRAPHICS_TYPE_SPICE &&
                   !graphics->data.spice.autoport) {
            if (graphics->data.spice.port > 0) {
                if (virPortAllocatorSetUsed(driver->remotePorts,
                                            graphics->data.spice.port,
                                            true) < 0)
                    goto cleanup;
                graphics->data.spice.portReserved = true;
            }

            if (graphics->data.spice.tlsPort > 0) {
                if (virPortAllocatorSetUsed(driver->remotePorts,
                                            graphics->data.spice.tlsPort,
                                            true) < 0)
                    goto cleanup;
                graphics->data.spice.tlsPortReserved = true;
            }
        }
    }

    for (i = 0; i < vm->def->ngraphics; ++i) {
        virDomainGraphicsDefPtr graphics = vm->def->graphics[i];
        if (graphics->type == VIR_DOMAIN_GRAPHICS_TYPE_VNC) {
            if (qemuProcessVNCAllocatePorts(driver, graphics) < 0)
                goto cleanup;
        } else if (graphics->type == VIR_DOMAIN_GRAPHICS_TYPE_SPICE) {
            if (qemuProcessSPICEAllocatePorts(driver, cfg, graphics, true) < 0)
                goto cleanup;
        }

        if (graphics->type == VIR_DOMAIN_GRAPHICS_TYPE_VNC ||
            graphics->type == VIR_DOMAIN_GRAPHICS_TYPE_SPICE) {
            if (graphics->nListens == 0) {
                if (VIR_EXPAND_N(graphics->listens, graphics->nListens, 1) < 0)
                    goto cleanup;
                graphics->listens[0].type = VIR_DOMAIN_GRAPHICS_LISTEN_TYPE_ADDRESS;
                if (VIR_STRDUP(graphics->listens[0].address,
                               graphics->type == VIR_DOMAIN_GRAPHICS_TYPE_VNC ?
                               cfg->vncListen : cfg->spiceListen) < 0) {
                    VIR_SHRINK_N(graphics->listens, graphics->nListens, 1);
                    goto cleanup;
                }
                graphics->listens[0].fromConfig = true;
            } else if (graphics->nListens > 1) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("QEMU does not support multiple listen "
                                 "addresses for one graphics device."));
                goto cleanup;
            }
        }
    }

    ret = 0;

 cleanup:
    virObjectUnref(cfg);
    return ret;
}


static int
qemuProcessSetupRawIO(virQEMUDriverPtr driver,
                      virDomainObjPtr vm,
                      virCommandPtr cmd ATTRIBUTE_UNUSED)
{
    bool rawio = false;
    size_t i;
    int ret = -1;

    /* in case a certain disk is desirous of CAP_SYS_RAWIO, add this */
    for (i = 0; i < vm->def->ndisks; i++) {
        virDomainDeviceDef dev;
        virDomainDiskDefPtr disk = vm->def->disks[i];

        if (disk->rawio == VIR_TRISTATE_BOOL_YES) {
            rawio = true;
#ifndef CAP_SYS_RAWIO
            break;
#endif
        }

        dev.type = VIR_DOMAIN_DEVICE_DISK;
        dev.data.disk = disk;
        if (qemuAddSharedDevice(driver, &dev, vm->def->name) < 0)
            goto cleanup;

        if (qemuSetUnprivSGIO(&dev) < 0)
            goto cleanup;
    }

    /* If rawio not already set, check hostdevs as well */
    if (!rawio) {
        for (i = 0; i < vm->def->nhostdevs; i++) {
            virDomainHostdevSubsysSCSIPtr scsisrc =
                &vm->def->hostdevs[i]->source.subsys.u.scsi;
            if (scsisrc->rawio == VIR_TRISTATE_BOOL_YES) {
                rawio = true;
                break;
            }
        }
    }

    ret = 0;

 cleanup:
    if (rawio) {
#ifdef CAP_SYS_RAWIO
        if (ret == 0)
            virCommandAllowCap(cmd, CAP_SYS_RAWIO);
#else
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Raw I/O is not supported on this platform"));
        ret = -1;
#endif
    }
    return ret;
}


static int
qemuProcessSetupBalloon(virQEMUDriverPtr driver,
                        virDomainObjPtr vm,
                        qemuDomainAsyncJob asyncJob)
{
    unsigned long long balloon = vm->def->mem.cur_balloon;
    qemuDomainObjPrivatePtr priv = vm->privateData;
    int period;
    int ret = -1;

    if (!vm->def->memballoon ||
        vm->def->memballoon->model == VIR_DOMAIN_MEMBALLOON_MODEL_NONE)
        return 0;

    period = vm->def->memballoon->period;

    if (qemuDomainObjEnterMonitorAsync(driver, vm, asyncJob) < 0)
        goto cleanup;

    if (period)
        qemuMonitorSetMemoryStatsPeriod(priv->mon, period);
    if (qemuMonitorSetBalloon(priv->mon, balloon) < 0)
        goto cleanup;

    ret = 0;

 cleanup:
    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        ret = -1;
    return ret;
}


static int
qemuProcessMakeDir(virQEMUDriverPtr driver,
                   virDomainObjPtr vm,
                   const char *parentDir)
{
    char *path = NULL;
    int ret = -1;

    if (virAsprintf(&path, "%s/domain-%s", parentDir, vm->def->name) < 0)
        goto cleanup;

    if (virFileMakePathWithMode(path, 0750) < 0) {
        virReportSystemError(errno, _("Cannot create directory '%s'"), path);
        goto cleanup;
    }

    if (virSecurityManagerDomainSetDirLabel(driver->securityManager,
                                            vm->def, path) < 0)
        goto cleanup;

    ret = 0;

 cleanup:
    VIR_FREE(path);
    return ret;
}


/**
 * qemuProcessStartValidate:
 * @vm: domain object
 * @qemuCaps: emulator capabilities
 * @migration: restoration of existing state
 *
 * This function aggregates checks independent from host state done prior to
 * start of a VM.
 */
int
qemuProcessStartValidate(virDomainDefPtr def,
                         virQEMUCapsPtr qemuCaps,
                         bool migration,
                         bool snapshot)
{
    if (qemuValidateCpuCount(def, qemuCaps) < 0)
        return -1;

    if (!migration && !snapshot &&
        virDomainDefCheckDuplicateDiskInfo(def) < 0)
        return -1;

    return 0;
}


/**
 * qemuProcessInit:
 *
 * Prepares the domain up to the point when priv->qemuCaps is initialized. The
 * function calls qemuProcessStop when needed.
 *
 * Returns 0 on success, -1 on error.
 */
int
qemuProcessInit(virQEMUDriverPtr driver,
                virDomainObjPtr vm,
                bool migration,
                bool snap)
{
    virQEMUDriverConfigPtr cfg = virQEMUDriverGetConfig(driver);
    virCapsPtr caps = NULL;
    qemuDomainObjPrivatePtr priv = vm->privateData;
    int stopFlags;
    int ret = -1;

    VIR_DEBUG("vm=%p name=%s id=%d migration=%d",
              vm, vm->def->name, vm->def->id, migration);

    VIR_DEBUG("Beginning VM startup process");

    if (virDomainObjIsActive(vm)) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("VM is already active"));
        goto cleanup;
    }

    if (!(caps = virQEMUDriverGetCapabilities(driver, false)))
        goto cleanup;

    VIR_DEBUG("Determining emulator version");
    virObjectUnref(priv->qemuCaps);
    if (!(priv->qemuCaps = virQEMUCapsCacheLookupCopy(driver->qemuCapsCache,
                                                      vm->def->emulator,
                                                      vm->def->os.machine)))
        goto cleanup;

    if (qemuProcessStartValidate(vm->def, priv->qemuCaps, migration, snap) < 0)
        goto cleanup;

    /* Some things, paths, ... are generated here and we want them to persist.
     * Fill them in prior to setting the domain def as transient. */
    VIR_DEBUG("Generating paths");

    if (qemuPrepareNVRAM(cfg, caps, vm, migration) < 0)
        goto stop;

    /* Do this upfront, so any part of the startup process can add
     * runtime state to vm->def that won't be persisted. This let's us
     * report implicit runtime defaults in the XML, like vnc listen/socket
     */
    VIR_DEBUG("Setting current domain def as transient");
    if (virDomainObjSetDefTransient(caps, driver->xmlopt, vm, true) < 0)
        goto stop;

    vm->def->id = qemuDriverAllocateID(driver);
    qemuDomainSetFakeReboot(driver, vm, false);
    virDomainObjSetState(vm, VIR_DOMAIN_PAUSED, VIR_DOMAIN_PAUSED_STARTING_UP);

    if (virAtomicIntInc(&driver->nactive) == 1 && driver->inhibitCallback)
        driver->inhibitCallback(true, driver->inhibitOpaque);

    /* Run an early hook to set-up missing devices */
    if (qemuProcessStartHook(driver, vm,
                             VIR_HOOK_QEMU_OP_PREPARE,
                             VIR_HOOK_SUBOP_BEGIN) < 0)
        goto stop;

    ret = 0;

 cleanup:
    virObjectUnref(cfg);
    virObjectUnref(caps);
    return ret;

 stop:
    stopFlags = VIR_QEMU_PROCESS_STOP_NO_RELABEL;
    if (migration)
        stopFlags |= VIR_QEMU_PROCESS_STOP_MIGRATED;
    qemuProcessStop(driver, vm, VIR_DOMAIN_SHUTOFF_FAILED, stopFlags);
    goto cleanup;
}


/**
 * qemuProcessSetupVcpu:
 * @vm: domain object
 * @vcpuid: id of VCPU to set defaults
 *
 * This function sets resource properties (cgroups, affinity, scheduler) for a
 * vCPU. This function expects that the vCPU is online and the vCPU pids were
 * correctly detected at the point when it's called.
 *
 * Returns 0 on success, -1 on error.
 */
int
qemuProcessSetupVcpu(virDomainObjPtr vm,
                     unsigned int vcpuid)
{
    pid_t vcpupid = qemuDomainGetVcpuPid(vm, vcpuid);
    virDomainVcpuInfoPtr vcpu = virDomainDefGetVcpu(vm->def, vcpuid);
    qemuDomainObjPrivatePtr priv = vm->privateData;
    char *mem_mask = NULL;
    virDomainNumatuneMemMode mem_mode;
    unsigned long long period = vm->def->cputune.period;
    long long quota = vm->def->cputune.quota;
    virCgroupPtr cgroup_vcpu = NULL;
    virBitmapPtr cpumask;
    int ret = -1;

    if (virCgroupHasController(priv->cgroup, VIR_CGROUP_CONTROLLER_CPU) ||
        virCgroupHasController(priv->cgroup, VIR_CGROUP_CONTROLLER_CPUSET)) {

        if (virDomainNumatuneGetMode(vm->def->numa, -1, &mem_mode) == 0 &&
            mem_mode == VIR_DOMAIN_NUMATUNE_MEM_STRICT &&
            virDomainNumatuneMaybeFormatNodeset(vm->def->numa,
                                                priv->autoNodeset,
                                                &mem_mask, -1) < 0)
            goto cleanup;

        if (virCgroupNewThread(priv->cgroup, VIR_CGROUP_THREAD_VCPU, vcpuid,
                               true, &cgroup_vcpu) < 0)
            goto cleanup;

        if (period || quota) {
            if (qemuSetupCgroupVcpuBW(cgroup_vcpu, period, quota) < 0)
                goto cleanup;
        }
    }

    /* infer which cpumask shall be used */
    if (vcpu->cpumask)
        cpumask = vcpu->cpumask;
    else if (vm->def->placement_mode == VIR_DOMAIN_CPU_PLACEMENT_MODE_AUTO)
        cpumask = priv->autoCpuset;
    else
        cpumask = vm->def->cpumask;

    /* setup cgroups */
    if (cgroup_vcpu) {
        if (virCgroupHasController(priv->cgroup, VIR_CGROUP_CONTROLLER_CPUSET)) {
            if (mem_mask && virCgroupSetCpusetMems(cgroup_vcpu, mem_mask) < 0)
                goto cleanup;

            if (cpumask && qemuSetupCgroupCpusetCpus(cgroup_vcpu, cpumask) < 0)
                goto cleanup;
        }

        /* move the thread for vcpu to sub dir */
        if (virCgroupAddTask(cgroup_vcpu, vcpupid) < 0)
            goto cleanup;
    }

    /* setup legacy affinty */
    if (cpumask && virProcessSetAffinity(vcpupid, cpumask) < 0)
        goto cleanup;

    /* set scheduler type and priority */
    if (vcpu->sched.policy != VIR_PROC_POLICY_NONE) {
        if (virProcessSetScheduler(vcpupid, vcpu->sched.policy,
                                   vcpu->sched.priority) < 0)
            goto cleanup;
    }

    ret = 0;

 cleanup:
    VIR_FREE(mem_mask);
    if (cgroup_vcpu) {
        if (ret < 0)
            virCgroupRemove(cgroup_vcpu);
        virCgroupFree(&cgroup_vcpu);
    }

    return ret;
}


static int
qemuProcessSetupVcpus(virDomainObjPtr vm)
{
    virDomainVcpuInfoPtr vcpu;
    unsigned int maxvcpus = virDomainDefGetVcpusMax(vm->def);
    size_t i;

    if ((vm->def->cputune.period || vm->def->cputune.quota) &&
        !virCgroupHasController(((qemuDomainObjPrivatePtr) vm->privateData)->cgroup,
                                VIR_CGROUP_CONTROLLER_CPU)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("cgroup cpu is required for scheduler tuning"));
        return -1;
    }

    if (!qemuDomainHasVcpuPids(vm)) {
        /* If any CPU has custom affinity that differs from the
         * VM default affinity, we must reject it */
        for (i = 0; i < maxvcpus; i++) {
            vcpu = virDomainDefGetVcpu(vm->def, i);

            if (!vcpu->online)
                continue;

            if (vcpu->cpumask &&
                !virBitmapEqual(vm->def->cpumask, vcpu->cpumask)) {
                virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                                _("cpu affinity is not supported"));
                return -1;
            }
        }

        return 0;
    }

    for (i = 0; i < maxvcpus; i++) {
        vcpu = virDomainDefGetVcpu(vm->def, i);

        if (!vcpu->online)
            continue;

        if (qemuProcessSetupVcpu(vm, i) < 0)
            return -1;
    }

    return 0;
}


/**
 * qemuProcessSetupIOThread:
 * @vm: domain object
 * @iothread: iothread data structure to set the data for
 *
 * This function sets resource properities (affinity, cgroups, scheduler) for a
 * IOThread. This function expects that the IOThread is online and the IOThread
 * pids were correctly detected at the point when it's called.
 *
 * Returns 0 on success, -1 on error.
 */
int
qemuProcessSetupIOThread(virDomainObjPtr vm,
                         virDomainIOThreadIDDefPtr iothread)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    unsigned long long period = vm->def->cputune.period;
    long long quota = vm->def->cputune.quota;
    virDomainNumatuneMemMode mem_mode;
    char *mem_mask = NULL;
    virCgroupPtr cgroup_iothread = NULL;
    virBitmapPtr cpumask = NULL;
    int ret = -1;

    if ((period || quota) &&
        !virCgroupHasController(priv->cgroup, VIR_CGROUP_CONTROLLER_CPU)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("cgroup cpu is required for scheduler tuning"));
        return -1;
    }

    if (virCgroupHasController(priv->cgroup, VIR_CGROUP_CONTROLLER_CPU) ||
        virCgroupHasController(priv->cgroup, VIR_CGROUP_CONTROLLER_CPUSET)) {
        if (virDomainNumatuneGetMode(vm->def->numa, -1, &mem_mode) == 0 &&
            mem_mode == VIR_DOMAIN_NUMATUNE_MEM_STRICT &&
            virDomainNumatuneMaybeFormatNodeset(vm->def->numa,
                                                priv->autoNodeset,
                                                &mem_mask, -1) < 0)
            goto cleanup;

        if (virCgroupNewThread(priv->cgroup, VIR_CGROUP_THREAD_IOTHREAD,
                               iothread->iothread_id,
                               true, &cgroup_iothread) < 0)
            goto cleanup;
    }

    if (iothread->cpumask)
        cpumask = iothread->cpumask;
    else if (vm->def->placement_mode == VIR_DOMAIN_CPU_PLACEMENT_MODE_AUTO)
        cpumask = priv->autoCpuset;
    else
        cpumask = vm->def->cpumask;

    if (period || quota) {
        if (qemuSetupCgroupVcpuBW(cgroup_iothread, period, quota) < 0)
            goto cleanup;
    }

    if (cgroup_iothread) {
        if (virCgroupHasController(priv->cgroup, VIR_CGROUP_CONTROLLER_CPUSET)) {
            if (mem_mask &&
                virCgroupSetCpusetMems(cgroup_iothread, mem_mask) < 0)
                goto cleanup;

            if (cpumask &&
                qemuSetupCgroupCpusetCpus(cgroup_iothread, cpumask) < 0)
                goto cleanup;
        }

        if (virCgroupAddTask(cgroup_iothread, iothread->thread_id) < 0)
            goto cleanup;
    }

    if (cpumask && virProcessSetAffinity(iothread->thread_id, cpumask) < 0)
        goto cleanup;

    if (iothread->sched.policy != VIR_PROC_POLICY_NONE &&
        virProcessSetScheduler(iothread->thread_id, iothread->sched.policy,
                               iothread->sched.priority) < 0)
        goto cleanup;

    ret = 0;

 cleanup:
    if (cgroup_iothread) {
        if (ret < 0)
            virCgroupRemove(cgroup_iothread);
        virCgroupFree(&cgroup_iothread);
    }

    VIR_FREE(mem_mask);
    return ret;
}


static int
qemuProcessSetupIOThreads(virDomainObjPtr vm)
{
    size_t i;

    for (i = 0; i < vm->def->niothreadids; i++) {
        virDomainIOThreadIDDefPtr info = vm->def->iothreadids[i];

        if (qemuProcessSetupIOThread(vm, info) < 0)
            return -1;
    }

    return 0;
}


/**
 * qemuProcessLaunch:
 *
 * Launch a new QEMU process with stopped virtual CPUs.
 *
 * The caller is supposed to call qemuProcessStop with appropriate
 * flags in case of failure.
 *
 * Returns 0 on success,
 *        -1 on error which happened before devices were labeled and thus
 *           there is no need to restore them,
 *        -2 on error requesting security labels to be restored.
 */
int
qemuProcessLaunch(virConnectPtr conn,
                  virQEMUDriverPtr driver,
                  virDomainObjPtr vm,
                  qemuDomainAsyncJob asyncJob,
                  qemuProcessIncomingDefPtr incoming,
                  virDomainSnapshotObjPtr snapshot,
                  virNetDevVPortProfileOp vmop,
                  unsigned int flags)
{
    int ret = -1;
    int rv;
    int logfile = -1;
    qemuDomainLogContextPtr logCtxt = NULL;
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virCommandPtr cmd = NULL;
    struct qemuProcessHookData hookData;
    size_t i;
    char *nodeset = NULL;
    virQEMUDriverConfigPtr cfg;
    virCapsPtr caps = NULL;
    unsigned int hostdev_flags = 0;
    size_t nnicindexes = 0;
    int *nicindexes = NULL;
    bool check_shmem = false;

    VIR_DEBUG("vm=%p name=%s id=%d asyncJob=%d "
              "incoming.launchURI=%s incoming.deferredURI=%s "
              "incoming.fd=%d incoming.path=%s "
              "snapshot=%p vmop=%d flags=0x%x",
              vm, vm->def->name, vm->def->id, asyncJob,
              NULLSTR(incoming ? incoming->launchURI : NULL),
              NULLSTR(incoming ? incoming->deferredURI : NULL),
              incoming ? incoming->fd : -1,
              NULLSTR(incoming ? incoming->path : NULL),
              snapshot, vmop, flags);

    /* Okay, these are just internal flags,
     * but doesn't hurt to check */
    virCheckFlags(VIR_QEMU_PROCESS_START_COLD |
                  VIR_QEMU_PROCESS_START_PAUSED |
                  VIR_QEMU_PROCESS_START_AUTODESTROY, -1);

    cfg = virQEMUDriverGetConfig(driver);

    hookData.conn = conn;
    hookData.vm = vm;
    hookData.driver = driver;
    /* We don't increase cfg's reference counter here. */
    hookData.cfg = cfg;

    if (!(caps = virQEMUDriverGetCapabilities(driver, false)))
        goto cleanup;

    /* network devices must be "prepared" before hostdevs, because
     * setting up a network device might create a new hostdev that
     * will need to be setup.
     */
    VIR_DEBUG("Preparing network devices");
    if (qemuNetworkPrepareDevices(vm->def) < 0)
        goto cleanup;

    /* Must be run before security labelling */
    VIR_DEBUG("Preparing host devices");
    if (!cfg->relaxedACS)
        hostdev_flags |= VIR_HOSTDEV_STRICT_ACS_CHECK;
    if (!incoming)
        hostdev_flags |= VIR_HOSTDEV_COLD_BOOT;
    if (qemuHostdevPrepareDomainDevices(driver, vm->def, priv->qemuCaps,
                                        hostdev_flags) < 0)
        goto cleanup;

    VIR_DEBUG("Preparing chr devices");
    if (virDomainChrDefForeach(vm->def,
                               true,
                               qemuProcessPrepareChardevDevice,
                               NULL) < 0)
        goto cleanup;

    VIR_DEBUG("Checking domain and device security labels");
    if (virSecurityManagerCheckAllLabel(driver->securityManager, vm->def) < 0)
        goto cleanup;

    /* If you are using a SecurityDriver with dynamic labelling,
       then generate a security label for isolation */
    VIR_DEBUG("Generating domain security label (if required)");
    if (virSecurityManagerGenLabel(driver->securityManager, vm->def) < 0) {
        virDomainAuditSecurityLabel(vm, false);
        goto cleanup;
    }
    virDomainAuditSecurityLabel(vm, true);

    if (vm->def->mem.nhugepages) {
        for (i = 0; i < cfg->nhugetlbfs; i++) {
            char *hugepagePath = qemuGetHugepagePath(&cfg->hugetlbfs[i]);

            if (!hugepagePath)
                goto cleanup;

            if (virSecurityManagerSetHugepages(driver->securityManager,
                                               vm->def, hugepagePath) < 0) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               "%s", _("Unable to set huge path in security driver"));
                VIR_FREE(hugepagePath);
                goto cleanup;
            }
            VIR_FREE(hugepagePath);
        }
    }

    /* Ensure no historical cgroup for this VM is lying around bogus
     * settings */
    VIR_DEBUG("Ensuring no historical cgroup is lying around");
    qemuRemoveCgroup(vm);

    VIR_DEBUG("Setting up ports for graphics");
    if (qemuProcessSetupGraphics(driver, vm) < 0)
        goto cleanup;

    if (virFileMakePath(cfg->logDir) < 0) {
        virReportSystemError(errno,
                             _("cannot create log directory %s"),
                             cfg->logDir);
        goto cleanup;
    }

    VIR_DEBUG("Creating domain log file");
    if (!(logCtxt = qemuDomainLogContextNew(driver, vm,
                                            QEMU_DOMAIN_LOG_CONTEXT_MODE_START)))
        goto cleanup;
    logfile = qemuDomainLogContextGetWriteFD(logCtxt);

    if (vm->def->virtType == VIR_DOMAIN_VIRT_KVM) {
        VIR_DEBUG("Checking for KVM availability");
        if (!virFileExists("/dev/kvm")) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("Domain requires KVM, but it is not available. "
                             "Check that virtualization is enabled in the host BIOS, "
                             "and host configuration is setup to load the kvm modules."));
            goto cleanup;
        }
    }

    if (qemuAssignDeviceAliases(vm->def, priv->qemuCaps) < 0)
        goto cleanup;

    /* Get the advisory nodeset from numad if 'placement' of
     * either <vcpu> or <numatune> is 'auto'.
     */
    if (virDomainDefNeedsPlacementAdvice(vm->def)) {
        nodeset = virNumaGetAutoPlacementAdvice(virDomainDefGetVcpus(vm->def),
                                                virDomainDefGetMemoryActual(vm->def));
        if (!nodeset)
            goto cleanup;

        VIR_DEBUG("Nodeset returned from numad: %s", nodeset);

        if (virBitmapParse(nodeset, 0, &priv->autoNodeset,
                           VIR_DOMAIN_CPUMASK_LEN) < 0)
            goto cleanup;

        if (!(priv->autoCpuset = virCapabilitiesGetCpusForNodemask(caps,
                                                                   priv->autoNodeset)))
            goto cleanup;
    }

    /* "volume" type disk's source must be translated before
     * cgroup and security setting.
     */
    for (i = 0; i < vm->def->ndisks; i++) {
        if (virStorageTranslateDiskSourcePool(conn, vm->def->disks[i]) < 0)
            goto cleanup;
    }

    if (qemuDomainCheckDiskPresence(driver, vm,
                                    flags & VIR_QEMU_PROCESS_START_COLD) < 0)
        goto cleanup;

    if (vm->def->mem.min_guarantee) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Parameter 'min_guarantee' "
                         "not supported by QEMU."));
        goto cleanup;
    }

    if (VIR_ALLOC(priv->monConfig) < 0)
        goto cleanup;

    VIR_DEBUG("Preparing monitor state");
    if (qemuProcessPrepareMonitorChr(cfg, priv->monConfig, vm->def->name) < 0)
        goto cleanup;

    priv->monJSON = virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_MONITOR_JSON);
    priv->monError = false;
    priv->monStart = 0;
    priv->gotShutdown = false;

    VIR_FREE(priv->pidfile);
    if (!(priv->pidfile = virPidFileBuildPath(cfg->stateDir, vm->def->name))) {
        virReportSystemError(errno,
                             "%s", _("Failed to build pidfile path."));
        goto cleanup;
    }

    if (unlink(priv->pidfile) < 0 &&
        errno != ENOENT) {
        virReportSystemError(errno,
                             _("Cannot remove stale PID file %s"),
                             priv->pidfile);
        goto cleanup;
    }

    /*
     * Normally PCI addresses are assigned in the virDomainCreate
     * or virDomainDefine methods. We might still need to assign
     * some here to cope with the question of upgrades. Regardless
     * we also need to populate the PCI address set cache for later
     * use in hotplug
     */
    if (virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_DEVICE)) {
        VIR_DEBUG("Assigning domain PCI addresses");
        if ((qemuDomainAssignAddresses(vm->def, priv->qemuCaps, vm)) < 0)
            goto cleanup;
    }

    VIR_DEBUG("Checking for any possible (non-fatal) issues");

    /*
     * For vhost-user to work, the domain has to have some type of
     * shared memory configured.  We're not the proper ones to judge
     * whether shared hugepages or shm are enough and will be in the
     * future, so we'll just warn in case neither is configured.
     * Moreover failing would give the false illusion that libvirt is
     * really checking that everything works before running the domain
     * and not only we are unable to do that, but it's also not our
     * aim to do so.
     */
    for (i = 0; i < vm->def->nnets; i++) {
        if (virDomainNetGetActualType(vm->def->nets[i]) ==
                                      VIR_DOMAIN_NET_TYPE_VHOSTUSER) {
            check_shmem = true;
            break;
        }
    }

    if (check_shmem) {
        bool shmem = vm->def->nshmems;

        /*
         * This check is by no means complete.  We merely check
         * whether there are *some* hugepages enabled and *some* NUMA
         * nodes with shared memory access.
         */
        if (!shmem && vm->def->mem.nhugepages) {
            for (i = 0; i < virDomainNumaGetNodeCount(vm->def->numa); i++) {
                if (virDomainNumaGetNodeMemoryAccessMode(vm->def->numa, i) ==
                    VIR_NUMA_MEM_ACCESS_SHARED) {
                    shmem = true;
                    break;
                }
            }
        }

        if (!shmem) {
            VIR_WARN("Detected vhost-user interface without any shared memory, "
                     "the interface might not be operational");
        }
    }

    VIR_DEBUG("Building emulator command line");
    if (!(cmd = qemuBuildCommandLine(conn, driver, vm->def, priv->monConfig,
                                     priv->monJSON, priv->qemuCaps,
                                     incoming ? incoming->launchURI : NULL,
                                     snapshot, vmop,
                                     &buildCommandLineCallbacks, false,
                                     qemuCheckFips(),
                                     priv->autoNodeset,
                                     &nnicindexes, &nicindexes)))
        goto cleanup;

    if (incoming && incoming->fd != -1)
        virCommandPassFD(cmd, incoming->fd, 0);

    /*
     * Create all per-domain directories in order to make sure domain
     * with any possible seclabels can access it.
     */
    if (qemuProcessMakeDir(driver, vm, cfg->libDir) < 0 ||
        qemuProcessMakeDir(driver, vm, cfg->channelTargetDir) < 0)
        goto cleanup;

    /* now that we know it is about to start call the hook if present */
    if (qemuProcessStartHook(driver, vm,
                             VIR_HOOK_QEMU_OP_START,
                             VIR_HOOK_SUBOP_BEGIN) < 0)
        goto cleanup;

    qemuLogOperation(vm, "starting up", cmd, logCtxt);

    qemuDomainObjCheckTaint(driver, vm, logCtxt);

    qemuDomainLogContextMarkPosition(logCtxt);

    VIR_DEBUG("Clear emulator capabilities: %d",
              cfg->clearEmulatorCapabilities);
    if (cfg->clearEmulatorCapabilities)
        virCommandClearCaps(cmd);

    VIR_DEBUG("Setting up raw IO");
    if (qemuProcessSetupRawIO(driver, vm, cmd) < 0)
        goto cleanup;

    virCommandSetPreExecHook(cmd, qemuProcessHook, &hookData);
    virCommandSetMaxProcesses(cmd, cfg->maxProcesses);
    virCommandSetMaxFiles(cmd, cfg->maxFiles);
    virCommandSetUmask(cmd, 0x002);

    VIR_DEBUG("Setting up security labelling");
    if (virSecurityManagerSetChildProcessLabel(driver->securityManager,
                                               vm->def, cmd) < 0)
        goto cleanup;

    virCommandSetOutputFD(cmd, &logfile);
    virCommandSetErrorFD(cmd, &logfile);
    virCommandNonblockingFDs(cmd);
    virCommandSetPidFile(cmd, priv->pidfile);
    virCommandDaemonize(cmd);
    virCommandRequireHandshake(cmd);

    if (virSecurityManagerPreFork(driver->securityManager) < 0)
        goto cleanup;
    rv = virCommandRun(cmd, NULL);
    virSecurityManagerPostFork(driver->securityManager);

    /* wait for qemu process to show up */
    if (rv == 0) {
        if (virPidFileReadPath(priv->pidfile, &vm->pid) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Domain %s didn't show up"), vm->def->name);
            rv = -1;
        }
        VIR_DEBUG("QEMU vm=%p name=%s running with pid=%llu",
                  vm, vm->def->name, (unsigned long long)vm->pid);
    } else {
        VIR_DEBUG("QEMU vm=%p name=%s failed to spawn",
                  vm, vm->def->name);
    }

    VIR_DEBUG("Writing early domain status to disk");
    if (virDomainSaveStatus(driver->xmlopt, cfg->stateDir, vm, driver->caps) < 0)
        goto cleanup;

    VIR_DEBUG("Waiting for handshake from child");
    if (virCommandHandshakeWait(cmd) < 0) {
        /* Read errors from child that occurred between fork and exec. */
        qemuProcessReportLogError(logCtxt,
                                  _("Process exited prior to exec"));
        goto cleanup;
    }

    VIR_DEBUG("Setting up domain cgroup (if required)");
    if (qemuSetupCgroup(driver, vm, nnicindexes, nicindexes) < 0)
        goto cleanup;

    /* This must be done after cgroup placement to avoid resetting CPU
     * affinity */
    if (!vm->def->cputune.emulatorpin &&
        qemuProcessInitCpuAffinity(vm) < 0)
        goto cleanup;

    VIR_DEBUG("Setting domain security labels");
    if (virSecurityManagerSetAllLabel(driver->securityManager,
                                      vm->def,
                                      incoming ? incoming->path : NULL) < 0)
        goto cleanup;

    /* TODO(ORBIT): Currently using this super ugly fix to set the correct
     *              security labels on quorum disks until quorums are
     *              implemented in libvirt.
     */
    if (vm->def->namespaceData) {
        qemuDomainCmdlineDefPtr qemucmd = vm->def->namespaceData;
        const char *pArg = "file.filename=";
        char *path;
        for (i = 0; i < qemucmd->num_args; i++) {
            if (strstr(qemucmd->args[i], "driver=quorum")) {
                char *start = strstr(qemucmd->args[i], pArg) + strlen(pArg);
                size_t length = strlen(start) - strlen(strchr(start, ',')) + 1;
                if (VIR_ALLOC_N(path, length) < 0)
                    goto cleanup;
                snprintf(path, length, "%s", start);
                VIR_DEBUG("Setting ownership of %s to %d:%d", path,
                          (int) cfg->user, (int) cfg->group);
                if (chown(path, cfg->user, cfg->group) < 0) {
                    virReportSystemError(errno,
                                         _("unable to set ownership of '%s' "
                                           "to user %d:%d"), path,
                                         (int) cfg->user, (int) cfg->group);
                    VIR_FREE(path);
                    goto cleanup;
                }
                VIR_FREE(path);
            }
        }
    }

    /* Security manager labeled all devices, therefore
     * if any operation from now on fails, we need to ask the caller to
     * restore labels.
     */
    ret = -2;

    if (incoming && incoming->fd != -1) {
        /* if there's an fd to migrate from, and it's a pipe, put the
         * proper security label on it
         */
        struct stat stdin_sb;

        VIR_DEBUG("setting security label on pipe used for migration");

        if (fstat(incoming->fd, &stdin_sb) < 0) {
            virReportSystemError(errno,
                                 _("cannot stat fd %d"), incoming->fd);
            goto cleanup;
        }
        if (S_ISFIFO(stdin_sb.st_mode) &&
            virSecurityManagerSetImageFDLabel(driver->securityManager,
                                              vm->def, incoming->fd) < 0)
            goto cleanup;
    }

    VIR_DEBUG("Labelling done, completing handshake to child");
    if (virCommandHandshakeNotify(cmd) < 0)
        goto cleanup;
    VIR_DEBUG("Handshake complete, child running");

    if (rv == -1) /* The VM failed to start; tear filters before taps */
        virDomainConfVMNWFilterTeardown(vm);

    if (rv == -1) /* The VM failed to start */
        goto cleanup;

    VIR_DEBUG("Setting cgroup for emulator (if required)");
    if (qemuSetupCgroupForEmulator(vm) < 0)
        goto cleanup;

    VIR_DEBUG("Setting affinity of emulator threads");
    if (qemuProcessSetEmulatorAffinity(vm) < 0)
        goto cleanup;

    VIR_DEBUG("Waiting for monitor to show up");
    if (qemuProcessWaitForMonitor(driver, vm, asyncJob, priv->qemuCaps, logCtxt) < 0)
        goto cleanup;

    /* Failure to connect to agent shouldn't be fatal */
    if ((rv = qemuConnectAgent(driver, vm)) < 0) {
        if (rv == -2)
            goto cleanup;

        VIR_WARN("Cannot connect to QEMU guest agent for %s",
                 vm->def->name);
        virResetLastError();
        priv->agentError = true;
    }

    VIR_DEBUG("Detecting if required emulator features are present");
    if (!qemuProcessVerifyGuestCPU(driver, vm, asyncJob))
        goto cleanup;

    VIR_DEBUG("Setting up post-init cgroup restrictions");
    if (qemuSetupCpusetMems(vm) < 0)
        goto cleanup;

    VIR_DEBUG("Detecting VCPU PIDs");
    if (qemuDomainDetectVcpuPids(driver, vm, asyncJob) < 0)
        goto cleanup;

    VIR_DEBUG("Detecting IOThread PIDs");
    if (qemuProcessDetectIOThreadPIDs(driver, vm, asyncJob) < 0)
        goto cleanup;

    VIR_DEBUG("Setting vCPU tuning/settings");
    if (qemuProcessSetupVcpus(vm) < 0)
        goto cleanup;

    VIR_DEBUG("Setting IOThread tuning/settings");
    if (qemuProcessSetupIOThreads(vm) < 0)
        goto cleanup;

    VIR_DEBUG("Setting any required VM passwords");
    if (qemuProcessInitPasswords(conn, driver, vm, asyncJob) < 0)
        goto cleanup;

    /* If we have -device, then addresses are assigned explicitly.
     * If not, then we have to detect dynamic ones here */
    if (!virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_DEVICE)) {
        VIR_DEBUG("Determining domain device PCI addresses");
        if (qemuProcessInitPCIAddresses(driver, vm, asyncJob) < 0)
            goto cleanup;
    }

    /* set default link states */
    /* qemu doesn't support setting this on the command line, so
     * enter the monitor */
    VIR_DEBUG("Setting network link states");
    if (qemuProcessSetLinkStates(driver, vm, asyncJob) < 0)
        goto cleanup;

    VIR_DEBUG("Fetching list of active devices");
    if (qemuDomainUpdateDeviceList(driver, vm, asyncJob) < 0)
        goto cleanup;

    VIR_DEBUG("Updating info of memory devices");
    if (qemuDomainUpdateMemoryDeviceInfo(driver, vm, asyncJob) < 0)
        goto cleanup;

    VIR_DEBUG("Setting initial memory amount");
    if (qemuProcessSetupBalloon(driver, vm, asyncJob) < 0)
        goto cleanup;

    /* Since CPUs were not started yet, the balloon could not return the memory
     * to the host and thus cur_balloon needs to be updated so that GetXMLdesc
     * and friends return the correct size in case they can't grab the job */
    if (!incoming && !snapshot &&
        qemuProcessRefreshBalloonState(driver, vm, asyncJob) < 0)
        goto cleanup;

    VIR_DEBUG("Detecting actual memory size for video device");
    if (qemuProcessUpdateVideoRamSize(driver, vm, asyncJob) < 0)
        goto cleanup;

    if (flags & VIR_QEMU_PROCESS_START_AUTODESTROY &&
        qemuProcessAutoDestroyAdd(driver, vm, conn) < 0)
        goto cleanup;

    ret = 0;

 cleanup:
    virCommandFree(cmd);
    qemuDomainLogContextFree(logCtxt);
    virObjectUnref(cfg);
    virObjectUnref(caps);
    VIR_FREE(nicindexes);
    VIR_FREE(nodeset);
    return ret;
}


/**
 * qemuProcessFinishStartup:
 *
 * Finish starting a new domain.
 */
int
qemuProcessFinishStartup(virConnectPtr conn,
                         virQEMUDriverPtr driver,
                         virDomainObjPtr vm,
                         qemuDomainAsyncJob asyncJob,
                         bool startCPUs,
                         virDomainPausedReason pausedReason)
{
    virQEMUDriverConfigPtr cfg = virQEMUDriverGetConfig(driver);
    int ret = -1;

    if (startCPUs) {
        VIR_DEBUG("Starting domain CPUs");
        if (qemuProcessStartCPUs(driver, vm, conn,
                                 VIR_DOMAIN_RUNNING_BOOTED,
                                 asyncJob) < 0) {
            if (!virGetLastError())
                virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                               _("resume operation failed"));
            goto cleanup;
        }
    } else {
        virDomainObjSetState(vm, VIR_DOMAIN_PAUSED, pausedReason);
    }

    VIR_DEBUG("Writing domain status to disk");
    if (virDomainSaveStatus(driver->xmlopt, cfg->stateDir, vm, driver->caps) < 0)
        goto cleanup;

    if (qemuProcessStartHook(driver, vm,
                             VIR_HOOK_QEMU_OP_STARTED,
                             VIR_HOOK_SUBOP_BEGIN) < 0)
        goto cleanup;

    ret = 0;

 cleanup:
    virObjectUnref(cfg);
    return ret;
}


int
qemuProcessStart(virConnectPtr conn,
                 virQEMUDriverPtr driver,
                 virDomainObjPtr vm,
                 qemuDomainAsyncJob asyncJob,
                 const char *migrateFrom,
                 int migrateFd,
                 const char *migratePath,
                 virDomainSnapshotObjPtr snapshot,
                 virNetDevVPortProfileOp vmop,
                 unsigned int flags)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    qemuProcessIncomingDefPtr incoming = NULL;
    unsigned int stopFlags;
    bool relabel = false;
    int ret = -1;
    int rv;

    VIR_DEBUG("conn=%p driver=%p vm=%p name=%s id=%d asyncJob=%s "
              "migrateFrom=%s migrateFd=%d migratePath=%s "
              "snapshot=%p vmop=%d flags=0x%x",
              conn, driver, vm, vm->def->name, vm->def->id,
              qemuDomainAsyncJobTypeToString(asyncJob),
              NULLSTR(migrateFrom), migrateFd, NULLSTR(migratePath),
              snapshot, vmop, flags);

    virCheckFlagsGoto(VIR_QEMU_PROCESS_START_COLD |
                      VIR_QEMU_PROCESS_START_PAUSED |
                      VIR_QEMU_PROCESS_START_AUTODESTROY, cleanup);

    if (qemuProcessInit(driver, vm, !!migrateFrom, !!snapshot) < 0)
        goto cleanup;

    if (migrateFrom) {
        incoming = qemuProcessIncomingDefNew(priv->qemuCaps, NULL, migrateFrom,
                                             migrateFd, migratePath);
        if (!incoming)
            goto stop;
    }

    if ((rv = qemuProcessLaunch(conn, driver, vm, asyncJob, incoming,
                                snapshot, vmop, flags)) < 0) {
        if (rv == -1)
            relabel = true;
        goto stop;
    }
    relabel = true;

    if (incoming &&
        incoming->deferredURI &&
        qemuMigrationRunIncoming(driver, vm, incoming->deferredURI, asyncJob) < 0)
        goto stop;

    if (qemuProcessFinishStartup(conn, driver, vm, asyncJob,
                                 !(flags & VIR_QEMU_PROCESS_START_PAUSED),
                                 incoming ?
                                 VIR_DOMAIN_PAUSED_MIGRATION :
                                 VIR_DOMAIN_PAUSED_USER) < 0)
        goto stop;

    /* Keep watching qemu log for errors during incoming migration, otherwise
     * unset reporting errors from qemu log. */
    if (!incoming)
        qemuMonitorSetDomainLog(priv->mon, NULL, NULL, NULL);

    ret = 0;

 cleanup:
    qemuProcessIncomingDefFree(incoming);
    return ret;

 stop:
    stopFlags = 0;
    if (!relabel)
        stopFlags |= VIR_QEMU_PROCESS_STOP_NO_RELABEL;
    if (migrateFrom)
        stopFlags |= VIR_QEMU_PROCESS_STOP_MIGRATED;
    if (priv->mon)
        qemuMonitorSetDomainLog(priv->mon, NULL, NULL, NULL);
    qemuProcessStop(driver, vm, VIR_DOMAIN_SHUTOFF_FAILED, stopFlags);
    goto cleanup;
}


int
qemuProcessKill(virDomainObjPtr vm, unsigned int flags)
{
    int ret;

    VIR_DEBUG("vm=%p name=%s pid=%llu flags=%x",
              vm, vm->def->name,
              (unsigned long long)vm->pid, flags);

    if (!(flags & VIR_QEMU_PROCESS_KILL_NOCHECK)) {
        if (!virDomainObjIsActive(vm)) {
            VIR_DEBUG("VM '%s' not active", vm->def->name);
            return 0;
        }
    }

    if (flags & VIR_QEMU_PROCESS_KILL_NOWAIT) {
        virProcessKill(vm->pid,
                       (flags & VIR_QEMU_PROCESS_KILL_FORCE) ?
                       SIGKILL : SIGTERM);
        return 0;
    }

    ret = virProcessKillPainfully(vm->pid,
                                  !!(flags & VIR_QEMU_PROCESS_KILL_FORCE));

    return ret;
}


void qemuProcessStop(virQEMUDriverPtr driver,
                     virDomainObjPtr vm,
                     virDomainShutoffReason reason,
                     unsigned int flags)
{
    int ret;
    int retries = 0;
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virErrorPtr orig_err;
    virDomainDefPtr def;
    virNetDevVPortProfilePtr vport = NULL;
    size_t i;
    char *timestamp;
    char *tmppath = NULL;
    virQEMUDriverConfigPtr cfg = virQEMUDriverGetConfig(driver);
    qemuDomainLogContextPtr logCtxt = NULL;

    VIR_DEBUG("Shutting down vm=%p name=%s id=%d pid=%llu flags=%x",
              vm, vm->def->name, vm->def->id,
              (unsigned long long)vm->pid, flags);

    if (!virDomainObjIsActive(vm)) {
        VIR_DEBUG("VM '%s' not active", vm->def->name);
        virObjectUnref(cfg);
        return;
    }

    /* This method is routinely used in clean up paths. Disable error
     * reporting so we don't squash a legit error. */
    orig_err = virSaveLastError();

    /*
     * We may unlock the vm in qemuProcessKill(), and another thread
     * can lock the vm, and then call qemuProcessStop(). So we should
     * set vm->def->id to -1 here to avoid qemuProcessStop() to be called twice.
     */
    vm->def->id = -1;

    if (virAtomicIntDecAndTest(&driver->nactive) && driver->inhibitCallback)
        driver->inhibitCallback(false, driver->inhibitOpaque);

    /* Wake up anything waiting on domain condition */
    virDomainObjBroadcast(vm);

    if ((logCtxt = qemuDomainLogContextNew(driver, vm,
                                           QEMU_DOMAIN_LOG_CONTEXT_MODE_STOP))) {
        if ((timestamp = virTimeStringNow()) != NULL) {
            qemuDomainLogContextWrite(logCtxt, "%s: shutting down\n", timestamp);
            VIR_FREE(timestamp);
        }
        qemuDomainLogContextFree(logCtxt);
    }

    /* Clear network bandwidth */
    virDomainClearNetBandwidth(vm);

    virDomainConfVMNWFilterTeardown(vm);

    if (cfg->macFilter) {
        def = vm->def;
        for (i = 0; i < def->nnets; i++) {
            virDomainNetDefPtr net = def->nets[i];
            if (net->ifname == NULL)
                continue;
            ignore_value(ebtablesRemoveForwardAllowIn(driver->ebtables,
                                                      net->ifname,
                                                      &net->mac));
        }
    }

    virPortAllocatorRelease(driver->migrationPorts, priv->nbdPort);
    priv->nbdPort = 0;

    if (priv->agent) {
        qemuAgentClose(priv->agent);
        priv->agent = NULL;
        priv->agentError = false;
    }

    if (priv->mon) {
        qemuMonitorClose(priv->mon);
        priv->mon = NULL;
    }

    if (priv->monConfig) {
        if (priv->monConfig->type == VIR_DOMAIN_CHR_TYPE_UNIX)
            unlink(priv->monConfig->data.nix.path);
        virDomainChrSourceDefFree(priv->monConfig);
        priv->monConfig = NULL;
    }

    ignore_value(virAsprintf(&tmppath, "%s/domain-%s",
                             cfg->libDir, vm->def->name));
    virFileDeleteTree(tmppath);
    VIR_FREE(tmppath);

    ignore_value(virAsprintf(&tmppath, "%s/domain-%s",
                             cfg->channelTargetDir, vm->def->name));
    virFileDeleteTree(tmppath);
    VIR_FREE(tmppath);

    ignore_value(virDomainChrDefForeach(vm->def,
                                        false,
                                        qemuProcessCleanupChardevDevice,
                                        NULL));


    /* shut it off for sure */
    ignore_value(qemuProcessKill(vm,
                                 VIR_QEMU_PROCESS_KILL_FORCE|
                                 VIR_QEMU_PROCESS_KILL_NOCHECK));

    qemuDomainCleanupRun(driver, vm);

    /* Stop autodestroy in case guest is restarted */
    qemuProcessAutoDestroyRemove(driver, vm);

    /* now that we know it's stopped call the hook if present */
    if (virHookPresent(VIR_HOOK_DRIVER_QEMU)) {
        char *xml = qemuDomainDefFormatXML(driver, vm->def, 0);

        /* we can't stop the operation even if the script raised an error */
        ignore_value(virHookCall(VIR_HOOK_DRIVER_QEMU, vm->def->name,
                                 VIR_HOOK_QEMU_OP_STOPPED, VIR_HOOK_SUBOP_END,
                                 NULL, xml, NULL));
        VIR_FREE(xml);
    }

    /* Reset Security Labels unless caller don't want us to */
    if (!(flags & VIR_QEMU_PROCESS_STOP_NO_RELABEL))
        virSecurityManagerRestoreAllLabel(driver->securityManager,
                                          vm->def,
                                          !!(flags & VIR_QEMU_PROCESS_STOP_MIGRATED));
    virSecurityManagerReleaseLabel(driver->securityManager, vm->def);

    for (i = 0; i < vm->def->ndisks; i++) {
        virDomainDeviceDef dev;
        virDomainDiskDefPtr disk = vm->def->disks[i];

        dev.type = VIR_DOMAIN_DEVICE_DISK;
        dev.data.disk = disk;
        ignore_value(qemuRemoveSharedDevice(driver, &dev, vm->def->name));
    }

    /* Clear out dynamically assigned labels */
    for (i = 0; i < vm->def->nseclabels; i++) {
        if (vm->def->seclabels[i]->type == VIR_DOMAIN_SECLABEL_DYNAMIC)
            VIR_FREE(vm->def->seclabels[i]->label);
        VIR_FREE(vm->def->seclabels[i]->imagelabel);
    }

    virStringFreeList(priv->qemuDevices);
    priv->qemuDevices = NULL;

    virDomainDefClearDeviceAliases(vm->def);
    if (!priv->persistentAddrs) {
        virDomainDefClearPCIAddresses(vm->def);
        virDomainPCIAddressSetFree(priv->pciaddrs);
        priv->pciaddrs = NULL;
        virDomainDefClearCCWAddresses(vm->def);
        virDomainCCWAddressSetFree(priv->ccwaddrs);
        priv->ccwaddrs = NULL;
        virDomainVirtioSerialAddrSetFree(priv->vioserialaddrs);
        priv->vioserialaddrs = NULL;
    }

    qemuHostdevReAttachDomainDevices(driver, vm->def);

    def = vm->def;
    for (i = 0; i < def->nnets; i++) {
        virDomainNetDefPtr net = def->nets[i];
        vport = virDomainNetGetActualVirtPortProfile(net);

        switch (virDomainNetGetActualType(net)) {
        case VIR_DOMAIN_NET_TYPE_DIRECT:
            ignore_value(virNetDevMacVLanDeleteWithVPortProfile(
                             net->ifname, &net->mac,
                             virDomainNetGetActualDirectDev(net),
                             virDomainNetGetActualDirectMode(net),
                             virDomainNetGetActualVirtPortProfile(net),
                             cfg->stateDir));
            break;
        case VIR_DOMAIN_NET_TYPE_BRIDGE:
        case VIR_DOMAIN_NET_TYPE_NETWORK:
#ifdef VIR_NETDEV_TAP_REQUIRE_MANUAL_CLEANUP
            if (!(vport && vport->virtPortType == VIR_NETDEV_VPORT_PROFILE_OPENVSWITCH))
                ignore_value(virNetDevTapDelete(net->ifname, net->backend.tap));
#endif
            break;
        }
        /* release the physical device (or any other resources used by
         * this interface in the network driver
         */
        if (vport) {
            if (vport->virtPortType == VIR_NETDEV_VPORT_PROFILE_MIDONET) {
                ignore_value(virNetDevMidonetUnbindPort(vport));
            } else if (vport->virtPortType == VIR_NETDEV_VPORT_PROFILE_OPENVSWITCH) {
                ignore_value(virNetDevOpenvswitchRemovePort(
                                 virDomainNetGetActualBridgeName(net),
                                 net->ifname));
            }
        }

        /* kick the device out of the hostdev list too */
        virDomainNetRemoveHostdev(def, net);
        networkReleaseActualDevice(vm->def, net);
    }

 retry:
    if ((ret = qemuRemoveCgroup(vm)) < 0) {
        if (ret == -EBUSY && (retries++ < 5)) {
            usleep(200*1000);
            goto retry;
        }
        VIR_WARN("Failed to remove cgroup for %s",
                 vm->def->name);
    }
    virCgroupFree(&priv->cgroup);

    qemuProcessRemoveDomainStatus(driver, vm);

    /* Remove VNC and Spice ports from port reservation bitmap, but only if
       they were reserved by the driver (autoport=yes)
    */
    for (i = 0; i < vm->def->ngraphics; ++i) {
        virDomainGraphicsDefPtr graphics = vm->def->graphics[i];
        if (graphics->type == VIR_DOMAIN_GRAPHICS_TYPE_VNC) {
            if (graphics->data.vnc.autoport) {
                virPortAllocatorRelease(driver->remotePorts,
                                        graphics->data.vnc.port);
            } else if (graphics->data.vnc.portReserved) {
                virPortAllocatorSetUsed(driver->remotePorts,
                                        graphics->data.spice.port,
                                        false);
                graphics->data.vnc.portReserved = false;
            }
            virPortAllocatorRelease(driver->webSocketPorts,
                                    graphics->data.vnc.websocket);
        }
        if (graphics->type == VIR_DOMAIN_GRAPHICS_TYPE_SPICE) {
            if (graphics->data.spice.autoport) {
                virPortAllocatorRelease(driver->remotePorts,
                                        graphics->data.spice.port);
                virPortAllocatorRelease(driver->remotePorts,
                                        graphics->data.spice.tlsPort);
            } else {
                if (graphics->data.spice.portReserved) {
                    virPortAllocatorSetUsed(driver->remotePorts,
                                            graphics->data.spice.port,
                                            false);
                    graphics->data.spice.portReserved = false;
                }

                if (graphics->data.spice.tlsPortReserved) {
                    virPortAllocatorSetUsed(driver->remotePorts,
                                            graphics->data.spice.tlsPort,
                                            false);
                    graphics->data.spice.tlsPortReserved = false;
                }
            }
        }
    }

    vm->taint = 0;
    vm->pid = -1;
    virDomainObjSetState(vm, VIR_DOMAIN_SHUTOFF, reason);
    VIR_FREE(priv->vcpupids);
    priv->nvcpupids = 0;
    for (i = 0; i < vm->def->niothreadids; i++)
        vm->def->iothreadids[i]->thread_id = 0;
    virObjectUnref(priv->qemuCaps);
    priv->qemuCaps = NULL;
    VIR_FREE(priv->pidfile);

    /* The "release" hook cleans up additional resources */
    if (virHookPresent(VIR_HOOK_DRIVER_QEMU)) {
        char *xml = qemuDomainDefFormatXML(driver, vm->def, 0);

        /* we can't stop the operation even if the script raised an error */
        virHookCall(VIR_HOOK_DRIVER_QEMU, vm->def->name,
                    VIR_HOOK_QEMU_OP_RELEASE, VIR_HOOK_SUBOP_END,
                    NULL, xml, NULL);
        VIR_FREE(xml);
    }

    if (vm->newDef) {
        virDomainDefFree(vm->def);
        vm->def = vm->newDef;
        vm->def->id = -1;
        vm->newDef = NULL;
    }

    if (orig_err) {
        virSetError(orig_err);
        virFreeError(orig_err);
    }
    virObjectUnref(cfg);
}


int qemuProcessAttach(virConnectPtr conn ATTRIBUTE_UNUSED,
                      virQEMUDriverPtr driver,
                      virDomainObjPtr vm,
                      pid_t pid,
                      const char *pidfile,
                      virDomainChrSourceDefPtr monConfig,
                      bool monJSON)
{
    size_t i;
    qemuDomainLogContextPtr logCtxt = NULL;
    char *timestamp;
    qemuDomainObjPrivatePtr priv = vm->privateData;
    bool running = true;
    virDomainPausedReason reason;
    virSecurityLabelPtr seclabel = NULL;
    virSecurityLabelDefPtr seclabeldef = NULL;
    bool seclabelgen = false;
    virSecurityManagerPtr* sec_managers = NULL;
    const char *model;
    virQEMUDriverConfigPtr cfg = virQEMUDriverGetConfig(driver);
    virCapsPtr caps = NULL;
    bool active = false;
    int ret;

    VIR_DEBUG("Beginning VM attach process");

    if (virDomainObjIsActive(vm)) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       "%s", _("VM is already active"));
        virObjectUnref(cfg);
        return -1;
    }

    if (!(caps = virQEMUDriverGetCapabilities(driver, false)))
        goto error;

    /* Do this upfront, so any part of the startup process can add
     * runtime state to vm->def that won't be persisted. This let's us
     * report implicit runtime defaults in the XML, like vnc listen/socket
     */
    VIR_DEBUG("Setting current domain def as transient");
    if (virDomainObjSetDefTransient(caps, driver->xmlopt, vm, true) < 0)
        goto error;

    vm->def->id = qemuDriverAllocateID(driver);

    if (virAtomicIntInc(&driver->nactive) == 1 && driver->inhibitCallback)
        driver->inhibitCallback(true, driver->inhibitOpaque);
    active = true;

    if (virFileMakePath(cfg->logDir) < 0) {
        virReportSystemError(errno,
                             _("cannot create log directory %s"),
                             cfg->logDir);
        goto error;
    }

    VIR_FREE(priv->pidfile);
    if (VIR_STRDUP(priv->pidfile, pidfile) < 0)
        goto error;

    vm->pid = pid;

    VIR_DEBUG("Detect security driver config");
    sec_managers = virSecurityManagerGetNested(driver->securityManager);
    if (sec_managers == NULL)
        goto error;

    for (i = 0; sec_managers[i]; i++) {
        seclabelgen = false;
        model = virSecurityManagerGetModel(sec_managers[i]);
        seclabeldef = virDomainDefGetSecurityLabelDef(vm->def, model);
        if (seclabeldef == NULL) {
            if (!(seclabeldef = virSecurityLabelDefNew(model)))
                goto error;
            seclabelgen = true;
        }
        seclabeldef->type = VIR_DOMAIN_SECLABEL_STATIC;
        if (VIR_ALLOC(seclabel) < 0)
            goto error;
        if (virSecurityManagerGetProcessLabel(sec_managers[i],
                                              vm->def, vm->pid, seclabel) < 0)
            goto error;

        if (VIR_STRDUP(seclabeldef->model, model) < 0)
            goto error;

        if (VIR_STRDUP(seclabeldef->label, seclabel->label) < 0)
            goto error;
        VIR_FREE(seclabel);

        if (seclabelgen) {
            if (VIR_APPEND_ELEMENT(vm->def->seclabels, vm->def->nseclabels, seclabeldef) < 0)
                goto error;
            seclabelgen = false;
        }
    }

    if (virSecurityManagerCheckAllLabel(driver->securityManager, vm->def) < 0)
        goto error;
    if (virSecurityManagerGenLabel(driver->securityManager, vm->def) < 0)
        goto error;

    VIR_DEBUG("Creating domain log file");
    if (!(logCtxt = qemuDomainLogContextNew(driver, vm,
                                            QEMU_DOMAIN_LOG_CONTEXT_MODE_ATTACH)))
        goto error;

    VIR_DEBUG("Determining emulator version");
    virObjectUnref(priv->qemuCaps);
    if (!(priv->qemuCaps = virQEMUCapsCacheLookupCopy(driver->qemuCapsCache,
                                                      vm->def->emulator,
                                                      vm->def->os.machine)))
        goto error;

    VIR_DEBUG("Preparing monitor state");
    priv->monConfig = monConfig;
    monConfig = NULL;
    priv->monJSON = monJSON;

    priv->gotShutdown = false;

    /*
     * Normally PCI addresses are assigned in the virDomainCreate
     * or virDomainDefine methods. We might still need to assign
     * some here to cope with the question of upgrades. Regardless
     * we also need to populate the PCI address set cache for later
     * use in hotplug
     */
    if (virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_DEVICE)) {
        VIR_DEBUG("Assigning domain PCI addresses");
        if ((qemuDomainAssignAddresses(vm->def, priv->qemuCaps, vm)) < 0)
            goto error;
    }

    if ((timestamp = virTimeStringNow()) == NULL)
        goto error;

    qemuDomainLogContextWrite(logCtxt, "%s: attaching\n", timestamp);
    VIR_FREE(timestamp);

    qemuDomainObjTaint(driver, vm, VIR_DOMAIN_TAINT_EXTERNAL_LAUNCH, logCtxt);

    VIR_DEBUG("Waiting for monitor to show up");
    if (qemuProcessWaitForMonitor(driver, vm, QEMU_ASYNC_JOB_NONE, priv->qemuCaps, NULL) < 0)
        goto error;

    /* Failure to connect to agent shouldn't be fatal */
    if ((ret = qemuConnectAgent(driver, vm)) < 0) {
        if (ret == -2)
            goto error;

        VIR_WARN("Cannot connect to QEMU guest agent for %s",
                 vm->def->name);
        virResetLastError();
        priv->agentError = true;
    }

    VIR_DEBUG("Detecting VCPU PIDs");
    if (qemuDomainDetectVcpuPids(driver, vm, QEMU_ASYNC_JOB_NONE) < 0)
        goto error;

    VIR_DEBUG("Detecting IOThread PIDs");
    if (qemuProcessDetectIOThreadPIDs(driver, vm, QEMU_ASYNC_JOB_NONE) < 0)
        goto error;

    /* If we have -device, then addresses are assigned explicitly.
     * If not, then we have to detect dynamic ones here */
    if (!virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_DEVICE)) {
        VIR_DEBUG("Determining domain device PCI addresses");
        if (qemuProcessInitPCIAddresses(driver, vm, QEMU_ASYNC_JOB_NONE) < 0)
            goto error;
    }

    VIR_DEBUG("Getting initial memory amount");
    qemuDomainObjEnterMonitor(driver, vm);
    if (qemuMonitorGetBalloonInfo(priv->mon, &vm->def->mem.cur_balloon) < 0)
        goto exit_monitor;
    if (qemuMonitorGetStatus(priv->mon, &running, &reason) < 0)
        goto exit_monitor;
    if (qemuMonitorGetVirtType(priv->mon, &vm->def->virtType) < 0)
        goto exit_monitor;
    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        goto error;

    if (running) {
        virDomainObjSetState(vm, VIR_DOMAIN_RUNNING,
                             VIR_DOMAIN_RUNNING_UNPAUSED);
        if (vm->def->memballoon &&
            vm->def->memballoon->model == VIR_DOMAIN_MEMBALLOON_MODEL_VIRTIO &&
            vm->def->memballoon->period) {
            qemuDomainObjEnterMonitor(driver, vm);
            qemuMonitorSetMemoryStatsPeriod(priv->mon,
                                            vm->def->memballoon->period);
            if (qemuDomainObjExitMonitor(driver, vm) < 0)
                goto error;
        }
    } else {
        virDomainObjSetState(vm, VIR_DOMAIN_PAUSED, reason);
    }

    VIR_DEBUG("Writing domain status to disk");
    if (virDomainSaveStatus(driver->xmlopt, cfg->stateDir, vm, driver->caps) < 0)
        goto error;

    /* Run an hook to allow admins to do some magic */
    if (virHookPresent(VIR_HOOK_DRIVER_QEMU)) {
        char *xml = qemuDomainDefFormatXML(driver, vm->def, 0);
        int hookret;

        hookret = virHookCall(VIR_HOOK_DRIVER_QEMU, vm->def->name,
                              VIR_HOOK_QEMU_OP_ATTACH, VIR_HOOK_SUBOP_BEGIN,
                              NULL, xml, NULL);
        VIR_FREE(xml);

        /*
         * If the script raised an error abort the launch
         */
        if (hookret < 0)
            goto error;
    }

    qemuDomainLogContextFree(logCtxt);
    VIR_FREE(seclabel);
    VIR_FREE(sec_managers);
    virObjectUnref(cfg);
    virObjectUnref(caps);

    return 0;

 exit_monitor:
    ignore_value(qemuDomainObjExitMonitor(driver, vm));
 error:
    /* We jump here if we failed to attach to the VM for any reason.
     * Leave the domain running, but pretend we never attempted to
     * attach to it.  */
    if (active && virAtomicIntDecAndTest(&driver->nactive) &&
        driver->inhibitCallback)
        driver->inhibitCallback(false, driver->inhibitOpaque);
    qemuDomainLogContextFree(logCtxt);
    VIR_FREE(seclabel);
    VIR_FREE(sec_managers);
    if (seclabelgen)
        virSecurityLabelDefFree(seclabeldef);
    virDomainChrSourceDefFree(monConfig);
    virObjectUnref(cfg);
    virObjectUnref(caps);
    return -1;
}


static virDomainObjPtr
qemuProcessAutoDestroy(virDomainObjPtr dom,
                       virConnectPtr conn,
                       void *opaque)
{
    virQEMUDriverPtr driver = opaque;
    qemuDomainObjPrivatePtr priv = dom->privateData;
    virObjectEventPtr event = NULL;
    unsigned int stopFlags = 0;

    VIR_DEBUG("vm=%s, conn=%p", dom->def->name, conn);

    virObjectRef(dom);

    if (priv->job.asyncJob == QEMU_ASYNC_JOB_MIGRATION_IN)
        stopFlags |= VIR_QEMU_PROCESS_STOP_MIGRATED;

    if (priv->job.asyncJob) {
        VIR_DEBUG("vm=%s has long-term job active, cancelling",
                  dom->def->name);
        qemuDomainObjDiscardAsyncJob(driver, dom);
    }

    if (qemuDomainObjBeginJob(driver, dom,
                              QEMU_JOB_DESTROY) < 0)
        goto cleanup;

    VIR_DEBUG("Killing domain");

    qemuProcessStop(driver, dom, VIR_DOMAIN_SHUTOFF_DESTROYED, stopFlags);

    virDomainAuditStop(dom, "destroyed");
    event = virDomainEventLifecycleNewFromObj(dom,
                                     VIR_DOMAIN_EVENT_STOPPED,
                                     VIR_DOMAIN_EVENT_STOPPED_DESTROYED);

    qemuDomainObjEndJob(driver, dom);

    qemuDomainRemoveInactive(driver, dom);

    qemuDomainEventQueue(driver, event);

 cleanup:
    virDomainObjEndAPI(&dom);
    return dom;
}

int qemuProcessAutoDestroyAdd(virQEMUDriverPtr driver,
                              virDomainObjPtr vm,
                              virConnectPtr conn)
{
    VIR_DEBUG("vm=%s, conn=%p", vm->def->name, conn);
    return virCloseCallbacksSet(driver->closeCallbacks, vm, conn,
                                qemuProcessAutoDestroy);
}

int qemuProcessAutoDestroyRemove(virQEMUDriverPtr driver,
                                 virDomainObjPtr vm)
{
    int ret;
    VIR_DEBUG("vm=%s", vm->def->name);
    ret = virCloseCallbacksUnset(driver->closeCallbacks, vm,
                                 qemuProcessAutoDestroy);
    return ret;
}

bool qemuProcessAutoDestroyActive(virQEMUDriverPtr driver,
                                  virDomainObjPtr vm)
{
    virCloseCallback cb;
    VIR_DEBUG("vm=%s", vm->def->name);
    cb = virCloseCallbacksGet(driver->closeCallbacks, vm, NULL);
    return cb == qemuProcessAutoDestroy;
}
