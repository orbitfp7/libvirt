/*
 * libxl_migration.c: methods for handling migration with libxenlight
 *
 * Copyright (C) 2014-2015 SUSE LINUX Products GmbH, Nuernberg, Germany.
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
 * Authors:
 *     Jim Fehlig <jfehlig@suse.com>
 *     Chunyan Liu <cyliu@suse.com>
 */

#include <config.h>

#include "internal.h"
#include "virlog.h"
#include "virerror.h"
#include "virconf.h"
#include "datatypes.h"
#include "virfile.h"
#include "viralloc.h"
#include "viruuid.h"
#include "vircommand.h"
#include "virstring.h"
#include "virobject.h"
#include "virthread.h"
#include "rpc/virnetsocket.h"
#include "libxl_domain.h"
#include "libxl_driver.h"
#include "libxl_conf.h"
#include "libxl_migration.h"
#include "locking/domain_lock.h"
#include "virtypedparam.h"

#define VIR_FROM_THIS VIR_FROM_LIBXL

VIR_LOG_INIT("libxl.libxl_migration");

typedef struct _libxlMigrationDstArgs {
    virObject parent;

    int recvfd;
    virConnectPtr conn;
    virDomainObjPtr vm;
    unsigned int flags;

    /* for freeing listen sockets */
    virNetSocketPtr *socks;
    size_t nsocks;
} libxlMigrationDstArgs;

static virClassPtr libxlMigrationDstArgsClass;

static void
libxlMigrationDstArgsDispose(void *obj)
{
    libxlMigrationDstArgs *args = obj;

    VIR_FREE(args->socks);
}

static int
libxlMigrationDstArgsOnceInit(void)
{
    if (!(libxlMigrationDstArgsClass = virClassNew(virClassForObject(),
                                                   "libxlMigrationDstArgs",
                                                   sizeof(libxlMigrationDstArgs),
                                                   libxlMigrationDstArgsDispose)))
        return -1;

    return 0;
}

VIR_ONCE_GLOBAL_INIT(libxlMigrationDstArgs)

static void
libxlDoMigrateReceive(void *opaque)
{
    libxlMigrationDstArgs *args = opaque;
    virDomainObjPtr vm = args->vm;
    virNetSocketPtr *socks = args->socks;
    size_t nsocks = args->nsocks;
    libxlDriverPrivatePtr driver = args->conn->privateData;
    int recvfd = args->recvfd;
    size_t i;
    int ret;
    bool remove_dom = 0;

    virObjectLock(vm);
    if (libxlDomainObjBeginJob(driver, vm, LIBXL_JOB_MODIFY) < 0)
        goto cleanup;

    /*
     * Always start the domain paused.  If needed, unpause in the
     * finish phase, after transfer of the domain is complete.
     */
    ret = libxlDomainStart(driver, vm, true, recvfd);

    if (ret < 0 && !vm->persistent)
        remove_dom = true;

    /* Remove all listen socks from event handler, and close them. */
    for (i = 0; i < nsocks; i++) {
        virNetSocketRemoveIOCallback(socks[i]);
        virNetSocketClose(socks[i]);
        virObjectUnref(socks[i]);
        socks[i] = NULL;
    }
    args->nsocks = 0;
    VIR_FORCE_CLOSE(recvfd);
    virObjectUnref(args);

    if (!libxlDomainObjEndJob(driver, vm))
        vm = NULL;

 cleanup:
    if (remove_dom && vm) {
        virDomainObjListRemove(driver->domains, vm);
        vm = NULL;
    }
    if (vm)
        virObjectUnlock(vm);
}


static void
libxlMigrateReceive(virNetSocketPtr sock,
                    int events ATTRIBUTE_UNUSED,
                    void *opaque)
{
    libxlMigrationDstArgs *args = opaque;
    virNetSocketPtr *socks = args->socks;
    size_t nsocks = args->nsocks;
    virNetSocketPtr client_sock;
    int recvfd = -1;
    virThread thread;
    size_t i;

    /* Accept migration connection */
    if (virNetSocketAccept(sock, &client_sock) < 0 || !client_sock) {
        virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                       _("Failed to accept migration connection"));
        goto fail;
    }
    VIR_DEBUG("Accepted migration connection."
              "  Spawing thread to process migration data");
    recvfd = virNetSocketDupFD(client_sock, true);
    virObjectUnref(client_sock);

    /*
     * Avoid blocking the event loop.  Start a thread to receive
     * the migration data
     */
    args->recvfd = recvfd;
    if (virThreadCreate(&thread, false,
                        libxlDoMigrateReceive, args) < 0) {
        virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                       _("Failed to create thread for receiving migration data"));
        goto fail;
    }

    return;

 fail:
    /* Remove all listen socks from event handler, and close them. */
    for (i = 0; i < nsocks; i++) {
        virNetSocketUpdateIOCallback(socks[i], 0);
        virNetSocketRemoveIOCallback(socks[i]);
        virNetSocketClose(socks[i]);
        socks[i] = NULL;
    }
    args->nsocks = 0;
    VIR_FORCE_CLOSE(recvfd);
    virObjectUnref(args);
}

static int
libxlDoMigrateSend(libxlDriverPrivatePtr driver,
                   virDomainObjPtr vm,
                   unsigned long flags,
                   int sockfd)
{
    libxlDriverConfigPtr cfg = libxlDriverConfigGet(driver);
    int xl_flags = 0;
    int ret;

    if (flags & VIR_MIGRATE_LIVE)
        xl_flags = LIBXL_SUSPEND_LIVE;

    ret = libxl_domain_suspend(cfg->ctx, vm->def->id, sockfd,
                               xl_flags, NULL);
    if (ret != 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Failed to send migration data to destination host"));
        ret = -1;
    }

    virObjectUnref(cfg);
    return ret;
}

static bool
libxlDomainMigrationIsAllowed(virDomainDefPtr def)
{
    /* Migration is not allowed if definition contains any hostdevs */
    if (def->nhostdevs > 0) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("domain has assigned host devices"));
        return false;
    }

    return true;
}

char *
libxlDomainMigrationBegin(virConnectPtr conn,
                          virDomainObjPtr vm,
                          const char *xmlin)
{
    libxlDriverPrivatePtr driver = conn->privateData;
    libxlDriverConfigPtr cfg = libxlDriverConfigGet(driver);
    virDomainDefPtr tmpdef = NULL;
    virDomainDefPtr def;
    char *xml = NULL;

    if (libxlDomainObjBeginJob(driver, vm, LIBXL_JOB_MODIFY) < 0)
        goto cleanup;

    if (xmlin) {
        if (!(tmpdef = virDomainDefParseString(xmlin, cfg->caps,
                                               driver->xmlopt,
                                               VIR_DOMAIN_DEF_PARSE_INACTIVE)))
            goto endjob;

        if (!libxlDomainDefCheckABIStability(driver, vm->def, tmpdef))
            goto endjob;

        def = tmpdef;
    } else {
        def = vm->def;
    }

    if (!libxlDomainMigrationIsAllowed(def))
        goto endjob;

    xml = virDomainDefFormat(def, cfg->caps, VIR_DOMAIN_DEF_FORMAT_SECURE);

 endjob:
    if (!libxlDomainObjEndJob(driver, vm))
        vm = NULL;

 cleanup:
    if (vm)
        virObjectUnlock(vm);

    virDomainDefFree(tmpdef);
    virObjectUnref(cfg);
    return xml;
}

virDomainDefPtr
libxlDomainMigrationPrepareDef(libxlDriverPrivatePtr driver,
                               const char *dom_xml,
                               const char *dname)
{
    libxlDriverConfigPtr cfg = libxlDriverConfigGet(driver);
    virDomainDefPtr def;
    char *name = NULL;

    if (!dom_xml) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("no domain XML passed"));
        return NULL;
    }

    if (!(def = virDomainDefParseString(dom_xml, cfg->caps, driver->xmlopt,
                                        VIR_DOMAIN_DEF_PARSE_INACTIVE)))
        goto cleanup;

    if (dname) {
        name = def->name;
        if (VIR_STRDUP(def->name, dname) < 0) {
            virDomainDefFree(def);
            def = NULL;
        }
    }

 cleanup:
    virObjectUnref(cfg);
    VIR_FREE(name);
    return def;
}

int
libxlDomainMigrationPrepare(virConnectPtr dconn,
                            virDomainDefPtr *def,
                            const char *uri_in,
                            char **uri_out,
                            unsigned int flags)
{
    libxlDriverPrivatePtr driver = dconn->privateData;
    virDomainObjPtr vm = NULL;
    char *hostname = NULL;
    unsigned short port;
    char portstr[100];
    virURIPtr uri = NULL;
    virNetSocketPtr *socks = NULL;
    size_t nsocks = 0;
    int nsocks_listen = 0;
    libxlMigrationDstArgs *args = NULL;
    size_t i;
    int ret = -1;

    if (!(vm = virDomainObjListAdd(driver->domains, *def,
                                   driver->xmlopt,
                                   VIR_DOMAIN_OBJ_LIST_ADD_LIVE |
                                   VIR_DOMAIN_OBJ_LIST_ADD_CHECK_LIVE,
                                   NULL)))
        goto error;
    *def = NULL;

    /* Create socket connection to receive migration data */
    if (!uri_in) {
        if ((hostname = virGetHostname()) == NULL)
            goto error;

        if (STRPREFIX(hostname, "localhost")) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("hostname on destination resolved to localhost,"
                             " but migration requires an FQDN"));
            goto error;
        }

        if (virPortAllocatorAcquire(driver->migrationPorts, &port) < 0)
            goto error;

        if (virAsprintf(uri_out, "tcp://%s:%d", hostname, port) < 0)
            goto error;
    } else {
        if (!(STRPREFIX(uri_in, "tcp://"))) {
            /* not full URI, add prefix tcp:// */
            char *tmp;
            if (virAsprintf(&tmp, "tcp://%s", uri_in) < 0)
                goto error;
            uri = virURIParse(tmp);
            VIR_FREE(tmp);
        } else {
            uri = virURIParse(uri_in);
        }

        if (uri == NULL) {
            virReportError(VIR_ERR_INVALID_ARG,
                           _("unable to parse URI: %s"),
                           uri_in);
            goto error;
        }

        if (uri->server == NULL) {
            virReportError(VIR_ERR_INVALID_ARG,
                           _("missing host in migration URI: %s"),
                           uri_in);
            goto error;
        } else {
            hostname = uri->server;
        }

        if (uri->port == 0) {
            if (virPortAllocatorAcquire(driver->migrationPorts, &port) < 0)
                goto error;

        } else {
            port = uri->port;
        }

        if (virAsprintf(uri_out, "tcp://%s:%d", hostname, port) < 0)
            goto error;
    }

    snprintf(portstr, sizeof(portstr), "%d", port);

    if (virNetSocketNewListenTCP(hostname, portstr,
                                 AF_UNSPEC,
                                 &socks, &nsocks) < 0) {
        virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                       _("Fail to create socket for incoming migration"));
        goto error;
    }

    if (libxlMigrationDstArgsInitialize() < 0)
        goto error;

    if (!(args = virObjectNew(libxlMigrationDstArgsClass)))
        goto error;

    args->conn = dconn;
    args->vm = vm;
    args->flags = flags;
    args->socks = socks;
    args->nsocks = nsocks;

    for (i = 0; i < nsocks; i++) {
        if (virNetSocketSetBlocking(socks[i], true) < 0)
             continue;

        if (virNetSocketListen(socks[i], 1) < 0)
            continue;

        if (virNetSocketAddIOCallback(socks[i],
                                      VIR_EVENT_HANDLE_READABLE,
                                      libxlMigrateReceive,
                                      args,
                                      NULL) < 0)
            continue;

        nsocks_listen++;
    }

    if (!nsocks_listen)
        goto error;

    ret = 0;
    goto done;

 error:
    for (i = 0; i < nsocks; i++) {
        virNetSocketClose(socks[i]);
        virObjectUnref(socks[i]);
    }
    VIR_FREE(socks);
    virObjectUnref(args);

    /* Remove virDomainObj from domain list */
    if (vm) {
        virDomainObjListRemove(driver->domains, vm);
        vm = NULL;
    }

 done:
    if (!uri_in)
        VIR_FREE(hostname);
    else
        virURIFree(uri);
    if (vm)
        virObjectUnlock(vm);
    return ret;
}

/* This function is a simplification of virDomainMigrateVersion3Full
 * excluding tunnel support and restricting it to migration v3
 * with params since it was the first to be introduced in libxl.
 */
static int
libxlDoMigrateP2P(libxlDriverPrivatePtr driver,
                  virDomainObjPtr vm,
                  virConnectPtr sconn,
                  const char *xmlin,
                  virConnectPtr dconn,
                  const char *dconnuri ATTRIBUTE_UNUSED,
                  const char *dname,
                  const char *uri,
                  unsigned int flags)
{
    virDomainPtr ddomain = NULL;
    virTypedParameterPtr params = NULL;
    int nparams = 0;
    int maxparams = 0;
    char *uri_out = NULL;
    char *dom_xml = NULL;
    unsigned long destflags;
    bool cancelled = true;
    virErrorPtr orig_err = NULL;
    int ret = -1;

    dom_xml = libxlDomainMigrationBegin(sconn, vm, xmlin);
    if (!dom_xml)
        goto cleanup;

    if (virTypedParamsAddString(&params, &nparams, &maxparams,
                                VIR_MIGRATE_PARAM_DEST_XML, dom_xml) < 0)
        goto cleanup;

    if (dname &&
        virTypedParamsAddString(&params, &nparams, &maxparams,
                                VIR_MIGRATE_PARAM_DEST_NAME, dname) < 0)
        goto cleanup;

    if (uri &&
        virTypedParamsAddString(&params, &nparams, &maxparams,
                                VIR_MIGRATE_PARAM_URI, uri) < 0)
        goto cleanup;

    /* We don't require the destination to have P2P support
     * as it looks to be normal migration from the receiver perpective.
     */
    destflags = flags & ~(VIR_MIGRATE_PEER2PEER);

    VIR_DEBUG("Prepare3");
    virObjectUnlock(vm);
    ret = dconn->driver->domainMigratePrepare3Params
        (dconn, params, nparams, NULL, 0, NULL, NULL, &uri_out, destflags);
    virObjectLock(vm);

    if (ret == -1)
        goto cleanup;

    if (uri_out) {
        if (virTypedParamsReplaceString(&params, &nparams,
                                        VIR_MIGRATE_PARAM_URI, uri_out) < 0) {
            orig_err = virSaveLastError();
            goto finish;
        }
    } else {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("domainMigratePrepare3 did not set uri"));
        goto finish;
    }

    VIR_DEBUG("Perform3 uri=%s", NULLSTR(uri_out));
    ret = libxlDomainMigrationPerform(driver, vm, NULL, NULL,
                                      uri_out, NULL, flags);

    if (ret < 0)
        orig_err = virSaveLastError();

    cancelled = (ret < 0);

 finish:
    VIR_DEBUG("Finish3 ret=%d", ret);
    if (virTypedParamsGetString(params, nparams,
                                VIR_MIGRATE_PARAM_DEST_NAME, NULL) <= 0 &&
        virTypedParamsReplaceString(&params, &nparams,
                                    VIR_MIGRATE_PARAM_DEST_NAME,
                                    vm->def->name) < 0) {
        ddomain = NULL;
    } else {
        virObjectUnlock(vm);
        ddomain = dconn->driver->domainMigrateFinish3Params
            (dconn, params, nparams, NULL, 0, NULL, NULL,
             destflags, cancelled);
        virObjectLock(vm);
    }

    cancelled = (ddomain == NULL);

    /* If Finish3Params set an error, and we don't have an earlier
     * one we need to preserve it in case confirm3 overwrites
     */
    if (!orig_err)
        orig_err = virSaveLastError();

    VIR_DEBUG("Confirm3 cancelled=%d vm=%p", cancelled, vm);
    ret = libxlDomainMigrationConfirm(driver, vm, flags, cancelled);

    if (ret < 0)
        VIR_WARN("Guest %s probably left in 'paused' state on source",
                 vm->def->name);

 cleanup:
    if (ddomain) {
        virObjectUnref(ddomain);
        ret = 0;
    } else {
        ret = -1;
    }

    if (orig_err) {
        virSetError(orig_err);
        virFreeError(orig_err);
    }

    VIR_FREE(dom_xml);
    VIR_FREE(uri_out);
    virTypedParamsFree(params, nparams);
    return ret;
}

static int virConnectCredType[] = {
    VIR_CRED_AUTHNAME,
    VIR_CRED_PASSPHRASE,
};

static virConnectAuth virConnectAuthConfig = {
    .credtype = virConnectCredType,
    .ncredtype = ARRAY_CARDINALITY(virConnectCredType),
};

/* On P2P mode there is only the Perform3 phase and we need to handle
 * the connection with the destination libvirtd and perform the migration.
 * Here we first tackle the first part of it, and libxlDoMigrationP2P handles
 * the migration process with an established virConnectPtr to the destination.
 */
int
libxlDomainMigrationPerformP2P(libxlDriverPrivatePtr driver,
                               virDomainObjPtr vm,
                               virConnectPtr sconn,
                               const char *xmlin,
                               const char *dconnuri,
                               const char *uri_str ATTRIBUTE_UNUSED,
                               const char *dname,
                               unsigned int flags)
{
    int ret = -1;
    bool useParams;
    virConnectPtr dconn = NULL;
    virErrorPtr orig_err = NULL;
    libxlDriverConfigPtr cfg = libxlDriverConfigGet(driver);

    virObjectUnlock(vm);
    dconn = virConnectOpenAuth(dconnuri, &virConnectAuthConfig, 0);
    virObjectLock(vm);

    if (dconn == NULL) {
        virReportError(VIR_ERR_OPERATION_FAILED,
                       _("Failed to connect to remote libvirt URI %s: %s"),
                       dconnuri, virGetLastErrorMessage());
        return ret;
    }

    if (virConnectSetKeepAlive(dconn, cfg->keepAliveInterval,
                               cfg->keepAliveCount) < 0)
        goto cleanup;

    virObjectUnlock(vm);
    useParams = VIR_DRV_SUPPORTS_FEATURE(dconn->driver, dconn,
                                         VIR_DRV_FEATURE_MIGRATION_PARAMS);
    virObjectLock(vm);

    if (!useParams) {
        virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                       _("Destination libvirt does not support migration with extensible parameters"));
        goto cleanup;
    }

    ret = libxlDoMigrateP2P(driver, vm, sconn, xmlin, dconn, dconnuri,
                            dname, uri_str, flags);

 cleanup:
    orig_err = virSaveLastError();
    virObjectUnlock(vm);
    virObjectUnref(dconn);
    virObjectUnref(cfg);
    virObjectLock(vm);
    if (orig_err) {
        virSetError(orig_err);
        virFreeError(orig_err);
    }
    return ret;
}

int
libxlDomainMigrationPerform(libxlDriverPrivatePtr driver,
                            virDomainObjPtr vm,
                            const char *dom_xml ATTRIBUTE_UNUSED,
                            const char *dconnuri ATTRIBUTE_UNUSED,
                            const char *uri_str,
                            const char *dname ATTRIBUTE_UNUSED,
                            unsigned int flags)
{
    libxlDomainObjPrivatePtr priv = vm->privateData;
    char *hostname = NULL;
    unsigned short port = 0;
    char portstr[100];
    virURIPtr uri = NULL;
    virNetSocketPtr sock;
    int sockfd = -1;
    int ret = -1;

    /* parse dst host:port from uri */
    uri = virURIParse(uri_str);
    if (uri == NULL || uri->server == NULL || uri->port == 0)
        goto cleanup;

    hostname = uri->server;
    port = uri->port;
    snprintf(portstr, sizeof(portstr), "%d", port);

    /* socket connect to dst host:port */
    if (virNetSocketNewConnectTCP(hostname, portstr,
                                  AF_UNSPEC,
                                  &sock) < 0)
        goto cleanup;

    if (virNetSocketSetBlocking(sock, true) < 0) {
        virObjectUnref(sock);
        goto cleanup;
    }

    sockfd = virNetSocketDupFD(sock, true);
    virObjectUnref(sock);

    if (virDomainLockProcessPause(driver->lockManager, vm, &priv->lockState) < 0)
        VIR_WARN("Unable to release lease on %s", vm->def->name);
    VIR_DEBUG("Preserving lock state '%s'", NULLSTR(priv->lockState));

    /* suspend vm and send saved data to dst through socket fd */
    virObjectUnlock(vm);
    ret = libxlDoMigrateSend(driver, vm, flags, sockfd);
    virObjectLock(vm);

 cleanup:
    VIR_FORCE_CLOSE(sockfd);
    virURIFree(uri);
    return ret;
}

virDomainPtr
libxlDomainMigrationFinish(virConnectPtr dconn,
                           virDomainObjPtr vm,
                           unsigned int flags,
                           int cancelled)
{
    libxlDriverPrivatePtr driver = dconn->privateData;
    libxlDriverConfigPtr cfg = libxlDriverConfigGet(driver);
    libxlDomainObjPrivatePtr priv = vm->privateData;
    virObjectEventPtr event = NULL;
    virDomainPtr dom = NULL;

    virPortAllocatorRelease(driver->migrationPorts, priv->migrationPort);
    priv->migrationPort = 0;

    if (cancelled)
        goto cleanup;

    /* Check if domain is alive */
    if (!virDomainObjIsActive(vm)) {
        /* Migration failed if domain is inactive */
        virReportError(VIR_ERR_OPERATION_FAILED,
                       "%s", _("Migration failed. Domain is not running "
                               "on destination host"));
        goto cleanup;
    }

    /* Unpause if requested */
    if (!(flags & VIR_MIGRATE_PAUSED)) {
        if (libxl_domain_unpause(cfg->ctx, vm->def->id) != 0) {
            virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                           _("Failed to unpause domain"));
            goto cleanup;
        }

        virDomainObjSetState(vm, VIR_DOMAIN_RUNNING,
                             VIR_DOMAIN_RUNNING_MIGRATED);
        event = virDomainEventLifecycleNewFromObj(vm,
                                         VIR_DOMAIN_EVENT_RESUMED,
                                         VIR_DOMAIN_EVENT_RESUMED_MIGRATED);
    } else {
        virDomainObjSetState(vm, VIR_DOMAIN_PAUSED, VIR_DOMAIN_PAUSED_USER);
        event = virDomainEventLifecycleNewFromObj(vm,
                                         VIR_DOMAIN_EVENT_SUSPENDED,
                                         VIR_DOMAIN_EVENT_SUSPENDED_PAUSED);
    }

    if (event) {
        libxlDomainEventQueue(driver, event);
        event = NULL;
    }

    if (virDomainSaveStatus(driver->xmlopt, cfg->stateDir, vm, cfg->caps) < 0)
        goto cleanup;

    dom = virGetDomain(dconn, vm->def->name, vm->def->uuid);

 cleanup:
    if (dom == NULL) {
        libxlDomainDestroyInternal(driver, vm);
        libxlDomainCleanup(driver, vm);
        virDomainObjSetState(vm, VIR_DOMAIN_SHUTOFF,
                             VIR_DOMAIN_SHUTOFF_FAILED);
        event = virDomainEventLifecycleNewFromObj(vm, VIR_DOMAIN_EVENT_STOPPED,
                                         VIR_DOMAIN_EVENT_STOPPED_FAILED);
        if (!vm->persistent)
            virDomainObjListRemove(driver->domains, vm);
    }

    if (event)
        libxlDomainEventQueue(driver, event);
    virObjectUnref(cfg);
    return dom;
}

int
libxlDomainMigrationConfirm(libxlDriverPrivatePtr driver,
                            virDomainObjPtr vm,
                            unsigned int flags,
                            int cancelled)
{
    libxlDriverConfigPtr cfg = libxlDriverConfigGet(driver);
    virObjectEventPtr event = NULL;
    int ret = -1;

    if (cancelled) {
        if (libxl_domain_resume(cfg->ctx, vm->def->id, 1, 0) == 0) {
            ret = 0;
        } else {
            VIR_DEBUG("Unable to resume domain '%s' after failed migration",
                      vm->def->name);
            virDomainObjSetState(vm, VIR_DOMAIN_PAUSED,
                                 VIR_DOMAIN_PAUSED_MIGRATION);
            event = virDomainEventLifecycleNewFromObj(vm, VIR_DOMAIN_EVENT_SUSPENDED,
                                     VIR_DOMAIN_EVENT_SUSPENDED_MIGRATED);
            ignore_value(virDomainSaveStatus(driver->xmlopt, cfg->stateDir, vm, cfg->caps));
        }
        goto cleanup;
    }

    libxlDomainDestroyInternal(driver, vm);
    libxlDomainCleanup(driver, vm);
    virDomainObjSetState(vm, VIR_DOMAIN_SHUTOFF,
                         VIR_DOMAIN_SHUTOFF_MIGRATED);
    event = virDomainEventLifecycleNewFromObj(vm, VIR_DOMAIN_EVENT_STOPPED,
                                              VIR_DOMAIN_EVENT_STOPPED_MIGRATED);

    VIR_DEBUG("Domain '%s' successfully migrated", vm->def->name);

    if (flags & VIR_MIGRATE_UNDEFINE_SOURCE)
        virDomainDeleteConfig(cfg->configDir, cfg->autostartDir, vm);

    if (!vm->persistent || (flags & VIR_MIGRATE_UNDEFINE_SOURCE)) {
        virDomainObjListRemove(driver->domains, vm);
        vm = NULL;
    }

    ret = 0;

 cleanup:
    if (event)
        libxlDomainEventQueue(driver, event);
    if (vm)
        virObjectUnlock(vm);
    virObjectUnref(cfg);
    return ret;
}
