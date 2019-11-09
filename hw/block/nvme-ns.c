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

static void nvme_ns_init(NvmeNamespace *ns)
{
    NvmeIdNs *id_ns = &ns->id_ns;

    id_ns->lbaf[0].ds = ns->params.lbads;

    id_ns->nsze = cpu_to_le64(nvme_ns_nlbas(ns));

    /* no thin provisioning */
    id_ns->ncap = id_ns->nsze;
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

static int nvme_ns_init_blk(NvmeCtrl *n, NvmeNamespace *ns, NvmeIdCtrl *id,
                            Error **errp)
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

static int nvme_ns_check_constraints(NvmeNamespace *ns, Error **errp)
{
    if (!ns->blk) {
        error_setg(errp, "block backend not configured");
        return -1;
    }

    if (ns->params.lbads < 9 || ns->params.lbads > 12) {
        error_setg(errp, "unsupported lbads (supported: 9-12)");
        return -1;
    }

    return 0;
}

int nvme_ns_setup(NvmeCtrl *n, NvmeNamespace *ns, Error **errp)
{
    if (nvme_ns_check_constraints(ns, errp)) {
        return -1;
    }

    if (nvme_ns_init_blk(n, ns, &n->id_ctrl, errp)) {
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
        ns->id_ns.nsfeat |= 0x4;
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
