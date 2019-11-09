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

#ifndef NVME_NS_H
#define NVME_NS_H

#define TYPE_NVME_NS "nvme-ns"
#define NVME_NS(obj) \
    OBJECT_CHECK(NvmeNamespace, (obj), TYPE_NVME_NS)

typedef struct NvmeNamespaceParams {
    uint32_t nsid;
    uint8_t  lbads;
} NvmeNamespaceParams;

typedef struct NvmeNamespace {
    DeviceState  parent_obj;
    BlockBackend *blk;
    BlockBackend *blk_state;
    int32_t      bootindex;
    int64_t      size;

    NvmeIdNs            id_ns;
    NvmeNamespaceParams params;

    unsigned long *utilization;

    struct {
        uint32_t err_rec;
    } features;
} NvmeNamespace;

static inline uint32_t nvme_nsid(NvmeNamespace *ns)
{
    if (ns) {
        return ns->params.nsid;
    }

    return -1;
}

static inline NvmeLBAF *nvme_ns_lbaf(NvmeNamespace *ns)
{
    NvmeIdNs *id_ns = &ns->id_ns;
    return &id_ns->lbaf[NVME_ID_NS_FLBAS_INDEX(id_ns->flbas)];
}

static inline uint8_t nvme_ns_lbads(NvmeNamespace *ns)
{
    return nvme_ns_lbaf(ns)->ds;
}

/* calculate the number of LBAs that the namespace can accomodate */
static inline uint64_t nvme_ns_nlbas(NvmeNamespace *ns)
{
    return ns->size >> nvme_ns_lbads(ns);
}

static inline size_t nvme_ns_blk_state_len(NvmeNamespace *ns)
{
    return ROUND_UP(DIV_ROUND_UP(nvme_ns_nlbas(ns), 8), BDRV_SECTOR_SIZE);
}

typedef struct NvmeCtrl NvmeCtrl;

int nvme_ns_setup(NvmeCtrl *n, NvmeNamespace *ns, Error **errp);

#endif /* NVME_NS_H */
