#ifndef HW_NVME_H
#define HW_NVME_H

#include "block/nvme.h"
#include "nvme-ns.h"

#define NVME_MAX_NAMESPACES 256

typedef struct NvmeParams {
    bool     defensive;
    char     *serial;
    uint32_t num_queues; /* deprecated since 5.1 */
    uint32_t max_ioqpairs;
    uint16_t msix_qsize;
    uint32_t cmb_size_mb;
    uint8_t  aerl;
    uint32_t aer_max_queued;
    uint8_t  mdts;
    bool     use_intel_id;
} NvmeParams;

static const NvmeEffectsLog nvme_effects[] = {
    [NVME_IOCS_NVM] = {
        .acs = {
            [NVME_ADM_CMD_DELETE_SQ]    = NVME_EFFECTS_CSUPP,
            [NVME_ADM_CMD_CREATE_SQ]    = NVME_EFFECTS_CSUPP,
            [NVME_ADM_CMD_GET_LOG_PAGE] = NVME_EFFECTS_CSUPP,
            [NVME_ADM_CMD_DELETE_CQ]    = NVME_EFFECTS_CSUPP,
            [NVME_ADM_CMD_CREATE_CQ]    = NVME_EFFECTS_CSUPP,
            [NVME_ADM_CMD_IDENTIFY]     = NVME_EFFECTS_CSUPP,
            [NVME_ADM_CMD_ABORT]        = NVME_EFFECTS_CSUPP,
            [NVME_ADM_CMD_SET_FEATURES] = NVME_EFFECTS_CSUPP |
                NVME_EFFECTS_CCC | NVME_EFFECTS_NIC | NVME_EFFECTS_NCC,
            [NVME_ADM_CMD_GET_FEATURES] = NVME_EFFECTS_CSUPP,
            [NVME_ADM_CMD_FORMAT_NVM]   = NVME_EFFECTS_CSUPP |
                NVME_EFFECTS_LBCC | NVME_EFFECTS_NCC | NVME_EFFECTS_NIC |
                NVME_EFFECTS_CSE_MULTI,
            [NVME_ADM_CMD_ASYNC_EV_REQ] = NVME_EFFECTS_CSUPP,
        },

        .iocs = {
            [NVME_CMD_FLUSH]            = NVME_EFFECTS_CSUPP,
            [NVME_CMD_WRITE]            = NVME_EFFECTS_CSUPP |
                NVME_EFFECTS_LBCC,
            [NVME_CMD_READ]             = NVME_EFFECTS_CSUPP,
            [NVME_CMD_WRITE_ZEROES]     = NVME_EFFECTS_CSUPP |
                NVME_EFFECTS_LBCC,
        },
    },

    [NVME_IOCS_ZONED] = {
        .iocs = {
            [NVME_CMD_ZONE_MGMT_RECV]   = NVME_EFFECTS_CSUPP,
        }
    },
};

typedef struct NvmeAsyncEvent {
    QTAILQ_ENTRY(NvmeAsyncEvent) entry;
    NvmeAerResult result;
} NvmeAsyncEvent;

typedef struct NvmeRequest NvmeRequest;
typedef void NvmeRequestCompletionFunc(NvmeRequest *req, void *opaque);

struct NvmeRequest {
    struct NvmeSQueue    *sq;
    struct NvmeNamespace *ns;

    NvmeCqe  cqe;
    NvmeCmd  cmd;
    uint16_t status;

    uint64_t slba;
    uint32_t nlb;

    QEMUSGList   qsg;
    QEMUIOVector iov;

    NvmeRequestCompletionFunc *cb;
    void                      *cb_arg;

    QTAILQ_HEAD(, NvmeAIO)    aio_tailq;
    QTAILQ_ENTRY(NvmeRequest) entry;
};

static inline void nvme_req_set_cb(NvmeRequest *req,
                                   NvmeRequestCompletionFunc *cb, void *cb_arg)
{
    req->cb = cb;
    req->cb_arg = cb_arg;
}

typedef struct NvmeSQueue {
    struct NvmeCtrl *ctrl;
    uint16_t    sqid;
    uint16_t    cqid;
    uint32_t    head;
    uint32_t    tail;
    uint32_t    size;
    uint64_t    dma_addr;
    QEMUTimer   *timer;
    NvmeRequest *io_req;
    QTAILQ_HEAD(, NvmeRequest) req_list;
    QTAILQ_HEAD(, NvmeRequest) out_req_list;
    QTAILQ_ENTRY(NvmeSQueue) entry;
} NvmeSQueue;

typedef struct NvmeCQueue {
    struct NvmeCtrl *ctrl;
    uint8_t     phase;
    uint16_t    cqid;
    uint16_t    irq_enabled;
    uint32_t    head;
    uint32_t    tail;
    uint32_t    vector;
    uint32_t    size;
    uint64_t    dma_addr;
    QEMUTimer   *timer;
    QTAILQ_HEAD(, NvmeSQueue) sq_list;
    QTAILQ_HEAD(, NvmeRequest) req_list;
} NvmeCQueue;

typedef enum NvmeAIOOp {
    NVME_AIO_OPC_NONE         = 0x0,
    NVME_AIO_OPC_FLUSH        = 0x1,
    NVME_AIO_OPC_READ         = 0x2,
    NVME_AIO_OPC_WRITE        = 0x3,
    NVME_AIO_OPC_WRITE_ZEROES = 0x4,
} NvmeAIOOp;

typedef enum NvmeAIOFlags {
    NVME_AIO_DMA      = 1 << 0,
    NVME_AIO_INTERNAL = 1 << 1,
} NvmeAIOFlags;

typedef struct NvmeAIO NvmeAIO;
typedef void NvmeAIOCompletionFunc(NvmeAIO *aio, void *opaque, int ret);

struct NvmeAIO {
    NvmeRequest *req;

    NvmeAIOOp       opc;
    int64_t         offset;
    size_t          len;
    BlockBackend    *blk;
    BlockAIOCB      *aiocb;
    BlockAcctCookie acct;

    NvmeAIOCompletionFunc *cb;
    void                  *cb_arg;

    int flags;
    void *payload;

    QTAILQ_ENTRY(NvmeAIO) tailq_entry;
};

static inline const char *nvme_aio_opc_str(NvmeAIO *aio)
{
    switch (aio->opc) {
    case NVME_AIO_OPC_NONE:         return "NVME_AIO_OP_NONE";
    case NVME_AIO_OPC_FLUSH:        return "NVME_AIO_OP_FLUSH";
    case NVME_AIO_OPC_READ:         return "NVME_AIO_OP_READ";
    case NVME_AIO_OPC_WRITE:        return "NVME_AIO_OP_WRITE";
    case NVME_AIO_OPC_WRITE_ZEROES: return "NVME_AIO_OP_WRITE_ZEROES";
    default:                        return "NVME_AIO_OP_UNKNOWN";
    }
}

static inline bool nvme_req_is_write(NvmeRequest *req)
{
    switch (req->cmd.opcode) {
    case NVME_CMD_WRITE:
    case NVME_CMD_WRITE_ZEROES:
        return true;
    default:
        return false;
    }
}

static inline bool nvme_req_is_dma(NvmeRequest *req)
{
    return req->qsg.sg != NULL;
}

#define TYPE_NVME_BUS "nvme-bus"
#define NVME_BUS(obj) OBJECT_CHECK(NvmeBus, (obj), TYPE_NVME_BUS)

typedef struct NvmeBus {
    BusState parent_bus;
} NvmeBus;

#define TYPE_NVME "nvme"
#define NVME(obj) \
        OBJECT_CHECK(NvmeCtrl, (obj), TYPE_NVME)

typedef struct NvmeFeatureVal {
    union {
        struct {
            uint16_t temp_thresh_hi;
            uint16_t temp_thresh_low;
        };
        uint32_t temp_thresh;
    };
    uint32_t    async_config;
    uint32_t    vwc;
    uint32_t    iocsci;
} NvmeFeatureVal;

static const uint32_t nvme_feature_cap[0x100] = {
    [NVME_TEMPERATURE_THRESHOLD]    = NVME_FEAT_CAP_CHANGE,
    [NVME_ERROR_RECOVERY]           = NVME_FEAT_CAP_CHANGE | NVME_FEAT_CAP_NS,
    [NVME_VOLATILE_WRITE_CACHE]     = NVME_FEAT_CAP_CHANGE,
    [NVME_NUMBER_OF_QUEUES]         = NVME_FEAT_CAP_CHANGE,
    [NVME_ASYNCHRONOUS_EVENT_CONF]  = NVME_FEAT_CAP_CHANGE,
    [NVME_TIMESTAMP]                = NVME_FEAT_CAP_CHANGE,
    [NVME_COMMAND_SET_PROFILE]      = NVME_FEAT_CAP_CHANGE,
};

static const uint32_t nvme_feature_default[0x100] = {
    [NVME_ARBITRATION]           = NVME_ARB_AB_NOLIMIT,
};

static const bool nvme_feature_support[0x100] = {
    [NVME_ARBITRATION]              = true,
    [NVME_POWER_MANAGEMENT]         = true,
    [NVME_TEMPERATURE_THRESHOLD]    = true,
    [NVME_ERROR_RECOVERY]           = true,
    [NVME_VOLATILE_WRITE_CACHE]     = true,
    [NVME_NUMBER_OF_QUEUES]         = true,
    [NVME_INTERRUPT_COALESCING]     = true,
    [NVME_INTERRUPT_VECTOR_CONF]    = true,
    [NVME_WRITE_ATOMICITY]          = true,
    [NVME_ASYNCHRONOUS_EVENT_CONF]  = true,
    [NVME_TIMESTAMP]                = true,
    [NVME_COMMAND_SET_PROFILE]      = true,
};

typedef struct NvmeCtrl {
    PCIDevice    parent_obj;
    MemoryRegion iomem;
    MemoryRegion ctrl_mem;
    NvmeBar      bar;
    NvmeParams   params;
    NvmeBus      bus;
    BlockConf    conf;

    bool        qs_created;
    uint32_t    page_size;
    uint16_t    page_bits;
    uint16_t    max_prp_ents;
    uint16_t    cqe_size;
    uint16_t    sqe_size;
    uint32_t    reg_size;
    uint32_t    num_namespaces;
    uint32_t    max_q_ents;
    uint8_t     outstanding_aers;
    uint8_t     *cmbuf;
    uint32_t    irq_status;
    uint64_t    host_timestamp;                 /* Timestamp sent by the host */
    uint64_t    timestamp_set_qemu_clock_ms;    /* QEMU clock time */
    uint64_t    starttime_ms;
    uint16_t    temperature;
    uint64_t    iocscs[512];

    HostMemoryBackend *pmrdev;

    uint8_t     aer_mask;
    NvmeRequest **aer_reqs;
    QTAILQ_HEAD(, NvmeAsyncEvent) aer_queue;
    int         aer_queued;

    NvmeNamespace   namespace;
    NvmeNamespace   *namespaces[NVME_MAX_NAMESPACES];
    NvmeSQueue      **sq;
    NvmeCQueue      **cq;
    NvmeSQueue      admin_sq;
    NvmeCQueue      admin_cq;
    NvmeIdCtrl      id_ctrl;
    void            *id_ctrl_iocss[256];
    NvmeFeatureVal  features;
} NvmeCtrl;

static inline NvmeNamespace *nvme_ns(NvmeCtrl *n, uint32_t nsid)
{
    if (!nsid || nsid > n->num_namespaces) {
        return NULL;
    }

    return n->namespaces[nsid - 1];
}

static inline uint16_t nvme_cid(NvmeRequest *req)
{
    if (req) {
        return le16_to_cpu(req->cqe.cid);
    }

    return 0xffff;
}

static inline uint16_t nvme_sqid(NvmeRequest *req)
{
    return le16_to_cpu(req->sq->sqid);
}

int nvme_register_namespace(NvmeCtrl *n, NvmeNamespace *ns, Error **errp);

#endif /* HW_NVME_H */
