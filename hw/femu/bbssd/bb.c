#include "../nvme.h"
#include "./bb.h"
#include "../param.h"
#include <stdlib.h>

/* bb <=> black-box */
static void bb_init(NvmeNamespace *ns, Error **errp)
{
    NvmNamespace *nns = ns->private = g_malloc0(sizeof(NvmNamespace));
    nns->params = ns->params;

    nns->ssdname = (char *)ns->ctrl->devname;
    femu_debug("Starting FEMU in Blackbox-SSD mode ...\n");
    ftl_init(ns);

    nns->to_ftl = g_malloc0(sizeof(struct rte_ring *) * (ns->ctrl->nr_pollers + 1));
    ns->ctrl->to_ftl = nns->to_ftl;
    for (int i = 1; i <= ns->ctrl->nr_pollers; i++) {
        nns->to_ftl[i] = femu_ring_create(FEMU_RING_TYPE_MP_SC, FEMU_MAX_INF_REQS);
        if (!nns->to_ftl[i]) {
            femu_err("Failed to create ring (n->to_ftl) ...\n");
            abort();
        }
        assert(rte_ring_empty(nns->to_ftl[i]));
    }
}

static void bb_exit(NvmeNamespace *ns)
{
    NvmNamespace *nns = ns->private;

    for (int i = 1; i <= ns->ctrl->nr_pollers; i++) {
        femu_ring_free(nns->to_ftl[i]);
    }
    g_free(nns->to_ftl);
    g_free(nns);
}

static void bb_flip(NvmeNamespace *ns, NvmeCmd *cmd)
{
    NvmParams *params = ns->params;
    int64_t cdw10 = le64_to_cpu(cmd->cdw10);
    char *devname = ns->ctrl->devname;

    switch (cdw10) {
    case FEMU_ENABLE_GC_DELAY:
        params->enable_gc_delay = true;
        femu_log("%s,FEMU GC Delay Emulation [Enabled]!\n", devname);
        break;
    case FEMU_DISABLE_GC_DELAY:
        params->enable_gc_delay = false;
        femu_log("%s,FEMU GC Delay Emulation [Disabled]!\n", devname);
        break;
    case FEMU_ENABLE_DELAY_EMU:
        params->pg_rd_lat = NAND_READ_LATENCY;
        params->pg_wr_lat = NAND_PROG_LATENCY;
        params->blk_er_lat = NAND_ERASE_LATENCY;
        params->ch_xfer_lat = 0;
        femu_log("%s,FEMU Delay Emulation [Enabled]!\n", devname);
        break;
    case FEMU_DISABLE_DELAY_EMU:
        params->pg_rd_lat = 0;
        params->pg_wr_lat = 0;
        params->blk_er_lat = 0;
        params->ch_xfer_lat = 0;
        femu_log("%s,FEMU Delay Emulation [Disabled]!\n", devname);
        break;
    case FEMU_RESET_ACCT:
        ns->ctrl->nr_tt_ios = 0;
        ns->ctrl->nr_tt_late_ios = 0;
        femu_log("%s,Reset tt_late_ios/tt_ios,%lu/%lu\n", devname,
                ns->ctrl->nr_tt_late_ios, ns->ctrl->nr_tt_ios);
        break;
    case FEMU_ENABLE_LOG:
        ns->ctrl->print_log = true;
        femu_log("%s,Log print [Enabled]!\n", devname);
        break;
    case FEMU_DISABLE_LOG:
        ns->ctrl->print_log = false;
        femu_log("%s,Log print [Disabled]!\n", devname);
        break;
    default:
        printf("FEMU:%s,Not implemented flip cmd (%lu)\n", devname, cdw10);
    }
}

static int bb_io_cmd(NvmeNamespace *ns, NvmeCmd *cmd, NvmeRequest *req)
{
    NvmNamespace *nns = ns->private;
    int rc = 0;

    switch (cmd->opcode) {
    case NVME_CMD_READ:
    case NVME_CMD_WRITE:
        rc = femu_ring_enqueue(nns->to_ftl[req->poller], (void *)&req, 1);
        if (rc != 1) {
            femu_err("enqueue failed, ret=%d\n", rc);
        }
        return 0;
    default:
        req->status = NVME_INVALID_OPCODE | NVME_DNR;
        return 1;
    }
}

static uint16_t bb_admin_cmd(NvmeNamespace *ns, NvmeCmd *cmd, NvmeCqe *cqe)
{
    switch (cmd->opcode) {
    case NVME_ADM_CMD_FEMU_FLIP:
        bb_flip(ns, cmd);
        return NVME_SUCCESS;
    default:
        return NVME_INVALID_OPCODE | NVME_DNR;
    }
}

static int bb_encode_params(cJSON *json, NvmeNamespace *ns)
{
    NvmParams *params = ns->params;

    ENCODE_PARAM(json, "secsz", Number, params->secsz);
    ENCODE_PARAM(json, "secs_per_pg", Number, params->secs_per_pg);
    ENCODE_PARAM(json, "pgs_per_blk", Number, params->pgs_per_blk);
    ENCODE_PARAM(json, "blks_per_pl", Number, params->blks_per_pl);
    ENCODE_PARAM(json, "pls_per_lun", Number, params->pls_per_lun);
    ENCODE_PARAM(json, "luns_per_ch", Number, params->luns_per_ch);
    ENCODE_PARAM(json, "nchs", Number, params->nchs);
    ENCODE_PARAM(json, "pg_rd_lat", Number, params->pg_rd_lat);
    ENCODE_PARAM(json, "pg_wr_lat", Number, params->pg_wr_lat);
    ENCODE_PARAM(json, "blk_er_lat", Number, params->blk_er_lat);
    ENCODE_PARAM(json, "ch_xfer_lat", Number, params->ch_xfer_lat);
    ENCODE_PARAM(json, "gc_thres_lines", Number, params->gc_thres_lines);
    ENCODE_PARAM(json, "gc_thres_lines_high", Number, params->gc_thres_lines_high);
    ENCODE_PARAM(json, "enable_gc_delay", Bool, params->enable_gc_delay);
    ENCODE_PARAM(json, "gc_thres_pcent", Number, params->gc_thres_pcent);
    ENCODE_PARAM(json, "gc_thres_pcent_high", Number, params->gc_thres_pcent_high);
    ENCODE_PARAM(json, "secs_per_blk", Number, params->secs_per_blk);
    ENCODE_PARAM(json, "secs_per_pl", Number, params->secs_per_pl);
    ENCODE_PARAM(json, "secs_per_lun", Number, params->secs_per_lun);
    ENCODE_PARAM(json, "secs_per_ch", Number, params->secs_per_ch);
    ENCODE_PARAM(json, "tt_secs", Number, params->tt_secs);
    ENCODE_PARAM(json, "pgs_per_pl", Number, params->pgs_per_pl);
    ENCODE_PARAM(json, "pgs_per_lun", Number, params->pgs_per_lun);
    ENCODE_PARAM(json, "pgs_per_ch", Number, params->pgs_per_ch);
    ENCODE_PARAM(json, "tt_pgs", Number, params->tt_pgs);
    ENCODE_PARAM(json, "blks_per_lun", Number, params->blks_per_lun);
    ENCODE_PARAM(json, "blks_per_ch", Number, params->blks_per_ch);
    ENCODE_PARAM(json, "tt_blks", Number, params->tt_blks);
    ENCODE_PARAM(json, "secs_per_line", Number, params->secs_per_line);
    ENCODE_PARAM(json, "pgs_per_line", Number, params->pgs_per_line);
    ENCODE_PARAM(json, "blks_per_line", Number, params->blks_per_line);
    ENCODE_PARAM(json, "tt_lines", Number, params->tt_lines);
    ENCODE_PARAM(json, "pls_per_ch", Number, params->pls_per_ch);
    ENCODE_PARAM(json, "tt_pls", Number, params->tt_pls);
    ENCODE_PARAM(json, "tt_luns", Number, params->tt_luns);

    return 0;
}

static int bb_decode_params(const cJSON *json, NvmeNamespace *ns)
{
    NvmParams *params = malloc(sizeof(NvmParams));
    ns->params = params;

    // the following parameters defines internal structure of SSD,
    // this structure is only used by FTL, it defines the maximum
    // size support by the blackbox SSD (e.g., 16GB).
    // params->blks_per_pl can be changed to enlarge SSD size
    DECODE_PARAM(json, "secsz", Number, params->secsz, 512);
    DECODE_PARAM(json, "secs_per_pg", Number, params->secs_per_pg, 8);
    DECODE_PARAM(json, "pgs_per_blk", Number, params->pgs_per_blk, 256);    // 1MB per block
    DECODE_PARAM(json, "blks_per_pl", Number, params->blks_per_pl, 2048);   // 256MB per plane
    DECODE_PARAM(json, "pls_per_lun", Number, params->pls_per_lun, 1);
    DECODE_PARAM(json, "luns_per_ch", Number, params->luns_per_ch, 8);      // 2GB per channel
    DECODE_PARAM(json, "nchs", Number, params->nchs, 8);                    // 16GB
    DECODE_PARAM(json, "pg_rd_lat", Number, params->pg_rd_lat, NAND_READ_LATENCY);
    DECODE_PARAM(json, "pg_wr_lat", Number, params->pg_wr_lat, NAND_PROG_LATENCY);
    DECODE_PARAM(json, "blk_er_lat", Number, params->blk_er_lat, NAND_ERASE_LATENCY);
    DECODE_PARAM(json, "ch_xfer_lat", Number, params->ch_xfer_lat, 0);

    // TODO: blks_per_pl should be calculated by ns size
    params->blks_per_pl = ns->size / ((uint64_t)params->secs_per_pg *
            params->pgs_per_blk * params->secsz * params->pls_per_lun *
            params->luns_per_ch * params->nchs);

    if ((uint64_t)params->blks_per_pl * params->secs_per_pg * params->pgs_per_blk *
            params->secsz * params->pls_per_lun * params->luns_per_ch *
            params->nchs != ns->size) {
        femu_err("namespace %d size %zu is not equal to secsz(%d) * secs_per_pg(%d) * "
                 "pgs_per_blk(%d) * blks_per_pl(%d) * pls_per_lun(%d) * "
                 "luns_per_ch(%d) * nchs(%d)!\n", ns->id, ns->size,
                 params->secsz, params->secs_per_pg, params->pgs_per_blk,
                 params->blks_per_pl, params->pls_per_lun,
                 params->luns_per_ch, params->nchs);
        abort();
    }

    /* calculated values */
    params->secs_per_blk = params->secs_per_pg * params->pgs_per_blk;
    params->secs_per_pl = params->secs_per_blk * params->blks_per_pl;
    params->secs_per_lun = params->secs_per_pl * params->pls_per_lun;
    params->secs_per_ch = params->secs_per_lun * params->luns_per_ch;
    params->tt_secs = params->secs_per_ch * params->nchs;

    if (params->tt_secs * params->secsz != ns->size) {
        femu_err("namespace %d size %zu is not equal to secsz(%d) * secs_per_pg(%d) * "
                 "pgs_per_blk(%d) * blks_per_pl(%d) * pls_per_lun(%d) * "
                 "luns_per_ch(%d) * nchs(%d)!\n", ns->id, ns->size,
                 params->secsz, params->secs_per_pg, params->pgs_per_blk,
                 params->blks_per_pl, params->pls_per_lun,
                 params->luns_per_ch, params->nchs);
        abort();
    }

    params->pgs_per_pl = params->pgs_per_blk * params->blks_per_pl;
    params->pgs_per_lun = params->pgs_per_pl * params->pls_per_lun;
    params->pgs_per_ch = params->pgs_per_lun * params->luns_per_ch;
    params->tt_pgs = params->pgs_per_ch * params->nchs;

    params->blks_per_lun = params->blks_per_pl * params->pls_per_lun;
    params->blks_per_ch = params->blks_per_lun * params->luns_per_ch;
    params->tt_blks = params->blks_per_ch * params->nchs;

    params->pls_per_ch =  params->pls_per_lun * params->luns_per_ch;
    params->tt_pls = params->pls_per_ch * params->nchs;

    params->tt_luns = params->luns_per_ch * params->nchs;

    /* line is special, put it at the end */
    params->blks_per_line = params->tt_luns; /* TODO: to fix under multiplanes */
    params->pgs_per_line = params->blks_per_line * params->pgs_per_blk;
    params->secs_per_line = params->pgs_per_line * params->secs_per_pg;
    params->tt_lines = params->blks_per_lun; /* TODO: to fix under multiplanes */

    params->gc_thres_pcent = 0.75;
    params->gc_thres_lines = (int)((1 - params->gc_thres_pcent) * params->tt_lines);
    params->gc_thres_pcent_high = 0.95;
    params->gc_thres_lines_high = (int)((1 - params->gc_thres_pcent_high) * params->tt_lines);
    params->enable_gc_delay = true;

    femu_log("bbssd: tt_lines %d, gc_thres_lines %d, gc_thres_lines_high %d", params->tt_lines, params->gc_thres_lines, params->gc_thres_lines_high);

    return 0;
}

static CommandSetOps nvm_command_set_ops = {
    .state            = NULL,
    .init             = bb_init,
    .exit             = bb_exit,
    .rw_check_req     = NULL,
    .admin_cmd        = bb_admin_cmd,
    .io_cmd           = bb_io_cmd,
    .get_log          = NULL,
    .encode_params    = bb_encode_params,
    .decode_params    = bb_decode_params
};

nvme_register(NVME_CSI_NVM, &nvm_command_set_ops);
