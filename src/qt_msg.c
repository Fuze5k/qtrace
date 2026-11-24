/*
 * Names of QNX messages, so that a MsgSend() can be shown as the POSIX call
 * that produced it. Resource-manager messages start with a 16-bit type and a
 * 16-bit subtype or combine length; the kernel trace records the first 12
 * bytes of every message in wide mode, which is enough for the type, the
 * subtype and the first argument (byte count, devctl command, ...).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "qt_engine.h"
#include "qt_msg.h"

#if defined(__QNX__)
#include <sys/iomsg.h>
/* The values below are also needed on hosts without the SDP headers (unit
 * tests, decoding on Linux). Make sure they match the real ones (cast: the SDP's
 * values and ours are different enum types). */
_Static_assert((int)_IO_CONNECT == (int)QT_IO_CONNECT, "_IO_CONNECT");
_Static_assert((int)_IO_READ == (int)QT_IO_READ, "_IO_READ");
_Static_assert((int)_IO_WRITE == (int)QT_IO_WRITE, "_IO_WRITE");
_Static_assert((int)_IO_STAT == (int)QT_IO_STAT, "_IO_STAT");
_Static_assert((int)_IO_NOTIFY == (int)QT_IO_NOTIFY, "_IO_NOTIFY");
_Static_assert((int)_IO_DEVCTL == (int)QT_IO_DEVCTL, "_IO_DEVCTL");
_Static_assert((int)_IO_PATHCONF == (int)QT_IO_PATHCONF, "_IO_PATHCONF");
_Static_assert((int)_IO_LSEEK == (int)QT_IO_LSEEK, "_IO_LSEEK");
_Static_assert((int)_IO_CHMOD == (int)QT_IO_CHMOD, "_IO_CHMOD");
_Static_assert((int)_IO_CHOWN == (int)QT_IO_CHOWN, "_IO_CHOWN");
_Static_assert((int)_IO_UTIME == (int)QT_IO_UTIME, "_IO_UTIME");
_Static_assert((int)_IO_OPENFD == (int)QT_IO_OPENFD, "_IO_OPENFD");
_Static_assert((int)_IO_FDINFO == (int)QT_IO_FDINFO, "_IO_FDINFO");
_Static_assert((int)_IO_LOCK == (int)QT_IO_LOCK, "_IO_LOCK");
_Static_assert((int)_IO_SPACE == (int)QT_IO_SPACE, "_IO_SPACE");
_Static_assert((int)_IO_SHUTDOWN == (int)QT_IO_SHUTDOWN, "_IO_SHUTDOWN");
_Static_assert((int)_IO_MMAP == (int)QT_IO_MMAP, "_IO_MMAP");
_Static_assert((int)_IO_MSG == (int)QT_IO_MSG, "_IO_MSG");
_Static_assert((int)_IO_DUP == (int)QT_IO_DUP, "_IO_DUP");
_Static_assert((int)_IO_CLOSE == (int)QT_IO_CLOSE, "_IO_CLOSE");
_Static_assert((int)_IO_SYNC == (int)QT_IO_SYNC, "_IO_SYNC");
_Static_assert((int)_IO_CONNECT_OPEN == (int)QT_IO_CONNECT_OPEN, "_IO_CONNECT_OPEN");
_Static_assert((int)_IO_CONNECT_UNLINK == (int)QT_IO_CONNECT_UNLINK, "_IO_CONNECT_UNLINK");
_Static_assert((int)_IO_CONNECT_RENAME == (int)QT_IO_CONNECT_RENAME, "_IO_CONNECT_RENAME");
_Static_assert((int)_IO_CONNECT_MKNOD == (int)QT_IO_CONNECT_MKNOD, "_IO_CONNECT_MKNOD");
_Static_assert((int)_IO_CONNECT_READLINK == (int)QT_IO_CONNECT_READLINK, "_IO_CONNECT_READLINK");
_Static_assert((int)_IO_CONNECT_LINK == (int)QT_IO_CONNECT_LINK, "_IO_CONNECT_LINK");
_Static_assert((int)_IO_CONNECT_MOUNT == (int)QT_IO_CONNECT_MOUNT, "_IO_CONNECT_MOUNT");
#endif

/* procnto's own messages: types below 0x100, from <sys/sysmsg.h>, <sys/procmsg.h>,
 * <sys/memmsg.h> and <sys/pathmsg.h>. Shown by the POSIX function that sends them
 * where there is one, else by the SDP constant's name. */
#define QT_SYSMSGS(X) \
    X(_SYS_CONF, 0x00, "sysconf") X(_SYS_CMD, 0x01, "sys_cmd") X(_SYS_LOG, 0x02, "sys_log") \
    X(_SYS_VENDOR, 0x03, "sys_vendor") X(_SYS_XCALL, 0x04, "sys_xcall") \
    X(_SYS_GETPAGESIZES, 0x05, "getpagesizes") X(_SYS_TRACE, 0x06, "sys_trace") \
    X(_PROC_SPAWN, 0x10, "spawn") X(_PROC_WAIT, 0x11, "wait") X(_PROC_FORK, 0x12, "fork") \
    X(_PROC_GETSETID, 0x13, "proc_getsetid") X(_PROC_SETPGID, 0x14, "setpgid") \
    X(_PROC_UMASK, 0x15, "umask") X(_PROC_SESSION, 0x17, "proc_session") \
    X(_PROC_DAEMON, 0x18, "procmgr_daemon") X(_PROC_EVENT, 0x19, "procmgr_event") \
    X(_PROC_RESOURCE, 0x1a, "proc_resource") X(_PROC_POSIX_SPAWN, 0x1b, "posix_spawn") \
    X(_PROC_POSIX_SPAWNP, 0x1c, "posix_spawnp") X(_PROC_VALUE, 0x1d, "procmgr_value") \
    X(_PROC_TIMER_TOLERANCE, 0x1e, "proc_timer_tolerance") X(_PROC_MAC, 0x20, "proc_mac") \
    X(_PROC_SERVMON, 0x21, "proc_servmon") X(_PROC_GETPID64, 0x22, "proc_getpid64") \
    X(_PROC_GETNAME, 0x23, "proc_getname") X(_PROC_GET_ALL_PID, 0x24, "proc_get_all_pid") \
    X(_PROC_DEBUG, 0x25, "proc_debug") X(_PROC_COREDUMP, 0x26, "proc_coredump") \
    X(_PROC_GET_ALL_CHILD_PID, 0x27, "proc_get_all_child_pid") \
    X(_MEM_MAP, 0x40, "mmap") X(_MEM_CTRL, 0x41, "mem_ctrl") X(_MEM_INFO, 0x42, "mem_info") \
    X(_MEM_OFFSET, 0x43, "mem_offset") X(_MEM_DEBUG_INFO, 0x44, "mem_debug_info") \
    X(_MEM_SWAP, 0x45, "mem_swap") X(_MEM_PMEM_ADD, 0x46, "mem_pmem_add") X(_MEM_PEER, 0x47, "mem_peer") \
    X(_MEM_DEBUG_CMD, 0x48, "mem_debug_cmd") X(_MEM_DEBUG_INFO64, 0x49, "mem_debug_info64") \
    X(_MEM_OBJ_INFO, 0x4a, "mem_obj_info") X(_MEM_SHMEM_HANDLE, 0x4b, "mem_shmem_handle") \
    X(_MEM_SHMEM_REVOKE, 0x4c, "mem_shmem_revoke") X(_MEM_AS_INFO, 0x4d, "mem_as_info") \
    X(_MEM_MAP_INFO, 0x4e, "mem_map_info") X(_MEM_READ_MEM, 0x4f, "mem_read_mem") \
    X(_PATH_RESOLVE, 0x70, "path_resolve") X(_PATH_CHDIR, 0x71, "chdir") \
    X(_PATH_CHROOT, 0x72, "chroot") X(_PATH_TRUST, 0x73, "path_trust") X(_PATH_OPENDIR, 0x74, "opendir")

/* File types an io_connect message asks for (_FTYPE_* in <sys/ftype.h>, from
 * _FTYPE_LINK on; _FTYPE_ANY and _FTYPE_FILE are plain path lookups). */
static const char *const ftype_names[] = {
    NULL, NULL, "link", "symlink", "pipe", "shmem", "mqueue", "socket", "sem",
    "photon", "dumper", "mount", "name", "tymem", "mq",
};

const char *qt_ftype_name(uint32_t ftype)
{
    return ftype < sizeof ftype_names / sizeof ftype_names[0] ? ftype_names[ftype] : NULL;
}

/* _MEM_CTRL subtypes, in <sys/memmsg.h> order. */
static const char *const mem_ctrl_names[] = {
    "munmap", "mprotect", "msync", "mlockall", "munlockall", "mlock", "munlock", "madvise",
};

#if defined(__QNX__)
#include <sys/sysmsg.h>
#include <sys/procmsg.h>
#include <sys/memmsg.h>
#include <sys/pathmsg.h>
#define QT_CHECK_SYSMSG(c, v, n) _Static_assert((int)(c) == (v), #c);
QT_SYSMSGS(QT_CHECK_SYSMSG)
_Static_assert(_MEM_CTRL_UNMAP == 0 && _MEM_CTRL_ADVISE == 7, "_MEM_CTRL subtypes");
#include <sys/ftype.h>
_Static_assert(_FTYPE_LINK == 2 && _FTYPE_PIPE == 4 && _FTYPE_SOCKET == 7 && _FTYPE_MQ == 14, "_FTYPE_*");
#endif

const char *qt_sysmsg_name(uint32_t msg0)
{
    uint16_t type = (uint16_t)(msg0 & 0xffff), sub = (uint16_t)(msg0 >> 16);
#define QT_SYSMSG_CASE(c, v, n) case v: return n;
    if (type == 0x41 && sub < sizeof mem_ctrl_names / sizeof mem_ctrl_names[0])
        return mem_ctrl_names[sub];
    switch (type) {
    QT_SYSMSGS(QT_SYSMSG_CASE)
    default: return NULL;
    }
}

static const struct { uint16_t type; const char *name; } io_names[] = {
    { QT_IO_CONNECT,  "connect"   }, { QT_IO_READ,     "read"      },
    { QT_IO_WRITE,    "write"     }, { QT_IO_STAT,     "fstat"     },
    { QT_IO_NOTIFY,   "notify"    }, { QT_IO_DEVCTL,   "devctl"    },
    { QT_IO_PATHCONF, "fpathconf" }, { QT_IO_LSEEK,    "lseek"     },
    { QT_IO_CHMOD,    "fchmod"    }, { QT_IO_CHOWN,    "fchown"    },
    { QT_IO_UTIME,    "futime"    }, { QT_IO_OPENFD,   "openfd"    },
    { QT_IO_FDINFO,   "fdinfo"    }, { QT_IO_LOCK,     "lock"      },
    { QT_IO_SPACE,    "space"     }, { QT_IO_SHUTDOWN, "shutdown"  },
    { QT_IO_MMAP,     "mmap"      }, { QT_IO_MSG,      "io_msg"    },
    { QT_IO_DUP,      "dup"       }, { QT_IO_CLOSE,    "close"     },
    { QT_IO_SYNC,     "fsync"     },
};

static const char *const connect_names[] = {
    [QT_IO_CONNECT_COMBINE]      = "combine",
    [QT_IO_CONNECT_COMBINE_SAVE] = "combine",
    [QT_IO_CONNECT_OPEN]         = "open",
    [QT_IO_CONNECT_UNLINK]       = "unlink",
    [QT_IO_CONNECT_RENAME]       = "rename",
    [QT_IO_CONNECT_MKNOD]        = "mknod",
    [QT_IO_CONNECT_READLINK]     = "readlink",
    [QT_IO_CONNECT_LINK]         = "link",
    [QT_IO_CONNECT_UNBLOCK]      = "unblock",
    [QT_IO_CONNECT_MOUNT]        = "mount",
};

const char *qt_msg_name(uint32_t msg0)
{
    uint16_t type = (uint16_t)(msg0 & 0xffff), sub = (uint16_t)(msg0 >> 16);
    size_t i;

    if (type == QT_IO_CONNECT) {
        if (sub < sizeof connect_names / sizeof connect_names[0] && connect_names[sub])
            return connect_names[sub];
        return "connect";
    }
    for (i = 0; i < sizeof io_names / sizeof io_names[0]; i++)
        if (io_names[i].type == type)
            return io_names[i].name;
    return NULL;
}

/* devctl() commands: direction(2) | size(14) | class(8) | number(8) */
static const char *const dcmd_class[] = {
    "?", "ALL", "FSYS", "CHR", "NET", "MISC", "IP", "MIXER", "PROC", "MEM",
    "INPUT", "PHOTON", "CAM", "USB", "MEDIA", "CAM_SIM",
};

void qt_dcmd_describe(uint32_t dcmd, char *buf, size_t len)
{
    unsigned cls = (dcmd >> 8) & 0xff, num = dcmd & 0xff, size = (dcmd >> 16) & 0x3fff;
    static const char *const dir[] = { "", "from", "to", "tofrom" };
    if (cls < sizeof dcmd_class / sizeof dcmd_class[0])
        snprintf(buf, len, "DCMD_%s #%u%s%s%s", dcmd_class[cls], num,
                 size ? ", " : "", size ? dir[dcmd >> 30] : "", size ? " dev" : "");
    else
        snprintf(buf, len, "dcmd 0x%08x", (unsigned)dcmd);
}
