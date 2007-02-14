/*
 * qemud.c: daemon start of day, guest process & i/o management
 *
 * Copyright (C) 2006, 2007 Red Hat, Inc.
 * Copyright (C) 2006 Daniel P. Berrange
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
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
 *
 * Author: Daniel P. Berrange <berrange@redhat.com>
 */

#include <config.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <paths.h>
#include <limits.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/poll.h>
#include <netinet/in.h>
#include <netdb.h>
#include <stdlib.h>
#include <pwd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <getopt.h>

#include <libvirt/virterror.h>

#include <gnutls/x509.h>

#include "internal.h"
#include "dispatch.h"
#include "driver.h"
#include "conf.h"
#include "iptables.h"

static void reapchild(int sig ATTRIBUTE_UNUSED) {
    /* We explicitly waitpid the child later */
}
static int qemudSetCloseExec(int fd) {
    int flags;
    if ((flags = fcntl(fd, F_GETFD)) < 0) {
        return -1;
    }
    flags |= FD_CLOEXEC;
    if ((fcntl(fd, F_SETFD, flags)) < 0) {
        return -1;
    }
    return 0;
}


static int qemudSetNonBlock(int fd) {
    int flags;
    if ((flags = fcntl(fd, F_GETFL)) < 0) {
        return -1;
    }
    flags |= O_NONBLOCK;
    if ((fcntl(fd, F_SETFL, flags)) < 0) {
        return -1;
    }
    return 0;
}

static int qemudGoDaemon(void) {
    int pid = fork();
    switch (pid) {
    case 0:
        {
            int stdinfd = -1;
            int stdoutfd = -1;
            int i, open_max, nextpid;

            if ((stdinfd = open(_PATH_DEVNULL, O_RDONLY)) < 0)
                goto cleanup;
            if ((stdoutfd = open(_PATH_DEVNULL, O_WRONLY)) < 0)
                goto cleanup;
            if (dup2(stdinfd, STDIN_FILENO) != STDIN_FILENO)
                goto cleanup;
            if (dup2(stdoutfd, STDOUT_FILENO) != STDOUT_FILENO)
                goto cleanup;
            if (dup2(stdoutfd, STDERR_FILENO) != STDERR_FILENO)
                goto cleanup;
            if (close(stdinfd) < 0)
                goto cleanup;
            stdinfd = -1;
            if (close(stdoutfd) < 0)
                goto cleanup;
            stdoutfd = -1;

            open_max = sysconf (_SC_OPEN_MAX);
            for (i = 0; i < open_max; i++)
                if (i != STDIN_FILENO &&
                    i != STDOUT_FILENO &&
                    i != STDERR_FILENO)
                    close(i);

            if (setsid() < 0)
                goto cleanup;

            nextpid = fork();
            switch (nextpid) {
            case 0:
                return 0;
            case -1:
                return -1;
            default:
                return nextpid;
            }

        cleanup:
            if (stdoutfd != -1)
                close(stdoutfd);
            if (stdinfd != -1)
                close(stdinfd);
            return -1;

        }

    case -1:
        return -1;

    default:
        {
            int got, status = 0;
            /* We wait to make sure the next child forked
               successfully */
            if ((got = waitpid(pid, &status, 0)) < 0 ||
                got != pid ||
                status != 0) {
                return -1;
            }
      
            return pid;
        }
    }
}

static int qemudListenUnix(struct qemud_server *server,
                           const char *path, int readonly) {
    struct qemud_socket *sock = calloc(1, sizeof(struct qemud_socket));
    struct sockaddr_un addr;
    mode_t oldmask;

    if (!sock)
        return -1;

    sock->readonly = readonly;
    sock->next = server->sockets;
    server->sockets = sock;
    server->nsockets++;

    if ((sock->fd = socket(PF_UNIX, SOCK_STREAM, 0)) < 0)
        return -1;

    if (qemudSetCloseExec(sock->fd) < 0)
        return -1;
    if (qemudSetNonBlock(sock->fd) < 0)
        return -1;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path)-1);
    if (addr.sun_path[0] == '@')
        addr.sun_path[0] = '\0';


    if (readonly)
        oldmask = umask(~(S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH));
    else
        oldmask = umask(~(S_IRUSR | S_IWUSR));
    if (bind(sock->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        return -1;
    umask(oldmask);

    if (listen(sock->fd, 30) < 0)
        return -1;

    return 0;
}

static int qemudListen(struct qemud_server *server, int sys) {
    char sockname[PATH_MAX];

    if (sys) {
        if (snprintf(sockname, sizeof(sockname), "%s/run/libvirt/qemud-sock", LOCAL_STATE_DIR) >= (int)sizeof(sockname)) {
            return -1;
        }
        unlink(sockname);
        if (qemudListenUnix(server, sockname, 0) < 0)
            return -1;


        if (snprintf(sockname, sizeof(sockname), "%s/run/libvirt/qemud-sock-ro", LOCAL_STATE_DIR) >= (int)sizeof(sockname)) {
            return -1;
        }
        unlink(sockname);
        if (qemudListenUnix(server, sockname, 1) < 0)
            return -1;
    } else {
        struct passwd *pw;
        int uid;

        if ((uid = geteuid()) < 0) {
            return -1;
        }

        if (!(pw = getpwuid(uid)))
            return -1;

        if (snprintf(sockname, sizeof(sockname), "@%s/.libvirt/qemud-sock", pw->pw_dir) >= (int)sizeof(sockname)) {
            return -1;
        }

        if (qemudListenUnix(server, sockname, 0) < 0)
            return -1;
    }

    return 0;
}

static struct qemud_server *qemudInitialize(int sys) {
    struct qemud_server *server;

    if (!(server = calloc(1, sizeof(struct qemud_server))))
        return NULL;

    /* XXX extract actual version */
    server->qemuVersion = (0*1000000)+(8*1000)+(0);
    /* We don't have a dom-0, so start from 1 */
    server->nextvmid = 1;

    if (sys) {
        if (snprintf(server->configDir, sizeof(server->configDir), "%s/libvirt/qemu", SYSCONF_DIR) >= (int)sizeof(server->configDir)) {
            goto cleanup;
        }
        if (snprintf(server->networkConfigDir, sizeof(server->networkConfigDir), "%s/libvirt/qemu/networks", SYSCONF_DIR) >= (int)sizeof(server->networkConfigDir)) {
            goto cleanup;
        }
    } else {
        struct passwd *pw;
        int uid;
        if ((uid = geteuid()) < 0) {
            goto cleanup;
        }
        if (!(pw = getpwuid(uid))) {
            goto cleanup;
        }

        if (snprintf(server->configDir, sizeof(server->configDir), "%s/.libvirt/qemu", pw->pw_dir) >= (int)sizeof(server->configDir)) {
            goto cleanup;
        }

        if (snprintf(server->networkConfigDir, sizeof(server->networkConfigDir), "%s/.libvirt/qemu/networks", pw->pw_dir) >= (int)sizeof(server->networkConfigDir)) {
            goto cleanup;
        }
    }


    if (qemudListen(server, sys) < 0) {
        goto cleanup;
    }

    if (qemudScanConfigs(server) < 0) {
        goto cleanup;
    }

    return server;

 cleanup:
    if (server) {
        struct qemud_socket *sock = server->sockets;
        while (sock) {
            close(sock->fd);
            sock = sock->next;
        }

        free(server);
    }
    return NULL;
}


static int qemudDispatchServer(struct qemud_server *server, struct qemud_socket *sock) {
    int fd;
    struct sockaddr_storage addr;
    unsigned int addrlen = sizeof(addr);
    struct qemud_client *client;

    if ((fd = accept(sock->fd, (struct sockaddr *)&addr, &addrlen)) < 0) {
        if (errno == EAGAIN)
            return 0;
        return -1;
    }

    if (qemudSetCloseExec(fd) < 0) {
        close(fd);
        return -1;
    }

    if (qemudSetNonBlock(fd) < 0) {
        close(fd);
        return -1;
    }

    client = calloc(1, sizeof(struct qemud_client));
    client->fd = fd;
    client->readonly = sock->readonly;

    client->next = server->clients;
    server->clients = client;
    server->nclients++;

    return 0;
}


static int
qemudLeaveFdOpen(int *openfds, int fd)
{
    int i;

    if (!openfds)
        return 0;

    for (i = 0; openfds[i] != -1; i++)
        if (fd == openfds[i])
            return 1;

    return 0;
}

static int
qemudExec(struct qemud_server *server, char **argv,
          int *retpid, int *outfd, int *errfd, int *openfds) {
    int pid, null;
    int pipeout[2] = {-1,-1};
    int pipeerr[2] = {-1,-1};

    if ((null = open(_PATH_DEVNULL, O_RDONLY)) < 0) {
        qemudReportError(server, VIR_ERR_INTERNAL_ERROR, "cannot open %s : %s",
                         _PATH_DEVNULL, strerror(errno));
        goto cleanup;
    }

    if ((outfd != NULL && pipe(pipeout) < 0) ||
        (errfd != NULL && pipe(pipeerr) < 0)) {
        qemudReportError(server, VIR_ERR_INTERNAL_ERROR, "cannot create pipe : %s",
                         strerror(errno));
        goto cleanup;
    }

    if ((pid = fork()) < 0) {
        qemudReportError(server, VIR_ERR_INTERNAL_ERROR, "cannot fork child process : %s",
                         strerror(errno));
        goto cleanup;
    }

    if (pid) { /* parent */
        close(null);
        if (outfd) {
            close(pipeout[1]);
            qemudSetNonBlock(pipeout[0]);
            *outfd = pipeout[0];
        }
        if (errfd) {
            close(pipeerr[1]);
            qemudSetNonBlock(pipeerr[0]);
            *errfd = pipeerr[0];
        }
        *retpid = pid;
        return 0;
    }

    /* child */

    if (pipeout[0] > 0 && close(pipeout[0]) < 0)
        _exit(1);
    if (pipeerr[0] > 0 && close(pipeerr[0]) < 0)
        _exit(1);

    if (dup2(null, STDIN_FILENO) < 0)
        _exit(1);
    if (dup2(pipeout[1] > 0 ? pipeout[1] : null, STDOUT_FILENO) < 0)
        _exit(1);
    if (dup2(pipeerr[1] > 0 ? pipeerr[1] : null, STDERR_FILENO) < 0)
        _exit(1);

    int i, open_max = sysconf (_SC_OPEN_MAX);
    for (i = 0; i < open_max; i++)
        if (i != STDOUT_FILENO &&
            i != STDERR_FILENO &&
            i != STDIN_FILENO &&
            !qemudLeaveFdOpen(openfds, i))
            close(i);

    execvp(argv[0], argv);

    _exit(1);

    return 0;

 cleanup:
    if (pipeerr[0] > 0)
        close(pipeerr[0] > 0);
    if (pipeerr[1])
        close(pipeerr[1] > 0);
    if (pipeout[0])
        close(pipeout[0] > 0);
    if (pipeout[1])
        close(pipeout[1] > 0);
    if (null > 0)
        close(null);
    return -1;
}


int qemudStartVMDaemon(struct qemud_server *server,
                       struct qemud_vm *vm) {
    char **argv = NULL;
    int i, ret = -1;

    if (vm->def->vncPort < 0)
        vm->def->vncActivePort = 5900 + server->nextvmid;
    else
        vm->def->vncActivePort = vm->def->vncPort;

    if (qemudBuildCommandLine(server, vm, &argv) < 0)
        return -1;

    if (qemudExec(server, argv, &vm->pid, &vm->stdout, &vm->stderr, vm->tapfds) == 0) {
        vm->id = server->nextvmid++;
        ret = 0;
    }

    if (vm->tapfds) {
        for (i = 0; vm->tapfds[i] != -1; i++) {
            close(vm->tapfds[i]);
            vm->tapfds[i] = -1;
        }
        free(vm->tapfds);
        vm->tapfds = NULL;
        vm->ntapfds = 0;
    }
  
    for (i = 0 ; argv[i] ; i++)
        free(argv[i]);
    free(argv);

    return ret;
}


static void qemudDispatchClientFailure(struct qemud_server *server, struct qemud_client *client) {
    struct qemud_client *tmp = server->clients;
    struct qemud_client *prev = NULL;
    while (tmp) {
        if (tmp == client) {
            if (prev == NULL)
                server->clients = client->next;
            else
                prev->next = client->next;
            server->nclients--;
            break;
        }
        prev = tmp;
        tmp = tmp->next;
    }
    close(client->fd);
    free(client);
}


static int qemudDispatchClientRequest(struct qemud_server *server, struct qemud_client *client) {
    if (qemudDispatch(server,
                      client,
                      &client->incoming,
                      &client->outgoing) < 0) {
        return -1;
    }

    client->outgoingSent = 0;
    client->tx = 1;
    client->incomingReceived = 0;

    return 0;
}

static int qemudClientRead(struct qemud_server *server,
                           struct qemud_client *client,
                           char *buf, size_t want) {
    int ret;
    if ((ret = read(client->fd, buf, want)) <= 0) {
        QEMUD_DEBUG("Plain read error %d\n", ret);
        if (!ret || errno != EAGAIN)
            qemudDispatchClientFailure(server, client);
        return -1;
    }
    QEMUD_DEBUG("Plain data read %d\n", ret);
    return ret;
}

static void qemudDispatchClientRead(struct qemud_server *server, struct qemud_client *client) {
    char *data = (char *)&client->incoming;
    unsigned int got = client->incomingReceived;
    int want;
    int ret;

 restart:
    if (got >= sizeof(struct qemud_packet_header)) {
        want = sizeof(struct qemud_packet_header) + client->incoming.header.dataSize - got;
    } else {
        want = sizeof(struct qemud_packet_header) - got;
    }

    if ((ret = qemudClientRead(server, client, data+got, want)) < 0) {
        return;
    }
    got += ret;
    client->incomingReceived += ret;

    /* If we've finished header, move onto body */
    if (client->incomingReceived == sizeof(struct qemud_packet_header)) {
        QEMUD_DEBUG("Type %d, data %d\n",
                    client->incoming.header.type,
                    client->incoming.header.dataSize);
        /* Client lied about dataSize */
        if (client->incoming.header.dataSize > sizeof(union qemud_packet_data)) {
            QEMUD_DEBUG("Bogus data size %u\n", client->incoming.header.dataSize);
            qemudDispatchClientFailure(server, client);
            return;
        }
        if (client->incoming.header.dataSize) {
            QEMUD_DEBUG("- Restarting recv to process body (%d bytes)\n",
                        client->incoming.header.dataSize);
            goto restart;
        }
    }

    /* If we've finished body, dispatch the request */
    if (ret == want) {
        if (qemudDispatchClientRequest(server, client) < 0)
            qemudDispatchClientFailure(server, client);
        QEMUD_DEBUG("Dispatch\n");
    }
}


static int qemudClientWrite(struct qemud_server *server,
                           struct qemud_client *client,
                           char *buf, size_t want) {
    int ret;
    if ((ret = write(client->fd, buf, want)) < 0) {
        QEMUD_DEBUG("Plain write error %d\n", ret);
        if (errno != EAGAIN)
            qemudDispatchClientFailure(server, client);
        return -1;
    }
    QEMUD_DEBUG("Plain data write %d\n", ret);
    return ret;
}


static void qemudDispatchClientWrite(struct qemud_server *server, struct qemud_client *client) {
    char *data = (char *)&client->outgoing;
    int sent = client->outgoingSent;
    int todo = sizeof(struct qemud_packet_header) + client->outgoing.header.dataSize - sent;
    int ret;
    if ((ret = qemudClientWrite(server, client, data+sent, todo)) < 0) {
        return;
    }
    client->outgoingSent += ret;
    QEMUD_DEBUG("Done %d %d\n", todo, ret);
    if (todo == ret)
        client->tx = 0;
}

static int qemudVMData(struct qemud_server *server ATTRIBUTE_UNUSED,
                       struct qemud_vm *vm, int fd) {
    char buf[4096];
    if (vm->pid < 0)
        return 0;

    for (;;) {
        int ret = read(fd, buf, sizeof(buf)-1);
        if (ret < 0) {
            if (errno == EAGAIN)
                return 0;
            return -1;
        }
        if (ret == 0) {
            return 0;
        }
        buf[ret] = '\0';

        /*
         * XXX this is bad - we should wait for tty and open the
         * monitor when actually starting the guest, so we can
         * reliably trap startup failures
         */
        if (vm->monitor == -1) {
            char monitor[20];
            /* Fairly lame assuming we receive the data all in one chunk.
               This isn't guarenteed, but in practice it seems good enough.
               This will probably bite me in the future.... */
            if (sscanf(buf, "char device redirected to %19s", monitor) == 1) {
                int monfd;

                if (!(monfd = open(monitor, O_RDWR))) {
                    perror("cannot open monitor");
                    return -1;
                }
                if (qemudSetCloseExec(monfd) < 0) {
                    close(monfd);
                    return -1;
                }
                if (qemudSetNonBlock(monfd) < 0) {
                    close(monfd);
                    return -1;
                }

                /* Consume & discard the initial greeting */
                /* XXX this is broken, we need to block until
                   we see the initial prompt to ensure startup
                   has completed */
                for(;;) {
                    char line[1024];
                    if (read(monfd, line, sizeof(line)) < 0) {
                        if (errno == EAGAIN) {
                            break;
                        }
                        close(monfd);
                        return -1;
                    }
                    QEMUD_DEBUG("[%s]\n", line);
                }
                vm->monitor = monfd;
            }
        }
        QEMUD_DEBUG("[%s]\n", buf);
    }
}

static void
qemudNetworkIfaceDisconnect(struct qemud_server *server,
                            struct qemud_vm *vm ATTRIBUTE_UNUSED,
                            struct qemud_vm_net_def *net) {
    iptablesRemovePhysdevForward(server->iptables, net->dst.network.tapifname);
}

int qemudShutdownVMDaemon(struct qemud_server *server, struct qemud_vm *vm) {
    struct qemud_vm *prev = NULL, *curr = server->activevms;
    struct qemud_vm_net_def *net;

    /* Already cleaned-up */
    if (vm->pid < 0)
        return 0;

    kill(vm->pid, SIGTERM);

    /* Move it to inactive vm list */
    while (curr) {
        if (curr == vm) {
            if (prev) {
                prev->next = curr->next;
            } else {
                server->activevms = curr->next;
            }
            server->nactivevms--;

            curr->next = server->inactivevms;
            server->inactivevms = curr;
            server->ninactivevms++;
            break;
        }
        prev = curr;
        curr = curr->next;
    }

    if (!curr) {
        QEMUD_DEBUG("Could not find VM to shutdown\n");
        return 0;
    }

    qemudVMData(server, vm, curr->stdout);
    qemudVMData(server, vm, curr->stderr);
    close(curr->stdout);
    close(curr->stderr);
    if (curr->monitor != -1)
        close(curr->monitor);
    curr->stdout = -1;
    curr->stderr = -1;
    curr->monitor = -1;
    server->nvmfds -= 2;

    net = vm->def->nets;
    while (net) {
        if (net->type == QEMUD_NET_NETWORK)
            qemudNetworkIfaceDisconnect(server, vm, net);
        net = net->next;
    }

    if (waitpid(vm->pid, NULL, WNOHANG) != vm->pid) {
        kill(vm->pid, SIGKILL);
        if (waitpid(vm->pid, NULL, 0) != vm->pid) {
            QEMUD_DEBUG("Got unexpected pid, damn\n");
        }
    }

    vm->pid = -1;
    vm->id = -1;

    if (vm->newDef) {
        qemudFreeVMDef(vm->def);
        vm->def = vm->newDef;
        vm->newDef = NULL;
    }

    return 0;
}

static int qemudDispatchVMLog(struct qemud_server *server, struct qemud_vm *vm, int fd) {
    if (qemudVMData(server, vm, fd) < 0)
        if (qemudShutdownVMDaemon(server, vm) < 0)
            return -1;
    return 0;
}

static int qemudDispatchVMFailure(struct qemud_server *server, struct qemud_vm *vm,
                                  int fd ATTRIBUTE_UNUSED) {
    if (qemudShutdownVMDaemon(server, vm) < 0)
        return -1;
    return 0;
}

static int
qemudBuildDnsmasqArgv(struct qemud_server *server,
                      struct qemud_network *network,
                      char ***argv) {
    int i, len;
    char buf[BR_INET_ADDR_MAXLEN * 2];
    struct qemud_dhcp_range_def *range;

    len =
        1 + /* dnsmasq */
        1 + /* --keep-in-foreground */
        1 + /* --bind-interfaces */
        2 + /* --pid-file "" */
        2 + /* --conf-file "" */
        2 + /* --except-interface lo */
        2 + /* --listen-address 10.0.0.1 */
        (2 * network->def.nranges) + /* --dhcp-range 10.0.0.2,10.0.0.254 */
        1;  /* NULL */

    if (!(*argv = malloc(len * sizeof(char *))))
        goto no_memory;

    memset(*argv, 0, len * sizeof(char *));

#define APPEND_ARG(v, n, s) do {     \
        if (!((v)[(n)] = strdup(s))) \
            goto no_memory;          \
    } while (0)

    i = 0;

    APPEND_ARG(*argv, i++, "dnsmasq");

    APPEND_ARG(*argv, i++, "--keep-in-foreground");
    APPEND_ARG(*argv, i++, "--bind-interfaces");

    APPEND_ARG(*argv, i++, "--pid-file");
    APPEND_ARG(*argv, i++, "");

    APPEND_ARG(*argv, i++, "--conf-file");
    APPEND_ARG(*argv, i++, "");

    APPEND_ARG(*argv, i++, "--except-interface");
    APPEND_ARG(*argv, i++, "lo");

    APPEND_ARG(*argv, i++, "--listen-address");
    APPEND_ARG(*argv, i++, network->def.ipAddress);

    range = network->def.ranges;
    while (range) {
        snprintf(buf, sizeof(buf), "%s,%s",
                 range->start, range->end);

        APPEND_ARG(*argv, i++, "--dhcp-range");
        APPEND_ARG(*argv, i++, buf);

        range = range->next;
    }

#undef APPEND_ARG

    return 0;

 no_memory:
    if (argv) {
        for (i = 0; (*argv)[i]; i++)
            free((*argv)[i]);
        free(*argv);
    }
    qemudReportError(server, VIR_ERR_NO_MEMORY, "dnsmasq argv");
    return -1;
}


static int
dhcpStartDhcpDaemon(struct qemud_server *server,
                    struct qemud_network *network)
{
    char **argv;
    int ret, i;

    if (network->def.ipAddress[0] == '\0') {
        qemudReportError(server, VIR_ERR_INTERNAL_ERROR,
                         "cannot start dhcp daemon without IP address for server");
        return -1;
    }

    argv = NULL;
    if (qemudBuildDnsmasqArgv(server, network, &argv) < 0)
        return -1;

    ret = qemudExec(server, argv, &network->dnsmasqPid, NULL, NULL, NULL);

    for (i = 0; argv[i]; i++)
        free(argv[i]);
    free(argv);

    return ret;
}

static int
qemudAddIptablesRules(struct qemud_server *server,
                      struct qemud_network *network) {
    int err;

    if (!server->iptables && !(server->iptables = iptablesContextNew())) {
        qemudReportError(server, VIR_ERR_NO_MEMORY, "iptables support");
        return 1;
    }

    /* allow bridging from the bridge interface itself */
    if ((err = iptablesAddPhysdevForward(server->iptables, network->bridge))) {
        qemudReportError(server, VIR_ERR_INTERNAL_ERROR,
                         "failed to add iptables rule to allow bridging from '%s' : %s\n",
                         network->bridge, strerror(err));
        goto err1;
    }

    /* allow forwarding packets from the bridge interface */
    if ((err = iptablesAddInterfaceForward(server->iptables, network->bridge))) {
        qemudReportError(server, VIR_ERR_INTERNAL_ERROR,
                         "failed to add iptables rule to allow forwarding from '%s' : %s\n",
                         network->bridge, strerror(err));
        goto err2;
    }

    /* allow forwarding packets to the bridge interface if they are part of an existing connection */
    if ((err = iptablesAddStateForward(server->iptables, network->bridge))) {
        qemudReportError(server, VIR_ERR_INTERNAL_ERROR,
                         "failed to add iptables rule to allow forwarding to '%s' : %s\n",
                         network->bridge, strerror(err));
        goto err3;
    }

    /* enable masquerading */
    if ((err = iptablesAddNonBridgedMasq(server->iptables))) {
        qemudReportError(server, VIR_ERR_INTERNAL_ERROR,
                         "failed to add iptables rule to enable masquerading : %s\n",
                         strerror(err));
        goto err4;
    }

    /* allow DHCP requests through to dnsmasq */
    if ((err = iptablesAddTcpInput(server->iptables, network->bridge, 67))) {
        qemudReportError(server, VIR_ERR_INTERNAL_ERROR,
                         "failed to add iptables rule to allow DHCP requests from '%s' : %s\n",
                         network->bridge, strerror(err));
        goto err5;
    }

    if ((err = iptablesAddUdpInput(server->iptables, network->bridge, 67))) {
        qemudReportError(server, VIR_ERR_INTERNAL_ERROR,
                         "failed to add iptables rule to allow DHCP requests from '%s' : %s\n",
                         network->bridge, strerror(err));
        goto err6;
    }

    /* allow DNS requests through to dnsmasq */
    if ((err = iptablesAddTcpInput(server->iptables, network->bridge, 53))) {
        qemudReportError(server, VIR_ERR_INTERNAL_ERROR,
                         "failed to add iptables rule to allow DNS requests from '%s' : %s\n",
                         network->bridge, strerror(err));
        goto err7;
    }

    if ((err = iptablesAddUdpInput(server->iptables, network->bridge, 53))) {
        qemudReportError(server, VIR_ERR_INTERNAL_ERROR,
                         "failed to add iptables rule to allow DNS requests from '%s' : %s\n",
                         network->bridge, strerror(err));
        goto err8;
    }

    return 1;

 err8:
    iptablesRemoveTcpInput(server->iptables, network->bridge, 53);
 err7:
    iptablesRemoveUdpInput(server->iptables, network->bridge, 67);
 err6:
    iptablesRemoveTcpInput(server->iptables, network->bridge, 67);
 err5:
    iptablesRemoveNonBridgedMasq(server->iptables);
 err4:
    iptablesRemoveStateForward(server->iptables, network->bridge);
 err3:
    iptablesRemoveInterfaceForward(server->iptables, network->bridge);
 err2:
    iptablesRemovePhysdevForward(server->iptables, network->bridge);
 err1:
    return 0;
}

static void
qemudRemoveIptablesRules(struct qemud_server *server,
                         struct qemud_network *network) {
    iptablesRemoveUdpInput(server->iptables, network->bridge, 53);
    iptablesRemoveTcpInput(server->iptables, network->bridge, 53);
    iptablesRemoveUdpInput(server->iptables, network->bridge, 67);
    iptablesRemoveTcpInput(server->iptables, network->bridge, 67);
    iptablesRemoveNonBridgedMasq(server->iptables);
    iptablesRemoveStateForward(server->iptables, network->bridge);
    iptablesRemoveInterfaceForward(server->iptables, network->bridge);
    iptablesRemovePhysdevForward(server->iptables, network->bridge);
}

static int
qemudEnableIpForwarding(void)
{
#define PROC_IP_FORWARD "/proc/sys/net/ipv4/ip_forward"

  int fd;

  if ((fd = open(PROC_IP_FORWARD, O_WRONLY|O_TRUNC)) == -1 ||
      write(fd, "1\n", 2) < 0)
      return 0;

  close (fd);

  return 1;

#undef PROC_IP_FORWARD
}

int qemudStartNetworkDaemon(struct qemud_server *server,
                            struct qemud_network *network) {
    const char *name;
    int err;

    if (network->active) {
        qemudReportError(server, VIR_ERR_INTERNAL_ERROR,
                         "network is already active");
        return -1;
    }

    if (!server->brctl && (err = brInit(&server->brctl))) {
        qemudReportError(server, VIR_ERR_INTERNAL_ERROR,
                         "cannot initialize bridge support: %s", strerror(err));
        return -1;
    }

    if (network->def.bridge[0] == '\0' ||
        strchr(network->def.bridge, '%')) {
        name = "vnet%d";
    } else {
        name = network->def.bridge;
    }

    if ((err = brAddBridge(server->brctl, name, network->bridge, sizeof(network->bridge)))) {
        qemudReportError(server, VIR_ERR_INTERNAL_ERROR,
                         "cannot create bridge '%s' : %s", name, strerror(err));
        return -1;
    }

    if (network->def.ipAddress[0] &&
        (err = brSetInetAddress(server->brctl, network->bridge, network->def.ipAddress))) {
        qemudReportError(server, VIR_ERR_INTERNAL_ERROR,
                         "cannot set IP address on bridge '%s' to '%s' : %s\n",
                         network->bridge, network->def.ipAddress, strerror(err));
        goto err_delbr;
    }

    if (network->def.netmask[0] &&
        (err = brSetInetNetmask(server->brctl, network->bridge, network->def.netmask))) {
        qemudReportError(server, VIR_ERR_INTERNAL_ERROR,
                         "cannot set netmask on bridge '%s' to '%s' : %s\n",
                         network->bridge, network->def.netmask, strerror(err));
        goto err_delbr;
    }

    if (network->def.ipAddress[0] &&
        (err = brSetInterfaceUp(server->brctl, network->bridge, 1))) {
        qemudReportError(server, VIR_ERR_INTERNAL_ERROR,
                         "failed to bring the bridge '%s' up : %s\n",
                         network->bridge, strerror(err));
        goto err_delbr;
    }

    if (!qemudAddIptablesRules(server, network))
        goto err_delbr1;

    if (!qemudEnableIpForwarding()) {
        qemudReportError(server, VIR_ERR_INTERNAL_ERROR,
                         "failed to enable IP forwarding : %s\n", strerror(err));
        goto err_delbr2;
    }

    if (network->def.ranges &&
        dhcpStartDhcpDaemon(server, network) < 0)
        goto err_delbr2;

    network->active = 1;

    return 0;

 err_delbr2:
    qemudRemoveIptablesRules(server, network);

 err_delbr1:
    if (network->def.ipAddress[0] &&
        (err = brSetInterfaceUp(server->brctl, network->bridge, 0))) {
        printf("Damn! Failed to bring down bridge '%s' : %s\n",
               network->bridge, strerror(err));
    }

 err_delbr:
    if ((err = brDeleteBridge(server->brctl, network->bridge))) {
        printf("Damn! Couldn't delete bridge '%s' : %s\n",
               network->bridge, strerror(err));
    }

    return -1;
}


int qemudShutdownNetworkDaemon(struct qemud_server *server,
                               struct qemud_network *network) {
    struct qemud_network *prev, *curr;
    int err;

    if (!network->active)
        return 0;

    if (network->dnsmasqPid > 0)
        kill(network->dnsmasqPid, SIGTERM);

    qemudRemoveIptablesRules(server, network);

    if (network->def.ipAddress[0] &&
        (err = brSetInterfaceUp(server->brctl, network->bridge, 0))) {
        printf("Damn! Failed to bring down bridge '%s' : %s\n",
               network->bridge, strerror(err));
    }

    if ((err = brDeleteBridge(server->brctl, network->bridge))) {
        printf("Damn! Failed to delete bridge '%s' : %s\n",
               network->bridge, strerror(err));
    }

    /* Move it to inactive networks list */
    prev = NULL;
    curr = server->activenetworks;
    while (curr) {
        if (curr == network) {
            if (prev) {
                prev->next = curr->next;
            } else {
                server->activenetworks = curr->next;
            }
            server->nactivenetworks--;

            curr->next = server->inactivenetworks;
            server->inactivenetworks = curr;
            server->ninactivenetworks++;
            break;
        }
        prev = curr;
        curr = curr->next;
    }

    if (network->dnsmasqPid > 0 &&
        waitpid(network->dnsmasqPid, NULL, WNOHANG) != network->dnsmasqPid) {
        kill(network->dnsmasqPid, SIGKILL);
        if (waitpid(network->dnsmasqPid, NULL, 0) != network->dnsmasqPid)
            printf("Got unexpected pid for dnsmasq, damn\n");
    }

    network->bridge[0] = '\0';
    network->dnsmasqPid = -1;
    network->active = 0;

    return 0;
}


static int qemudDispatchPoll(struct qemud_server *server, struct pollfd *fds) {
    struct qemud_socket *sock = server->sockets;
    struct qemud_client *client = server->clients;
    struct qemud_vm *vm = server->activevms;
    struct qemud_vm *tmp;
    struct qemud_network *network, *prevnet;
    int ret = 0;
    int fd = 0;

    while (sock) {
        struct qemud_socket *next = sock->next;
        if (fds[fd].revents)
            if (qemudDispatchServer(server, sock) < 0)
                return -1;
        fd++;
        sock = next;
    }

    while (client) {
        struct qemud_client *next = client->next;
        if (fds[fd].revents) {
            QEMUD_DEBUG("Poll data normal\n");
            if (fds[fd].revents == POLLOUT)
                qemudDispatchClientWrite(server, client);
            else if (fds[fd].revents == POLLIN)
                qemudDispatchClientRead(server, client);
            else
                qemudDispatchClientFailure(server, client);
        }
        fd++;
        client = next;
    }
    while (vm) {
        struct qemud_vm *next = vm->next;
        int failed = 0,
            stdoutfd = vm->stdout,
            stderrfd = vm->stderr;

        if (stdoutfd != -1) {
            if (fds[fd].revents) {
                if (fds[fd].revents == POLLIN) {
                    if (qemudDispatchVMLog(server, vm, fds[fd].fd) < 0)
                        failed = 1;
                } else {
                    if (qemudDispatchVMFailure(server, vm, fds[fd].fd) < 0)
                        failed = 1;
                }
            }
            fd++;
        }
        if (stderrfd != -1) {
            if (!failed) {
                if (fds[fd].revents) {
                    if (fds[fd].revents == POLLIN) {
                        if (qemudDispatchVMLog(server, vm, fds[fd].fd) < 0)
                            failed = 1;
                    } else {
                        if (qemudDispatchVMFailure(server, vm, fds[fd].fd) < 0)
                            failed = 1;
                    }
                }
            }
            fd++;
        }
        vm = next;
        if (failed)
            ret = -1;
    }

    /* Cleanup any VMs which shutdown & dont have an associated
       config file */
    vm = server->inactivevms;
    tmp = NULL;
    while (vm) {
        if (!vm->configFile[0]) {
            struct qemud_vm *next = vm->next;
            if (tmp) {
                tmp->next = next;
            } else {
                server->inactivevms = next;
            }
            qemudFreeVM(vm);
            vm = next;
        } else {
            tmp = vm;
            vm = vm->next;
        }
    }

    /* Cleanup any networks too */
    network = server->inactivenetworks;
    prevnet = NULL;
    while (network) {
        if (!network->configFile[0]) {
            struct qemud_network *next = network->next;
            if (prevnet) {
                prevnet->next = next;
            } else {
                server->inactivenetworks = next;
            }
            qemudFreeNetwork(network);
            network = next;
        } else {
            prevnet = network;
            network = network->next;
        }
    }

    return ret;
}

static void qemudPreparePoll(struct qemud_server *server, struct pollfd *fds) {
    int  fd = 0;
    struct qemud_socket *sock;
    struct qemud_client *client;
    struct qemud_vm *vm;

    for (sock = server->sockets ; sock ; sock = sock->next) {
        fds[fd].fd = sock->fd;
        fds[fd].events = POLLIN;
        fd++;
    }

    for (client = server->clients ; client ; client = client->next) {
        fds[fd].fd = client->fd;
        /* Refuse to read more from client if tx is pending to
           rate limit */
        if (client->tx)
            fds[fd].events = POLLOUT | POLLERR | POLLHUP;
        else
            fds[fd].events = POLLIN | POLLERR | POLLHUP;
        fd++;
    }
    for (vm = server->activevms ; vm ; vm = vm->next) {
        if (vm->stdout != -1) {
            fds[fd].fd = vm->stdout;
            fds[fd].events = POLLIN | POLLERR | POLLHUP;
            fd++;
        }
        if (vm->stderr != -1) {
            fds[fd].fd = vm->stderr;
            fds[fd].events = POLLIN | POLLERR | POLLHUP;
            fd++;
        }
    }
}



static int qemudOneLoop(struct qemud_server *server, int timeout) {
    int nfds = server->nsockets + server->nclients + server->nvmfds;
    struct pollfd fds[nfds];
    int thistimeout = -1;
    int ret;

    /* If we have no clients or vms, then timeout after
       30 seconds, letting daemon exit */
    if (timeout > 0 &&
        !server->nclients &&
        !server->nactivevms)
        thistimeout = timeout;

    qemudPreparePoll(server, fds);

 retry:

    if ((ret = poll(fds, nfds, thistimeout * 1000)) < 0) {
        if (errno == EINTR) {
            goto retry;
        }
        return -1;
    }

    /* Must have timed out */
    if (ret == 0)
        return -1;

    if (qemudDispatchPoll(server, fds) < 0)
        return -1;

    return 0;
}

static int qemudRunLoop(struct qemud_server *server, int timeout) {
    int ret;

    while ((ret = qemudOneLoop(server, timeout)) == 0)
        ;

    return ret == -1 ? -1 : 0;
}

static void qemudCleanup(struct qemud_server *server) {
    struct qemud_socket *sock = server->sockets;
    while (sock) {
        close(sock->fd);
        sock = sock->next;
    }
    if (server->brctl)
        brShutdown(server->brctl);
    if (server->iptables)
        iptablesContextFree(server->iptables);
    free(server);
}

#define MAX_LISTEN 5
int main(int argc, char **argv) {
    int godaemon = 0;
    int verbose = 0;
    int sys = 0;
    int timeout = -1;
    struct qemud_server *server;

    struct option opts[] = {
        { "verbose", no_argument, &verbose, 1},
        { "daemon", no_argument, &godaemon, 1},
        { "system", no_argument, &sys, 1},
        { "timeout", required_argument, 0, 't'},
        {0, 0, 0, 0}
    };

    while (1) {
        int optidx = 0;
        int c;
        char *tmp;

        c = getopt_long(argc, argv, "vsdt:", opts, &optidx);

        if (c == -1) {
            break;
        }

        switch (c) {
        case 0:
            /* Got one of the flags */
            break;
        case 'v':
            verbose = 1;
            break;
        case 'd':
            godaemon = 1;
            break;
        case 's':
            sys = 1;
            break;

        case 't':
            timeout = strtol(optarg, &tmp, 10);
            if (!tmp)
                timeout = -1;
            if (timeout <= 0)
                timeout = -1;
            break;
        case '?':
            return 2;
            break;

        default:
            abort();
        }
    }

    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR)
        return 3;
    if (signal(SIGCHLD, reapchild) == SIG_ERR)
        return 3;

    if (godaemon) {
        int pid = qemudGoDaemon();
        if (pid < 0)
            return 1;
        if (pid > 0)
            return 0;
    }

    if (!(server = qemudInitialize(sys)))
        return 2;

    qemudRunLoop(server, timeout);

    qemudCleanup(server);

    return 0;
}

/*
 * Local variables:
 *  indent-tabs-mode: nil
 *  c-indent-level: 4
 *  c-basic-offset: 4
 *  tab-width: 4
 * End:
 */
