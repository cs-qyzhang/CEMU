#include <assert.h>
#include <stdlib.h>
#include "./nvme.h"
#include "./statistics/statistics.h"
#include "hw/femu/csd/sched.h"
#include "hw/femu/inc/pqueue.h"

static int nvme_io_cmd(FemuCtrl *n, NvmeCmd *cmd, NvmeRequest *req);

static void nvme_update_sq_eventidx(const NvmeSQueue *sq)
{
    if (sq->eventidx_addr_hva) {
        *((uint32_t *)(sq->eventidx_addr_hva)) = sq->tail;
        return;
    }

    if (sq->eventidx_addr) {
        nvme_addr_write(sq->ctrl, sq->eventidx_addr, (void *)&sq->tail,
                        sizeof(sq->tail));
    }
}

static inline void nvme_copy_cmd(NvmeCmd *dst, NvmeCmd *src)
{
#if defined(__AVX__)
    __m256i *d256 = (__m256i *)dst;
    const __m256i *s256 = (const __m256i *)src;

    _mm256_store_si256(&d256[0], _mm256_load_si256(&s256[0]));
    _mm256_store_si256(&d256[1], _mm256_load_si256(&s256[1]));
#elif defined(__SSE2__)
    __m128i *d128 = (__m128i *)dst;
    const __m128i *s128 = (const __m128i *)src;

    _mm_store_si128(&d128[0], _mm_load_si128(&s128[0]));
    _mm_store_si128(&d128[1], _mm_load_si128(&s128[1]));
    _mm_store_si128(&d128[2], _mm_load_si128(&s128[2]));
    _mm_store_si128(&d128[3], _mm_load_si128(&s128[3]));
#else
    *dst = *src;
#endif
}

static void nvme_process_sq_io(void *opaque, int index_poller)
{
    NvmeSQueue *sq = opaque;
    FemuCtrl *n = sq->ctrl;

    int finished;
    hwaddr addr;
    NvmeCmd cmd;
    NvmeRequest *req;
    uint64_t *max_processed = &n->stat->max_processed;
    int processed = 0;

    nvme_update_sq_tail(sq);
    while (!(nvme_sq_empty(sq))) {
        if (processed > 32) {
            break;
        }
        if (sq->phys_contig) {
            addr = sq->dma_addr + sq->head * n->sqe_size;
            nvme_copy_cmd(&cmd, (void *)&(((NvmeCmd *)sq->dma_addr_hva)[sq->head]));
        } else {
            addr = nvme_discontig(sq->prp_list, sq->head, n->page_size,
                                  n->sqe_size);
            nvme_addr_read(n, addr, (void *)&cmd, sizeof(cmd));
        }

        pthread_spin_lock(&sq->lock);
        nvme_inc_sq_head(sq);
        req = QTAILQ_FIRST(&sq->req_list);
        QTAILQ_REMOVE(&sq->req_list, req, entry);
        pthread_spin_unlock(&sq->lock);
        memset(&req->cqe, 0, sizeof(req->cqe));
        req->cqe.cid = cmd.cid;
        req->cmd_opcode = cmd.opcode;
        memset(&req->stat, 0, sizeof(req->stat));
        /* Coperd: record req->stime at earliest convenience */
        req->stat.expire_time = req->stat.stime = clock_ns();
        req->stat.opcode = cmd.opcode;
        memcpy(&req->cmd, &cmd, sizeof(NvmeCmd));
        req->p2p_req = 0;

        if (n->print_log) {
            femu_debug("%s,cid:%d\n", __func__, cmd.cid);
        }

        uint32_t nsid = le32_to_cpu(cmd.nsid);
        if (nsid == 0 || nsid > n->num_namespaces) {
            femu_err("%s, NVME_INVALID_NSID %" PRIu32 "\n", __func__, nsid);
            req->status = NVME_INVALID_NSID | NVME_DNR;
            processed++;
            continue;
        }
        req->ns = &n->namespaces[nsid - 1];
        req->poller = n->multipoller_enabled ? index_poller : 1;

        finished = nvme_io_cmd(n, &cmd, req);
        if (finished && req->status != NVME_SUCCESS) {
            femu_err("Error IO processed! opcode %d\n", cmd.opcode);
        }
        if (finished) {
            femu_ring_enqueue(n->to_poller[req->poller], (void*)&req, 1);
        }

        processed++;
    }

    nvme_update_sq_eventidx(sq);
    sq->completed += processed;
    *max_processed = *max_processed > processed ? *max_processed : processed;
}

static void nvme_post_cqe(NvmeCQueue *cq, NvmeRequest *req)
{
    FemuCtrl *n = cq->ctrl;
    NvmeSQueue *sq = req->sq;
    NvmeCqe *cqe = &req->cqe;
    uint8_t phase = cq->phase;
    hwaddr addr;

    if (n->print_log) {
        femu_debug("%s,req,lba:%lu,lat:%u\n", n->devname, req->slba, req->stat.reqlat);
    }
    cqe->status = cpu_to_le16((req->status << 1) | phase);
    cqe->sq_id = cpu_to_le16(sq->sqid);
    pthread_spin_lock(&sq->lock);
    cqe->sq_head = cpu_to_le16(sq->head);
    pthread_spin_unlock(&sq->lock);

    if (cq->phys_contig) {
        addr = cq->dma_addr + cq->tail * n->cqe_size;
        ((NvmeCqe *)cq->dma_addr_hva)[cq->tail] = *cqe;
    } else {
        addr = nvme_discontig(cq->prp_list, cq->tail, n->page_size, n->cqe_size);
        nvme_addr_write(n, addr, (void *)cqe, sizeof(*cqe));
    }

    nvme_inc_cq_tail(cq);
}
typedef struct NvmeMemoryCopyCmd {
    uint8_t     opcode;
    uint8_t     flags;
    uint16_t    cid;
    uint32_t    nsid;
    uint64_t    len;            // length
    uint64_t    rsvd4;
    uint64_t    prp1;           // source range entries
    uint64_t    prp2;
    uint64_t    sdaddr;         // starting destination address
    uint32_t    nr     : 8;     // number of ranges
    uint32_t    cdft   : 4;     // copy descriptor format type
    uint32_t    rsvd12 : 20;
    uint32_t    rsvd13[3];
} NvmeMemoryCopyCmd;

static void nvme_process_cq_cpl(FemuCtrl *n, int index_poller)
{
    NvmeCQueue *cq = NULL;
    NvmeSQueue *sq = NULL;
    NvmeRequest *req = NULL;
    struct rte_ring *rp = n->to_poller[index_poller];
    pqueue_t *pq = n->pq[index_poller];
    uint64_t pcie_prop_delay = n->pcie_propagation_delay;
    double pcie_bw = n->pcie_bandwidth;
    uint64_t pcie_trans_delay = 0;
    uint64_t now;
    int processed = 0;
    int rc;
    int i;

    while (!femu_ring_empty(rp)) {
        req = NULL;
        rc = femu_ring_dequeue(rp, (void *)&req, 1);
        if (rc != 1) {
            femu_err("dequeue from to_poller request failed\n");
        }
        assert(req);

        pqueue_insert(pq, req);
    }

    while ((req = pqueue_peek(pq))) {
        if (req->stat.pcie_lat == 1) {
            // add pcie latency
            pcie_trans_delay = MAX(10, req->stat.data_size / pcie_bw);
            req->stat.pcie_lat = pcie_trans_delay + pcie_prop_delay;
            req->stat.reqlat += req->stat.pcie_lat;
            // TODO: multithread poller lock
            uint64_t *pcie_next_avail_time;
            if (req->is_write) {
                pcie_next_avail_time = &n->pcie_rx_next_avail_time;
            } else {
                pcie_next_avail_time = &n->pcie_tx_next_avail_time;
            }
            *pcie_next_avail_time = (*pcie_next_avail_time > req->stat.expire_time) ?
                                     *pcie_next_avail_time + pcie_trans_delay :
                                     req->stat.expire_time + pcie_trans_delay;
            req->stat.rw.pcie_queueing_lat = *pcie_next_avail_time - req->stat.expire_time;
            req->stat.expire_time = *pcie_next_avail_time + pcie_prop_delay;
            if (req->p2p_req) {
                // memory copy p2p request, the request is finished in another
                // drive, only add pcie latency here
                // TODO send stat
                // femu_debug("memory copy croos drive p2p request\n");
                femu_stat_add_req(n, req);
                pqueue_pop(pq);
                free(req);
            } else {
                pqueue_change_priority(pq, req->stat.expire_time, req);
            }
            continue;
        }

        now = clock_ns();
        if (now < req->stat.expire_time) {
            break;
        }
        if (req->cmd.opcode == NVME_CMD_CSD_EXEC) {
            // uint64_t context_switch_time = req->stat.reqlat -
            //         req->stat.exec.queueing_time - req->stat.exec.runtime;
            // femu_debug("exec finish: thread %u late %lu, runtime %u, "
            //            "queueing time %u, nr context switch %d, "
            //            "context switch time %lu\n",
            //            req->stat.exec.thread, now - req->stat.expire_time,
            //            req->stat.exec.runtime, req->stat.exec.queueing_time,
            //            req->stat.exec.nr_context_switch, context_switch_time);
        }
        // if (req->cmd.nsid == 2 && req->cmd.opcode == 1) {
            // femu_log("lat: %u, real lat: %lu\n", req->stat.reqlat, now - req->stat.stime);
        // }

        sq = req->sq;
        cq = n->cq[sq->sqid];
        if (!cq->is_active)
            continue;
        nvme_post_cqe(cq, req);
        pthread_spin_lock(&sq->lock);
        QTAILQ_INSERT_TAIL(&sq->req_list, req, entry);
        pthread_spin_unlock(&sq->lock);
        pqueue_pop(pq);
        processed++;
        n->nr_tt_ios++;
        if (now - req->stat.expire_time >= 1000) {
            n->nr_tt_late_ios++;
            if (n->print_log) {
                femu_debug("%s,diff,pq.count=%lu,%" PRId64 ", %lu/%lu\n",
                           n->devname, pqueue_size(pq), now - req->stat.expire_time,
                           n->nr_tt_late_ios, n->nr_tt_ios);
            }
        }
        n->should_isr[sq->sqid] = true;

        req->stat.etime = now;
        femu_stat_add_req(n, req);
    }

    if (processed == 0)
        return;

    switch (n->multipoller_enabled) {
    case 1:
        nvme_isr_notify_io(n->cq[index_poller]);
        break;
    default:
        for (i = 1; i <= n->nr_io_queues; i++) {
            if (n->should_isr[i]) {
                nvme_isr_notify_io(n->cq[i]);
                n->should_isr[i] = false;
            }
        }
        break;
    }
}

void *nvme_poller(void *arg)
{
    FemuCtrl *n = ((NvmePollerThreadArgument *)arg)->n;
    int index = ((NvmePollerThreadArgument *)arg)->index;
    int i;

    while (!n->dataplane_started) {
        usleep(1000);
    }

    switch (n->multipoller_enabled) {
    case 1:
        while (1) {
            NvmeSQueue *sq = n->sq[index];
            NvmeCQueue *cq = n->cq[index];
            if (sq && sq->is_active && cq && cq->is_active) {
                nvme_process_sq_io(sq, index);
            }
            if (!n->cq_poller_enabled)
                nvme_process_cq_cpl(n, index);
        }
        break;
    default:
        while (1) {
            for (i = 1; i <= n->nr_io_queues; i++) {
                NvmeSQueue *sq = n->sq[i];
                NvmeCQueue *cq = n->cq[i];
                if (sq && sq->is_active && cq && cq->is_active) {
                    nvme_process_sq_io(sq, index);
                }
            }
            if (!n->cq_poller_enabled)
                nvme_process_cq_cpl(n, index);
        }
        break;
    }

    return NULL;
}

void *nvme_cq_poller(void *arg)
{
    FemuCtrl *n = ((NvmePollerThreadArgument *)arg)->n;
    int index = ((NvmePollerThreadArgument *)arg)->index;

    while (!n->dataplane_started) {
        usleep(1000);
    }

    while (1) {
        nvme_process_cq_cpl(n, index);
    }

    return NULL;
}

uint16_t nvme_rw(NvmeNamespace *ns, NvmeCmd *cmd, NvmeRequest *req)
{
    NvmeRwCmd *rw = (NvmeRwCmd *)cmd;
    uint16_t ctrl = le16_to_cpu(rw->control);
    uint32_t nlb  = le16_to_cpu(rw->nlb) + 1;
    uint64_t slba = le64_to_cpu(rw->slba);
    uint64_t prp1 = le64_to_cpu(rw->prp1);
    uint64_t prp2 = le64_to_cpu(rw->prp2);
    const uint8_t lba_index = NVME_ID_NS_FLBAS_INDEX(ns->id_ns.flbas);
    const uint16_t ms = le16_to_cpu(ns->id_ns.lbaf[lba_index].ms);
    const uint8_t data_shift = ns->id_ns.lbaf[lba_index].lbads;
    uint64_t data_size = (uint64_t)nlb << data_shift;
    uint64_t data_offset = slba << data_shift;
    uint64_t meta_size = nlb * ms;
    uint64_t elba = slba + nlb;
    uint16_t err;
    int ret;

    req->is_write = (rw->opcode == NVME_CMD_WRITE) ? 1 : 0;

    // femu_debug("nvme_rw slba %zu nlb %u prp1 %p\n", slba, nlb, (void *)prp1);

    err = femu_nvme_rw_check_req(ns->ctrl, ns, cmd, req, slba, elba, nlb, ctrl,
                                 data_size, meta_size);
    if (err)
        return err;

    if (nvme_map_prp(&req->qsg, &req->iov, prp1, prp2, data_size, ns->ctrl)) {
        nvme_set_error_page(ns->ctrl, req->sq->sqid, cmd->cid, NVME_INVALID_FIELD,
                            offsetof(NvmeRwCmd, prp1), 0, ns->id);
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    assert((nlb << data_shift) == req->qsg.size);

    uint64_t bar_addr = pci_get_bar_addr(&ns->ctrl->parent_obj, 2);
    if (prp1 == bar_addr) {
        femu_debug("nvme_rw prp1 equal to BAR2!\n");
    }

    req->slba = slba;
    req->status = NVME_SUCCESS;
    req->nlb = nlb;
    req->stat.data_size = data_size;
    req->stat.pcie_lat = 1;
    req->stat.rw.nlb = nlb;
    req->stat.rw.slba = slba;

    assert(req->qsg.size == data_size);

#ifdef FEMU_DEBUG_MEMCPY
    int64_t start = clock_ns();
#endif

    ret = backend_rw(ns->backend, &req->qsg, &data_offset, req->is_write);

#ifdef FEMU_DEBUG_MEMCPY
    int64_t end = clock_ns();
    ns->ctrl->memcpy_time += end - start;
    ns->ctrl->memcpy_size += data_size;
#endif

    if (!ret) {
        return NVME_SUCCESS;
    }

    return NVME_DNR;
}

static uint16_t nvme_dsm(FemuCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
                         NvmeRequest *req)
{
    uint32_t dw10 = le32_to_cpu(cmd->cdw10);
    uint32_t dw11 = le32_to_cpu(cmd->cdw11);
    uint64_t prp1 = le64_to_cpu(cmd->dptr.prp1);
    uint64_t prp2 = le64_to_cpu(cmd->dptr.prp2);
    int i;

    if (dw11 & NVME_DSMGMT_AD) {
        uint16_t nr = (dw10 & 0xff) + 1;

        uint64_t slba;
        uint32_t nlb;
        NvmeDsmRange range[nr];

        if (dma_write_prp(n, (uint8_t *)range, sizeof(range), prp1, prp2)) {
            nvme_set_error_page(n, req->sq->sqid, cmd->cid, NVME_INVALID_FIELD,
                                offsetof(NvmeCmd, dptr.prp1), 0, ns->id);
            return NVME_INVALID_FIELD | NVME_DNR;
        }

        req->status = NVME_SUCCESS;
        for (i = 0; i < nr; i++) {
            slba = le64_to_cpu(range[i].slba);
            nlb = le32_to_cpu(range[i].nlb);
            if (slba + nlb > le64_to_cpu(ns->id_ns.nsze)) {
                nvme_set_error_page(n, req->sq->sqid, cmd->cid, NVME_LBA_RANGE,
                                    offsetof(NvmeCmd, cdw10), slba + nlb, ns->id);
                return NVME_LBA_RANGE | NVME_DNR;
            }

            bitmap_clear(ns->util, slba, nlb);
        }
    }

    return NVME_SUCCESS;
}

static uint16_t nvme_compare(FemuCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
                             NvmeRequest *req)
{
    NvmeRwCmd *rw = (NvmeRwCmd *)cmd;
    uint32_t nlb  = le16_to_cpu(rw->nlb) + 1;
    uint64_t slba = le64_to_cpu(rw->slba);
    uint64_t prp1 = le64_to_cpu(rw->prp1);
    uint64_t prp2 = le64_to_cpu(rw->prp2);
    int i;

    uint64_t elba = slba + nlb;
    uint8_t lba_index = NVME_ID_NS_FLBAS_INDEX(ns->id_ns.flbas);
    uint8_t data_shift = ns->id_ns.lbaf[lba_index].lbads;
    uint64_t data_size = nlb << data_shift;
    uint64_t offset  = ns->start_block + (slba << data_shift);

    if ((slba + nlb) > le64_to_cpu(ns->id_ns.nsze)) {
        nvme_set_error_page(n, req->sq->sqid, cmd->cid, NVME_LBA_RANGE,
                            offsetof(NvmeRwCmd, nlb), elba, ns->id);
        return NVME_LBA_RANGE | NVME_DNR;
    }
    if (n->id_ctrl.mdts && data_size > n->page_size * (1 << n->id_ctrl.mdts)) {
        nvme_set_error_page(n, req->sq->sqid, cmd->cid, NVME_INVALID_FIELD,
                            offsetof(NvmeRwCmd, nlb), nlb, ns->id);
        return NVME_INVALID_FIELD | NVME_DNR;
    }
    if (nvme_map_prp(&req->qsg, &req->iov, prp1, prp2, data_size, n)) {
        nvme_set_error_page(n, req->sq->sqid, cmd->cid, NVME_INVALID_FIELD,
                            offsetof(NvmeRwCmd, prp1), 0, ns->id);
        return NVME_INVALID_FIELD | NVME_DNR;
    }
    if (find_next_bit(ns->uncorrectable, elba, slba) < elba) {
        return NVME_UNRECOVERED_READ;
    }

    for (i = 0; i < req->qsg.nsg; i++) {
        uint32_t len = req->qsg.sg[i].len;
        uint8_t tmp[2][len];

        nvme_addr_read(n, req->qsg.sg[i].base, tmp[1], len);
        if (memcmp(tmp[0], tmp[1], len)) {
            qemu_sglist_destroy(&req->qsg);
            return NVME_CMP_FAILURE;
        }
        offset += len;
    }

    qemu_sglist_destroy(&req->qsg);

    return NVME_SUCCESS;
}

static uint16_t nvme_flush(FemuCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
                           NvmeRequest *req)
{
    return NVME_SUCCESS;
}

static uint16_t nvme_write_zeros(FemuCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
                                 NvmeRequest *req)
{
    NvmeRwCmd *rw = (NvmeRwCmd *)cmd;
    uint64_t slba = le64_to_cpu(rw->slba);
    uint32_t nlb  = le16_to_cpu(rw->nlb) + 1;

    if ((slba + nlb) > ns->id_ns.nsze) {
        nvme_set_error_page(n, req->sq->sqid, cmd->cid, NVME_LBA_RANGE,
                            offsetof(NvmeRwCmd, nlb), slba + nlb, ns->id);
        return NVME_LBA_RANGE | NVME_DNR;
    }

    return NVME_SUCCESS;
}

static uint16_t nvme_write_uncor(FemuCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
                                 NvmeRequest *req)
{
    NvmeRwCmd *rw = (NvmeRwCmd *)cmd;
    uint64_t slba = le64_to_cpu(rw->slba);
    uint32_t nlb  = le16_to_cpu(rw->nlb) + 1;

    if ((slba + nlb) > ns->id_ns.nsze) {
        nvme_set_error_page(n, req->sq->sqid, cmd->cid, NVME_LBA_RANGE,
                            offsetof(NvmeRwCmd, nlb), slba + nlb, ns->id);
        return NVME_LBA_RANGE | NVME_DNR;
    }

    bitmap_set(ns->uncorrectable, slba, nlb);

    return NVME_SUCCESS;
}

// return 1 if the command is finished processing, 0 if not
static int nvme_io_cmd(FemuCtrl *n, NvmeCmd *cmd, NvmeRequest *req)
{
    NvmeNamespace *ns = req->ns;

    if (ns->csi == NVME_CSI_MEMORY || ns->csi == NVME_CSI_COMPUTE) {
        return ns->ops->io_cmd(ns, cmd, req);
    }

    switch (cmd->opcode) {
    case NVME_CMD_FLUSH:
        if (ns->csi == NVME_CSI_MEMORY) {
            req->status = ns->ops->io_cmd(ns, cmd, req);
            break;
        }
        if (!n->id_ctrl.vwc || !n->features.volatile_wc) {
            req->status = NVME_SUCCESS;
            break;
        }
        req->status = nvme_flush(n, ns, cmd, req);
        break;
    case NVME_CMD_DSM:
        if (NVME_ONCS_DSM & n->oncs) {
            req->status = nvme_dsm(n, ns, cmd, req);
            break;
        }
        req->status = NVME_INVALID_OPCODE | NVME_DNR;
        break;
    case NVME_CMD_COMPARE:
        if (NVME_ONCS_COMPARE & n->oncs) {
            req->status = nvme_compare(n, ns, cmd, req);
            break;
        }
        req->status = NVME_INVALID_OPCODE | NVME_DNR;
        break;
    case NVME_CMD_WRITE_ZEROES:
        if (NVME_ONCS_WRITE_ZEROS & n->oncs) {
            req->status = nvme_write_zeros(n, ns, cmd, req);
            break;
        }
        req->status =  NVME_INVALID_OPCODE | NVME_DNR;
        break;
    case NVME_CMD_WRITE_UNCOR:
        if (NVME_ONCS_WRITE_UNCORR & n->oncs) {
            req->status = nvme_write_uncor(n, ns, cmd, req);
            break;
        }
        req->status = NVME_INVALID_OPCODE | NVME_DNR;
        break;
    default:
        if (ns->ops->io_cmd) {
            return ns->ops->io_cmd(ns, cmd, req);
        }
    }
    return 1;
}

void nvme_post_cqes_io(void *opaque)
{
    NvmeCQueue *cq = opaque;
    NvmeRequest *req, *next;
    int64_t cur_time, ntt = 0;
    int processed = 0;

    QTAILQ_FOREACH_SAFE(req, &cq->req_list, entry, next) {
        if (nvme_cq_full(cq)) {
            break;
        }

        cur_time = clock_ns();
        if (cq->cqid != 0 && cur_time < req->stat.expire_time) {
            ntt = req->stat.expire_time;
            break;
        }

        nvme_post_cqe(cq, req);
        processed++;
    }

    if (ntt == 0) {
        ntt = clock_ns() + CQ_POLLING_PERIOD_NS;
    }

    /* Only interrupt guest when we "do" complete some I/Os */
    if (processed > 0) {
        nvme_isr_notify_io(cq);
    }
}
