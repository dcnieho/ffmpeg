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

#ifndef AVDEVICE_INTERNAL_H
#define AVDEVICE_INTERNAL_H

#include "libavutil/log.h"
#include "libavutil/opt.h"
#include "libavutil/pixfmt.h"
#include "libavutil/rational.h"
#include "libavutil/samplefmt.h"
#include "libavcodec/codec_id.h"
#include "libavformat/avformat.h"

av_warn_unused_result
int ff_alloc_input_device_context(struct AVFormatContext **avctx, const AVInputFormat *iformat,
                                  const char *format);

/**
 * Structure describes device capabilities.
 *
 * It is used by devices in conjunction with ff_device_capabilities AVOption table
 * to implement capabilities probing API based on AVOption API.
 */
struct AVDeviceCapabilitiesQuery {
    const AVClass *av_class;
    AVFormatContext *device_context;
    enum AVCodecID codec;
    enum AVSampleFormat sample_format;
    enum AVPixelFormat pixel_format;
    int sample_rate;
    int channels;
    int64_t channel_layout;
    int window_width;
    int window_height;
    int frame_width;
    int frame_height;
    AVRational fps;
};

/**
 * AVOption table used by devices to implement device capabilities API.
 */
extern const AVOption ff_device_capabilities[];

#endif
