/*
 * QEMU NVM Express Virtual Namespace
 *
 * Copyright (c) 2019 CNEX Labs
 * Copyright (c) 2020 Samsung Electronics
 *
 * Authors:
 *  Klaus Jensen      <k.jensen@samsung.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2. See the
 * COPYING file in the top-level directory.
 *
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/cutils.h"
#include "qemu/log.h"
#include "hw/block/block.h"
#include "hw/pci/pci.h"
#include "sysemu/sysemu.h"
#include "sysemu/block-backend.h"
#include "qapi/error.h"

#include "hw/qdev-properties.h"
#include "hw/qdev-core.h"

#include "nvme.h"
#include "nvme-ns.h"

const char *nvme_zs_str(NvmeZone *zone)
{
    return nvme_zs_to_str(nvme_zs(zone));
}

const char *nvme_zs_to_str(NvmeZoneState zs)
{
    switch (zs) {
    case NVME_ZS_ZSE:  return "ZSE";
    case NVME_ZS_ZSIO: return "ZSIO";
    case NVME_ZS_ZSEO: return "ZSEO";
    case NVME_ZS_ZSC:  return "ZSC";
    case NVME_ZS_ZSRO: return "ZSRO";
    case NVME_ZS_ZSF:  return "ZSF";
    case NVME_ZS_ZSO:  return "ZSO";
    }

    return NULL;
}

static int nvme_ns_blk_resize(BlockBackend *blk, size_t len, Error **errp)
{
	Error *local_err = NULL;
	int ret;
	uint64_t perm, shared_perm;

	blk_get_perm(blk, &perm, &shared_perm);

	ret = blk_set_perm(blk, perm | BLK_PERM_RESIZE, shared_perm, &local_err);
	if (ret < 0) {
		error_propagate_prepend(errp, local_err, "blk_set_perm: ");
		return ret;
	}

	ret = blk_truncate(blk, len, false, PREALLOC_MODE_OFF, 0, &local_err);
	if (ret < 0) {
		error_propagate_prepend(errp, local_err, "blk_truncate: ");
		return ret;
	}

	ret = blk_set_perm(blk, perm, shared_perm, &local_err);
	if (ret < 0) {
		error_propagate_prepend(errp, local_err, "blk_set_perm: ");
		return ret;
	}

	return 0;
}

static int nvme_ns_init_blk_zoneinfo(NvmeNamespace *ns, size_t len,
                                     Error **errp)
{
    NvmeZone *zone;
    NvmeZoneDescriptor *zd;
    uint64_t zslba;
    int ret;

    BlockBackend *blk = ns->zns.info.blk;

    Error *local_err = NULL;

    for (int i = 0; i < ns->zns.info.num_zones; i++) {
        zslba = i * nvme_ns_zsze(ns);
        zone = nvme_ns_get_zone(ns, zslba);
        zd = &zone->zd;

        zd->zt = NVME_ZT_SEQ;
        nvme_zs_set(zone, NVME_ZS_ZSE);
        zd->zcap = ns->params.zns.zcap;
        zone->wp_staging = zslba;
        zd->wp = zd->zslba = cpu_to_le64(zslba);
    }

    ret = nvme_ns_blk_resize(blk, len, &local_err);
    if (ret) {
        error_propagate_prepend(errp, local_err,
                                "could not resize zoneinfo blockdev: ");
        return ret;
    }

    for (int i = 0; i < ns->zns.info.num_zones; i++) {
        zd = &ns->zns.info.zones[i].zd;

        ret = blk_pwrite(blk, i * sizeof(NvmeZoneDescriptor), zd,
                         sizeof(NvmeZoneDescriptor), 0);
        if (ret < 0) {
            error_setg_errno(errp, -ret, "blk_pwrite: ");
            return ret;
        }
    }

    return 0;
}

static int nvme_ns_setup_blk_zoneinfo(NvmeNamespace *ns, Error **errp)
{
    NvmeZone *zone;
    NvmeZoneDescriptor *zd;
    BlockBackend *blk = ns->zns.info.blk;
    uint64_t perm, shared_perm;
    int64_t len, zoneinfo_len;

    Error *local_err = NULL;
    int ret;

    perm = BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE;
    shared_perm = BLK_PERM_ALL;

    ret = blk_set_perm(blk, perm, shared_perm, &local_err);
    if (ret) {
        error_propagate_prepend(errp, local_err, "blk_set_perm: ");
        return ret;
    }

    zoneinfo_len = ROUND_UP(ns->zns.info.num_zones *
                            sizeof(NvmeZoneDescriptor), BDRV_SECTOR_SIZE);

    len = blk_getlength(blk);
    if (len < 0) {
        error_setg_errno(errp, -len, "blk_getlength: ");
        return len;
    }

    if (len) {
        if (len != zoneinfo_len) {
            error_setg(errp, "zoneinfo size mismatch "
                       "(expected %"PRIu64" bytes; was %"PRIu64" bytes)",
                       zoneinfo_len, len);
            error_append_hint(errp, "Did you change the zone size or "
                              "zone descriptor size?\n");
            return -1;
        }

        for (int i = 0; i < ns->zns.info.num_zones; i++) {
            zone = &ns->zns.info.zones[i];
            zd = &zone->zd;

            ret = blk_pread(blk, i * sizeof(NvmeZoneDescriptor), zd,
                            sizeof(NvmeZoneDescriptor));
            if (ret < 0) {
                error_setg_errno(errp, -ret, "blk_pread: ");
                return ret;
            } else if (ret != sizeof(NvmeZoneDescriptor)) {
                error_setg(errp, "blk_pread: short read");
                return -1;
            }

            zone->wp_staging = nvme_wp(zone);

            switch (nvme_zs(zone)) {
            case NVME_ZS_ZSE:
            case NVME_ZS_ZSF:
            case NVME_ZS_ZSRO:
            case NVME_ZS_ZSO:
                continue;

            case NVME_ZS_ZSC:
                if (nvme_wp(zone) == nvme_zslba(zone)) {
                    nvme_zs_set(zone, NVME_ZS_ZSE);
                    continue;
                }

                /* fallthrough */

            case NVME_ZS_ZSIO:
            case NVME_ZS_ZSEO:
                nvme_zs_set(zone, NVME_ZS_ZSF);
                NVME_ZA_SET_ZFC(zd->za, 0x1);
            }
        }

        for (int i = 0; i < ns->zns.info.num_zones; i++) {
            zd = &ns->zns.info.zones[i].zd;

            ret = blk_pwrite(blk, i * sizeof(NvmeZoneDescriptor), zd,
                             sizeof(NvmeZoneDescriptor), 0);
            if (ret < 0) {
                error_setg_errno(errp, -ret, "blk_pwrite: ");
                return ret;
            }
        }

        return 0;
    }

    if (nvme_ns_init_blk_zoneinfo(ns, zoneinfo_len, &local_err)) {
        error_propagate_prepend(errp, local_err,
                                "could not initialize zoneinfo blockdev: ");
    }

    return 0;
}

static void nvme_ns_init_zoned(NvmeNamespace *ns)
{
    NvmeIdNsNvm *id_ns = nvme_ns_id_nvm(ns);
    NvmeIdNsZns *id_ns_zns = nvme_ns_id_zoned(ns);

    id_ns_zns->zoc = cpu_to_le16(ns->params.zns.zoc);
    id_ns_zns->ozcs = cpu_to_le16(ns->params.zns.ozcs);

    for (int i = 0; i <= id_ns->nlbaf; i++) {
        id_ns_zns->lbafe[i].zsze = cpu_to_le64(pow2ceil(ns->params.zns.zcap));
    }

    ns->zns.info.num_zones = nvme_ns_nlbas(ns) / nvme_ns_zsze(ns);
    ns->zns.info.zones = g_malloc0_n(ns->zns.info.num_zones, sizeof(NvmeZone));

    id_ns->ncap = ns->zns.info.num_zones * ns->params.zns.zcap;

    id_ns_zns->mar = 0xffffffff;
    id_ns_zns->mor = 0xffffffff;
}

static void nvme_ns_init(NvmeNamespace *ns)
{
    NvmeIdNsNvm *id_ns;

    int unmap = blk_get_flags(ns->blk) & BDRV_O_UNMAP;

    ns->id_ns[NVME_IOCS_NVM] = g_new0(NvmeIdNsNvm, 1);
    id_ns = nvme_ns_id_nvm(ns);

    ns->iocs = ns->params.iocs;

    id_ns->dlfeat = unmap ? 0x9 : 0x0;
    if (!nvme_ns_zoned(ns)) {
        id_ns->dlfeat = unmap ? 0x9 : 0x0;
    }
    id_ns->lbaf[0].ds = ns->params.lbads;

    id_ns->nsze = cpu_to_le64(nvme_ns_nlbas(ns));
    id_ns->ncap = id_ns->nsze;

    if (ns->iocs == NVME_IOCS_ZONED) {
        ns->id_ns[NVME_IOCS_ZONED] = g_new0(NvmeIdNsZns, 1);
        nvme_ns_init_zoned(ns);
    }

    /* no thin provisioning */
    id_ns->nuse = id_ns->ncap;
}

static int nvme_ns_init_blk_state(NvmeNamespace *ns, Error **errp)
{
    BlockBackend *blk = ns->blk_state;
    uint64_t perm, shared_perm;
    int64_t len, state_len;

    Error *local_err = NULL;
    int ret;

    perm = BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE;
    shared_perm = BLK_PERM_ALL;

    ns->utilization = bitmap_new(nvme_ns_nlbas(ns));

    ret = blk_set_perm(blk, perm, shared_perm, &local_err);
    if (ret) {
        error_propagate_prepend(errp, local_err, "blk_set_perm: ");
        return ret;
    }

    state_len = nvme_ns_blk_state_len(ns);

    len = blk_getlength(blk);
    if (len < 0) {
        error_setg_errno(errp, -len, "blk_getlength: ");
        return len;
    }

    if (len) {
        if (len != state_len) {
            error_setg(errp, "state size mismatch "
                "(expected %"PRIu64" bytes; was %"PRIu64" bytes)",
                state_len, len);
            error_append_hint(errp,
                "Did you change the 'lbads' parameter? "
                "Or re-formatted the namespace using Format NVM?\n");
            return -1;
        }

        ret = blk_pread(blk, 0, ns->utilization, state_len);
        if (ret < 0) {
            error_setg_errno(errp, -ret, "blk_pread: ");
            return ret;
        } else if (ret != state_len) {
            error_setg(errp, "blk_pread: short read");
            return -1;
        }

        return 0;
    }

    ret = nvme_ns_blk_resize(blk, state_len, &local_err);
    if (ret < 0) {
        error_propagate_prepend(errp, local_err, "nvme_ns_blk_resize: ");
        return ret;
    }

    return 0;
}

static int nvme_ns_init_blk(NvmeCtrl *n, NvmeNamespace *ns, Error **errp)
{
    uint64_t perm, shared_perm;

    Error *local_err = NULL;
    int ret;

    perm = BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE;
    shared_perm = BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE_UNCHANGED |
        BLK_PERM_GRAPH_MOD;

    ret = blk_set_perm(ns->blk, perm, shared_perm, &local_err);
    if (ret) {
        error_propagate_prepend(errp, local_err,
                                "could not set block permissions: ");
        return ret;
    }

    ns->size = blk_getlength(ns->blk);
    if (ns->size < 0) {
        error_setg_errno(errp, -ns->size, "could not get blockdev size");
        return -1;
    }

    switch (n->conf.wce) {
    case ON_OFF_AUTO_ON:
        n->features.vwc = 1;
        break;
    case ON_OFF_AUTO_OFF:
        n->features.vwc = 0;
        break;
    case ON_OFF_AUTO_AUTO:
        n->features.vwc = blk_enable_write_cache(ns->blk);
        break;
    default:
        abort();
    }

    blk_set_enable_write_cache(ns->blk, n->features.vwc);

    return 0;
}

static int nvme_ns_check_constraints(NvmeCtrl *n, NvmeNamespace *ns, Error
                                     **errp)
{
    if (!ns->blk) {
        error_setg(errp, "block backend not configured");
        return -1;
    }

    if (ns->params.lbads < 9 || ns->params.lbads > 12) {
        error_setg(errp, "unsupported lbads (supported: 9-12)");
        return -1;
    }

    switch (ns->params.iocs) {
    case NVME_IOCS_NVM:
        break;

    case NVME_IOCS_ZONED:
        if (!ns->zns.info.blk) {
            error_setg(errp, "zone info block backend not configured");
            return -1;
        }

        if (!ns->params.zns.zcap) {
            error_setg(errp, "zero zone capacity");
            return -1;
        }

        break;

    default:
        error_setg(errp, "unsupported I/O command set");
        return -1;
    }

    return 0;
}

int nvme_ns_setup(NvmeCtrl *n, NvmeNamespace *ns, Error **errp)
{
    if (nvme_ns_check_constraints(n, ns, errp)) {
        return -1;
    }

    if (nvme_ns_init_blk(n, ns, errp)) {
        return -1;
    }

    nvme_ns_init(ns);

    if (ns->blk_state) {
        if (nvme_ns_init_blk_state(ns, errp)) {
            return -1;
        }

        /*
         * With a state file in place we can enable the Deallocated or
         * Unwritten Logical Block Error feature.
         */
        NvmeIdNsNvm *id_ns = nvme_ns_id_nvm(ns);
        id_ns->nsfeat |= 0x4;
    }

    if (nvme_ns_zoned(ns)) {
        if (nvme_ns_setup_blk_zoneinfo(ns, errp)) {
            return -1;
        }
    }

    if (nvme_register_namespace(n, ns, errp)) {
        return -1;
    }

    return 0;
}

static void nvme_ns_realize(DeviceState *dev, Error **errp)
{
    NvmeNamespace *ns = NVME_NS(dev);
    BusState *s = qdev_get_parent_bus(dev);
    NvmeCtrl *n = NVME(s->parent);
    Error *local_err = NULL;

    if (nvme_ns_setup(n, ns, &local_err)) {
        error_propagate_prepend(errp, local_err,
                                "could not setup namespace: ");
        return;
    }
}

static Property nvme_ns_props[] = {
    DEFINE_PROP_DRIVE("drive", NvmeNamespace, blk),
    DEFINE_PROP_UINT32("nsid", NvmeNamespace, params.nsid, 0),
    DEFINE_PROP_UINT8("lbads", NvmeNamespace, params.lbads, BDRV_SECTOR_BITS),
    DEFINE_PROP_DRIVE("state", NvmeNamespace, blk_state),
    DEFINE_PROP_UINT8("iocs", NvmeNamespace, params.iocs, 0x0),
    DEFINE_PROP_DRIVE("zns.zoneinfo", NvmeNamespace, zns.info.blk),
    DEFINE_PROP_UINT64("zns.zcap", NvmeNamespace, params.zns.zcap, 0),
    DEFINE_PROP_UINT16("zns.zoc", NvmeNamespace, params.zns.zoc, 0),
    DEFINE_PROP_UINT16("zns.ozcs", NvmeNamespace, params.zns.ozcs, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static void nvme_ns_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);

    dc->bus_type = TYPE_NVME_BUS;
    dc->realize = nvme_ns_realize;
    device_class_set_props(dc, nvme_ns_props);
    dc->desc = "Virtual NVMe namespace";
}

static void nvme_ns_instance_init(Object *obj)
{
    NvmeNamespace *ns = NVME_NS(obj);
    char *bootindex = g_strdup_printf("/namespace@%d,0", ns->params.nsid);

    device_add_bootindex_property(obj, &ns->bootindex, "bootindex",
                                  bootindex, DEVICE(obj));

    g_free(bootindex);
}

static const TypeInfo nvme_ns_info = {
    .name = TYPE_NVME_NS,
    .parent = TYPE_DEVICE,
    .class_init = nvme_ns_class_init,
    .instance_size = sizeof(NvmeNamespace),
    .instance_init = nvme_ns_instance_init,
};

static void nvme_ns_register_types(void)
{
    type_register_static(&nvme_ns_info);
}

type_init(nvme_ns_register_types)
