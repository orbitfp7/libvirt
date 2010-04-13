/*
 * interface.c: interface support functions
 *
 * Copyright (C) 2010 IBM Corp.
 * Copyright (C) 2010 Stefan Berger
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
 * chgIfaceFlags originated from bridge.c
 *
 * Author: Stefan Berger <stefanb@us.ibm.com>
 */

#include <config.h>

#include <sys/socket.h>
#include <sys/ioctl.h>
#include <linux/if.h>

#include "internal.h"

#include "util.h"
#include "interface.h"
#include "virterror_internal.h"

#define ifaceError(code, ...) \
        virReportErrorHelper(NULL, VIR_FROM_NET, code, __FILE__, \
                             __FUNCTION__, __LINE__, __VA_ARGS__)

/*
 * chgIfFlags: Change flags on an interface
 *
 * @ifname : name of the interface
 * @flagclear : the flags to clear
 * @flagset : the flags to set
 *
 * The new flags of the interface will be calculated as
 * flagmask = (~0 ^ flagclear)
 * newflags = (curflags & flagmask) | flagset;
 *
 * Returns 0 on success, errno on failure.
 */
static int chgIfaceFlags(const char *ifname, short flagclear, short flagset) {
    struct ifreq ifr;
    int rc = 0;
    int flags;
    short flagmask = (~0 ^ flagclear);
    int fd = socket(PF_PACKET, SOCK_DGRAM, 0);

    if (fd < 0)
        return errno;

    if (virStrncpy(ifr.ifr_name,
                   ifname, strlen(ifname), sizeof(ifr.ifr_name)) == NULL) {
        rc = ENODEV;
        goto err_exit;
    }

    if (ioctl(fd, SIOCGIFFLAGS, &ifr) < 0) {
        rc = errno;
        goto err_exit;
    }

    flags = (ifr.ifr_flags & flagmask) | flagset;

    if (ifr.ifr_flags != flags) {
        ifr.ifr_flags = flags;

        if (ioctl(fd, SIOCSIFFLAGS, &ifr) < 0)
            rc = errno;
    }

err_exit:
    close(fd);
    return rc;
}


/*
 * ifaceCtrl
 * @name: name of the interface
 * @up: true (1) for up, false (0) for down
 *
 * Function to control if an interface is activated (up, 1) or not (down, 0)
 *
 * Returns 0 in case of success or an errno code in case of failure.
 */
int
ifaceCtrl(const char *name, bool up)
{
    return chgIfaceFlags(name,
                         (up) ? 0      : IFF_UP,
                         (up) ? IFF_UP : 0);
}


/**
 * ifaceCheck
 *
 * @reportError: whether to report errors or keep silent
 * @ifname: Name of the interface
 * @macaddr: expected MAC address of the interface; not checked if NULL
 * @ifindex: expected index of the interface; not checked if '-1'
 *
 * Determine whether a given interface is still available. If so,
 * it must have the given MAC address and if an interface index is
 * passed, it must also match the interface index.
 *
 * Returns 0 on success, an error code on failure.
 *   ENODEV : if interface with given name does not exist or its interface
 *            index is different than the one passed
 *   EINVAL : if interface name is invalid (too long)
 */
int
ifaceCheck(bool reportError, const char *ifname,
           const unsigned char *macaddr, int ifindex)
{
    struct ifreq ifr;
    int fd = -1;
    int rc = 0;
    int idx;

    if (macaddr != NULL) {
        fd = socket(PF_PACKET, SOCK_DGRAM, 0);
        if (fd < 0)
            return errno;

        if (virStrncpy(ifr.ifr_name,
                       ifname, strlen(ifname), sizeof(ifr.ifr_name)) == NULL) {
            if (reportError)
                ifaceError(VIR_ERR_INTERNAL_ERROR,
                           _("invalid interface name %s"),
                           ifname);
            rc = EINVAL;
            goto err_exit;
        }

        if (ioctl(fd, SIOCGIFHWADDR, &ifr) < 0) {
            if (reportError)
                ifaceError(VIR_ERR_INTERNAL_ERROR,
                           _("coud not get MAC address of interface %s"),
                           ifname);
            rc = errno;
            goto err_exit;
        }

        if (memcmp(&ifr.ifr_hwaddr.sa_data, macaddr, VIR_MAC_BUFLEN) != 0) {
            rc = ENODEV;
            goto err_exit;
        }
    }

    if (ifindex != -1) {
        rc = ifaceGetIndex(reportError, ifname, &idx);
        if (rc == 0 && idx != ifindex)
            rc = ENODEV;
    }

 err_exit:
    if (fd >= 0)
        close(fd);

    return rc;
}


/**
 * ifaceGetIndex
 *
 * @reportError: whether to report errors or keep silent
 * @ifname : Name of the interface whose index is to be found
 * @ifindex: Pointer to int where the index will be written into
 *
 * Get the index of an interface given its name.
 *
 * Returns 0 on success, an error code on failure.
 *   ENODEV : if interface with given name does not exist
 *   EINVAL : if interface name is invalid (too long)
 */
int
ifaceGetIndex(bool reportError, const char *ifname, int *ifindex)
{
    int rc = 0;
    struct ifreq ifreq;
    int fd = socket(PF_PACKET, SOCK_DGRAM, 0);

    if (fd < 0)
        return errno;

    if (virStrncpy(ifreq.ifr_name, ifname, strlen(ifname),
                   sizeof(ifreq.ifr_name)) == NULL) {
        if (reportError)
            ifaceError(VIR_ERR_INTERNAL_ERROR,
                       _("invalid interface name %s"),
                       ifname);
        rc = EINVAL;
        goto err_exit;
    }

    if (ioctl(fd, SIOCGIFINDEX, &ifreq) >= 0)
        *ifindex = ifreq.ifr_ifindex;
    else {
        if (reportError)
            ifaceError(VIR_ERR_INTERNAL_ERROR,
                       _("interface %s does not exist"),
                       ifname);
        rc = ENODEV;
    }

err_exit:
    close(fd);

    return rc;
}
