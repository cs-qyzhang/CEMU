#include <stdatomic.h>
#include "../nvme.h"

static int nop_io_cmd(NvmeNamespace *ns, NvmeCmd *cmd, NvmeRequest *req)
{
    switch (cmd->opcode) {
    case NVME_CMD_READ:
    case NVME_CMD_WRITE:
        req->status = nvme_rw(ns, cmd, req);
        return 1;
    default:
        req->status = NVME_INVALID_OPCODE | NVME_DNR;
        return 1;
    }
}

CommandSetOps nop_command_set_ops = {
    .state            = NULL,
    .init             = NULL,
    .exit             = NULL,
    .rw_check_req     = NULL,
    .admin_cmd        = NULL,
    .io_cmd           = nop_io_cmd,
    .get_log          = NULL,
};
