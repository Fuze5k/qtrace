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
