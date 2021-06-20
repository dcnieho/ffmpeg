/*
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef AVDEVICE_VERSION_H
#define AVDEVICE_VERSION_H

/**
 * @file
 * @ingroup lavd
 * Libavdevice version macros
 */

#include "libavutil/version.h"
#include "libavutil/macros.h"

#include "version_major.h"

#define LIBAVDEVICE_VERSION_MINOR   2
#define LIBAVDEVICE_VERSION_MICRO 101

#define LIBAVDEVICE_VERSION_INT AV_VERSION_INT(LIBAVDEVICE_VERSION_MAJOR, \
                                               LIBAVDEVICE_VERSION_MINOR, \
                                               LIBAVDEVICE_VERSION_MICRO)
#define LIBAVDEVICE_VERSION     AV_VERSION(LIBAVDEVICE_VERSION_MAJOR, \
                                           LIBAVDEVICE_VERSION_MINOR, \
                                           LIBAVDEVICE_VERSION_MICRO)
#define LIBAVDEVICE_BUILD       LIBAVDEVICE_VERSION_INT

#define LIBAVDEVICE_IDENT       "Lavd" AV_STRINGIFY(LIBAVDEVICE_VERSION)

/**
 * avdevice_version_same_minor() expands to a function with
 * the same minor and major version it was compiled against
 * encoded in it. Enables locking to the minor version of
 * other libraries they were compiled against. Does not have
 * to be called by user.
 */
#define avdevice_version_same_minor AV_MAKE_MAJOR_MINOR_FUNC_NAME(device,LIBAVFORMAT_VERSION_MAJOR,LIBAVFORMAT_VERSION_MINOR)
unsigned avdevice_version_same_minor(void);

#endif /* AVDEVICE_VERSION_H */
