/*
 * Copyright (C) Tildeslash Ltd. All rights reserved.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * In addition, as a special exception, the copyright holders give
 * permission to link the code of portions of this program with the
 * OpenSSL library under certain conditions as described in each
 * individual source file, and distribute linked combinations
 * including the two.
 *
 * You must obey the GNU Affero General Public License in all respects
 * for all of the code used other than OpenSSL.
 */

/**
 *  System dependent filesystem methods.
 *
 *  @file
 */

#include "config.h"

#ifdef HAVE_STDIO_H
#include <stdio.h>
#endif

#ifdef HAVE_ERRNO_H
#include <errno.h>
#endif

#ifdef HAVE_STRING_H
#include <string.h>
#endif

#ifdef HAVE_CTYPE_H
#include <ctype.h>
#endif

#ifdef HAVE_SYS_PARAM_H
#include <sys/param.h>
#endif

#ifdef HAVE_SYS_MOUNT_H
#include <sys/mount.h>
#endif

#ifdef HAVE_SYS_SYSCTL_H
#include <sys/sysctl.h>
#endif

#ifdef HAVE_SYS_DISK_H
#include <sys/disk.h>
#endif

#include "monit.h"

// libmonit
#include "system/Time.h"
#include "io/File.h"


/* ------------------------------------------------------------- Definitions */


static struct {
        uint64_t timestamp;
        size_t diskCount;
        size_t diskLength;
        struct diskstats *disk;
} _statistics = {};


/* ------------------------------------------------------- Static destructor */


static void __attribute__ ((destructor)) _destructor() {
        FREE(_statistics.disk);
}


/* ----------------------------------------------------------------- Private */


static uint64_t _timevalToMilli(struct timeval *time) {
        return time->tv_sec * 1000 + time->tv_usec / 1000.;
}


// Parse the device path like /dev/sd0a -> sd0
static boolean_t _parseDevice(const char *path, char device[DS_DISKNAMELEN]) {
        const char *base = File_basename(path);
        for (int len = strlen(base), i = len - 1; i >= 0; i--) {
                if (isdigit(*(base + i))) {
                        strncpy(device, base, i + 1 < sizeof(device) ? i + 1 : sizeof(device) - 1);
                        return true;
                }
        }
        return false;
}


static boolean_t _getDevice(char *mountpoint, char device[DS_DISKNAMELEN], Info_T inf) {
        int countfs = getfsstat(NULL, 0, MNT_NOWAIT);
        if (countfs != -1) {
                struct statfs *statfs = CALLOC(countfs, sizeof(struct statfs));
                if ((countfs = getfsstat(statfs, countfs * sizeof(struct statfs), MNT_NOWAIT)) != -1) {
                        for (int i = 0; i < countfs; i++) {
                                struct statfs *sfs = statfs + i;
                                if (IS(sfs->f_mntonname, mountpoint)) {
                                        snprintf(inf->priv.filesystem.device.type, sizeof(inf->priv.filesystem.device.type), "%s", sfs->f_fstypename);
                                        boolean_t rv = _parseDevice(sfs->f_mntfromname, device);
                                        FREE(statfs);
                                        return rv;
                                }
                        }
                }
                FREE(statfs);
        }
        LogError("Mount point %s -- %s\n", mountpoint, STRERROR);
        return false;
}


static boolean_t _getStatistics(uint64_t now) {
        // Refresh only if the statistics are older then 1 second (handle also backward time jumps)
        if (now > _statistics.timestamp + 1000 || now < _statistics.timestamp - 1000) {
                size_t len = sizeof(_statistics.diskCount);
                int mib[2] = {CTL_HW, HW_DISKCOUNT};
                if (sysctl(mib, 2, &(_statistics.diskCount), &len, NULL, 0) == -1) {
                        LogError("filesystem statistic error -- cannot get disks count: %s\n", STRERROR);
                        return false;
                }
                int length = _statistics.diskCount * sizeof(struct diskstats);
                if (_statistics.diskLength != length) {
                        _statistics.diskLength = length;
                        RESIZE(_statistics.disk, length);
                }
                mib[1] = HW_DISKSTATS;
                if (sysctl(mib, 2, _statistics.disk, &(_statistics.diskLength), NULL, 0) == -1) {
                        LogError("filesystem statistic error -- cannot get disks statistics: %s\n", STRERROR);
                        return false;
                }
                _statistics.timestamp = now;
        }
        return true;
}


static boolean_t _getDiskActivity(char *mountpoint, Info_T inf) {
        boolean_t rv = true;
        char device[DS_DISKNAMELEN] = {};
        if (_getDevice(mountpoint, device, inf)) {
                uint64_t now = Time_milli();
                if ((rv = _getStatistics(now))) {
                        for (int i = 0; i < _statistics.diskCount; i++)     {
                                if (Str_isEqual(device, _statistics.disk[i].ds_name)) {
                                        Statistics_update(&(inf->priv.filesystem.read.bytes), now, _statistics.disk[i].ds_rbytes);
                                        Statistics_update(&(inf->priv.filesystem.write.bytes), now, _statistics.disk[i].ds_wbytes);
                                        Statistics_update(&(inf->priv.filesystem.read.operations),  now, _statistics.disk[i].ds_rxfer);
                                        Statistics_update(&(inf->priv.filesystem.write.operations), now, _statistics.disk[i].ds_wxfer);
                                        Statistics_update(&(inf->priv.filesystem.runTime), now, _timevalToMilli(&(_statistics.disk[i].ds_time)));
                                        break;
                                }
                        }
                }
        } else {
                Statistics_reset(&(inf->priv.filesystem.read.time));
                Statistics_reset(&(inf->priv.filesystem.read.bytes));
                Statistics_reset(&(inf->priv.filesystem.read.operations));
                Statistics_reset(&(inf->priv.filesystem.write.time));
                Statistics_reset(&(inf->priv.filesystem.write.bytes));
                Statistics_reset(&(inf->priv.filesystem.write.operations));
        }
        return rv;
}


static boolean_t _getDiskUsage(char *mountpoint, Info_T inf) {
        struct statfs usage;
        if (statfs(mountpoint, &usage) != 0) {
                LogError("Error getting usage statistics for filesystem '%s' -- %s\n", mountpoint, STRERROR);
                return false;
        }
        inf->priv.filesystem.f_bsize =           usage.f_bsize;
        inf->priv.filesystem.f_blocks =          usage.f_blocks;
        inf->priv.filesystem.f_blocksfree =      usage.f_bavail;
        inf->priv.filesystem.f_blocksfreetotal = usage.f_bfree;
        inf->priv.filesystem.f_files =           usage.f_files;
        inf->priv.filesystem.f_filesfree =       usage.f_ffree;
        inf->priv.filesystem._flags =            inf->priv.filesystem.flags;
        inf->priv.filesystem.flags =             usage.f_flags;
        return true;
}


/* ------------------------------------------------------------------ Public */


char *device_mountpoint_sysdep(char *dev, char *buf, int buflen) {
        ASSERT(dev);
        ASSERT(buf);
        int countfs = getfsstat(NULL, 0, MNT_NOWAIT);
        if (countfs != -1) {
                struct statfs *statfs = CALLOC(countfs, sizeof(struct statfs));
                if ((countfs = getfsstat(statfs, countfs * sizeof(struct statfs), MNT_NOWAIT)) != -1) {
                        for (int i = 0; i < countfs; i++) {
                                struct statfs *sfs = statfs + i;
                                if (IS(sfs->f_mntfromname, dev)) {
                                        snprintf(buf, buflen, "%s", sfs->f_mntonname);
                                        FREE(statfs);
                                        return buf;
                                }
                        }
                }
                FREE(statfs);
        }
        LogError("Error getting mountpoint for filesystem '%s' -- %s\n", dev, STRERROR);
        return NULL;
}


boolean_t filesystem_usage_sysdep(char *mountpoint, Info_T inf) {
        ASSERT(mountpoint);
        ASSERT(inf);
        return (_getDiskUsage(mountpoint, inf) && _getDiskActivity(mountpoint, inf));
}

