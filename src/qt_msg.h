/* SPDX-License-Identifier: Apache-2.0 */
#ifndef QT_MSG_H
#define QT_MSG_H

#include <stddef.h>
#include <stdint.h>

/* Message types from <sys/iomsg.h>; checked against the SDP in qt_msg.c. */
enum {
    QT_IO_CONNECT = 0x100, QT_IO_READ, QT_IO_WRITE, QT_IO_RSVD_CLOSE_OCB,
    QT_IO_STAT, QT_IO_NOTIFY, QT_IO_DEVCTL, QT_IO_RSVD_UNBLOCK, QT_IO_PATHCONF,
    QT_IO_LSEEK, QT_IO_CHMOD, QT_IO_CHOWN, QT_IO_UTIME, QT_IO_OPENFD,
    QT_IO_FDINFO, QT_IO_LOCK, QT_IO_SPACE, QT_IO_SHUTDOWN, QT_IO_MMAP,
    QT_IO_MSG, QT_IO_RSVD, QT_IO_DUP, QT_IO_CLOSE, QT_IO_RSVD_LOCK_OCB,
    QT_IO_RSVD_UNLOCK_OCB, QT_IO_SYNC
};
enum {
    QT_IO_CONNECT_COMBINE, QT_IO_CONNECT_COMBINE_SAVE, QT_IO_CONNECT_OPEN,
    QT_IO_CONNECT_UNLINK, QT_IO_CONNECT_RENAME, QT_IO_CONNECT_MKNOD,
    QT_IO_CONNECT_READLINK, QT_IO_CONNECT_LINK, QT_IO_CONNECT_UNBLOCK,
    QT_IO_CONNECT_MOUNT
};

/* Connection IDs at or above this value are side channels, not file descriptors. */
#define QT_SIDE_CHANNEL 0x40000000u

void qt_dcmd_describe(uint32_t dcmd, char *buf, size_t len);

#endif
