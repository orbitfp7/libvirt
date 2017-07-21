/*
 * cpu_ppc64.c: CPU driver for 64-bit PowerPC CPUs
 *
 * Copyright (C) 2013 Red Hat, Inc.
 * Copyright (C) IBM Corporation, 2010
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
 *      Anton Blanchard <anton@au.ibm.com>
 *      Prerna Saxena <prerna@linux.vnet.ibm.com>
 *      Li Zhang <zhlcindy@linux.vnet.ibm.com>
 */

#include <config.h>
#include <stdint.h>

#include "virlog.h"
#include "viralloc.h"
#include "cpu.h"
#include "virstring.h"
#include "cpu_map.h"
#include "virbuffer.h"

#define VIR_FROM_THIS VIR_FROM_CPU

VIR_LOG_INIT("cpu.cpu_ppc64");

static const virArch archs[] = { VIR_ARCH_PPC64, VIR_ARCH_PPC64LE };

struct ppc64_vendor {
    char *name;
    struct ppc64_vendor *next;
};

struct ppc64_model {
    char *name;
    const struct ppc64_vendor *vendor;
    virCPUppc64Data *data;
    struct ppc64_model *next;
};

struct ppc64_map {
    struct ppc64_vendor *vendors;
    struct ppc64_model *models;
};

/* Convert a legacy CPU definition by transforming
 * model names to generation names:
 *   POWER7_v2.1  => POWER7
 *   POWER7_v2.3  => POWER7
 *   POWER7+_v2.1 => POWER7
 *   POWER8_v1.0  => POWER8 */
static virCPUDefPtr
ppc64ConvertLegacyCPUDef(const virCPUDef *legacy)
{
    virCPUDefPtr cpu;

    if (!(cpu = virCPUDefCopy(legacy)))
        goto out;

    if (!cpu->model ||
        !(STREQ(cpu->model, "POWER7_v2.1") ||
          STREQ(cpu->model, "POWER7_v2.3") ||
          STREQ(cpu->model, "POWER7+_v2.1") ||
          STREQ(cpu->model, "POWER8_v1.0"))) {
        goto out;
    }

    cpu->model[strlen("POWERx")] = 0;

 out:
    return cpu;
}

/* Some hosts can run guests in compatibility mode, but not all
 * host CPUs support this and not all combinations are valid.
 * This function performs the necessary checks */
static virCPUCompareResult
ppc64CheckCompatibilityMode(const char *host_model,
                            const char *compat_mode)
{
    int host;
    int compat;
    char *tmp;
    virCPUCompareResult ret = VIR_CPU_COMPARE_ERROR;

    if (!compat_mode)
        return VIR_CPU_COMPARE_IDENTICAL;

    /* Valid host CPUs: POWER6, POWER7, POWER8 */
    if (!STRPREFIX(host_model, "POWER") ||
        !(tmp = (char *) host_model + strlen("POWER")) ||
        virStrToLong_i(tmp, NULL, 10, &host) < 0 ||
        host < 6 || host > 8) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s",
                       _("Host CPU does not support compatibility modes"));
        goto out;
    }

    /* Valid compatibility modes: power6, power7, power8 */
    if (!STRPREFIX(compat_mode, "power") ||
        !(tmp = (char *) compat_mode + strlen("power")) ||
        virStrToLong_i(tmp, NULL, 10, &compat) < 0 ||
        compat < 6 || compat > 8) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Unknown compatibility mode %s"),
                       compat_mode);
        goto out;
    }

    /* Version check */
    if (compat > host)
        ret = VIR_CPU_COMPARE_INCOMPATIBLE;
    else
        ret = VIR_CPU_COMPARE_IDENTICAL;

 out:
    return ret;
}

static void
ppc64DataFree(virCPUppc64Data *data)
{
    if (!data)
        return;

    VIR_FREE(data->pvr);
    VIR_FREE(data);
}

static virCPUppc64Data *
ppc64DataCopy(const virCPUppc64Data *data)
{
    virCPUppc64Data *copy;
    size_t i;

    if (VIR_ALLOC(copy) < 0)
        goto error;

    if (VIR_ALLOC_N(copy->pvr, data->len) < 0)
        goto error;

    copy->len = data->len;

    for (i = 0; i < data->len; i++) {
        copy->pvr[i].value = data->pvr[i].value;
        copy->pvr[i].mask = data->pvr[i].mask;
    }

    return copy;

 error:
    ppc64DataFree(copy);
    return NULL;
}

static void
ppc64VendorFree(struct ppc64_vendor *vendor)
{
    if (!vendor)
        return;

    VIR_FREE(vendor->name);
    VIR_FREE(vendor);
}

static struct ppc64_vendor *
ppc64VendorFind(const struct ppc64_map *map,
                const char *name)
{
    struct ppc64_vendor *vendor;

    vendor = map->vendors;
    while (vendor) {
        if (STREQ(vendor->name, name))
            return vendor;

        vendor = vendor->next;
    }

    return NULL;
}

static void
ppc64ModelFree(struct ppc64_model *model)
{
    if (!model)
        return;

    ppc64DataFree(model->data);
    VIR_FREE(model->name);
    VIR_FREE(model);
}

static struct ppc64_model *
ppc64ModelCopy(const struct ppc64_model *model)
{
    struct ppc64_model *copy;

    if (VIR_ALLOC(copy) < 0)
        goto error;

    if (VIR_STRDUP(copy->name, model->name) < 0)
        goto error;

    if (!(copy->data = ppc64DataCopy(model->data)))
        goto error;

    copy->vendor = model->vendor;

    return copy;

 error:
    ppc64ModelFree(copy);
    return NULL;
}

static struct ppc64_model *
ppc64ModelFind(const struct ppc64_map *map,
               const char *name)
{
    struct ppc64_model *model;

    model = map->models;
    while (model) {
        if (STREQ(model->name, name))
            return model;

        model = model->next;
    }

    return NULL;
}

static struct ppc64_model *
ppc64ModelFindPVR(const struct ppc64_map *map,
                  uint32_t pvr)
{
    struct ppc64_model *model;
    size_t i;

    model = map->models;
    while (model) {
        for (i = 0; i < model->data->len; i++) {
            if ((pvr & model->data->pvr[i].mask) == model->data->pvr[i].value)
                return model;
        }
        model = model->next;
    }

    return NULL;
}

static struct ppc64_model *
ppc64ModelFromCPU(const virCPUDef *cpu,
                  const struct ppc64_map *map)
{
    struct ppc64_model *model;

    if (!(model = ppc64ModelFind(map, cpu->model))) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Unknown CPU model %s"), cpu->model);
        return NULL;
    }

    return ppc64ModelCopy(model);
}

static void
ppc64MapFree(struct ppc64_map *map)
{
    if (!map)
        return;

    while (map->models) {
        struct ppc64_model *model = map->models;
        map->models = model->next;
        ppc64ModelFree(model);
    }

    while (map->vendors) {
        struct ppc64_vendor *vendor = map->vendors;
        map->vendors = vendor->next;
        ppc64VendorFree(vendor);
    }

    VIR_FREE(map);
}

static int
ppc64VendorLoad(xmlXPathContextPtr ctxt,
                struct ppc64_map *map)
{
    struct ppc64_vendor *vendor;

    if (VIR_ALLOC(vendor) < 0)
        return -1;

    vendor->name = virXPathString("string(@name)", ctxt);
    if (!vendor->name) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("Missing CPU vendor name"));
        goto ignore;
    }

    if (ppc64VendorFind(map, vendor->name)) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("CPU vendor %s already defined"), vendor->name);
        goto ignore;
    }

    if (!map->vendors) {
        map->vendors = vendor;
    } else {
        vendor->next = map->vendors;
        map->vendors = vendor;
    }

 cleanup:
    return 0;

 ignore:
    ppc64VendorFree(vendor);
    goto cleanup;
}

static int
ppc64ModelLoad(xmlXPathContextPtr ctxt,
               struct ppc64_map *map)
{
    struct ppc64_model *model;
    xmlNodePtr *nodes = NULL;
    xmlNodePtr bookmark;
    char *vendor = NULL;
    unsigned long pvr;
    size_t i;
    int n;

    /* Save the node the context was pointing to, as we're going
     * to change it later. It's going to be restored on exit */
    bookmark = ctxt->node;

    if (VIR_ALLOC(model) < 0)
        return -1;

    if (VIR_ALLOC(model->data) < 0) {
        ppc64ModelFree(model);
        return -1;
    }

    model->name = virXPathString("string(@name)", ctxt);
    if (!model->name) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("Missing CPU model name"));
        goto ignore;
    }

    if (ppc64ModelFind(map, model->name)) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("CPU model %s already defined"), model->name);
        goto ignore;
    }

    if (virXPathBoolean("boolean(./vendor)", ctxt)) {
        vendor = virXPathString("string(./vendor/@name)", ctxt);
        if (!vendor) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Invalid vendor element in CPU model %s"),
                           model->name);
            goto ignore;
        }

        if (!(model->vendor = ppc64VendorFind(map, vendor))) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Unknown vendor %s referenced by CPU model %s"),
                           vendor, model->name);
            goto ignore;
        }
    }

    if ((n = virXPathNodeSet("./pvr", ctxt, &nodes)) <= 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Missing PVR information for CPU model %s"),
                       model->name);
        goto ignore;
    }

    if (VIR_ALLOC_N(model->data->pvr, n) < 0)
        goto ignore;

    model->data->len = n;

    for (i = 0; i < n; i++) {
        ctxt->node = nodes[i];

        if (virXPathULongHex("string(./@value)", ctxt, &pvr) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Missing or invalid PVR value in CPU model %s"),
                           model->name);
            goto ignore;
        }
        model->data->pvr[i].value = pvr;

        if (virXPathULongHex("string(./@mask)", ctxt, &pvr) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Missing or invalid PVR mask in CPU model %s"),
                           model->name);
            goto ignore;
        }
        model->data->pvr[i].mask = pvr;
    }

    if (!map->models) {
        map->models = model;
    } else {
        model->next = map->models;
        map->models = model;
    }

 cleanup:
    ctxt->node = bookmark;
    VIR_FREE(vendor);
    VIR_FREE(nodes);
    return 0;

 ignore:
    ppc64ModelFree(model);
    goto cleanup;
}

static int
ppc64MapLoadCallback(cpuMapElement element,
                     xmlXPathContextPtr ctxt,
                     void *data)
{
    struct ppc64_map *map = data;

    switch (element) {
    case CPU_MAP_ELEMENT_VENDOR:
        return ppc64VendorLoad(ctxt, map);
    case CPU_MAP_ELEMENT_MODEL:
        return ppc64ModelLoad(ctxt, map);
    case CPU_MAP_ELEMENT_FEATURE:
    case CPU_MAP_ELEMENT_LAST:
        break;
    }

    return 0;
}

static struct ppc64_map *
ppc64LoadMap(void)
{
    struct ppc64_map *map;

    if (VIR_ALLOC(map) < 0)
        goto error;

    if (cpuMapLoad("ppc64", ppc64MapLoadCallback, map) < 0)
        goto error;

    return map;

 error:
    ppc64MapFree(map);
    return NULL;
}

static virCPUDataPtr
ppc64MakeCPUData(virArch arch,
                 virCPUppc64Data *data)
{
    virCPUDataPtr cpuData;

    if (VIR_ALLOC(cpuData) < 0)
        return NULL;

    cpuData->arch = arch;

    if (!(cpuData->data.ppc64 = ppc64DataCopy(data)))
        VIR_FREE(cpuData);

    return cpuData;
}

static virCPUCompareResult
ppc64Compute(virCPUDefPtr host,
             const virCPUDef *other,
             virCPUDataPtr *guestData,
             char **message)
{
    struct ppc64_map *map = NULL;
    struct ppc64_model *host_model = NULL;
    struct ppc64_model *guest_model = NULL;
    virCPUDefPtr cpu = NULL;
    virCPUCompareResult ret = VIR_CPU_COMPARE_ERROR;
    virArch arch;
    size_t i;

    /* Ensure existing configurations are handled correctly */
    if (!(cpu = ppc64ConvertLegacyCPUDef(other)))
        goto cleanup;

    if (cpu->arch != VIR_ARCH_NONE) {
        bool found = false;

        for (i = 0; i < ARRAY_CARDINALITY(archs); i++) {
            if (archs[i] == cpu->arch) {
                found = true;
                break;
            }
        }

        if (!found) {
            VIR_DEBUG("CPU arch %s does not match host arch",
                      virArchToString(cpu->arch));
            if (message &&
                virAsprintf(message,
                            _("CPU arch %s does not match host arch"),
                            virArchToString(cpu->arch)) < 0)
                goto cleanup;

            ret = VIR_CPU_COMPARE_INCOMPATIBLE;
            goto cleanup;
        }
        arch = cpu->arch;
    } else {
        arch = host->arch;
    }

    if (cpu->vendor &&
        (!host->vendor || STRNEQ(cpu->vendor, host->vendor))) {
        VIR_DEBUG("host CPU vendor does not match required CPU vendor %s",
                  cpu->vendor);
        if (message &&
            virAsprintf(message,
                        _("host CPU vendor does not match required "
                        "CPU vendor %s"),
                        cpu->vendor) < 0)
            goto cleanup;

        ret = VIR_CPU_COMPARE_INCOMPATIBLE;
        goto cleanup;
    }

    if (!(map = ppc64LoadMap()))
        goto cleanup;

    /* Host CPU information */
    if (!(host_model = ppc64ModelFromCPU(host, map)))
        goto cleanup;

    if (cpu->type == VIR_CPU_TYPE_GUEST) {
        /* Guest CPU information */
        virCPUCompareResult tmp;
        switch (cpu->mode) {
        case VIR_CPU_MODE_HOST_MODEL:
            /* host-model only:
             * we need to take compatibility modes into account */
            tmp = ppc64CheckCompatibilityMode(host->model, cpu->model);
            if (tmp != VIR_CPU_COMPARE_IDENTICAL) {
                ret = tmp;
                goto cleanup;
            }
            /* fallthrough */

        case VIR_CPU_MODE_HOST_PASSTHROUGH:
            /* host-model and host-passthrough:
             * the guest CPU is the same as the host */
            if (!(guest_model = ppc64ModelCopy(host_model)))
                goto cleanup;
            break;

        case VIR_CPU_MODE_CUSTOM:
            /* custom:
             * look up guest CPU information */
            if (!(guest_model = ppc64ModelFromCPU(cpu, map)))
                goto cleanup;
            break;
        }
    } else {
        /* Other host CPU information */
        if (!(guest_model = ppc64ModelFromCPU(cpu, map)))
            goto cleanup;
    }

    if (STRNEQ(guest_model->name, host_model->name)) {
        VIR_DEBUG("host CPU model does not match required CPU model %s",
                  guest_model->name);
        if (message &&
            virAsprintf(message,
                        _("host CPU model does not match required "
                        "CPU model %s"),
                        guest_model->name) < 0)
            goto cleanup;

        ret = VIR_CPU_COMPARE_INCOMPATIBLE;
        goto cleanup;
    }

    if (guestData)
        if (!(*guestData = ppc64MakeCPUData(arch, guest_model->data)))
            goto cleanup;

    ret = VIR_CPU_COMPARE_IDENTICAL;

 cleanup:
    virCPUDefFree(cpu);
    ppc64MapFree(map);
    ppc64ModelFree(host_model);
    ppc64ModelFree(guest_model);
    return ret;
}

static virCPUCompareResult
ppc64DriverCompare(virCPUDefPtr host,
                   virCPUDefPtr cpu,
                   bool failIncompatible)
{
    virCPUCompareResult ret;
    char *message = NULL;

    ret = ppc64Compute(host, cpu, NULL, &message);

    if (failIncompatible && ret == VIR_CPU_COMPARE_INCOMPATIBLE) {
        ret = VIR_CPU_COMPARE_ERROR;
        if (message) {
            virReportError(VIR_ERR_CPU_INCOMPATIBLE, "%s", message);
        } else {
            virReportError(VIR_ERR_CPU_INCOMPATIBLE, NULL);
        }
    }
    VIR_FREE(message);

    return ret;
}

static int
ppc64DriverDecode(virCPUDefPtr cpu,
                  const virCPUData *data,
                  const char **models,
                  unsigned int nmodels,
                  const char *preferred ATTRIBUTE_UNUSED,
                  unsigned int flags)
{
    int ret = -1;
    struct ppc64_map *map;
    const struct ppc64_model *model;

    virCheckFlags(VIR_CONNECT_BASELINE_CPU_EXPAND_FEATURES, -1);

    if (!data || !(map = ppc64LoadMap()))
        return -1;

    if (!(model = ppc64ModelFindPVR(map, data->data.ppc64->pvr[0].value))) {
        virReportError(VIR_ERR_OPERATION_FAILED,
                       _("Cannot find CPU model with PVR 0x%08x"),
                       data->data.ppc64->pvr[0].value);
        goto cleanup;
    }

    if (!cpuModelIsAllowed(model->name, models, nmodels)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("CPU model %s is not supported by hypervisor"),
                       model->name);
        goto cleanup;
    }

    if (VIR_STRDUP(cpu->model, model->name) < 0 ||
        (model->vendor && VIR_STRDUP(cpu->vendor, model->vendor->name) < 0)) {
        goto cleanup;
    }

    ret = 0;

 cleanup:
    ppc64MapFree(map);

    return ret;
}

static void
ppc64DriverFree(virCPUDataPtr data)
{
    if (!data)
        return;

    ppc64DataFree(data->data.ppc64);
    VIR_FREE(data);
}

static virCPUDataPtr
ppc64DriverNodeData(virArch arch)
{
    virCPUDataPtr nodeData;
    virCPUppc64Data *data;

    if (VIR_ALLOC(nodeData) < 0)
        goto error;

    if (VIR_ALLOC(nodeData->data.ppc64) < 0)
        goto error;

    data = nodeData->data.ppc64;

    if (VIR_ALLOC_N(data->pvr, 1) < 0)
        goto error;

    data->len = 1;

#if defined(__powerpc__) || defined(__powerpc64__)
    asm("mfpvr %0"
        : "=r" (data->pvr[0].value));
#endif
    data->pvr[0].mask = 0xfffffffful;

    nodeData->arch = arch;

    return nodeData;

 error:
    ppc64DriverFree(nodeData);
    return NULL;
}

static virCPUCompareResult
ppc64DriverGuestData(virCPUDefPtr host,
                     virCPUDefPtr guest,
                     virCPUDataPtr *data,
                     char **message)
{
    return ppc64Compute(host, guest, data, message);
}

static int
ppc64DriverUpdate(virCPUDefPtr guest,
                  const virCPUDef *host)
{
    switch ((virCPUMode) guest->mode) {
    case VIR_CPU_MODE_HOST_PASSTHROUGH:
        guest->match = VIR_CPU_MATCH_EXACT;
        guest->fallback = VIR_CPU_FALLBACK_FORBID;
        virCPUDefFreeModel(guest);
        return virCPUDefCopyModel(guest, host, true);

    case VIR_CPU_MODE_HOST_MODEL:
    case VIR_CPU_MODE_CUSTOM:
        return 0;

    case VIR_CPU_MODE_LAST:
        break;
    }

    virReportError(VIR_ERR_INTERNAL_ERROR,
                   _("Unexpected CPU mode: %d"), guest->mode);
    return -1;
}

static virCPUDefPtr
ppc64DriverBaseline(virCPUDefPtr *cpus,
                    unsigned int ncpus,
                    const char **models ATTRIBUTE_UNUSED,
                    unsigned int nmodels ATTRIBUTE_UNUSED,
                    unsigned int flags)
{
    struct ppc64_map *map;
    const struct ppc64_model *model;
    const struct ppc64_vendor *vendor = NULL;
    virCPUDefPtr cpu = NULL;
    size_t i;

    virCheckFlags(VIR_CONNECT_BASELINE_CPU_EXPAND_FEATURES |
                  VIR_CONNECT_BASELINE_CPU_MIGRATABLE, NULL);

    if (!(map = ppc64LoadMap()))
        goto error;

    if (!(model = ppc64ModelFind(map, cpus[0]->model))) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Unknown CPU model %s"), cpus[0]->model);
        goto error;
    }

    for (i = 0; i < ncpus; i++) {
        const struct ppc64_vendor *vnd;

        /* Hosts running old (<= 1.2.18) versions of libvirt will report
         * strings like 'power7+' or 'power8e' instead of proper CPU model
         * names in the capabilities XML; moreover, they lack information
         * about some proper CPU models like 'POWER8'.
         * This implies two things:
         *   1) baseline among such hosts never worked
         *   2) while a few models, eg. 'POWER8_v1.0', could work on both
         *      old and new versions of libvirt, the information we have
         *      here is not enough to pick such a model
         * Hence we just compare models by name to decide whether or not
         * two hosts are compatible */
        if (STRNEQ(cpus[i]->model, model->name)) {
            virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                           _("CPUs are incompatible"));
            goto error;
        }

        if (!cpus[i]->vendor)
            continue;

        if (!(vnd = ppc64VendorFind(map, cpus[i]->vendor))) {
            virReportError(VIR_ERR_OPERATION_FAILED,
                           _("Unknown CPU vendor %s"), cpus[i]->vendor);
            goto error;
        }

        if (model->vendor) {
            if (model->vendor != vnd) {
                virReportError(VIR_ERR_OPERATION_FAILED,
                               _("CPU vendor %s of model %s differs from "
                                 "vendor %s"),
                               model->vendor->name, model->name,
                               vnd->name);
                goto error;
            }
        } else if (vendor) {
            if (vendor != vnd) {
                virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                               _("CPU vendors do not match"));
                goto error;
            }
        } else {
            vendor = vnd;
        }
    }

    if (VIR_ALLOC(cpu) < 0 ||
        VIR_STRDUP(cpu->model, model->name) < 0)
        goto error;

    if (vendor && VIR_STRDUP(cpu->vendor, vendor->name) < 0)
        goto error;

    cpu->type = VIR_CPU_TYPE_GUEST;
    cpu->match = VIR_CPU_MATCH_EXACT;
    cpu->fallback = VIR_CPU_FALLBACK_FORBID;

 cleanup:
    ppc64MapFree(map);

    return cpu;

 error:
    virCPUDefFree(cpu);
    cpu = NULL;
    goto cleanup;
}

static int
ppc64DriverGetModels(char ***models)
{
    struct ppc64_map *map;
    struct ppc64_model *model;
    char *name;
    size_t nmodels = 0;

    if (!(map = ppc64LoadMap()))
        goto error;

    if (models && VIR_ALLOC_N(*models, 0) < 0)
        goto error;

    model = map->models;
    while (model) {
        if (models) {
            if (VIR_STRDUP(name, model->name) < 0)
                goto error;

            if (VIR_APPEND_ELEMENT(*models, nmodels, name) < 0)
                goto error;
        } else {
            nmodels++;
        }

        model = model->next;
    }

 cleanup:
    ppc64MapFree(map);

    return nmodels;

 error:
    if (models) {
        virStringFreeList(*models);
        *models = NULL;
    }
    nmodels = -1;
    goto cleanup;
}

struct cpuArchDriver cpuDriverPPC64 = {
    .name       = "ppc64",
    .arch       = archs,
    .narch      = ARRAY_CARDINALITY(archs),
    .compare    = ppc64DriverCompare,
    .decode     = ppc64DriverDecode,
    .encode     = NULL,
    .free       = ppc64DriverFree,
    .nodeData   = ppc64DriverNodeData,
    .guestData  = ppc64DriverGuestData,
    .baseline   = ppc64DriverBaseline,
    .update     = ppc64DriverUpdate,
    .hasFeature = NULL,
    .getModels  = ppc64DriverGetModels,
};
