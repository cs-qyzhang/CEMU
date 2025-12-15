#include "../statistics/statistics.h"
#include "hw/femu/nvme.h"
#include "bb.h"
#include "../csd/memory.h"

static void *ftl_thread(void *arg);

/* A simple way to lower internal io priority */
// #define CEMU_THROTTLE_INTERNAL

struct cache_entry {
    uint64_t time;
    int pos;
    int entries;
};

struct memory_copy_req {
    int slba;
    int nlb;
    int is_write;
    int is_last;
    uint64_t ts;
    NvmeRequest *req;
};

struct memory_copy_queue {
    struct memory_copy_req *reqs;
    int head;
    int tail;
    int size;
    int capacity;
    uint64_t next_ts;
};

static inline bool should_gc(NvmNamespace *nns)
{
    return (nns->lm.free_line_cnt <= nns->params->gc_thres_lines);
}

static inline bool should_gc_high(NvmNamespace *nns)
{
    return (nns->lm.free_line_cnt <= nns->params->gc_thres_lines_high);
}

static inline struct ppa get_maptbl_ent(NvmNamespace *nns, uint64_t lpn)
{
    return nns->maptbl[lpn];
}

static inline void set_maptbl_ent(NvmNamespace *nns, uint64_t lpn, struct ppa *ppa)
{
    assert(lpn < nns->params->tt_pgs);
    nns->maptbl[lpn] = *ppa;
}

static uint64_t ppa2pgidx(NvmNamespace *nns, struct ppa *ppa)
{
    NvmParams *params = nns->params;
    uint64_t pgidx;

    pgidx = ppa->g.ch  * params->pgs_per_ch  + \
            ppa->g.lun * params->pgs_per_lun + \
            ppa->g.pl  * params->pgs_per_pl  + \
            ppa->g.blk * params->pgs_per_blk + \
            ppa->g.pg;

    assert(pgidx < params->tt_pgs);

    return pgidx;
}

static inline uint64_t get_rmap_ent(NvmNamespace *nns, struct ppa *ppa)
{
    uint64_t pgidx = ppa2pgidx(nns, ppa);

    return nns->rmap[pgidx];
}

/* set rmap[page_no(ppa)] -> lpn */
static inline void set_rmap_ent(NvmNamespace *nns, uint64_t lpn, struct ppa *ppa)
{
    uint64_t pgidx = ppa2pgidx(nns, ppa);

    nns->rmap[pgidx] = lpn;
}

static inline int victim_line_cmp_pri(pqueue_pri_t next, pqueue_pri_t curr)
{
    return (next > curr);
}

static inline pqueue_pri_t victim_line_get_pri(void *a)
{
    return ((struct line *)a)->vpc;
}

static inline void victim_line_set_pri(void *a, pqueue_pri_t pri)
{
    ((struct line *)a)->vpc = pri;
}

static inline size_t victim_line_get_pos(void *a)
{
    return ((struct line *)a)->pos;
}

static inline void victim_line_set_pos(void *a, size_t pos)
{
    ((struct line *)a)->pos = pos;
}

static void ssd_init_lines(NvmNamespace *nns)
{
    NvmParams *params = nns->params;
    struct line_mgmt *lm = &nns->lm;
    struct line *line;

    lm->tt_lines = params->blks_per_pl;
    assert(lm->tt_lines == params->tt_lines);
    lm->lines = g_malloc0(sizeof(struct line) * lm->tt_lines);

    QTAILQ_INIT(&lm->free_line_list);
    lm->victim_line_pq = pqueue_init(params->tt_lines, victim_line_cmp_pri,
            victim_line_get_pri, victim_line_set_pri,
            victim_line_get_pos, victim_line_set_pos);
    QTAILQ_INIT(&lm->full_line_list);

    lm->free_line_cnt = 0;
    for (int i = 0; i < lm->tt_lines; i++) {
        line = &lm->lines[i];
        line->id = i;
        line->ipc = 0;
        line->vpc = 0;
        line->pos = 0;
        /* initialize all the lines as free lines */
        QTAILQ_INSERT_TAIL(&lm->free_line_list, line, entry);
        lm->free_line_cnt++;
    }

    assert(lm->free_line_cnt == lm->tt_lines);
    lm->victim_line_cnt = 0;
    lm->full_line_cnt = 0;
}

static void ssd_init_write_pointer(NvmNamespace *nns)
{
    struct write_pointer *wpp = &nns->wp;
    struct line_mgmt *lm = &nns->lm;
    struct line *curline = NULL;

    curline = QTAILQ_FIRST(&lm->free_line_list);
    QTAILQ_REMOVE(&lm->free_line_list, curline, entry);
    lm->free_line_cnt--;

    /* wpp->curline is always our next-to-write super-block */
    wpp->curline = curline;
    wpp->ch = 0;
    wpp->lun = 0;
    wpp->pg = 0;
    wpp->blk = 0;
    wpp->pl = 0;
}

static inline void check_addr(int a, int max)
{
    assert(a >= 0 && a < max);
}

static struct line *get_next_free_line(NvmNamespace *nns)
{
    struct line_mgmt *lm = &nns->lm;
    struct line *curline = NULL;

    curline = QTAILQ_FIRST(&lm->free_line_list);
    if (!curline) {
        femu_err("No free lines left in [%s] !!!!\n", nns->ssdname);
        return NULL;
    }

    QTAILQ_REMOVE(&lm->free_line_list, curline, entry);
    lm->free_line_cnt--;
    return curline;
}

static void ssd_advance_write_pointer(NvmNamespace *nns)
{
    NvmParams *params = nns->params;
    struct write_pointer *wpp = &nns->wp;
    struct line_mgmt *lm = &nns->lm;

    check_addr(wpp->ch, params->nchs);
    wpp->ch++;
    if (wpp->ch == params->nchs) {
        wpp->ch = 0;
        check_addr(wpp->lun, params->luns_per_ch);
        wpp->lun++;
        /* in this case, we should go to next lun */
        if (wpp->lun == params->luns_per_ch) {
            wpp->lun = 0;
            /* go to next page in the block */
            check_addr(wpp->pg, params->pgs_per_blk);
            wpp->pg++;
            if (wpp->pg == params->pgs_per_blk) {
                wpp->pg = 0;
                /* move current line to {victim,full} line list */
                if (wpp->curline->vpc == params->pgs_per_line) {
                    /* all pgs are still valid, move to full line list */
                    assert(wpp->curline->ipc == 0);
                    QTAILQ_INSERT_TAIL(&lm->full_line_list, wpp->curline, entry);
                    lm->full_line_cnt++;
                } else {
                    assert(wpp->curline->vpc >= 0 && wpp->curline->vpc < params->pgs_per_line);
                    /* there must be some invalid pages in this line */
                    assert(wpp->curline->ipc > 0);
                    pqueue_insert(lm->victim_line_pq, wpp->curline);
                    lm->victim_line_cnt++;
                }
                /* current line is used up, pick another empty line */
                check_addr(wpp->blk, params->blks_per_pl);
                wpp->curline = NULL;
                wpp->curline = get_next_free_line(nns);
                if (!wpp->curline) {
                    /* TODO */
                    femu_err("No free lines left in [%s] !!!!\n", nns->ssdname);
                    abort();
                }
                wpp->blk = wpp->curline->id;
                check_addr(wpp->blk, params->blks_per_pl);
                /* make sure we are starting from page 0 in the super block */
                assert(wpp->pg == 0);
                assert(wpp->lun == 0);
                assert(wpp->ch == 0);
                /* TODO: assume # of pl_per_lun is 1, fix later */
                assert(wpp->pl == 0);
            }
        }
    }
}

static struct ppa get_new_page(NvmNamespace *nns)
{
    struct write_pointer *wpp = &nns->wp;
    struct ppa ppa;
    ppa.ppa = 0;
    ppa.g.ch = wpp->ch;
    ppa.g.lun = wpp->lun;
    ppa.g.pg = wpp->pg;
    ppa.g.blk = wpp->blk;
    ppa.g.pl = wpp->pl;
    assert(ppa.g.pl == 0);

    return ppa;
}

static void ssd_init_nand_page(struct nand_page *pg, NvmParams *params)
{
    pg->nsecs = params->secs_per_pg;
    pg->sec = g_malloc0(sizeof(nand_sec_status_t) * pg->nsecs);
    for (int i = 0; i < pg->nsecs; i++) {
        pg->sec[i] = SEC_FREE;
    }
    pg->status = PG_FREE;
}

static void ssd_init_nand_blk(struct nand_block *blk, NvmParams *params)
{
    blk->npgs = params->pgs_per_blk;
    blk->pg = g_malloc0(sizeof(struct nand_page) * blk->npgs);
    for (int i = 0; i < blk->npgs; i++) {
        ssd_init_nand_page(&blk->pg[i], params);
    }
    blk->ipc = 0;
    blk->vpc = 0;
    blk->erase_cnt = 0;
    blk->wp = 0;
}

static void ssd_init_nand_plane(struct nand_plane *pl, NvmParams *params)
{
    pl->nblks = params->blks_per_pl;
    pl->blk = g_malloc0(sizeof(struct nand_block) * pl->nblks);
    for (int i = 0; i < pl->nblks; i++) {
        ssd_init_nand_blk(&pl->blk[i], params);
    }
}

static void ssd_init_nand_lun(struct nand_lun *lun, NvmParams *params)
{
    lun->npls = params->pls_per_lun;
    lun->pl = g_malloc0(sizeof(struct nand_plane) * lun->npls);
    for (int i = 0; i < lun->npls; i++) {
        ssd_init_nand_plane(&lun->pl[i], params);
    }
    lun->next_lun_avail_time = 0;
    lun->busy = false;
}

static void ssd_init_ch(struct ssd_channel *ch, NvmParams *params)
{
    ch->nluns = params->luns_per_ch;
    ch->lun = g_malloc0(sizeof(struct nand_lun) * ch->nluns);
    for (int i = 0; i < ch->nluns; i++) {
        ssd_init_nand_lun(&ch->lun[i], params);
    }
    ch->next_ch_avail_time = 0;
    ch->busy = 0;
}

static void ssd_init_maptbl(NvmNamespace *nns)
{
    NvmParams *params = nns->params;

    nns->maptbl = g_malloc0(sizeof(struct ppa) * params->tt_pgs);
    for (int i = 0; i < params->tt_pgs; i++) {
        nns->maptbl[i].ppa = UNMAPPED_PPA;
    }
}

static void ssd_init_rmap(NvmNamespace *nns)
{
    NvmParams *params = nns->params;

    nns->rmap = g_malloc0(sizeof(uint64_t) * params->tt_pgs);
    for (int i = 0; i < params->tt_pgs; i++) {
        nns->rmap[i] = INVALID_LPN;
    }
}

static int cache_cmp_pri(pqueue_pri_t next, pqueue_pri_t curr) {
    return (next > curr);
}

static void cache_set_pri(void *a, pqueue_pri_t pri) {
    ((struct cache_entry *)a)->time = pri;
}

static void cache_set_pos(void *a, size_t pos) {
    ((struct cache_entry *)a)->pos = pos;
}

static pqueue_pri_t cache_get_pri(void *a) {
    return ((struct cache_entry *)a)->time;
}

static size_t cache_get_pos(void *a) {
    return ((struct cache_entry *)a)->pos;
}

void ftl_init(NvmeNamespace *ns)
{
    NvmNamespace *nns = ns->private;
    NvmParams *params = nns->params;

    /* initialize ssd internal layout architecture */
    nns->ch = g_malloc0(sizeof(struct ssd_channel) * params->nchs);
    for (int i = 0; i < params->nchs; i++) {
        ssd_init_ch(&nns->ch[i], params);
    }

    /* initialize maptbl */
    ssd_init_maptbl(nns);

    /* initialize rmap */
    ssd_init_rmap(nns);

    /* initialize all the lines */
    ssd_init_lines(nns);

    /* initialize write pointer, this is how we allocate new pages for writes */
    ssd_init_write_pointer(nns);

    nns->cache_pq = pqueue_init(1024ULL*128, cache_cmp_pri, cache_get_pri, cache_set_pri, cache_get_pos, cache_set_pos);
    if (!nns->cache_pq) {
        femu_err("Failed to initialize cache_pq\n");
        abort();
    }
    params->write_cache_entries = 1024*256*2;

    qemu_thread_create(&nns->ftl_thread, "FEMU-FTL-Thread", ftl_thread, ns,
                       QEMU_THREAD_JOINABLE);
}

static inline bool valid_ppa(NvmNamespace *nns, struct ppa *ppa)
{
    NvmParams *params = nns->params;
    int ch = ppa->g.ch;
    int lun = ppa->g.lun;
    int pl = ppa->g.pl;
    int blk = ppa->g.blk;
    int pg = ppa->g.pg;
    int sec = ppa->g.sec;

    if (ch >= 0 && ch < params->nchs && lun >= 0 && lun < params->luns_per_ch && pl >=
        0 && pl < params->pls_per_lun && blk >= 0 && blk < params->blks_per_pl && pg
        >= 0 && pg < params->pgs_per_blk && sec >= 0 && sec < params->secs_per_pg)
        return true;

    return false;
}

static inline bool valid_lpn(NvmNamespace *nns, uint64_t lpn)
{
    return (lpn < nns->params->tt_pgs);
}

static inline bool mapped_ppa(struct ppa *ppa)
{
    return !(ppa->ppa == UNMAPPED_PPA);
}

static inline struct ssd_channel *get_ch(NvmNamespace *nns, struct ppa *ppa)
{
    return &(nns->ch[ppa->g.ch]);
}

static inline struct nand_lun *get_lun(NvmNamespace *nns, struct ppa *ppa)
{
    struct ssd_channel *ch = get_ch(nns, ppa);
    return &(ch->lun[ppa->g.lun]);
}

static inline struct nand_plane *get_pl(NvmNamespace *nns, struct ppa *ppa)
{
    struct nand_lun *lun = get_lun(nns, ppa);
    return &(lun->pl[ppa->g.pl]);
}

static inline struct nand_block *get_blk(NvmNamespace *nns, struct ppa *ppa)
{
    struct nand_plane *pl = get_pl(nns, ppa);
    return &(pl->blk[ppa->g.blk]);
}

static inline struct line *get_line(NvmNamespace *nns, struct ppa *ppa)
{
    return &(nns->lm.lines[ppa->g.blk]);
}

static inline struct nand_page *get_pg(NvmNamespace *nns, struct ppa *ppa)
{
    struct nand_block *blk = get_blk(nns, ppa);
    return &(blk->pg[ppa->g.pg]);
}

static uint64_t ssd_advance_status(NvmNamespace *nns, struct ppa *ppa, struct
        nand_cmd *ncmd)
{
    int c = ncmd->cmd;
    uint64_t cmd_stime = (ncmd->stime == 0) ? \
        clock_ns() : ncmd->stime;
    uint64_t nand_stime;
    NvmParams *params = nns->params;
    struct nand_lun *lun = get_lun(nns, ppa);
    uint64_t lat = 0;
    // uint64_t chnl_stime;
    // struct ssd_channel *ch = get_ch(nns, ppa);

    switch (c) {
    case NAND_READ:
        /* read: perform NAND cmd first */
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + params->pg_rd_lat;
        lat = lun->next_lun_avail_time - cmd_stime;
        // femu_debug("ssd_advance_status read: nand_stime %zu, next_avail %zu, lat %zu, pg_rd_lat %d\n", nand_stime, lun->next_lun_avail_time, lat, params->pg_rd_lat);
#if 0
        lun->next_lun_avail_time = nand_stime + params->pg_rd_lat;

        /* read: then data transfer through channel */
        chnl_stime = (ch->next_ch_avail_time < lun->next_lun_avail_time) ? \
            lun->next_lun_avail_time : ch->next_ch_avail_time;
        ch->next_ch_avail_time = chnl_stime + params->ch_xfer_lat;

        lat = ch->next_ch_avail_time - cmd_stime;
#endif
        break;

    case NAND_WRITE:
        /* write: transfer data through channel first */
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
        if (ncmd->type == USER_IO) {
            lun->next_lun_avail_time = nand_stime + params->pg_wr_lat;
        } else {
            lun->next_lun_avail_time = nand_stime + params->pg_wr_lat;
        }
        lat = lun->next_lun_avail_time - cmd_stime;

#if 0
        chnl_stime = (ch->next_ch_avail_time < cmd_stime) ? cmd_stime : \
                     ch->next_ch_avail_time;
        ch->next_ch_avail_time = chnl_stime + params->ch_xfer_lat;

        /* write: then do NAND program */
        nand_stime = (lun->next_lun_avail_time < ch->next_ch_avail_time) ? \
            ch->next_ch_avail_time : lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + params->pg_wr_lat;

        lat = lun->next_lun_avail_time - cmd_stime;
#endif
        break;

    case NAND_ERASE:
        /* erase: only need to advance NAND status */
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + params->blk_er_lat;

        lat = lun->next_lun_avail_time - cmd_stime;
        break;

    default:
        femu_err("Unsupported NAND command: 0x%x\n", c);
    }

    return lat;
}

/* update SSD status about one page from PG_VALID -> PG_VALID */
static void mark_page_invalid(NvmNamespace *nns, struct ppa *ppa)
{
    struct line_mgmt *lm = &nns->lm;
    NvmParams *params = nns->params;
    struct nand_block *blk = NULL;
    struct nand_page *pg = NULL;
    bool was_full_line = false;
    struct line *line;

    /* update corresponding page status */
    pg = get_pg(nns, ppa);
    assert(pg->status == PG_VALID);
    pg->status = PG_INVALID;

    /* update corresponding block status */
    blk = get_blk(nns, ppa);
    assert(blk->ipc >= 0 && blk->ipc < params->pgs_per_blk);
    blk->ipc++;
    assert(blk->vpc > 0 && blk->vpc <= params->pgs_per_blk);
    blk->vpc--;

    /* update corresponding line status */
    line = get_line(nns, ppa);
    assert(line->ipc >= 0 && line->ipc < params->pgs_per_line);
    if (line->vpc == params->pgs_per_line) {
        assert(line->ipc == 0);
        was_full_line = true;
    }
    line->ipc++;
    assert(line->vpc > 0 && line->vpc <= params->pgs_per_line);
    /* Adjust the position of the victime line in the pq under over-writes */
    if (line->pos) {
        /* Note that line->vpc will be updated by this call */
        pqueue_change_priority(lm->victim_line_pq, line->vpc - 1, line);
    } else {
        line->vpc--;
    }

    if (was_full_line) {
        /* move line: "full" -> "victim" */
        QTAILQ_REMOVE(&lm->full_line_list, line, entry);
        lm->full_line_cnt--;
        pqueue_insert(lm->victim_line_pq, line);
        lm->victim_line_cnt++;
    }
}

static void mark_page_valid(NvmNamespace *nns, struct ppa *ppa)
{
    struct nand_block *blk = NULL;
    struct nand_page *pg = NULL;
    struct line *line;

    /* update page status */
    pg = get_pg(nns, ppa);
    assert(pg->status == PG_FREE);
    pg->status = PG_VALID;

    /* update corresponding block status */
    blk = get_blk(nns, ppa);
    assert(blk->vpc >= 0 && blk->vpc < nns->params->pgs_per_blk);
    blk->vpc++;

    /* update corresponding line status */
    line = get_line(nns, ppa);
    assert(line->vpc >= 0 && line->vpc < nns->params->pgs_per_line);
    line->vpc++;
}

static void mark_block_free(NvmNamespace *nns, struct ppa *ppa)
{
    NvmParams *params = nns->params;
    struct nand_block *blk = get_blk(nns, ppa);
    struct nand_page *pg = NULL;

    for (int i = 0; i < params->pgs_per_blk; i++) {
        /* reset page status */
        pg = &blk->pg[i];
        assert(pg->nsecs == params->secs_per_pg);
        pg->status = PG_FREE;
    }

    /* reset block status */
    assert(blk->npgs == params->pgs_per_blk);
    blk->ipc = 0;
    blk->vpc = 0;
    blk->erase_cnt++;
}

static void gc_read_page(NvmNamespace *nns, struct ppa *ppa)
{
    /* advance ssd status, we don't care about how long it takes */
    if (nns->params->enable_gc_delay) {
        struct nand_cmd gcr;
        gcr.type = GC_IO;
        gcr.cmd = NAND_READ;
        gcr.stime = 0;
        ssd_advance_status(nns, ppa, &gcr);
    }
}

/* move valid page data (already in DRAM) from victim line to a new page */
static uint64_t gc_write_page(NvmNamespace *nns, struct ppa *old_ppa)
{
    struct ppa new_ppa;
    struct nand_lun *new_lun;
    uint64_t lpn = get_rmap_ent(nns, old_ppa);

    assert(valid_lpn(nns, lpn));
    new_ppa = get_new_page(nns);
    /* update maptbl */
    set_maptbl_ent(nns, lpn, &new_ppa);
    /* update rmap */
    set_rmap_ent(nns, lpn, &new_ppa);

    mark_page_valid(nns, &new_ppa);

    /* need to advance the write pointer here */
    ssd_advance_write_pointer(nns);

    if (nns->params->enable_gc_delay) {
        struct nand_cmd gcw;
        gcw.type = GC_IO;
        gcw.cmd = NAND_WRITE;
        gcw.stime = 0;
        ssd_advance_status(nns, &new_ppa, &gcw);
    }

    /* advance per-ch gc_endtime as well */
#if 0
    new_ch = get_ch(nns, &new_ppa);
    new_ch->gc_endtime = new_ch->next_ch_avail_time;
#endif

    new_lun = get_lun(nns, &new_ppa);
    new_lun->gc_endtime = new_lun->next_lun_avail_time;

    return 0;
}

static struct line *select_victim_line(NvmNamespace *nns, bool force)
{
    struct line_mgmt *lm = &nns->lm;
    struct line *victim_line = NULL;

    victim_line = pqueue_peek(lm->victim_line_pq);
    if (!victim_line) {
        return NULL;
    }

    if (!force && victim_line->ipc < nns->params->pgs_per_line / 8) {
        return NULL;
    }

    pqueue_pop(lm->victim_line_pq);
    victim_line->pos = 0;
    lm->victim_line_cnt--;

    /* victim_line is a danggling node now */
    return victim_line;
}

/* here ppa identifies the block we want to clean */
static void clean_one_block(NvmNamespace *nns, struct ppa *ppa)
{
    NvmParams *params = nns->params;
    struct nand_page *pg_iter = NULL;
    int cnt = 0;

    for (int pg = 0; pg < params->pgs_per_blk; pg++) {
        ppa->g.pg = pg;
        pg_iter = get_pg(nns, ppa);
        /* there shouldn't be any free page in victim blocks */
        assert(pg_iter->status != PG_FREE);
        if (pg_iter->status == PG_VALID) {
            gc_read_page(nns, ppa);
            /* delay the maptbl update until "write" happens */
            gc_write_page(nns, ppa);
            cnt++;
        }
    }

    assert(get_blk(nns, ppa)->vpc == cnt);
}

static void mark_line_free(NvmNamespace *nns, struct ppa *ppa)
{
    struct line_mgmt *lm = &nns->lm;
    struct line *line = get_line(nns, ppa);
    line->ipc = 0;
    line->vpc = 0;
    /* move this line to free line list */
    QTAILQ_INSERT_TAIL(&lm->free_line_list, line, entry);
    lm->free_line_cnt++;
}

static int do_gc(NvmNamespace *nns, bool force)
{
    struct line *victim_line = NULL;
    NvmParams *params = nns->params;
    struct nand_lun *lunp;
    struct ppa ppa;
    int ch, lun;

    victim_line = select_victim_line(nns, force);
    if (!victim_line) {
        return -1;
    }

    ppa.g.blk = victim_line->id;
    femu_debug("GC-ing line:%d,ipc=%d,victim=%d,full=%d,free=%d\n", ppa.g.blk,
              victim_line->ipc, nns->lm.victim_line_cnt, nns->lm.full_line_cnt,
              nns->lm.free_line_cnt);

    /* copy back valid data */
    for (ch = 0; ch < params->nchs; ch++) {
        for (lun = 0; lun < params->luns_per_ch; lun++) {
            ppa.g.ch = ch;
            ppa.g.lun = lun;
            ppa.g.pl = 0;
            lunp = get_lun(nns, &ppa);
            clean_one_block(nns, &ppa);
            mark_block_free(nns, &ppa);

            if (params->enable_gc_delay) {
                struct nand_cmd gce;
                gce.type = GC_IO;
                gce.cmd = NAND_ERASE;
                gce.stime = 0;
                ssd_advance_status(nns, &ppa, &gce);
            }

            lunp->gc_endtime = lunp->next_lun_avail_time;
        }
    }

    /* update line status */
    mark_line_free(nns, &ppa);

    return 0;
}

static uint64_t ssd_read(NvmNamespace *nns, uint64_t slba, uint16_t nlb, uint64_t stime)
{
    NvmParams *params = nns->params;
    uint64_t lba = slba;
    int nsecs = nlb;
    struct ppa ppa;
    uint64_t start_lpn = lba / params->secs_per_pg;
    uint64_t end_lpn = (lba + nsecs - 1) / params->secs_per_pg;
    uint64_t lpn;
    uint64_t sublat, maxlat = 0;

    if (end_lpn >= params->tt_pgs) {
        femu_err("ssd_read start_lpn=%"PRIu64",tt_pgs=%lld\n", start_lpn, nns->params->tt_pgs);
    }

    /* normal IO read path */
    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
        ppa = get_maptbl_ent(nns, lpn);
        if (!mapped_ppa(&ppa) || !valid_ppa(nns, &ppa)) {
            femu_log("%s,lpn(%" PRId64 ") not mapped to valid ppa!\n", nns->ssdname, lpn);
            // ppa.ppa = lpn;
            // printf("%s,lpn(%" PRId64 ") not mapped to valid ppa\n", nns->ssdname, lpn);
            // printf("Invalid ppa,ch:%d,lun:%d,blk:%d,pl:%d,pg:%d,sec:%d\n",
            // ppa.g.ch, ppa.g.lun, ppa.g.blk, ppa.g.pl, ppa.g.pg, ppa.g.sec);
            continue;
        }

        struct nand_cmd srd;
        srd.type = USER_IO;
        srd.cmd = NAND_READ;
        srd.stime = stime;
        sublat = ssd_advance_status(nns, &ppa, &srd);
        maxlat = MAX(sublat, maxlat);
    }

    // femu_debug("ssd_read: slba %zu, nlb %d, start_lpn %zu, end_lpn %zu, now %zu, stime %zu, maxlat %zu\n", slba, nlb, start_lpn, end_lpn, clock_ns(), stime, maxlat);

    return maxlat;
}

static uint64_t ssd_write(NvmNamespace *nns, uint64_t slba, uint16_t nlb, uint64_t stime)
{
    uint64_t lba = slba;
    NvmParams *params = nns->params;
    int len = nlb;
    uint64_t start_lpn = lba / params->secs_per_pg;
    uint64_t end_lpn = (lba + len - 1) / params->secs_per_pg;
    struct ppa ppa;
    uint64_t lpn;
    uint64_t curlat = 0, maxlat = 0;
    int r;
    // int cache_used = 0;
    // uint64_t nand_max_lat = 0;

    if (end_lpn >= params->tt_pgs) {
        femu_err("ssd_write start_lpn=%"PRIu64",tt_pgs=%lld\n", start_lpn, nns->params->tt_pgs);
    }

    while (should_gc_high(nns)) {
        /* perform GC here until !should_gc(nns) */
        r = do_gc(nns, true);
        if (r == -1)
            break;
    }

    // uint64_t now = clock_ns();
    // struct cache_entry *entry;
    // while ((entry = pqueue_peek(nns->cache_pq))) {
    //     if (now >= entry->time) {
    //         pqueue_pop(nns->cache_pq);
    //         nns->cache_used -= entry->entries;
    //         free(entry);
    //         // femu_log("write cache free left: %d\n", nns->cache_used);
    //     } else {
    //         break;
    //     }
    // }
    // int use_cache = nns->cache_used < (nns->params->write_cache_entries / 2) || (now % 10) < 7;
    // int use_cache = 0;

    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
        ppa = get_maptbl_ent(nns, lpn);
        if (mapped_ppa(&ppa)) {
            /* update old page information first */
            mark_page_invalid(nns, &ppa);
            set_rmap_ent(nns, INVALID_LPN, &ppa);
        }

        /* new write */
        ppa = get_new_page(nns);
        /* update maptbl */
        set_maptbl_ent(nns, lpn, &ppa);
        /* update rmap */
        set_rmap_ent(nns, lpn, &ppa);

        mark_page_valid(nns, &ppa);

        /* need to advance the write pointer here */
        ssd_advance_write_pointer(nns);

        struct nand_cmd swr;
        swr.type = USER_IO;
        swr.cmd = NAND_WRITE;
        swr.stime = stime;
        /* get latency statistics */
        curlat = ssd_advance_status(nns, &ppa, &swr);
        // if (use_cache && nns->cache_used < nns->params->write_cache_entries) {
        //     nand_max_lat = MAX(nand_max_lat, curlat);
        //     cache_used++;
        //     curlat = 20000;
        // }
        maxlat = MAX(curlat, maxlat);
    }

    // if (cache_used) {
    //     // femu_log("write cache used: %d, left: %d\n", cache_used, nns->cache_used);
    //     entry = malloc(sizeof(struct cache_entry));
    //     entry->entries = cache_used;
    //     entry->time = now + nand_max_lat;
    //     pqueue_insert(nns->cache_pq, entry);
    //     nns->cache_used += cache_used;
    // }

    // femu_log("ssd_write: maxlat %zu, used entry %d\n", maxlat, nns->cache_used);

    return maxlat;
}

#ifdef CEMU_THROTTLE_INTERNAL
static uint64_t memory_copy_nvm(NvmeNamespace *ns, NvmeRequest *req, struct memory_copy_queue *memcpy_queue)
#else
static uint64_t memory_copy_nvm(NvmeNamespace *ns, NvmeRequest *req)
#endif
{
    FemuCtrl *n = ns->ctrl;
    void *sdaddr = req->sdaddr;

    // femu_log("memory_copy lat: %lu\n", clock_ns() - req->stat.stime);
    uint64_t copyed = 0;
    uint64_t maxlat = 0;
    femu_debug("memory_copy nr_sres %d\n", req->nr_sres);
    for (int i = 0; i < req->nr_sres; i++) {
        femu_debug("memory_copy i %d\n", i);
        NvmeCopyFormat2 *sre = &req->sres[i].cf2;
        uint64_t slba = le64_to_cpu(sre->slba);
        uint16_t nlb = le16_to_cpu(sre->nlb) + 1;
        uint32_t snsid = le32_to_cpu(sre->snsid);
        uint64_t lat = 0;
        femu_debug("memory_copy sre slba %lu, nlb %u, snsid %u\n", slba, nlb, snsid);
        if (snsid != 1) {
            femu_err("memory_copy: nvm<->fdm snsid != 1!\n");
            return NVME_INVALID_NSID;
        }
        NvmeNamespace *sns = nvme_find_namespace(n, snsid);
        const uint8_t lba_index = NVME_ID_NS_FLBAS_INDEX(sns->id_ns.flbas);
        const uint8_t data_shift = sns->id_ns.lbaf[lba_index].lbads;
        uint64_t nbyte = (uint64_t)nlb << data_shift;
        uint64_t off = slba << data_shift;
        femu_debug("memory_copy off %lu, nbyte %lu, sdaddr 0x%p\n", off, nbyte, sdaddr);
        backend_rw_internal(sns->backend, sdaddr, off, nbyte, req->is_write);
        femu_debug("memory_copy backend_rw_internal finished\n");
#ifdef CEMU_THROTTLE_INTERNAL
        for (int j = 0; j < nlb; j += 32) {
            int req_nlb = MIN(32, nlb - j);
            memcpy_queue->reqs[memcpy_queue->tail].req = req;
            memcpy_queue->reqs[memcpy_queue->tail].slba = slba + j;
            memcpy_queue->reqs[memcpy_queue->tail].nlb = req_nlb;
            memcpy_queue->reqs[memcpy_queue->tail].is_last = (i == req->nr_sres - 1) && (j + req_nlb == nlb);
            memcpy_queue->reqs[memcpy_queue->tail].is_write = req->is_write;
            memcpy_queue->reqs[memcpy_queue->tail].ts = req->stat.stime;
            // femu_log("memory_copy_req: memcpy_queue size %d, i %d, req->nr_sres %d, j %d, req_nlb %d, nlb %d, is_last %d\n", memcpy_queue->size, i, req->nr_sres, j, req_nlb, nlb, memcpy_queue->reqs[memcpy_queue->tail].is_last);
            memcpy_queue->tail = (memcpy_queue->tail + 1) % memcpy_queue->capacity;
            memcpy_queue->size++;
            if (memcpy_queue->size >= memcpy_queue->capacity) {
                femu_err("memory_copy: memcpy_queue full!\n");
                abort();
            }
        }
#else
        if (req->is_write)
            lat = ssd_write(nvm_ns(sns), slba, nlb, req->stat.stime);
        else
            lat = ssd_read(nvm_ns(sns), slba, nlb, req->stat.stime);
#endif
        femu_debug("memory_copy nvm%sfdm: lat %zu, cache left %d, stime %lu, slba %lu, nlb %u, snsid %u, soff %lu, nbyte %lu\n",
                    req->is_write ? "<-" : "->", lat, nvm_ns(sns)->cache_used, req->stat.stime, slba, nlb, snsid, off, nbyte);
        sdaddr += nbyte;
        copyed += nbyte;
        maxlat = MAX(maxlat, lat);
    }
    femu_debug("memory_copy lat at end: %lu\n", clock_ns() - req->stat.stime);
    req->stat.data_size = copyed;
    req->cqe.n.result = copyed;
    req->stat.pcie_lat = n != req->mem_ctrl ? 1 : 0;
    req->stat.opcode = (n != req->mem_ctrl) ? 0x27 : (req->is_write ? 0x24 : 0x23);
    req->status = NVME_SUCCESS;
    femu_debug("memory_copy finished\n");

    // standard hasn't defined the result. use copyed bytes as result
#ifdef CEMU_THROTTLE_INTERNAL
    return 0;
#else
    if (req->mem_ctrl != n) {
        femu_debug("memory copy send cross drive, mem_ctrl %p, n %p\n", req->mem_ctrl, n);
        femu_debug("memory copy send cross drive req %d -> %d to corresponding drive\n", req->mem_ctrl->drive_id, n->drive_id);
        NvmeRequest *mem_req = malloc(sizeof(NvmeRequest));
        memcpy(mem_req, req, sizeof(NvmeRequest));
        mem_req->p2p_req = 1;
        mem_req->stat.rw.nand_lat = maxlat;
        mem_req->stat.reqlat += maxlat;
        mem_req->stat.expire_time += maxlat;
        mem_req->stat.opcode = 0x26;
        if (req->is_write) {
            mem_req->stat.src_drive = req->mem_ctrl->drive_id;
            mem_req->stat.dst_drive = n->drive_id;
        } else {
            mem_req->stat.src_drive = n->drive_id;
            mem_req->stat.dst_drive = req->mem_ctrl->drive_id;
        }
        femu_debug("memory_copy: send cross drive req %d -> %d to corresponding drive\n", mem_req->stat.src_drive, mem_req->stat.dst_drive);
        int rc = femu_ring_enqueue(req->mem_ctrl->to_poller[1], (void *)&mem_req, 1);
        if (rc != 1) {
            femu_err("FTL to_poller enqueue failed\n");
        }
    }
    return maxlat;
#endif
}

static uint64_t get_total_bytes(NvmeNamespace *ns, NvmeRequest *req)
{
    FemuCtrl *n = ns->ctrl;
    uint64_t total_bytes = 0;
    for (int i = 0; i < req->nr_sres; i++) {
        NvmeCopyFormat2 *sre = &req->sres[i].cf2;
        uint16_t nlb = le16_to_cpu(sre->nlb) + 1;
        uint32_t snsid = le32_to_cpu(sre->snsid);
        if (snsid != 1) {
            femu_err("!!memory_copy: nvm<->fdm snsid != 1!\n");
        }
        NvmeNamespace *sns = nvme_find_namespace(n, snsid);
        const uint8_t lba_index = NVME_ID_NS_FLBAS_INDEX(sns->id_ns.flbas);
        const uint8_t data_shift = sns->id_ns.lbaf[lba_index].lbads;
        uint64_t nbyte = (uint64_t)nlb << data_shift;
        total_bytes += nbyte;
    }
    return total_bytes;
}

static void *ftl_thread(void *arg)
{
    NvmeNamespace *ns = (NvmeNamespace *)arg;
    NvmNamespace *nns = nvm_ns(ns);
    NvmeRequest *req = NULL;
    FemuCtrl *n = ns->ctrl;

    NvmeRequest *pending_queue[8192];
    int pending_queue_flag[8192];
    int num_pending = 0;
    uint64_t internal_pcie_rx_next_avail_time = 0;
    uint64_t internal_pcie_tx_next_avail_time = 0;
    uint64_t *internal_pcie_next_avail_time = NULL;
    double internal_bw_gb = (double)(n->internal_bandwidth) / 1024.0;
    uint64_t ts = 0;

    uint64_t lat = 0;
    int rc;
    int i;

    for (i = 0; i < 8192; i++) {
        pending_queue_flag[i] = 0;
        pending_queue[i] = NULL;
    }

    if (n->internal_bandwidth > 0) {
        femu_log("ftl internal bandwidth: %.2lf GB\n", internal_bw_gb);
    } else {
        femu_log("ftl internal bandwidth: no limit\n");
    }

    while (!n->dataplane_started) {
        usleep(100000);
    }

#ifdef CEMU_THROTTLE_INTERNAL
    struct memory_copy_queue memcpy_queue;
    memcpy_queue.size = 0;
    memcpy_queue.capacity = 4096;
    memcpy_queue.reqs = malloc(memcpy_queue.capacity * sizeof(struct memory_copy_req));
    memcpy_queue.head = 0;
    memcpy_queue.tail = 0;
    int cnt = 0;
#endif

    while (1) {
        for (i = 1; i <= n->nr_pollers; i++) {
#ifdef CEMU_THROTTLE_INTERNAL
            int empty = femu_ring_empty(nns->to_ftl[i]);
            if (memcpy_queue.size && (!empty || (empty && cnt++ > 3000))) {
                cnt = 0;
                struct memory_copy_req *mreq = &memcpy_queue.reqs[memcpy_queue.head];
                req = mreq->req;
                if (mreq->is_write) {
                    lat = ssd_write(nvm_ns(ns), mreq->slba, mreq->nlb, mreq->ts);
                } else {
                    lat = ssd_read(nvm_ns(ns), mreq->slba, mreq->nlb, mreq->ts);
                }
                req->stat.rw.nand_lat = MAX(req->stat.rw.nand_lat, lat);
                // femu_log("ftl memory_copy_req: is_last %d, nr_pollers %d\n", mreq->is_last, n->nr_pollers);
                if (mreq->is_last) {
                    req->stat.reqlat += req->stat.rw.nand_lat;
                    req->stat.expire_time += req->stat.rw.nand_lat;
                    if (req->mem_ctrl != n) {
                        NvmeRequest *mem_req = malloc(sizeof(NvmeRequest));
                        memcpy(mem_req, req, sizeof(NvmeRequest));
                        mem_req->p2p_req = 1;
                        mem_req->stat.opcode = 0x26;
                        if (req->is_write) {
                            mem_req->stat.src_drive = req->mem_ctrl->drive_id;
                            mem_req->stat.dst_drive = n->drive_id;
                        } else {
                            mem_req->stat.src_drive = n->drive_id;
                            mem_req->stat.dst_drive = req->mem_ctrl->drive_id;
                        }
                        femu_debug("memory_copy: send cross drive req %d -> %d to corresponding drive\n", mem_req->stat.src_drive, mem_req->stat.dst_drive);
                        int rc = femu_ring_enqueue(req->mem_ctrl->to_poller[1], (void *)&mem_req, 1);
                        if (rc != 1) {
                            femu_err("FTL to_poller enqueue failed\n");
                        }
                    }
                    rc = femu_ring_enqueue(n->to_poller[i], (void *)&req, 1);
                    if (rc != 1) {
                        femu_err("FTL to_poller enqueue failed\n");
                    }
                }
                memcpy_queue.head = (memcpy_queue.head + 1) % memcpy_queue.capacity;
                memcpy_queue.size--;
            }
#endif

            req = NULL;
            int is_pending = 0;
            if (num_pending > 0) {
                int checked = 0;
                ts = clock_ns();
                for (int j = 0; j < 8192; j++) {
                    if (pending_queue_flag[j]) {
                        if (ts >= pending_queue[j]->stat.stime) {
                            req = pending_queue[j];
                            pending_queue[j] = NULL;
                            pending_queue_flag[j] = 0;
                            is_pending = 1;
                            num_pending--;
                            break;
                        }
                        checked++;
                    }
                    if (checked >= num_pending)
                        break;
                }
            }

            if (req == NULL) {
                if (!nns->to_ftl[i] || femu_ring_empty(nns->to_ftl[i]))
                    continue;

                rc = femu_ring_dequeue(nns->to_ftl[i], (void *)&req, 1);
                if (rc != 1) {
                    femu_err("FTL to_ftl dequeue failed\n");
                }
            }

            if (is_pending) {
                lat = memory_copy_nvm(req->ns, req);
                // free(req->sres);
            } else if (req->ns == ns) {
                // nvm namespace
                switch (req->cmd.opcode) {
                case NVME_CMD_WRITE:
                    req->status = nvme_rw(req->ns, &req->cmd, req);
                    lat = ssd_write(nns, req->slba, req->nlb, req->stat.stime);
                    break;
                case NVME_CMD_READ:
                    req->status = nvme_rw(req->ns, &req->cmd, req);
                    lat = ssd_read(nns, req->slba, req->nlb, req->stat.stime);
                    break;
                case NVME_CMD_DSM:
                    lat = 0;
                    break;
                default:
                    lat = 0;
                    break;
                    //femu_err("FTL received unkown request type, ERROR\n");
                }
            } else if (req->cmd.nsid == 2 || req->cmd.nsid == 3) {
                // nsid == 2: normal memory copy
                // nsid == 3: memory copy from indirect compute task
#ifdef CEMU_THROTTLE_INTERNAL
                lat = memory_copy_nvm(req->ns, req, &memcpy_queue);
#else
                if (n->internal_bandwidth == 0) {
                    // no internal bandwidth limit
                    lat = memory_copy_nvm(req->ns, req);
                } else {
                    if (req->is_write) {
                        internal_pcie_next_avail_time = &internal_pcie_tx_next_avail_time;
                    } else {
                        internal_pcie_next_avail_time = &internal_pcie_rx_next_avail_time;
                    }
                    *internal_pcie_next_avail_time = MAX(clock_ns(), *internal_pcie_next_avail_time);
                    req->stat.stime = *internal_pcie_next_avail_time;
                    req->stat.stime += 100000;
                    req->stat.expire_time = req->stat.stime;
                    uint64_t total_bytes = get_total_bytes(req->ns, req);
                    *internal_pcie_next_avail_time = *internal_pcie_next_avail_time + total_bytes / internal_bw_gb;
                    num_pending++;
                    for (int j = 0; j < 8192; j++) {
                        if (!pending_queue_flag[j]) {
                            pending_queue[j] = req;
                            pending_queue_flag[j] = 1;
                            break;
                        }
                    }
                    continue;
                }
#endif
                if (req->cmd.nsid == 2) {
                    // don't free sres belong to indirect compute task (nsid==3)
                    free(req->sres);
                }
#ifdef CEMU_THROTTLE_INTERNAL
                continue;
#endif
            } else {
                femu_err("!!!memory copy: unknown nsid %d\n", req->cmd.nsid);
                abort();
            }

            req->stat.rw.nand_lat = lat;
            req->stat.reqlat += lat;
            req->stat.expire_time += lat;

            if (req->cmd.nsid == 3) {
                // io of indirect job
                femu_ring_enqueue(req->indirect_task.ring, (void *)&req, 1);
                continue;
            }

            rc = femu_ring_enqueue(n->to_poller[i], (void *)&req, 1);
            if (rc != 1) {
                femu_err("FTL to_poller enqueue failed\n");
            }

            /* clean one line if needed (in the background) */
            if (should_gc(nns)) {
                do_gc(nns, false);
            }
        }
    }

    return NULL;
}

