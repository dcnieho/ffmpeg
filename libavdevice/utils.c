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

#include "internal.h"
#include "libavutil/opt.h"
#include "libavformat/avformat.h"
#include "libavutil/avassert.h"

int ff_alloc_input_device_context(AVFormatContext **avctx, const AVInputFormat *iformat, const char *format)
{
    AVFormatContext *s;
    int ret = 0;

    *avctx = NULL;
    if (!iformat && !format)
        return AVERROR(EINVAL);
    if (!(s = avformat_alloc_context()))
        return AVERROR(ENOMEM);

    if (!iformat)
        iformat = av_find_input_format(format);
    if (!iformat || !iformat->priv_class || !AV_IS_INPUT_DEVICE(iformat->priv_class->category)) {
        ret = AVERROR(EINVAL);
        goto error;
    }
    s->iformat = iformat;
    if (s->iformat->priv_data_size > 0) {
        s->priv_data = av_mallocz(s->iformat->priv_data_size);
        if (!s->priv_data) {
            ret = AVERROR(ENOMEM);
            goto error;
        }
        if (s->iformat->priv_class) {
            *(const AVClass**)s->priv_data= s->iformat->priv_class;
            av_opt_set_defaults(s->priv_data);
        }
    } else
        s->priv_data = NULL;

    *avctx = s;
    return 0;
  error:
    avformat_free_context(s);
    return ret;
}

typedef struct AVDeviceCapabilitiesQueryTypeEntry {
    const char*                         name;
    enum AVDeviceCapabilitiesQueryType  query_type;
} AVDeviceCapabilitiesQueryTypeEntry;

static const AVDeviceCapabilitiesQueryTypeEntry query_table[] = {
    // both audio and video
    { "codec",          AV_DEV_CAP_QUERY_CODEC },
    // audio
    { "sample_format",  AV_DEV_CAP_QUERY_SAMPLE_FORMAT },
    { "sample_rate",    AV_DEV_CAP_QUERY_SAMPLE_RATE },
    { "channels",       AV_DEV_CAP_QUERY_CHANNELS },
    { "channel_layout", AV_DEV_CAP_QUERY_CHANNEL_LAYOUT },
    // video
    { "pixel_format",   AV_DEV_CAP_QUERY_PIXEL_FORMAT },
    { "frame_size",     AV_DEV_CAP_QUERY_FRAME_SIZE },
    { "window_size",    AV_DEV_CAP_QUERY_WINDOW_SIZE },
    { "fps",            AV_DEV_CAP_QUERY_FPS },
};

enum AVDeviceCapabilitiesQueryType ff_device_get_query_type(const char* option_name)
{
    for (int i = 0; i < FF_ARRAY_ELEMS(query_table); ++i) {
        if (!strcmp(query_table[i].name, option_name))
            return query_table[i].query_type;
    }
    // not found
    return AV_DEV_CAP_QUERY_NONE;
}

const char* ff_device_get_query_component_name(enum AVDeviceCapabilitiesQueryType query_type, int component)
{
    if (query_type == AV_DEV_CAP_QUERY_WINDOW_SIZE || query_type == AV_DEV_CAP_QUERY_FRAME_SIZE) {
        // special case: different name for each component
        return component == 0 ? "pixel_count" : (component == 1 ? "width" : (component == 2 ? "height" : ""));
    }
    else {
        av_assert0(component == 0);
        for (int i = 0; i < FF_ARRAY_ELEMS(query_table); ++i) {
            if (query_table[i].query_type == query_type)
                return query_table[i].name;
        }
    }
    // not found
    return NULL;
}