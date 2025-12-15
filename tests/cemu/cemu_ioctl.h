/* Userspace CEMU ioctl ABI
 *
 * Keep in sync with the kernel UAPI header:
 *   /home/qyzhang/cemu/linux-cemu/include/uapi/linux/cemu_ioctl.h
 */
#ifndef _CEMU_TEST_IOCTL_H
#define _CEMU_TEST_IOCTL_H

#include <stdint.h>
#include <sys/ioctl.h>

#define CEMU_IOCTL_MAGIC  'C'

/* IOCTL_CEMU_DOWNLOAD / IOCTL_CEMU_UNLOAD / ACTIVATE / DEACTIVATE argument */
struct ioctl_download {
    const char *name;
    void       *addr;
    int32_t     size;
    int32_t     ptype;
    int32_t     runtime;
    int32_t     runtime_scale;
    int32_t     jit;
    int32_t     indirect;
    int32_t     pind;   /* out for DOWNLOAD */
};

/* IOCTL_CEMU_EXECUTE argument */
struct ioctl_execute {
    uint64_t    cparam1;
    uint64_t    cparam2;
    int        *memory_fd;
    void       *buffer;
    uint16_t    nr_fd;
    uint16_t    buffer_len;
    uint16_t    pind;
    uint16_t    rsid;
    uint32_t    runtime;
};

/* IOCTL_CEMU_CREATE_MRS / IOCTL_CEMU_DELETE_MRS argument */
struct ioctl_create_mrs {
    int         nr_fd;
    int        *fd;     /* fd array of FDMFS */
    long long  *off;    /* offset array */
    long long  *size;   /* size array */
    uint16_t    rsid;   /* out for CREATE_MRS / in for DELETE_MRS */
};

#define IOCTL_CEMU_DOWNLOAD    _IOWR(CEMU_IOCTL_MAGIC, 0x00, struct ioctl_download)
#define IOCTL_CEMU_UNLOAD      _IOW (CEMU_IOCTL_MAGIC, 0x01, struct ioctl_download)
#define IOCTL_CEMU_ACTIVATE    _IOW (CEMU_IOCTL_MAGIC, 0x02, struct ioctl_download)
#define IOCTL_CEMU_DEACTIVATE  _IOW (CEMU_IOCTL_MAGIC, 0x03, struct ioctl_download)
#define IOCTL_CEMU_EXECUTE     _IOW (CEMU_IOCTL_MAGIC, 0x04, struct ioctl_execute)
#define IOCTL_CEMU_CREATE_MRS  _IOWR(CEMU_IOCTL_MAGIC, 0x05, struct ioctl_create_mrs)
#define IOCTL_CEMU_DELETE_MRS  _IOW (CEMU_IOCTL_MAGIC, 0x06, struct ioctl_create_mrs)

#endif /* _CEMU_TEST_IOCTL_H */
