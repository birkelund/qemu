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
    uint8_t  iocs;
    uint8_t  lbads;

    struct {
        uint64_t zcap;
        uint8_t  zdes;
        uint16_t zoc;
        uint16_t ozcs;
        uint32_t mar;
        uint32_t mor;
        uint32_t rrl;
        uint32_t frl;
        uint32_t rrld;
        uint32_t frld;
    } zns;
} NvmeNamespaceParams;

typedef struct NvmeZone {
    NvmeZoneDescriptor  zd;
    uint8_t             *zde;

    uint64_t wp_staging;

    struct {
        int64_t activated_ns;
        int64_t finished_ns;
    } stats;

    QTAILQ_ENTRY(NvmeZone) lru_entry;
} NvmeZone;

typedef struct NvmeNamespace {
    DeviceState  parent_obj;
    BlockBackend *blk;
    BlockBackend *blk_state;
    int32_t      bootindex;
    int64_t      size;
    uint8_t      iocs;

    void         *id_ns[256];
    NvmeNamespaceParams params;

    unsigned long *utilization;

    struct {
        uint32_t err_rec;
    } features;

    struct {
        struct {
            BlockBackend *blk;

            uint64_t  num_zones;
            NvmeZone *zones;
        } info;

        struct {
            uint32_t open;
            uint32_t active;

            QTAILQ_HEAD(, NvmeZone) lru_open;
            QTAILQ_HEAD(, NvmeZone) lru_active;
        } resources;

        NvmeChangedZoneList changed_list;

        QTAILQ_HEAD(, NvmeZone) lru_finished;
        QEMUTimer *timer;
        int64_t rrl_ns, rrld_ns, frl_ns, frld_ns;
    } zns;
} NvmeNamespace;

static inline bool nvme_ns_zoned(NvmeNamespace *ns)
{
    return ns->iocs == NVME_IOCS_ZONED;
}

static inline uint32_t nvme_nsid(NvmeNamespace *ns)
{
    if (ns) {
        return ns->params.nsid;
    }

    return -1;
}

static inline NvmeIdNsNvm *nvme_ns_id_nvm(NvmeNamespace *ns)
{
    return ns->id_ns[NVME_IOCS_NVM];
}

static inline NvmeIdNsZns *nvme_ns_id_zoned(NvmeNamespace *ns)
{
    return ns->id_ns[NVME_IOCS_ZONED];
}

static inline NvmeLBAF *nvme_ns_lbaf(NvmeNamespace *ns)
{
    NvmeIdNsNvm *id_ns = nvme_ns_id_nvm(ns);
    return &id_ns->lbaf[NVME_ID_NS_FLBAS_INDEX(id_ns->flbas)];
}

static inline NvmeLBAFE *nvme_ns_lbafe(NvmeNamespace *ns)
{
    NvmeIdNsNvm *id_ns = nvme_ns_id_nvm(ns);
    NvmeIdNsZns *id_ns_zns = nvme_ns_id_zoned(ns);
    return &id_ns_zns->lbafe[NVME_ID_NS_FLBAS_INDEX(id_ns->flbas)];
}

static inline uint8_t nvme_ns_lbads(NvmeNamespace *ns)
{
    return nvme_ns_lbaf(ns)->ds;
}

static inline uint64_t nvme_ns_zsze(NvmeNamespace *ns)
{
    return nvme_ns_lbafe(ns)->zsze;
}

static inline uint64_t nvme_ns_zsze_bytes(NvmeNamespace *ns)
{
    return nvme_ns_zsze(ns) << nvme_ns_lbads(ns);
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

static inline uint64_t nvme_ns_zone_idx(NvmeNamespace *ns, uint64_t lba)
{
    return lba / nvme_ns_zsze(ns);
}

static inline NvmeZone *nvme_ns_get_zone(NvmeNamespace *ns, uint64_t lba)
{
    uint64_t idx = nvme_ns_zone_idx(ns, lba);
    if (unlikely(idx >= ns->zns.info.num_zones)) {
        return NULL;
    }

    return &ns->zns.info.zones[idx];
}

static inline NvmeZoneState nvme_zs(NvmeZone *zone)
{
    return (zone->zd.zs >> 4) & 0xf;
}

static inline void nvme_zs_set(NvmeZone *zone, NvmeZoneState zs)
{
    zone->zd.zs = zs << 4;
}

static inline size_t nvme_ns_zdes_bytes(NvmeNamespace *ns)
{
    return ns->params.zns.zdes << 6;
}

static inline bool nvme_ns_zone_wp_valid(NvmeZone *zone)
{
    switch (nvme_zs(zone)) {
    case NVME_ZS_ZSF:
    case NVME_ZS_ZSRO:
    case NVME_ZS_ZSO:
        return false;
    default:
        return false;
    }
}

static inline uint64_t nvme_zslba(NvmeZone *zone)
{
    return le64_to_cpu(zone->zd.zslba);
}

static inline uint64_t nvme_zcap(NvmeZone *zone)
{
    return le64_to_cpu(zone->zd.zcap);
}

static inline uint64_t nvme_wp(NvmeZone *zone)
{
    return le64_to_cpu(zone->zd.wp);
}

typedef struct NvmeCtrl NvmeCtrl;

const char *nvme_zs_str(NvmeZone *zone);
const char *nvme_zs_to_str(NvmeZoneState zs);

int nvme_ns_setup(NvmeCtrl *n, NvmeNamespace *ns, Error **errp);

#endif /* NVME_NS_H */
