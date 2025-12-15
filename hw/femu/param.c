#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cjson/cJSON.h>
#include "param.h"
#include "nvme.h"

typedef bool (*line_filter_t)(const char *, size_t, void *);

static int info_reader(const char *s, bool is_cmd, line_filter_t f, void *ctx)
{
    if (f == NULL || s == NULL) {
        return -EINVAL;
    }

    FILE *fp = NULL;
    if (is_cmd) {
        fp = popen(s, "r");
    } else {
        fp = fopen(s, "r");
    }
    if (fp == NULL) {
        perror("open()");
        return -errno;
    }

    char *line = NULL;
    size_t n = 0;
    int ret;
    while ((ret = getline(&line, &n, fp)) > 0) {
        if (f(line, n, ctx)) {
            break;
        }
        free(line);
        line = NULL;
    }

    if (is_cmd) {
        pclose(fp);
    } else {
        fclose(fp);
    }

    return ret;
}

static bool cpu_model_name_filter(const char *line, size_t n, void *ctx)
{
    if (strstr(line, "model name")) {
        sscanf(line, "model name\t: %[^\n]", (char *)ctx);
        return true;
    }
    return false;
}

static bool mem_total_size_filter(const char *line, size_t n, void *ctx)
{
    if (strstr(line, "MemTotal")) {
        sscanf(line, "MemTotal: %lu kB", (uint64_t *)ctx);
        return true;
    }
    return false;
}

static bool linux_version_filter(const char *line, size_t n, void *ctx)
{
    memcpy(ctx, line, n);
    return true;
}

static void add_env_info(cJSON *base)
{
    cJSON *json = cJSON_AddObjectToObject(base, "env_info");

    char linux_info[256];
    char cpu_info[64];
    uint64_t total_mem_size_in_kb = 0;
    memset(linux_info, 0, sizeof(linux_info));
    memset(cpu_info, 0, sizeof(cpu_info));

    info_reader("/proc/cpuinfo", false, cpu_model_name_filter, cpu_info);
    info_reader("/proc/meminfo", false, mem_total_size_filter,
                            &total_mem_size_in_kb);
    info_reader("uname -a", true, linux_version_filter, linux_info);

    ENCODE_PARAM(json, "linux_info", String, linux_info);
    ENCODE_PARAM(json, "cpu_info", String, cpu_info);
    ENCODE_PARAM(json, "total_mem_size_in_mb", Number, (long long)(total_mem_size_in_kb / 1024.0));
}

static void save_config(cJSON *base, FemuCtrl *n)
{
    char *json = cJSON_Print(base);

    static char formatted_time[20] = "";
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

    pthread_mutex_lock(&lock);
    if (formatted_time[0] == '\0') {
        time_t now = time(NULL);
        struct tm *tm_now = localtime(&now);
        strftime(formatted_time, sizeof(formatted_time), "%Y%m%d_%H%M%S", tm_now);
    }
    pthread_mutex_unlock(&lock);

    char dump_file[64];
    snprintf(dump_file, 64, "cemu_dump_config_%s_%d.json", formatted_time, n->drive_id);
    FILE *fp = fopen(dump_file, "w");
    fwrite(json, strlen(json), 1, fp);
    femu_log("dump_config:\n%s\n", json);
    fclose(fp);
    free(json);
}

static void add_femu_prop(cJSON *base, const struct FemuCtrl *femu_ctrl)
{
    cJSON *json = cJSON_AddObjectToObject(base, "femu_property");

    ENCODE_PARAM(json, "serial", String, femu_ctrl->serial);
    ENCODE_PARAM(json, "entries", Number, femu_ctrl->max_q_ents);
    ENCODE_PARAM(json, "cq_poller_enabled", Number, femu_ctrl->cq_poller_enabled);
    ENCODE_PARAM(json, "max_cqes", Number, femu_ctrl->max_cqes);
    ENCODE_PARAM(json, "stride", Number, femu_ctrl->db_stride);
    ENCODE_PARAM(json, "acl", Number, femu_ctrl->acl);
    ENCODE_PARAM(json, "mdts", Number, femu_ctrl->mdts);
    ENCODE_PARAM(json, "cqr", Number, femu_ctrl->cqr);
    ENCODE_PARAM(json, "vwc", Number, femu_ctrl->vwc);
    ENCODE_PARAM(json, "intc", Number, femu_ctrl->intc);
    ENCODE_PARAM(json, "intc_thresh", Number, femu_ctrl->intc_thresh);
    ENCODE_PARAM(json, "intc_time", Number, femu_ctrl->intc_time);
    ENCODE_PARAM(json, "ms", Number, femu_ctrl->ms);
    ENCODE_PARAM(json, "ms_max", Number, femu_ctrl->ms_max);
    ENCODE_PARAM(json, "dlfeat", Number, femu_ctrl->dlfeat);
    ENCODE_PARAM(json, "mpsmin", Number, femu_ctrl->mpsmin);
    ENCODE_PARAM(json, "mpsmax", Number, femu_ctrl->mpsmax);
    ENCODE_PARAM(json, "nlbaf", Number, femu_ctrl->nlbaf);
    ENCODE_PARAM(json, "lba_index", Number, femu_ctrl->lba_index);
    ENCODE_PARAM(json, "extended", Number, femu_ctrl->extended);
    ENCODE_PARAM(json, "dpc", Number, femu_ctrl->dpc);
    ENCODE_PARAM(json, "dps", Number, femu_ctrl->dps);
    ENCODE_PARAM(json, "mc", Number, femu_ctrl->mc);
    ENCODE_PARAM(json, "meta", Number, femu_ctrl->meta);
    ENCODE_PARAM(json, "cmbsz", Number, femu_ctrl->cmbsz);
    ENCODE_PARAM(json, "cmbloc", Number, femu_ctrl->cmbloc);
    ENCODE_PARAM(json, "oacs", Number, femu_ctrl->oacs);
    ENCODE_PARAM(json, "oncs", Number, femu_ctrl->oncs);
    ENCODE_PARAM(json, "vid", Number, femu_ctrl->vid);
    ENCODE_PARAM(json, "did", Number, femu_ctrl->did);
    ENCODE_PARAM(json, "femu_mode", Number, femu_ctrl->femu_mode);
    ENCODE_PARAM(json, "flash_type", Number, femu_ctrl->flash_type);
    ENCODE_PARAM(json, "pcie_bandwidth", Number, femu_ctrl->pcie_bandwidth);
    ENCODE_PARAM(json, "pcie_propagation_delay", Number, femu_ctrl->pcie_propagation_delay);
    ENCODE_PARAM(json, "internal_bandwidth", Number, femu_ctrl->internal_bandwidth);
    ENCODE_PARAM(json, "lver", Number, femu_ctrl->lver);
    ENCODE_PARAM(json, "lsec_size", Number, femu_ctrl->oc_params.sec_size);
    ENCODE_PARAM(json, "lsecs_per_pg", Number, femu_ctrl->oc_params.secs_per_pg);
    ENCODE_PARAM(json, "lpgs_per_blk", Number, femu_ctrl->oc_params.pgs_per_blk);
    ENCODE_PARAM(json, "lmax_sec_per_rq", Number, femu_ctrl->oc_params.max_sec_per_rq);
    ENCODE_PARAM(json, "lnum_ch", Number, femu_ctrl->oc_params.num_ch);
    ENCODE_PARAM(json, "lnum_lun", Number, femu_ctrl->oc_params.num_lun);
    ENCODE_PARAM(json, "lnum_pln", Number, femu_ctrl->oc_params.num_pln);
    ENCODE_PARAM(json, "lmetasize", Number, femu_ctrl->oc_params.sos);
    ENCODE_PARAM(json, "zns_num_ch", Number, femu_ctrl->zns_params.zns_num_ch);
    ENCODE_PARAM(json, "zns_num_lun", Number, femu_ctrl->zns_params.zns_num_lun);
    ENCODE_PARAM(json, "zns_read", Number, femu_ctrl->zns_params.zns_read);
    ENCODE_PARAM(json, "zns_write", Number, femu_ctrl->zns_params.zns_write);
}

void dump_config(FemuCtrl *n)
{
    cJSON* dump = cJSON_CreateObject();

    // dump namespaces
    cJSON *namespaces = cJSON_AddArrayToObject(dump, "namespaces");
    for (int i = 0; i < n->num_namespaces; i++) {
        NvmeNamespace *ns = &n->namespaces[i];
        cJSON *namespace = cJSON_CreateObject();
        cJSON_AddStringToObject(namespace, "type", nvme_csi_str(ns->csi));
        cJSON_AddNumberToObject(namespace, "size", ns->size);
        if (ns->ops->encode_params) {
            cJSON *params = cJSON_AddObjectToObject(namespace, "parameters");
            ns->ops->encode_params(params, ns);
        }
        cJSON_AddItemToArray(namespaces, namespace);
    }

    add_env_info(dump);
    add_femu_prop(dump, n);
    save_config(dump, n);
    cJSON_Delete(dump);
}

/********************** DECODE **********************/

static cJSON *load_config(const char *config_path)
{
    FILE *fp = fopen(config_path, "r");
    if (fp == NULL) {
        femu_err("fail to open config file %s\n", config_path);
        return NULL;
    }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buffer = malloc(size + 1);
    if (buffer == NULL) {
        femu_err("fail to allocate buffer for config file %s\n", config_path);
        return NULL;
    }
    long ret = fread(buffer, 1, size, fp);
    assert(ret == size);
    buffer[size] = 0;
    fclose(fp);
    cJSON *config = cJSON_Parse(buffer);
    if (config == NULL) {
        femu_err("fail to parse config file %s, because %s\n", config_path,
                         cJSON_GetErrorPtr());
    }
    free(buffer);
    return config;
}

static int parse_femu_parameters(const cJSON *param, FemuCtrl *params)
{
    DECODE_PARAM(param, "serial", String, params->serial, NULL);
    DECODE_PARAM(param, "queues", Number, params->nr_io_queues, 8);
    DECODE_PARAM(param, "entries", Number, params->max_q_ents, 0x7ff);
    DECODE_PARAM(param, "multipoller_enabled", Number, params->multipoller_enabled, 0);
    DECODE_PARAM(param, "cq_poller_enabled", Number, params->cq_poller_enabled, 0);
    DECODE_PARAM(param, "max_cqes", Number, params->max_cqes, 4);
    DECODE_PARAM(param, "max_sqes", Number, params->max_sqes, 6);
    DECODE_PARAM(param, "stride", Number, params->db_stride, 0);
    DECODE_PARAM(param, "aerl", Number, params->aerl, 3);
    DECODE_PARAM(param, "acl", Number, params->acl, 3);
    DECODE_PARAM(param, "elpe", Number, params->elpe, 3);
    DECODE_PARAM(param, "mdts", Number, params->mdts, 5);
    DECODE_PARAM(param, "cqr", Number, params->cqr, 1);
    DECODE_PARAM(param, "vwc", Number, params->vwc, 0);
    DECODE_PARAM(param, "intc", Number, params->intc, 0);
    DECODE_PARAM(param, "intc_thresh", Number, params->intc_thresh, 0);
    DECODE_PARAM(param, "intc_time", Number, params->intc_time, 0);
    DECODE_PARAM(param, "ms", Number, params->ms, 16);
    DECODE_PARAM(param, "ms_max", Number, params->ms_max, 64);
    DECODE_PARAM(param, "dlfeat", Number, params->dlfeat, 1);
    DECODE_PARAM(param, "mpsmin", Number, params->mpsmin, 0);
    DECODE_PARAM(param, "mpsmax", Number, params->mpsmax, 0);
    DECODE_PARAM(param, "nlbaf", Number, params->nlbaf, 5);
    DECODE_PARAM(param, "lba_index", Number, params->lba_index, 0);
    DECODE_PARAM(param, "extended", Number, params->extended, 0);
    DECODE_PARAM(param, "dpc", Number, params->dpc, 0);
    DECODE_PARAM(param, "dps", Number, params->dps, 0);
    DECODE_PARAM(param, "mc", Number, params->mc, 0);
    DECODE_PARAM(param, "meta", Number, params->meta, 0);
    DECODE_PARAM(param, "cmbsz", Number, params->cmbsz, 0);
    DECODE_PARAM(param, "cmbloc", Number, params->cmbloc, 0);
    DECODE_PARAM(param, "oacs", Number, params->oacs, NVME_OACS_FORMAT);
    DECODE_PARAM(param, "oncs", Number, params->oncs, NVME_ONCS_DSM);
    DECODE_PARAM(param, "vid", Number, params->vid, PCI_VENDOR_ID_INTEL);
    DECODE_PARAM(param, "did", Number, params->did, 0x8888);
    DECODE_PARAM(param, "femu_mode", Number, params->femu_mode, FEMU_NOSSD_MODE);
    DECODE_PARAM(param, "flash_type", Number, params->flash_type, MLC);
    DECODE_PARAM(param, "pcie_bandwidth", Number, params->pcie_bandwidth, 4); // in G
    DECODE_PARAM(param, "pcie_propagation_delay", Number, params->pcie_propagation_delay, 200); // in n
    DECODE_PARAM(param, "internal_bandwidth", Number, params->internal_bandwidth, 0); // in MB, 0 means no limit
    DECODE_PARAM(param, "lver", Number, params->lver, 0x2);
    DECODE_PARAM(param, "lsec_size", Number, params->oc_params.sec_size, 4096);
    DECODE_PARAM(param, "lsecs_per_pg", Number, params->oc_params.secs_per_pg, 4);
    DECODE_PARAM(param, "lpgs_per_blk", Number, params->oc_params.pgs_per_blk, 512);
    DECODE_PARAM(param, "lmax_sec_per_rq", Number, params->oc_params.max_sec_per_rq, 64);
    DECODE_PARAM(param, "lnum_ch", Number, params->oc_params.num_ch, 2);
    DECODE_PARAM(param, "lnum_lun", Number, params->oc_params.num_lun, 8);
    DECODE_PARAM(param, "lnum_pln", Number, params->oc_params.num_pln, 2);
    DECODE_PARAM(param, "lmetasize", Number, params->oc_params.sos, 16);
    DECODE_PARAM(param, "zns_num_ch", Number, params->zns_params.zns_num_ch, 2);
    DECODE_PARAM(param, "zns_num_lun", Number, params->zns_params.zns_num_lun, 4);
    DECODE_PARAM(param, "zns_read", Number, params->zns_params.zns_read, 40000);
    DECODE_PARAM(param, "zns_write", Number, params->zns_params.zns_write, 200000);

    params->nr_pollers = params->multipoller_enabled ? params->nr_io_queues : 1;

    return 0;
}

static ssize_t parse_size_str(const char *str)
{
    int len = strlen(str);
    ssize_t factor = 1;

    char *end;
    ssize_t size = strtol(str, &end, 10);

    const char *p = str + len - 1;
    while (p != end - 1) {
        char c = tolower(*p); // 将单位字符转换为小写
        switch(c) {
            case 'k': factor = 1024; break; // 1KB = 1024字节
            case 'm': factor = 1024 * 1024; break; // 1MB = 1024 * 1024字节
            case 'g': factor = 1024 * 1024 * 1024; break; // 1GB = 1024 * 1024 * 1024字节
            case 'b': factor = 1; break;
            default:
                femu_err("parse_size_str: invalid size unit!\n");
                return -1;
        }
        p--;
    }

    return size * factor;
}

int parse_config(FemuCtrl *n) {
    int status = 0;
    const cJSON *namespace;
    int nsid = 0;

    femu_debug("parse config file: %s...\n", n->config_file);
    cJSON *config_json = load_config(n->config_file);
    if (config_json == NULL) {
        const char *error_ptr = cJSON_GetErrorPtr();
        femu_err("parse config_file %s error!", n->config_file);
        if (error_ptr != NULL) {
            femu_err(" error before: %s", error_ptr);
        }
        femu_err("\n");
        abort();
    }

    // FEMU paramter
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(config_json, "parameters");
    parse_femu_parameters(params, n);

    // NVMe namespaces
    const cJSON *namespaces = cJSON_GetObjectItemCaseSensitive(config_json, "namespaces");
    if (namespaces == NULL || !cJSON_IsArray(namespaces)) {
        femu_err("parse_config: namespaces error!\n");
        abort();
    }
    n->num_namespaces = cJSON_GetArraySize(namespaces);
    femu_debug("num_namespaces = %d\n", n->num_namespaces);
    n->namespaces = g_malloc0(sizeof(NvmeNamespace) * n->num_namespaces);
    cJSON_ArrayForEach(namespace, namespaces) {
        cJSON *type = cJSON_GetObjectItemCaseSensitive(namespace, "type");
        if (!cJSON_IsString(type) || (type->valuestring == NULL)) {
            femu_err("parse_config: namespaces.type error!\n");
            abort();
        }

        struct NvmeNamespace *ns = &n->namespaces[nsid++];

        // parse namespace type
        ns->csi = nvme_csi_from_str(type->valuestring);
        if (ns->csi >= NVME_CSI_MAX) {
            femu_err("parse_config: namespaces.type error!\n");
            abort();
        }
        ns->ops = command_set_ops[ns->csi];

        // parse namespace size
        ssize_t ns_size = 0;
        if (cJSON_HasObjectItem(namespace, "size")) {
            cJSON *size = cJSON_GetObjectItemCaseSensitive(namespace, "size");
            if (cJSON_IsNumber(size)) {
                ns_size = size->valueint;
            } else if (cJSON_IsString(size)) {
                ns_size = parse_size_str(size->valuestring);
            } else {
                femu_err("parse_config: namespace.size error!\n");
                abort();
            }
        }

        // validate
        if (ns->csi != NVME_CSI_COMPUTE && ns_size == 0) {
            femu_err("parse_config: namespace.size is zero!\n");
            abort();
        }

        ns->id = nsid;
        ns->size = ns_size;

        // parse namespace parameters
        if (ns->ops->decode_params) {
            cJSON *param = cJSON_GetObjectItemCaseSensitive(namespace, "parameters");
            ns->ops->decode_params(param, ns);
        }
    }

    femu_debug("parse_config: status = %d\n", status);
    cJSON_Delete(config_json);
    return status;
}