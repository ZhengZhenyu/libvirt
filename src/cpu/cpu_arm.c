/*
 * cpu_arm.c: CPU driver for arm CPUs
 *
 * Copyright (C) 2013 Red Hat, Inc.
 * Copyright (C) Canonical Ltd. 2012
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

#include "viralloc.h"
#include "virlog.h"
#include "cpu.h"
#include "cpu_map.h"
#include "vircommand.h"
#include "virstring.h"
#include "virxml.h"
#include "cpu_map.h"
#include "cpu_arm.h"

#define VIR_FROM_THIS VIR_FROM_CPU

VIR_LOG_INIT("cpu.cpu_arm");

static const char *lsCpuPath = "/usr/bin/lscpu";

#define LSCPU lsCpuPath
#define MAX_LSCPU_SIZE = (1024*1024)   /* 1MB limit for lscpu output */

static const virArch archs[] = {
    VIR_ARCH_ARMV6L,
    VIR_ARCH_ARMV7B,
    VIR_ARCH_ARMV7L,
    VIR_ARCH_AARCH64,
};

typedef struct _virCPUarmVendor virCPUarmVendor;
typedef virCPUarmVendor *virCPUarmVendorPtr;
struct _virCPUarmVendor {
    char *name;
    unsigned long value;
};

typedef struct _virCPUarmModel virCPUarmModel;
typedef virCPUarmModel *virCPUarmModelPtr;
struct _virCPUarmModel {
    char *name;
    virCPUarmVendorPtr vendor;
    virCPUarmData data;
};

typedef struct _virCPUarmFeature virCPUarmFeature;
typedef virCPUarmFeature *virCPUarmFeaturePtr;
struct _virCPUarmFeature {
    char *name;
};

static virCPUarmFeaturePtr
virCPUarmFeatureNew(void)
{
    return g_new0(virCPUarmFeature, 1);
}

static void
virCPUarmFeatureFree(virCPUarmFeaturePtr feature)
{
    if (!feature)
        return;

    g_free(feature->name);

    g_free(feature);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(virCPUarmFeature, virCPUarmFeatureFree);

typedef struct _virCPUarmMap virCPUarmMap;
typedef virCPUarmMap *virCPUarmMapPtr;
struct _virCPUarmMap {
    size_t nvendors;
    virCPUarmVendorPtr *vendors;
    size_t nmodels;
    virCPUarmModelPtr *models;
    GPtrArray *features;
};

static virCPUarmMapPtr
virCPUarmMapNew(void)
{
    virCPUarmMapPtr map;

    map = g_new0(virCPUarmMap, 1);

    map->features = g_ptr_array_new();
    g_ptr_array_set_free_func(map->features,
                              (GDestroyNotify) virCPUarmFeatureFree);

    return map;
}

static void
virCPUarmDataClear(virCPUarmData *data)
{
    if (!data)
        return;

    VIR_FREE(data->features);
}

static void
virCPUarmDataFree(virCPUDataPtr cpuData)
{
    if (!cpuData)
        return;

    virCPUarmDataClear(&cpuData->data.arm);
    VIR_FREE(cpuData);
}

static void
armModelFree(virCPUarmModelPtr model)
{
    if (!model)
        return;

    virCPUarmDataClear(&model->data);
    g_free(model->name);
    g_free(model);
}

static void
armVendorFree(virCPUarmVendorPtr vendor)
{
    if (!vendor)
        return;

    g_free(vendor->name);
    g_free(vendor);
}

static void
virCPUarmMapFree(virCPUarmMapPtr map)
{
    if (!map)
        return;

    size_t i;

    for (i = 0; i < map->nmodels; i++)
        armModelFree(map->models[i]);
    g_free(map->models);

    for (i = 0; i < map->nvendors; i++)
        armVendorFree(map->vendors[i]);
    g_free(map->vendors);

    g_ptr_array_free(map->features, TRUE);

    g_free(map);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(virCPUarmMap, virCPUarmMapFree);

static virCPUarmFeaturePtr
virCPUarmMapFeatureFind(virCPUarmMapPtr map,
                        const char *name)
{
    size_t i;

    for (i = 0; i < map->features->len; i++) {
        virCPUarmFeaturePtr feature = g_ptr_array_index(map->features, i);

        if (STREQ(feature->name, name))
            return feature;
    }

    return NULL;
}

static virCPUarmVendorPtr
armVendorFindByID(virCPUarmMapPtr map,
                  unsigned long vendor_id)
{
    size_t i;

    for (i = 0; i < map->nvendors; i++) {
        if (map->vendors[i]->value == vendor_id)
            return map->vendors[i];
    }

    return NULL;
}


static virCPUarmVendorPtr
armVendorFindByName(virCPUarmMapPtr map,
                    const char *name)
{
    size_t i;

    for (i = 0; i < map->nvendors; i++) {
        if (STREQ(map->vendors[i]->name, name))
            return map->vendors[i];
    }

    return NULL;
}


static int
armVendorParse(xmlXPathContextPtr ctxt,
               const char *name,
               void *data)
{
    virCPUarmMapPtr map = (virCPUarmMapPtr)data;
    virCPUarmVendorPtr vendor = NULL;
    int ret = -1;

    if (VIR_ALLOC(vendor) < 0)
        return ret;

    vendor->name = g_strdup(name);

    if (armVendorFindByName(map, vendor->name)) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("CPU vendor %s already defined"), vendor->name);
        goto cleanup;
    }

    if (virXPathULongHex("string(@value)", ctxt, &vendor->value) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("Missing CPU vendor value"));
        goto cleanup;
    }

    if (armVendorFindByID(map, vendor->value)) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("CPU vendor value 0x%2lx already defined"), vendor->value);
        goto cleanup;
    }

    if (VIR_APPEND_ELEMENT(map->vendors, map->nvendors, vendor) < 0)
        goto cleanup;

    ret = 0;

 cleanup:
    armVendorFree(vendor);
    return ret;

}

static virCPUarmModelPtr
armModelFind(virCPUarmMapPtr map,
             const char *name)
{
    size_t i;

    for (i = 0; i < map->nmodels; i++) {
        if (STREQ(map->models[i]->name, name))
            return map->models[i];
    }

    return NULL;
}

static int
armModelParse(xmlXPathContextPtr ctxt,
              const char *name,
              void *data)
{
    virCPUarmMapPtr map = (virCPUarmMapPtr)data;
    virCPUarmModel *model;
    xmlNodePtr *nodes = NULL;
    char *vendor = NULL;
    int ret = -1;

    if (VIR_ALLOC(model) < 0)
        goto error;

    model->name = g_strdup(name);

    if (armModelFind(map, model->name)) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("CPU model %s already defined"), model->name);
        goto error;
    }

    if (virXPathBoolean("boolean(./vendor)", ctxt)) {
        vendor = virXPathString("string(./vendor/@name)", ctxt);
        if (!vendor) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Invalid vendor element in CPU model %s"),
                           model->name);
            goto error;
        }

        if (!(model->vendor = armVendorFindByName(map, vendor))) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Unknown vendor %s referenced by CPU model %s"),
                           vendor, model->name);
            goto error;
        }
    }

    if (VIR_APPEND_ELEMENT(map->models, map->nmodels, model) < 0)
        goto error;

    ret = 0;

 cleanup:
    VIR_FREE(vendor);
    VIR_FREE(nodes);
    return ret;

 error:
    armModelFree(model);
    goto cleanup;
}

static int
virCPUarmMapFeatureParse(xmlXPathContextPtr ctxt G_GNUC_UNUSED,
                         const char *name,
                         void *data)
{
    g_autoptr(virCPUarmFeature) feature = NULL;
    virCPUarmMapPtr map = data;

    if (virCPUarmMapFeatureFind(map, name)) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("CPU feature %s already defined"), name);
        return -1;
    }

    feature = virCPUarmFeatureNew();
    feature->name = g_strdup(name);

    g_ptr_array_add(map->features, g_steal_pointer(&feature));

    return 0;
}

static virCPUarmMapPtr
virCPUarmLoadMap(void)
{
    g_autoptr(virCPUarmMap) map = NULL;

    map = virCPUarmMapNew();

    if (cpuMapLoad("arm", armVendorParse, virCPUarmMapFeatureParse, armModelParse, map) < 0)
        return NULL;

    return g_steal_pointer(&map);
}

static virCPUarmMapPtr cpuMap;

int virCPUarmDriverOnceInit(void);
VIR_ONCE_GLOBAL_INIT(virCPUarmDriver);

int
virCPUarmDriverOnceInit(void)
{
    if (!(cpuMap = virCPUarmLoadMap()))
        return -1;

    return 0;
}

static virCPUarmMapPtr
virCPUarmGetMap(void)
{
    if (virCPUarmDriverInitialize() < 0)
        return NULL;

    return cpuMap;
}

static int
virCPUarmUpdate(virCPUDefPtr guest,
                const virCPUDef *host)
{
    int ret = -1;
    virCPUDefPtr updated = NULL;

    if (guest->mode != VIR_CPU_MODE_HOST_MODEL)
        return 0;

    if (!host) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("unknown host CPU model"));
        goto cleanup;
    }

    if (!(updated = virCPUDefCopyWithoutModel(guest)))
        goto cleanup;

    updated->mode = VIR_CPU_MODE_CUSTOM;
    if (virCPUDefCopyModel(updated, host, true) < 0)
        goto cleanup;

    virCPUDefStealModel(guest, updated, false);
    guest->mode = VIR_CPU_MODE_CUSTOM;
    guest->match = VIR_CPU_MATCH_EXACT;
    ret = 0;

 cleanup:
    virCPUDefFree(updated);
    return ret;
}


static virCPUDefPtr
virCPUarmBaseline(virCPUDefPtr *cpus,
                  unsigned int ncpus G_GNUC_UNUSED,
                  virDomainCapsCPUModelsPtr models G_GNUC_UNUSED,
                  const char **features G_GNUC_UNUSED,
                  bool migratable G_GNUC_UNUSED)
{
    virCPUDefPtr cpu = NULL;

    cpu = virCPUDefNew();

    cpu->model = g_strdup(cpus[0]->model);

    cpu->type = VIR_CPU_TYPE_GUEST;
    cpu->match = VIR_CPU_MATCH_EXACT;

    return cpu;
}

static virCPUCompareResult
virCPUarmCompare(virCPUDefPtr host G_GNUC_UNUSED,
                 virCPUDefPtr cpu G_GNUC_UNUSED,
                 bool failMessages G_GNUC_UNUSED)
{
    return VIR_CPU_COMPARE_IDENTICAL;
}

static int
virCPUarmValidateFeatures(virCPUDefPtr cpu)
{
    virCPUarmMapPtr map;
    size_t i;

    if (!(map = virCPUarmGetMap()))
        return -1;

    for (i = 0; i < cpu->nfeatures; i++) {
        virCPUFeatureDefPtr feature = &cpu->features[i];

        if (!virCPUarmMapFeatureFind(map, feature->name)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("unknown CPU feature: %s"),
                           feature->name);
            return -1;
        }
    }

    return 0;
}

static int
armCpuDataFromLsCpu(virCPUarmData *data)
{
    int ret = -1;
    char *outbuf = NULL;
    char *eol = NULL;
    const char *cur;
    virCommandPtr cmd = NULL;
    g_autofree char *lscpu = NULL;

    if (!data)
        return ret;

    lscpu = virFindFileInPath("lscpu");
    cmd = virCommandNew(lscpu);
    virCommandSetOutputBuffer(cmd, &outbuf);

    if (virCommandRun(cmd, NULL) < 0) 
        goto cleanup;

    if ((cur = strstr(outbuf, "Vendor ID")) == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("there is no \"Vendor ID\" info in %s command result"), LSCPU);
        goto cleanup;
    }
    cur = strchr(cur, ':') + 1;
    eol = strchr(cur, '\n');
    virSkipSpaces(&cur);
    if (!eol)
        goto cleanup;

    data->vendor_id = g_strndup(cur, eol - cur);

    if ((cur = strstr(outbuf, "Model name")) == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("there is no \"Model name\" info in %s command result"), LSCPU);
        goto cleanup;
    }
    cur = strchr(cur, ':') + 1;
    eol = strchr(cur, '\n');
    virSkipSpaces(&cur);
    if (!eol)
        goto cleanup;

    data->model_name = g_strndup(cur, eol - cur);

    if ((cur = strstr(outbuf, "Flags")) == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("there is no \"Flags\" info in %s command result"), LSCPU);
        goto cleanup;
    }
    cur = strchr(cur, ':') + 1;
    eol = strchr(cur, '\n');
    virSkipSpaces(&cur);
    if (!eol)
        goto cleanup;

    data->features = g_strndup(cur, eol - cur);

    ret = 0;

 cleanup:
    virCommandFree(cmd);
    VIR_FREE(outbuf);
    return ret;
}

static int
armCpuDataParseFeatures(virCPUDefPtr cpu,
                        const virCPUarmData *cpuData)
{
    int ret = -1;
    size_t i;
    char **features;

    if (!cpu || !cpuData)
        return ret;

    if (!(features = virStringSplitCount(cpuData->features, " ",
                                         0, &cpu->nfeatures)))
        return ret;
    if (cpu->nfeatures) {
        if (VIR_ALLOC_N(cpu->features, cpu->nfeatures) < 0)
            goto error;

        for (i = 0; i < cpu->nfeatures; i++) {
            cpu->features[i].policy = VIR_CPU_FEATURE_REQUIRE;
            cpu->features[i].name = g_strdup(features[i]);
        }
    }

    ret = 0;

 cleanup:
    virStringListFree(features);
    return ret;

 error:
    for (i = 0; i < cpu->nfeatures; i++)
        VIR_FREE(cpu->features[i].name);
    VIR_FREE(cpu->features);
    cpu->nfeatures = 0;
    goto cleanup;
}

static int
armDecode(virCPUDefPtr cpu,
          const virCPUarmData *cpuData,
          virDomainCapsCPUModelsPtr models)
{
    virCPUarmMapPtr map;
    virCPUarmModelPtr model;
    virCPUarmVendorPtr vendor = NULL;

    if (!cpuData || !(map = virCPUarmGetMap()))
        return -1;

    if (!(model = armModelFind(map, cpuData->model_name))) {
        virReportError(VIR_ERR_OPERATION_FAILED,
                       _("Cannot find CPU model with name %s"),
                       cpuData->model_name);
        return -1;
    }

    if (!virCPUModelIsAllowed(model->name, models)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("CPU model %s is not supported by hypervisor"),
                       model->name);
        return -1;
    }

    cpu->model = g_strdup(model->name);

    if (cpuData->vendor_id &&
        !(vendor = armVendorFindByName(map, cpuData->vendor_id))) {
        virReportError(VIR_ERR_OPERATION_FAILED,
                       _("Cannot find CPU vendor with vendor id %s"),
                       cpuData->vendor_id);
        return -1;
    }


    if (vendor)
        cpu->vendor = g_strdup(vendor->name);

    if (cpuData->features &&
        armCpuDataParseFeatures(cpu, cpuData) < 0)
        return -1;

    return 0;
}

static int
armDecodeCPUData(virCPUDefPtr cpu,
                 const virCPUData *data,
                 virDomainCapsCPUModelsPtr models)
{
    return armDecode(cpu, &data->data.arm, models);
}

static int
virCPUarmGetHost(virCPUDefPtr cpu,
                 virDomainCapsCPUModelsPtr models)
{
    virCPUDataPtr cpuData = NULL;
    int ret = -1;

    if (virCPUarmDriverInitialize() < 0)
        goto cleanup;

    if (!(cpuData = virCPUDataNew(archs[0])))
        goto cleanup;

    if (armCpuDataFromLsCpu(&cpuData->data.arm) < 0)
        goto cleanup;

    ret = armDecodeCPUData(cpu, cpuData, models);

 cleanup:
    virCPUarmDataFree(cpuData);
    return ret;
}

struct cpuArchDriver cpuDriverArm = {
    .name = "arm",
    .arch = archs,
    .narch = G_N_ELEMENTS(archs),
    .compare = virCPUarmCompare,
    .decode = armDecodeCPUData,
    .encode = NULL,
    .dataFree = virCPUarmDataFree,
    .getHost = virCPUarmGetHost,
    .baseline = virCPUarmBaseline,
    .update = virCPUarmUpdate,
    .validateFeatures = virCPUarmValidateFeatures,
};
