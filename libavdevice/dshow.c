/*
 * Directshow capture interface
 * Copyright (c) 2010 Ramiro Polla
 *
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

#include "dshow_capture.h"
#include "libavutil/parseutils.h"
#include "libavutil/pixdesc.h"
#include "libavutil/opt.h"
#include "libavutil/mem.h"
#include "libavformat/internal.h"
#include "libavformat/riff.h"
#include "avdevice.h"
#include "internal.h"
#include "libavcodec/raw.h"
#include "objidl.h"
#include "shlwapi.h"
// NB: technically, we should include dxva.h and use
// DXVA_ExtendedFormat, but that type is not defined in
// the MinGW headers. The DXVA2_ExtendedFormat and the
// contents of its fields is identical to
// DXVA_ExtendedFormat (see https://docs.microsoft.com/en-us/windows/win32/medfound/extended-color-information#color-space-in-media-types)
// and is provided by MinGW as well, so we use that
// instead. NB also that per the Microsoft docs, the
// lowest 8 bits of the structure, i.e. the SampleFormat
// field, contain AMCONTROL_xxx flags instead of sample
// format information, and should thus not be used.
// NB further that various values in the structure's
// fields (e.g. BT.2020 color space) are not provided
// for either of the DXVA structs, but are provided in
// the flags of the corresponding fields of Media Foundation.
// These may be provided by DirectShow devices (e.g. LAVFilters
// does so). So we use those values here too (the equivalence is
// indicated by Microsoft example code: https://docs.microsoft.com/en-us/windows/win32/api/dxva2api/ns-dxva2api-dxva2_videodesc)
#include "d3d9types.h"
#include "dxva2api.h"

#ifndef AMCONTROL_COLORINFO_PRESENT
// not defined in some versions of MinGW's dvdmedia.h
#   define AMCONTROL_COLORINFO_PRESENT 0x00000080 // if set, indicates DXVA color info is present in the upper (24) bits of the dwControlFlags
#endif


static enum AVPixelFormat dshow_pixfmt(DWORD biCompression, WORD biBitCount)
{
    switch(biCompression) {
    case BI_BITFIELDS:
    case BI_RGB:
        switch(biBitCount) { /* 1-8 are untested */
            case 1:
                return AV_PIX_FMT_MONOWHITE;
            case 4:
                return AV_PIX_FMT_RGB4;
            case 8:
                return AV_PIX_FMT_RGB8;
            case 16:
                return AV_PIX_FMT_RGB555;
            case 24:
                return AV_PIX_FMT_BGR24;
            case 32:
                return AV_PIX_FMT_0RGB32;
        }
    }
    return avpriv_pix_fmt_find(PIX_FMT_LIST_RAW, biCompression); // all others
}

static enum AVCodecID waveform_codec_id(enum AVSampleFormat sample_fmt)
{
    switch (sample_fmt) {
    case AV_SAMPLE_FMT_U8:  return AV_CODEC_ID_PCM_U8;
    case AV_SAMPLE_FMT_S16: return AV_CODEC_ID_PCM_S16LE;
    case AV_SAMPLE_FMT_S32: return AV_CODEC_ID_PCM_S32LE;
    default:                return AV_CODEC_ID_NONE; /* Should never happen. */
    }
}

static enum AVSampleFormat sample_fmt_bits_per_sample(int bits)
{
    switch (bits) {
    case 8:  return AV_SAMPLE_FMT_U8;
    case 16: return AV_SAMPLE_FMT_S16;
    case 32: return AV_SAMPLE_FMT_S32;
    default: return AV_SAMPLE_FMT_NONE; /* Should never happen. */
    }
}

static enum AVColorRange dshow_color_range(DXVA2_ExtendedFormat *fmt_info)
{
    switch (fmt_info->NominalRange)
    {
    case DXVA2_NominalRange_Unknown:
        return AVCOL_RANGE_UNSPECIFIED;
    case DXVA2_NominalRange_Normal: // equal to DXVA2_NominalRange_0_255
        return AVCOL_RANGE_JPEG;
    case DXVA2_NominalRange_Wide:   // equal to DXVA2_NominalRange_16_235
        return AVCOL_RANGE_MPEG;
    case DXVA2_NominalRange_48_208:
        // not an ffmpeg color range
        return AVCOL_RANGE_UNSPECIFIED;

    // values from MediaFoundation SDK (mfobjects.h)
    case 4:     // MFNominalRange_64_127
        // not an ffmpeg color range
        return AVCOL_RANGE_UNSPECIFIED;

    default:
        return AVCOL_RANGE_UNSPECIFIED;
    }
}

static enum AVColorSpace dshow_color_space(DXVA2_ExtendedFormat *fmt_info)
{
    switch (fmt_info->VideoTransferMatrix)
    {
    case DXVA2_VideoTransferMatrix_BT709:
        return AVCOL_SPC_BT709;
    case DXVA2_VideoTransferMatrix_BT601:
        return AVCOL_SPC_BT470BG;
    case DXVA2_VideoTransferMatrix_SMPTE240M:
        return AVCOL_SPC_SMPTE240M;

    // values from MediaFoundation SDK (mfobjects.h)
    case 4:     // MFVideoTransferMatrix_BT2020_10
    case 5:     // MFVideoTransferMatrix_BT2020_12
        if (fmt_info->VideoTransferFunction == 12)  // MFVideoTransFunc_2020_const
            return AVCOL_SPC_BT2020_CL;
        else
            return AVCOL_SPC_BT2020_NCL;

    default:
        return AVCOL_SPC_UNSPECIFIED;
    }
}

static enum AVColorPrimaries dshow_color_primaries(DXVA2_ExtendedFormat *fmt_info)
{
    switch (fmt_info->VideoPrimaries)
    {
    case DXVA2_VideoPrimaries_Unknown:
        return AVCOL_PRI_UNSPECIFIED;
    case DXVA2_VideoPrimaries_reserved:
        return AVCOL_PRI_RESERVED;
    case DXVA2_VideoPrimaries_BT709:
        return AVCOL_PRI_BT709;
    case DXVA2_VideoPrimaries_BT470_2_SysM:
        return AVCOL_PRI_BT470M;
    case DXVA2_VideoPrimaries_BT470_2_SysBG:
    case DXVA2_VideoPrimaries_EBU3213:   // this is PAL
        return AVCOL_PRI_BT470BG;
    case DXVA2_VideoPrimaries_SMPTE170M:
    case DXVA2_VideoPrimaries_SMPTE_C:
        return AVCOL_PRI_SMPTE170M;
    case DXVA2_VideoPrimaries_SMPTE240M:
        return AVCOL_PRI_SMPTE240M;

    // values from MediaFoundation SDK (mfobjects.h)
    case 9:     // MFVideoPrimaries_BT2020
        return AVCOL_PRI_BT2020;
    case 10:    // MFVideoPrimaries_XYZ
        return AVCOL_PRI_SMPTE428;
    case 11:    // MFVideoPrimaries_DCI_P3
        return AVCOL_PRI_SMPTE431;
    case 12:    // MFVideoPrimaries_ACES (Academy Color Encoding System)
        // not an FFmpeg color primary
        return AVCOL_PRI_UNSPECIFIED;

    default:
        return AVCOL_PRI_UNSPECIFIED;
    }
}

static enum AVColorTransferCharacteristic dshow_color_trc(DXVA2_ExtendedFormat *fmt_info)
{
    switch (fmt_info->VideoTransferFunction)
    {
    case DXVA2_VideoTransFunc_Unknown:
        return AVCOL_TRC_UNSPECIFIED;
    case DXVA2_VideoTransFunc_10:
        return AVCOL_TRC_LINEAR;
    case DXVA2_VideoTransFunc_18:
        // not an FFmpeg transfer characteristic
        return AVCOL_TRC_UNSPECIFIED;
    case DXVA2_VideoTransFunc_20:
        // not an FFmpeg transfer characteristic
        return AVCOL_TRC_UNSPECIFIED;
    case DXVA2_VideoTransFunc_22:
        return AVCOL_TRC_GAMMA22;
    case DXVA2_VideoTransFunc_709:
        return AVCOL_TRC_BT709;
    case DXVA2_VideoTransFunc_240M:
        return AVCOL_TRC_SMPTE240M;
    case DXVA2_VideoTransFunc_sRGB:
        return AVCOL_TRC_IEC61966_2_1;
    case DXVA2_VideoTransFunc_28:
        return AVCOL_TRC_GAMMA28;

    // values from MediaFoundation SDK (mfobjects.h)
    case 9:     // MFVideoTransFunc_Log_100
        return AVCOL_TRC_LOG;
    case 10:    // MFVideoTransFunc_Log_316
        return AVCOL_TRC_LOG_SQRT;
    case 11:    // MFVideoTransFunc_709_sym
        // not an FFmpeg transfer characteristic
        return AVCOL_TRC_UNSPECIFIED;
    case 12:    // MFVideoTransFunc_2020_const
    case 13:    // MFVideoTransFunc_2020
        if (fmt_info->VideoTransferMatrix == 5) // MFVideoTransferMatrix_BT2020_12
            return AVCOL_TRC_BT2020_12;
        else
            return AVCOL_TRC_BT2020_10;
    case 14:    // MFVideoTransFunc_26
        // not an FFmpeg transfer characteristic
        return AVCOL_TRC_UNSPECIFIED;
    case 15:    // MFVideoTransFunc_2084
        return AVCOL_TRC_SMPTEST2084;
    case 16:    // MFVideoTransFunc_HLG
        return AVCOL_TRC_ARIB_STD_B67;
    case 17:    // MFVideoTransFunc_10_rel
        // not an FFmpeg transfer characteristic? Undocumented also by MS
        return AVCOL_TRC_UNSPECIFIED;

    default:
        return AVCOL_TRC_UNSPECIFIED;
    }
}

static enum AVChromaLocation dshow_chroma_loc(DXVA2_ExtendedFormat *fmt_info)
{
    if (fmt_info->VideoChromaSubsampling == DXVA2_VideoChromaSubsampling_Cosited)       // that is: (DXVA2_VideoChromaSubsampling_Horizontally_Cosited | DXVA2_VideoChromaSubsampling_Vertically_Cosited | DXVA2_VideoChromaSubsampling_Vertically_AlignedChromaPlanes)
        return AVCHROMA_LOC_TOPLEFT;
    else if (fmt_info->VideoChromaSubsampling == DXVA2_VideoChromaSubsampling_MPEG1)    // that is: DXVA2_VideoChromaSubsampling_Vertically_AlignedChromaPlanes
        return AVCHROMA_LOC_CENTER;
    else if (fmt_info->VideoChromaSubsampling == DXVA2_VideoChromaSubsampling_MPEG2)    // that is: (DXVA2_VideoChromaSubsampling_Horizontally_Cosited | DXVA2_VideoChromaSubsampling_Vertically_AlignedChromaPlanes)
        return AVCHROMA_LOC_LEFT;
    else if (fmt_info->VideoChromaSubsampling == DXVA2_VideoChromaSubsampling_DV_PAL)   // that is: (DXVA2_VideoChromaSubsampling_Horizontally_Cosited | DXVA2_VideoChromaSubsampling_Vertically_Cosited)
        return AVCHROMA_LOC_TOPLEFT;
    else
        // unknown
        return AVCHROMA_LOC_UNSPECIFIED;
}

static int
dshow_read_close(AVFormatContext *s)
{
    struct dshow_ctx *ctx = s->priv_data;
    PacketListEntry *pktl;

    if (ctx->graph_builder2[VideoDevice])
        ICaptureGraphBuilder2_Release(ctx->graph_builder2[VideoDevice]);
    if (ctx->graph_builder2[AudioDevice])
        ICaptureGraphBuilder2_Release(ctx->graph_builder2[AudioDevice]);
    ctx->graph_builder2[0] = NULL;
    ctx->graph_builder2[1] = NULL;

    if (ctx->control) {
        IMediaControl_Stop(ctx->control);
        IMediaControl_Release(ctx->control);
    }
    ctx->control = NULL;

    if (ctx->media_event)
        IMediaEvent_Release(ctx->media_event);
    ctx->media_event = NULL;

    if (ctx->graph) {
        IEnumFilters *fenum;
        int r;
        r = IGraphBuilder_EnumFilters(ctx->graph, &fenum);
        if (r == S_OK) {
            IBaseFilter *f;
            IEnumFilters_Reset(fenum);
            while (IEnumFilters_Next(fenum, 1, &f, NULL) == S_OK) {
                if (IGraphBuilder_RemoveFilter(ctx->graph, f) == S_OK)
                    IEnumFilters_Reset(fenum); /* When a filter is removed,
                                                * the list must be reset. */
                IBaseFilter_Release(f);
            }
            IEnumFilters_Release(fenum);
        }
        IGraphBuilder_Release(ctx->graph);
        ctx->graph = NULL;
    }

    if (ctx->capture_pin[VideoDevice])
        ff_dshow_pin_Release(ctx->capture_pin[VideoDevice]);
    if (ctx->capture_pin[AudioDevice])
        ff_dshow_pin_Release(ctx->capture_pin[AudioDevice]);
    ctx->capture_pin[0] = NULL;
    ctx->capture_pin[1] = NULL;
    if (ctx->capture_filter[VideoDevice])
        ff_dshow_filter_Release(ctx->capture_filter[VideoDevice]);
    if (ctx->capture_filter[AudioDevice])
        ff_dshow_filter_Release(ctx->capture_filter[AudioDevice]);
    ctx->capture_filter[0] = NULL;
    ctx->capture_filter[1] = NULL;

    if (ctx->device_pin[VideoDevice])
        IPin_Release(ctx->device_pin[VideoDevice]);
    if (ctx->device_pin[AudioDevice])
        IPin_Release(ctx->device_pin[AudioDevice]);
    ctx->device_pin[0] = NULL;
    ctx->device_pin[1] = NULL;
    if (ctx->device_filter[VideoDevice])
        IBaseFilter_Release(ctx->device_filter[VideoDevice]);
    if (ctx->device_filter[AudioDevice])
        IBaseFilter_Release(ctx->device_filter[AudioDevice]);
    ctx->device_filter[0] = NULL;
    ctx->device_filter[1] = NULL;

    av_freep(&ctx->device_name[0]);
    av_freep(&ctx->device_name[1]);
    av_freep(&ctx->device_unique_name[0]);
    av_freep(&ctx->device_unique_name[1]);

    if(ctx->mutex)
        CloseHandle(ctx->mutex);
    ctx->mutex = NULL;
    if(ctx->event[0])
        CloseHandle(ctx->event[0]);
    if(ctx->event[1])
        CloseHandle(ctx->event[1]);
    ctx->event[0] = NULL;
    ctx->event[1] = NULL;

    pktl = ctx->pktl;
    while (pktl) {
        PacketListEntry *next = pktl->next;
        av_packet_unref(&pktl->pkt);
        av_free(pktl);
        pktl = next;
    }
    ctx->pktl = NULL;

    CoUninitialize();

    return 0;
}

static char *dup_wchar_to_utf8(wchar_t *w)
{
    char *s = NULL;
    int l = WideCharToMultiByte(CP_UTF8, 0, w, -1, 0, 0, 0, 0);
    s = av_malloc(l);
    if (s)
        WideCharToMultiByte(CP_UTF8, 0, w, -1, s, l, 0, 0);
    return s;
}

static int shall_we_drop(AVFormatContext *s, int index, enum dshowDeviceType devtype)
{
    struct dshow_ctx *ctx = s->priv_data;
    static const uint8_t dropscore[] = {62, 75, 87, 100};
    const int ndropscores = FF_ARRAY_ELEMS(dropscore);
    unsigned int buffer_fullness = (ctx->curbufsize[index]*100)/s->max_picture_buffer;
    const char *devtypename = (devtype == VideoDevice) ? "video" : "audio";

    if(dropscore[++ctx->video_frame_num%ndropscores] <= buffer_fullness) {
        av_log(s, AV_LOG_ERROR,
              "real-time buffer [%s] [%s input] too full or near too full (%d%% of size: %d [rtbufsize parameter])! frame dropped!\n",
              ctx->device_name[devtype], devtypename, buffer_fullness, s->max_picture_buffer);
        return 1;
    }

    return 0;
}

static void
callback(void *priv_data, int index, uint8_t *buf, int buf_size, int64_t time, enum dshowDeviceType devtype)
{
    AVFormatContext *s = priv_data;
    struct dshow_ctx *ctx = s->priv_data;
    PacketListEntry **ppktl, *pktl_next;

//    dump_videohdr(s, vdhdr);

    WaitForSingleObject(ctx->mutex, INFINITE);

    if(shall_we_drop(s, index, devtype))
        goto fail;

    pktl_next = av_mallocz(sizeof(*pktl_next));
    if(!pktl_next)
        goto fail;

    if(av_new_packet(&pktl_next->pkt, buf_size) < 0) {
        av_free(pktl_next);
        goto fail;
    }

    pktl_next->pkt.stream_index = index;
    pktl_next->pkt.pts = time;
    memcpy(pktl_next->pkt.data, buf, buf_size);

    for(ppktl = &ctx->pktl ; *ppktl ; ppktl = &(*ppktl)->next);
    *ppktl = pktl_next;
    ctx->curbufsize[index] += buf_size;

    SetEvent(ctx->event[1]);
    ReleaseMutex(ctx->mutex);

    return;
fail:
    ReleaseMutex(ctx->mutex);
    return;
}

static void
dshow_get_device_media_types(AVFormatContext *avctx, enum dshowDeviceType devtype,
                                         enum dshowSourceFilterType sourcetype, IBaseFilter *device_filter,
                                         enum AVMediaType **media_types, int *nb_media_types)
{
    IEnumPins *pins = 0;
    IPin *pin;
    int has_audio = 0, has_video = 0;

    if (IBaseFilter_EnumPins(device_filter, &pins) != S_OK)
        return;

    while (IEnumPins_Next(pins, 1, &pin, NULL) == S_OK) {
        IKsPropertySet *p = NULL;
        PIN_INFO info = { 0 };
        GUID category;
        DWORD r2;
        IEnumMediaTypes *types = NULL;
        AM_MEDIA_TYPE *type;

        if (IPin_QueryPinInfo(pin, &info) != S_OK)
            goto next;
        IBaseFilter_Release(info.pFilter);

        if (info.dir != PINDIR_OUTPUT)
            goto next;
        if (IPin_QueryInterface(pin, &IID_IKsPropertySet, (void **) &p) != S_OK)
            goto next;
        if (IKsPropertySet_Get(p, &AMPROPSETID_Pin, AMPROPERTY_PIN_CATEGORY,
                               NULL, 0, &category, sizeof(GUID), &r2) != S_OK)
            goto next;
        if (!IsEqualGUID(&category, &PIN_CATEGORY_CAPTURE))
            goto next;

        if (IPin_EnumMediaTypes(pin, &types) != S_OK)
            goto next;

        // enumerate media types exposed by pin
        // NB: don't know if a pin can expose both audio and video, check 'm all to be safe
        IEnumMediaTypes_Reset(types);
        while (IEnumMediaTypes_Next(types, 1, &type, NULL) == S_OK) {
            if (IsEqualGUID(&type->majortype, &MEDIATYPE_Video)) {
                has_video = 1;
            } else if (IsEqualGUID(&type->majortype, &MEDIATYPE_Audio)) {
                has_audio = 1;
            }
            CoTaskMemFree(type);
        }

    next:
        if (types)
            IEnumMediaTypes_Release(types);
        if (p)
            IKsPropertySet_Release(p);
        if (pin)
            IPin_Release(pin);
    }

    IEnumPins_Release(pins);

    if (has_audio || has_video) {
        int nb_types = has_audio + has_video;
        *media_types = av_malloc_array(nb_types, sizeof(enum AVMediaType));
        if (*media_types) {
            if (has_audio)
                (*media_types)[0] = AVMEDIA_TYPE_AUDIO;
            if (has_video)
                (*media_types)[0 + has_audio] = AVMEDIA_TYPE_VIDEO;
            *nb_media_types = nb_types;
        }
    }
}

/**
 * Cycle through available devices using the device enumerator devenum,
 * retrieve the device with type specified by devtype and return the
 * pointer to the object found in *pfilter.
 * If pfilter is NULL, list all device names.
 * If device_list is not NULL, populate it with found devices instead of
 * outputting device names to log
 */
static int
dshow_cycle_devices(AVFormatContext *avctx, ICreateDevEnum *devenum,
                    enum dshowDeviceType devtype, enum dshowSourceFilterType sourcetype,
                    IBaseFilter **pfilter, char **device_unique_name,
                    AVDeviceInfoList **device_list)
{
    struct dshow_ctx *ctx = avctx->priv_data;
    IBaseFilter *device_filter = NULL;
    IEnumMoniker *classenum = NULL;
    IMoniker *m = NULL;
    const char *device_name = ctx->device_name[devtype];
    int skip = (devtype == VideoDevice) ? ctx->video_device_number
                                        : ctx->audio_device_number;
    int r;

    const GUID *device_guid[2] = { &CLSID_VideoInputDeviceCategory,
                                   &CLSID_AudioInputDeviceCategory };
    const char *devtypename = (devtype == VideoDevice) ? "video" : "audio only";
    const char *sourcetypename = (sourcetype == VideoSourceDevice) ? "video" : "audio";

    r = ICreateDevEnum_CreateClassEnumerator(devenum, device_guid[sourcetype],
                                             (IEnumMoniker **) &classenum, 0);
    if (r != S_OK) {
        av_log(avctx, AV_LOG_ERROR, "Could not enumerate %s devices (or none found).\n",
               devtypename);
        return AVERROR(EIO);
    }

    while (!device_filter && IEnumMoniker_Next(classenum, 1, &m, NULL) == S_OK) {
        IPropertyBag *bag = NULL;
        char *friendly_name = NULL;
        char *unique_name = NULL;
        VARIANT var;
        IBindCtx *bind_ctx = NULL;
        LPOLESTR olestr = NULL;
        LPMALLOC co_malloc = NULL;
        AVDeviceInfo *device = NULL;
        enum AVMediaType *media_types = NULL;
        int nb_media_types = 0;
        int i;

        r = CoGetMalloc(1, &co_malloc);
        if (r != S_OK)
            goto fail;
        r = CreateBindCtx(0, &bind_ctx);
        if (r != S_OK)
            goto fail;
        /* GetDisplayname works for both video and audio, DevicePath doesn't */
        r = IMoniker_GetDisplayName(m, bind_ctx, NULL, &olestr);
        if (r != S_OK)
            goto fail;
        unique_name = dup_wchar_to_utf8(olestr);
        /* replace ':' with '_' since we use : to delineate between sources */
        for (i = 0; i < strlen(unique_name); i++) {
            if (unique_name[i] == ':')
                unique_name[i] = '_';
        }

        r = IMoniker_BindToStorage(m, 0, 0, &IID_IPropertyBag, (void *) &bag);
        if (r != S_OK)
            goto fail;

        var.vt = VT_BSTR;
        r = IPropertyBag_Read(bag, L"FriendlyName", &var, NULL);
        if (r != S_OK)
            goto fail;
        friendly_name = dup_wchar_to_utf8(var.bstrVal);

        if (pfilter) {
            if (strcmp(device_name, friendly_name) && strcmp(device_name, unique_name))
                goto fail;

            if (!skip--) {
                r = IMoniker_BindToObject(m, 0, 0, &IID_IBaseFilter, (void *) &device_filter);
                if (r != S_OK) {
                    av_log(avctx, AV_LOG_ERROR, "Unable to BindToObject for %s\n", device_name);
                    goto fail;
                }
                *device_unique_name = unique_name;
                unique_name = NULL;
                // success, loop will end now
            }
        } else {
            // get media types exposed by pins of device
            if (IMoniker_BindToObject(m, 0, 0, &IID_IBaseFilter, (void* ) &device_filter) == S_OK) {
                dshow_get_device_media_types(avctx, devtype, sourcetype, device_filter, &media_types, &nb_media_types);
                IBaseFilter_Release(device_filter);
                device_filter = NULL;
            }
            if (device_list) {
                device = av_mallocz(sizeof(AVDeviceInfo));
                if (!device)
                    goto fail;

                device->device_name = av_strdup(unique_name);
                device->device_description = av_strdup(friendly_name);
                if (!device->device_name || !device->device_description)
                    goto fail;

                // make space in device_list for this new device
                if (av_reallocp_array(&(*device_list)->devices,
                                     (*device_list)->nb_devices + 1,
                                     sizeof(*(*device_list)->devices)) < 0)
                    goto fail;

                // attach media_types to device
                device->nb_media_types = nb_media_types;
                device->media_types = media_types;
                nb_media_types = 0;
                media_types = NULL;

                // store device in list
                (*device_list)->devices[(*device_list)->nb_devices] = device;
                (*device_list)->nb_devices++;
                device = NULL;  // copied into array, make sure not freed below
            }
            else {
                av_log(avctx, AV_LOG_INFO, "\"%s\"", friendly_name);
                if (nb_media_types > 0) {
                    const char* media_type = av_get_media_type_string(media_types[0]);
                    av_log(avctx, AV_LOG_INFO, " (%s", media_type ? media_type : "unknown");
                    for (int i = 1; i < nb_media_types; ++i) {
                        media_type = av_get_media_type_string(media_types[i]);
                        av_log(avctx, AV_LOG_INFO, ", %s", media_type ? media_type : "unknown");
                    }
                    av_log(avctx, AV_LOG_INFO, ")");
                } else {
                    av_log(avctx, AV_LOG_INFO, " (none)");
                }
                av_log(avctx, AV_LOG_INFO, "\n");
                av_log(avctx, AV_LOG_INFO, "  Alternative name \"%s\"\n", unique_name);
            }
        }

    fail:
        av_freep(&media_types);
        if (device) {
            av_freep(&device->device_name);
            av_freep(&device->device_description);
            // NB: no need to av_freep(&device->media_types), its only moved to device once nothing can fail anymore
            av_free(device);
        }
        if (olestr && co_malloc)
            IMalloc_Free(co_malloc, olestr);
        if (bind_ctx)
            IBindCtx_Release(bind_ctx);
        av_freep(&friendly_name);
        av_freep(&unique_name);
        if (bag)
            IPropertyBag_Release(bag);
        IMoniker_Release(m);
    }

    IEnumMoniker_Release(classenum);

    if (pfilter) {
        if (!device_filter) {
            av_log(avctx, AV_LOG_ERROR, "Could not find %s device with name [%s] among source devices of type %s.\n",
                   devtypename, device_name, sourcetypename);
            return AVERROR(EIO);
        }
        *pfilter = device_filter;
    }

    return 0;
}

static int dshow_get_device_list(AVFormatContext *avctx, AVDeviceInfoList *device_list)
{
    ICreateDevEnum *devenum = NULL;
    int r;
    int ret = AVERROR(EIO);

    if (!device_list)
        return AVERROR(EINVAL);

    CoInitialize(0);

    r = CoCreateInstance(&CLSID_SystemDeviceEnum, NULL, CLSCTX_INPROC_SERVER,
        &IID_ICreateDevEnum, (void**)&devenum);
    if (r != S_OK) {
        av_log(avctx, AV_LOG_ERROR, "Could not enumerate system devices.\n");
        goto error;
    }

    ret = dshow_cycle_devices(avctx, devenum, VideoDevice, VideoSourceDevice, NULL, NULL, &device_list);
    if (ret < S_OK)
        goto error;
    ret = dshow_cycle_devices(avctx, devenum, AudioDevice, AudioSourceDevice, NULL, NULL, &device_list);

error:
    if (devenum)
        ICreateDevEnum_Release(devenum);

    CoUninitialize();

    return ret;
}

static int dshow_should_set_format(AVFormatContext *avctx, enum dshowDeviceType devtype)
{
    struct dshow_ctx *ctx = avctx->priv_data;

    return (devtype == VideoDevice && (ctx->framerate ||
                                      (ctx->requested_width && ctx->requested_height) ||
                                       ctx->pixel_format != AV_PIX_FMT_NONE ||
                                       ctx->video_codec_id != AV_CODEC_ID_RAWVIDEO))
        || (devtype == AudioDevice && (ctx->channels || ctx->sample_size || ctx->sample_rate));
}


struct dshow_format_info {
    enum dshowDeviceType devtype;
    // video
    int64_t framerate;
    enum AVPixelFormat pix_fmt;
    enum AVCodecID codec_id;
    enum AVColorRange col_range;
    enum AVColorSpace col_space;
    enum AVColorPrimaries col_prim;
    enum AVColorTransferCharacteristic col_trc;
    enum AVChromaLocation chroma_loc;
    int width;
    int height;
    // audio
    int sample_rate;
    int sample_size;
    int channels;
};

// user must av_free the returned pointer
static struct dshow_format_info *dshow_get_format_info(AM_MEDIA_TYPE *type)
{
    struct dshow_format_info *fmt_info = NULL;
    BITMAPINFOHEADER *bih;
    DXVA2_ExtendedFormat *extended_format_info = NULL;
    WAVEFORMATEX *fx;
    enum dshowDeviceType devtype;
    int64_t framerate;

    if (!type)
        return NULL;

    if (IsEqualGUID(&type->formattype, &FORMAT_VideoInfo)) {
        VIDEOINFOHEADER *v = (void *) type->pbFormat;
        framerate = v->AvgTimePerFrame;
        bih       = &v->bmiHeader;
        devtype   = VideoDevice;
    } else if (IsEqualGUID(&type->formattype, &FORMAT_VideoInfo2)) {
        VIDEOINFOHEADER2 *v = (void *) type->pbFormat;
        devtype   = VideoDevice;
        framerate = v->AvgTimePerFrame;
        bih       = &v->bmiHeader;
        if (v->dwControlFlags & AMCONTROL_COLORINFO_PRESENT)
            extended_format_info = (DXVA2_ExtendedFormat *) &v->dwControlFlags;
    } else if (IsEqualGUID(&type->formattype, &FORMAT_WaveFormatEx)) {
        fx = (void *) type->pbFormat;
        devtype = AudioDevice;
    } else {
        return NULL;
    }

    fmt_info = av_mallocz(sizeof(struct dshow_format_info));
    if (!fmt_info)
        return NULL;
    // initialize fields where unset is not zero
    fmt_info->pix_fmt = AV_PIX_FMT_NONE;
    fmt_info->col_space = AVCOL_SPC_UNSPECIFIED;
    fmt_info->col_prim = AVCOL_PRI_UNSPECIFIED;
    fmt_info->col_trc = AVCOL_TRC_UNSPECIFIED;
    // now get info about format
    fmt_info->devtype = devtype;
    if (devtype == VideoDevice) {
        fmt_info->width = bih->biWidth;
        fmt_info->height = bih->biHeight;
        fmt_info->framerate = framerate;
        fmt_info->pix_fmt = dshow_pixfmt(bih->biCompression, bih->biBitCount);
        if (fmt_info->pix_fmt == AV_PIX_FMT_NONE) {
            const AVCodecTag *const tags[] = { avformat_get_riff_video_tags(), NULL };
            fmt_info->codec_id = av_codec_get_id(tags, bih->biCompression);
        }
        else
            fmt_info->codec_id = AV_CODEC_ID_RAWVIDEO;

        if (extended_format_info) {
            fmt_info->col_range = dshow_color_range(extended_format_info);
            fmt_info->col_space = dshow_color_space(extended_format_info);
            fmt_info->col_prim = dshow_color_primaries(extended_format_info);
            fmt_info->col_trc = dshow_color_trc(extended_format_info);
            fmt_info->chroma_loc = dshow_chroma_loc(extended_format_info);
        }
    } else {
        fmt_info->sample_rate = fx->nSamplesPerSec;
        fmt_info->sample_size = fx->wBitsPerSample;
        fmt_info->channels = fx->nChannels;
    }

    return fmt_info;
}

static void dshow_get_default_format(IPin *pin, IAMStreamConfig *config, enum dshowDeviceType devtype, AM_MEDIA_TYPE **type)
{
    HRESULT hr;

    if ((hr = IAMStreamConfig_GetFormat(config, type)) != S_OK) {
        if (hr == E_NOTIMPL || !IsEqualGUID(&(*type)->majortype, devtype == VideoDevice ? &MEDIATYPE_Video : &MEDIATYPE_Audio)) {
            // default not available or of wrong type,
            // fall back to iterating exposed formats
            // until one of the right type is found
            IEnumMediaTypes* types = NULL;
            if (IPin_EnumMediaTypes(pin, &types) != S_OK)
                return;
            IEnumMediaTypes_Reset(types);
            while (IEnumMediaTypes_Next(types, 1, type, NULL) == S_OK) {
                if (IsEqualGUID(&(*type)->majortype, devtype == VideoDevice ? &MEDIATYPE_Video : &MEDIATYPE_Audio)) {
                    break;
                }
                CoTaskMemFree(*type);
                *type = NULL;
            }
            IEnumMediaTypes_Release(types);
        }
    }
}

/**
 * Cycle through available formats available from the specified pin,
 * try to set parameters specified through AVOptions, or the pin's
 * default format if no such parameters were set. If successful,
 * return 1 in *pformat_set.
 * If pformat_set is NULL or the ranges input is not NULL, list all
 * pin capabilities.
 * When listing pin capabilities, if ranges is NULL, output to log,
 * else store capabilities in ranges.
 */
static void
dshow_cycle_formats(AVFormatContext *avctx, enum dshowDeviceType devtype,
                    IPin *pin, int *pformat_set,
                    AVOptionRanges *ranges, enum AVDeviceCapabilitiesQueryType query_type)
{
    struct dshow_ctx *ctx = avctx->priv_data;
    IAMStreamConfig *config = NULL;
    AM_MEDIA_TYPE *type = NULL;
    AM_MEDIA_TYPE *previous_match_type = NULL;
    int format_set = 0;
    void *caps = NULL;
    int i, n, size, r;
    int wait_for_better = 0;
    int use_default;

    // format parameters requested by user
    // if none are requested by user, the values will below be set to
    // those of the default format
    // video
    enum AVCodecID requested_video_codec_id   = ctx->video_codec_id;
    enum AVPixelFormat requested_pixel_format = ctx->pixel_format;
    int64_t requested_framerate               = ctx->framerate ? ((int64_t)ctx->requested_framerate.den * 10000000)
                                                                    / ctx->requested_framerate.num : 0;
    int requested_width                       = ctx->requested_width;
    int requested_height                      = ctx->requested_height;
    // audio
    int requested_sample_rate                 = ctx->sample_rate;
    int requested_sample_size                 = ctx->sample_size;
    int requested_channels                    = ctx->channels;

    if (IPin_QueryInterface(pin, &IID_IAMStreamConfig, (void **) &config) != S_OK)
        return;
    if (IAMStreamConfig_GetNumberOfCapabilities(config, &n, &size) != S_OK)
        goto end;

    caps = av_malloc(size);
    if (!caps)
        goto end;

    /**
     * If we should open the device with the default format,
     * then:
     * 1. check what the format of the default device is, and
     * 2. below we iterate all formats till we find a matching
     *    one, with most info exposed (see comment below).
     */
    use_default = !dshow_should_set_format(avctx, devtype);
    if (use_default && pformat_set && !ranges)
    {
        // get default
        dshow_get_default_format(pin, config, devtype, &type);
        if (!type)
            // this pin does not expose any formats of the expected type
            goto end;

        if (type) {
            // interrogate default format, so we know what to search for below
            struct dshow_format_info *fmt_info = dshow_get_format_info(type);
            if (fmt_info) {
                if (fmt_info->devtype == VideoDevice) {
                    requested_video_codec_id = fmt_info->codec_id;
                    requested_pixel_format   = fmt_info->pix_fmt;
                    requested_framerate      = fmt_info->framerate;
                    requested_width          = fmt_info->width;
                    requested_height         = fmt_info->height;
                } else {
                    requested_sample_rate = fmt_info->sample_rate;
                    requested_sample_size = fmt_info->sample_size;
                    requested_channels    = fmt_info->channels;
                }
                av_free(fmt_info);  // free but don't set to NULL to enable below check
            }

            if (type && type->pbFormat)
                CoTaskMemFree(type->pbFormat);
            CoTaskMemFree(type);
            type = NULL;
            if (!fmt_info)
                // default format somehow invalid, can't continue with this pin
                goto end;
            fmt_info = NULL;
        }
    }

    // NB: some devices (e.g. Logitech C920) expose each video format twice:
    // both a format containing a VIDEOINFOHEADER and a format containing
    // a VIDEOINFOHEADER2. We want, if possible, to select a format with a
    // VIDEOINFOHEADER2, as this potentially provides more info about the
    // format. So, if in the iteration below we have found a matching format,
    // but it is a VIDEOINFOHEADER, keep looking for a matching format that
    // exposes contains a VIDEOINFOHEADER2. Fall back to the VIDEOINFOHEADER
    // format if no corresponding VIDEOINFOHEADER2 is found when we finish
    // iterating.
    for (i = 0; i < n && (!format_set || ranges); i++) {
        AVOptionRange *new_range[3] = { NULL };
        int nb_range = 0;
        struct dshow_format_info *fmt_info = NULL;
        
        r = IAMStreamConfig_GetStreamCaps(config, i, &type, (void *) caps);
        if (r != S_OK)
            goto next;
#if DSHOWDEBUG
        ff_print_AM_MEDIA_TYPE(type);
#endif

        fmt_info = dshow_get_format_info(type);
        if (!fmt_info)
            goto next;

        if (devtype == VideoDevice) {
            VIDEO_STREAM_CONFIG_CAPS *vcaps = caps;
            BITMAPINFOHEADER *bih;
            int64_t *fr;
#if DSHOWDEBUG
            ff_print_VIDEO_STREAM_CONFIG_CAPS(vcaps);
#endif

            if (fmt_info->devtype != VideoDevice)
                goto next;

            if (IsEqualGUID(&type->formattype, &FORMAT_VideoInfo)) {
                VIDEOINFOHEADER *v = (void *) type->pbFormat;
                fr  = &v->AvgTimePerFrame;
                bih = &v->bmiHeader;
                wait_for_better = 1;
            } else if (IsEqualGUID(&type->formattype, &FORMAT_VideoInfo2)) {
                VIDEOINFOHEADER2 *v = (void *) type->pbFormat;
                fr  = &v->AvgTimePerFrame;
                bih = &v->bmiHeader;
                wait_for_better = 0;
            }

            if (!pformat_set && !ranges) {
                const char *chroma = av_chroma_location_name(fmt_info->chroma_loc);
                if (fmt_info->pix_fmt == AV_PIX_FMT_NONE) {
                    const AVCodec *codec = avcodec_find_decoder(fmt_info->codec_id);
                    if (fmt_info->codec_id == AV_CODEC_ID_NONE || !codec) {
                        av_log(avctx, AV_LOG_INFO, "  unknown compression type 0x%X", (int) bih->biCompression);
                    } else {
                        av_log(avctx, AV_LOG_INFO, "  vcodec=%s", codec->name);
                    }
                } else {
                    av_log(avctx, AV_LOG_INFO, "  pixel_format=%s", av_get_pix_fmt_name(fmt_info->pix_fmt));
                }
                av_log(avctx, AV_LOG_INFO, "  min s=%ldx%ld fps=%g max s=%ldx%ld fps=%g",
                       vcaps->MinOutputSize.cx, vcaps->MinOutputSize.cy,
                       1e7 / vcaps->MaxFrameInterval,
                       vcaps->MaxOutputSize.cx, vcaps->MaxOutputSize.cy,
                       1e7 / vcaps->MinFrameInterval);

                if (fmt_info->col_range != AVCOL_RANGE_UNSPECIFIED ||
                    fmt_info->col_space != AVCOL_SPC_UNSPECIFIED ||
                    fmt_info->col_prim != AVCOL_PRI_UNSPECIFIED ||
                    fmt_info->col_trc != AVCOL_TRC_UNSPECIFIED) {
                    const char *range = av_color_range_name(fmt_info->col_range);
                    const char *space = av_color_space_name(fmt_info->col_space);
                    const char *prim = av_color_primaries_name(fmt_info->col_prim);
                    const char *trc = av_color_transfer_name(fmt_info->col_trc);
                    av_log(avctx, AV_LOG_INFO, " (%s, %s/%s/%s",
                        range ? range : "unknown",
                        space ? space : "unknown",
                        prim  ? prim  : "unknown",
                        trc   ? trc   : "unknown");
                    if (fmt_info->chroma_loc != AVCHROMA_LOC_UNSPECIFIED)
                        av_log(avctx, AV_LOG_INFO, ", %s", chroma ? chroma : "unknown");
                    av_log(avctx, AV_LOG_INFO, ")");
                }
                else if (fmt_info->chroma_loc != AVCHROMA_LOC_UNSPECIFIED)
                    av_log(avctx, AV_LOG_INFO, "(%s)", chroma ? chroma : "unknown");

                av_log(avctx, AV_LOG_INFO, "\n");
                goto next;
            }
            if (requested_video_codec_id != AV_CODEC_ID_RAWVIDEO) {
                if (requested_video_codec_id != fmt_info->codec_id)
                    goto next;
            }
            if (requested_pixel_format != AV_PIX_FMT_NONE &&
                requested_pixel_format != fmt_info->pix_fmt) {
                goto next;
            }
            if (requested_framerate) {
                if (requested_framerate > vcaps->MaxFrameInterval ||
                    requested_framerate < vcaps->MinFrameInterval)
                    goto next;
                *fr = requested_framerate;
            }
            if (requested_width && requested_height) {
                if (requested_width  > vcaps->MaxOutputSize.cx ||
                    requested_width  < vcaps->MinOutputSize.cx ||
                    requested_height > vcaps->MaxOutputSize.cy ||
                    requested_height < vcaps->MinOutputSize.cy)
                    goto next;
                bih->biWidth  = requested_width;
                bih->biHeight = requested_height;
            }

            if (ranges) {
                for (int j = 0; j < ranges->nb_components; j++) {
                    new_range[j] = av_mallocz(sizeof(**new_range));
                    if (!new_range[j])
                        goto next;
                    new_range[j]->value_max = -1.;  // init (min:0, max:-1 means value not set)
                    ++nb_range;

                    switch (query_type)
                    {
                    case AV_DEV_CAP_QUERY_CODEC:
                        if (dshow_pixfmt(bih->biCompression, bih->biBitCount) == AV_PIX_FMT_NONE) {
                            const AVCodecTag *const tags[] = { avformat_get_riff_video_tags(), NULL };
                            new_range[j]->value_min = av_codec_get_id(tags, bih->biCompression);
                        }
                        else
                            new_range[j]->value_min = AV_CODEC_ID_RAWVIDEO;
                        new_range[j]->value_max = new_range[j]->value_min;
                        break;
                    case AV_DEV_CAP_QUERY_PIXEL_FORMAT:
                        new_range[j]->value_min = new_range[j]->value_max = dshow_pixfmt(bih->biCompression, bih->biBitCount);
                        new_range[j]->value_min;
                        break;
                    case AV_DEV_CAP_QUERY_FRAME_SIZE:
                    {
                        switch (j)
                        {
                        case 0:
                            new_range[j]->value_min = vcaps->MinOutputSize.cx * vcaps->MinOutputSize.cy;
                            new_range[j]->value_max = vcaps->MaxOutputSize.cx * vcaps->MaxOutputSize.cy;
                            break;
                        case 1:
                            new_range[j]->value_min = vcaps->MinOutputSize.cx;
                            new_range[j]->value_max = vcaps->MaxOutputSize.cx;
                            break;
                        case 2:
                            new_range[j]->value_min = vcaps->MinOutputSize.cy;
                            new_range[j]->value_max = vcaps->MaxOutputSize.cy;
                            break;
                        }
                        break;
                    }
                    case AV_DEV_CAP_QUERY_FPS:
                        new_range[j]->value_min = 1e7 / vcaps->MaxFrameInterval;
                        new_range[j]->value_max = 1e7 / vcaps->MinFrameInterval;
                        break;

                    // default:
                    // an audio property is being queried, output all fields 0 (mallocz above) is fine
                    }
                }
            }

        } else {
            WAVEFORMATEX *fx;
            AUDIO_STREAM_CONFIG_CAPS *acaps = caps;
#if DSHOWDEBUG
            ff_print_AUDIO_STREAM_CONFIG_CAPS(acaps);
#endif
            if (IsEqualGUID(&type->formattype, &FORMAT_WaveFormatEx)) {
                fx = (void *) type->pbFormat;
            } else {
                goto next;
            }
            if (!pformat_set && !ranges) {
                av_log(
                    avctx,
                    AV_LOG_INFO,
                    "  ch=%2u, bits=%2u, rate=%6lu\n",
                    fx->nChannels, fx->wBitsPerSample, fx->nSamplesPerSec
                );
                continue;
            }
            if (
                (requested_sample_rate && requested_sample_rate != fx->nSamplesPerSec) ||
                (requested_sample_size && requested_sample_size != fx->wBitsPerSample) ||
                (requested_channels    && requested_channels    != fx->nChannels     )
            ) {
                goto next;
            }

            if (ranges) {
                for (int j = 0; j < ranges->nb_components; j++) {
                    new_range[j] = av_mallocz(sizeof(**new_range));
                    if (!new_range[j])
                        goto next;
                    new_range[j]->value_max = -1.;  // init (min:0, max:-1 means value not set)
                    ++nb_range;

                    switch (query_type)
                    {
                    case AV_DEV_CAP_QUERY_SAMPLE_FORMAT:
                        new_range[j]->value_min = sample_fmt_bits_per_sample(acaps->MinimumBitsPerSample);
                        new_range[j]->value_max = sample_fmt_bits_per_sample(acaps->MaximumBitsPerSample);
                        break;
                    case AV_DEV_CAP_QUERY_SAMPLE_RATE:
                        new_range[j]->value_min = acaps->MinimumSampleFrequency;
                        new_range[j]->value_max = acaps->MaximumSampleFrequency;
                        break;
                    case AV_DEV_CAP_QUERY_CHANNELS:
                        new_range[j]->value_min = acaps->MinimumChannels;
                        new_range[j]->value_max = acaps->MaximumChannels;
                        break;

                    // default:
                    // a video property is being queried, output all fields 0 (mallocz above) is fine
                    // NB: this is a for-loop since some of the video queries are multi-component
                    // and all components should be set
                    }
                }
            }
        }

        // found a matching format. Either apply or store
        // for safekeeping if we might maybe find a better
        // format with more info attached to it (see comment
        // above loop). If storing all capabilities of device
        // in ranges, try to apply in all cases, and store
        // caps if successfully applied
        if (!wait_for_better || ranges) {
            if (IAMStreamConfig_SetFormat(config, type) != S_OK)
                goto next;
            else if (ranges) {
                // format matched and could be set successfully.
                // fill in some fields for each capability
                for (int j = 0; j < nb_range; j++) {
                    new_range[j]->str = av_strdup(ff_device_get_query_component_name(query_type, j));
                    if (!new_range[j]->str)
                        goto next;
                    new_range[j]->is_range = new_range[j]->value_min < new_range[j]->value_max;
                }

                // store to ranges output
                if (av_reallocp_array(&ranges->range,
                                      ranges->nb_ranges * ranges->nb_components + nb_range * ranges->nb_components,
                                      sizeof(*ranges->range)) < 0)
                    goto next;
                for (int j = 0; j < nb_range; ++j) {
                    ranges->range[ranges->nb_ranges] = new_range[j];
                    ranges->nb_ranges++;
                    new_range[j] = NULL;  // copied into array, make sure not freed below
                }
            }
            format_set = 1;
        }
        else if (!previous_match_type) {
            // store this matching format for possible later use.
            // If we have already found a matching format, ignore it
            previous_match_type = type;
            type = NULL;
        }
next:
        av_freep(&fmt_info);
        for (int j = 0; j < nb_range; ++j) {
            if (new_range[j]) {
                av_freep(&new_range[j]->str);
                av_freep(&new_range[j]);
            }
        }
        if (type && type->pbFormat)
            CoTaskMemFree(type->pbFormat);
        CoTaskMemFree(type);
        type = NULL;
    }

    // set the pin's format, if wanted
    if (pformat_set && !format_set) {
        if (previous_match_type) {
            // previously found a matching VIDEOINFOHEADER format and stored
            // it for safe keeping. Searching further for a matching
            // VIDEOINFOHEADER2 format yielded nothing. So set the pin's
            // format based on the VIDEOINFOHEADER format.
            // NB: this never applies to an audio format because
            // previous_match_type always NULL in that case
            if (IAMStreamConfig_SetFormat(config, previous_match_type) == S_OK)
                format_set = 1;
        }
        else if (use_default) {
            // default format returned by device apparently was not contained
            // in the capabilities of any of the formats returned by the device
            // (sic?). Fall back to directly setting the default format
            dshow_get_default_format(pin, config, devtype, &type);
            if (IAMStreamConfig_SetFormat(config, type) == S_OK)
                format_set = 1;
            if (type && type->pbFormat)
                CoTaskMemFree(type->pbFormat);
            CoTaskMemFree(type);
            type = NULL;
        }
    }

end:
    if (previous_match_type && previous_match_type->pbFormat)
        CoTaskMemFree(previous_match_type->pbFormat);
    CoTaskMemFree(previous_match_type);
    IAMStreamConfig_Release(config);
    av_free(caps);
    if (pformat_set)
        *pformat_set = format_set;
}

/**
 * Set audio device buffer size in milliseconds (which can directly impact
 * latency, depending on the device).
 */
static int
dshow_set_audio_buffer_size(AVFormatContext *avctx, IPin *pin)
{
    struct dshow_ctx *ctx = avctx->priv_data;
    IAMBufferNegotiation *buffer_negotiation = NULL;
    ALLOCATOR_PROPERTIES props = { -1, -1, -1, -1 };
    IAMStreamConfig *config = NULL;
    AM_MEDIA_TYPE *type = NULL;
    int ret = AVERROR(EIO);

    if (IPin_QueryInterface(pin, &IID_IAMStreamConfig, (void **) &config) != S_OK)
        goto end;
    if (IAMStreamConfig_GetFormat(config, &type) != S_OK)
        goto end;
    if (!IsEqualGUID(&type->formattype, &FORMAT_WaveFormatEx))
        goto end;

    props.cbBuffer = (((WAVEFORMATEX *) type->pbFormat)->nAvgBytesPerSec)
                   * ctx->audio_buffer_size / 1000;

    if (IPin_QueryInterface(pin, &IID_IAMBufferNegotiation, (void **) &buffer_negotiation) != S_OK)
        goto end;
    if (IAMBufferNegotiation_SuggestAllocatorProperties(buffer_negotiation, &props) != S_OK)
        goto end;

    ret = 0;

end:
    if (buffer_negotiation)
        IAMBufferNegotiation_Release(buffer_negotiation);
    if (type) {
        if (type->pbFormat)
            CoTaskMemFree(type->pbFormat);
        CoTaskMemFree(type);
    }
    if (config)
        IAMStreamConfig_Release(config);

    return ret;
}

/**
 * Pops up a user dialog allowing them to adjust properties for the given filter, if possible.
 */
void
ff_dshow_show_filter_properties(IBaseFilter *device_filter, AVFormatContext *avctx) {
    ISpecifyPropertyPages *property_pages = NULL;
    IUnknown *device_filter_iunknown = NULL;
    HRESULT hr;
    FILTER_INFO filter_info = {0}; /* a warning on this line is false positive GCC bug 53119 AFAICT */
    CAUUID ca_guid = {0};

    hr  = IBaseFilter_QueryInterface(device_filter, &IID_ISpecifyPropertyPages, (void **)&property_pages);
    if (hr != S_OK) {
        av_log(avctx, AV_LOG_WARNING, "requested filter does not have a property page to show");
        goto end;
    }
    hr = IBaseFilter_QueryFilterInfo(device_filter, &filter_info);
    if (hr != S_OK) {
        goto fail;
    }
    hr = IBaseFilter_QueryInterface(device_filter, &IID_IUnknown, (void **)&device_filter_iunknown);
    if (hr != S_OK) {
        goto fail;
    }
    hr = ISpecifyPropertyPages_GetPages(property_pages, &ca_guid);
    if (hr != S_OK) {
        goto fail;
    }
    hr = OleCreatePropertyFrame(NULL, 0, 0, filter_info.achName, 1, &device_filter_iunknown, ca_guid.cElems,
        ca_guid.pElems, 0, 0, NULL);
    if (hr != S_OK) {
        goto fail;
    }
    goto end;
fail:
    av_log(avctx, AV_LOG_ERROR, "Failure showing property pages for filter");
end:
    if (property_pages)
        ISpecifyPropertyPages_Release(property_pages);
    if (device_filter_iunknown)
        IUnknown_Release(device_filter_iunknown);
    if (filter_info.pGraph)
        IFilterGraph_Release(filter_info.pGraph);
    if (ca_guid.pElems)
        CoTaskMemFree(ca_guid.pElems);
}

/**
 * Cycle through available pins using the device_filter device, of type
 * devtype, retrieve the first output pin and return the pointer to the
 * object found in *ppin.
 * If ppin is NULL, cycle through all pins listing audio/video capabilities.
 * If ppin is not NULL and ranges is also not null, enumerate all formats
 * supported by the selected pin.
 */
static int
dshow_cycle_pins(AVFormatContext *avctx, enum dshowDeviceType devtype,
                 enum dshowSourceFilterType sourcetype, IBaseFilter *device_filter,
                 IPin **ppin, AVOptionRanges *ranges, enum AVDeviceCapabilitiesQueryType query_type)
{
    struct dshow_ctx *ctx = avctx->priv_data;
    IEnumPins *pins = 0;
    IPin *device_pin = NULL;
    IPin *pin;
    int r;

    const char *devtypename = (devtype == VideoDevice) ? "video" : "audio only";
    const char *sourcetypename = (sourcetype == VideoSourceDevice) ? "video" : "audio";

    int set_format = dshow_should_set_format(avctx, devtype);
    int format_set = 0;
    int should_show_properties = (devtype == VideoDevice) ? ctx->show_video_device_dialog : ctx->show_audio_device_dialog;

    if (should_show_properties)
        ff_dshow_show_filter_properties(device_filter, avctx);

    r = IBaseFilter_EnumPins(device_filter, &pins);
    if (r != S_OK) {
        av_log(avctx, AV_LOG_ERROR, "Could not enumerate pins.\n");
        return AVERROR(EIO);
    }

    if (!ppin) {
        av_log(avctx, AV_LOG_INFO, "DirectShow %s device options (from %s devices)\n",
               devtypename, sourcetypename);
    }

    while (!device_pin && IEnumPins_Next(pins, 1, &pin, NULL) == S_OK) {
        IKsPropertySet *p = NULL;
        PIN_INFO info = {0};
        GUID category;
        DWORD r2;
        char *name_buf = NULL;
        wchar_t *pin_id = NULL;
        char *pin_buf = NULL;
        char *desired_pin_name = devtype == VideoDevice ? ctx->video_pin_name : ctx->audio_pin_name;
        int nb_ranges = ranges ? ranges->nb_ranges : 0;

        IPin_QueryPinInfo(pin, &info);
        IBaseFilter_Release(info.pFilter);

        if (info.dir != PINDIR_OUTPUT)
            goto next;
        if (IPin_QueryInterface(pin, &IID_IKsPropertySet, (void **) &p) != S_OK)
            goto next;
        if (IKsPropertySet_Get(p, &AMPROPSETID_Pin, AMPROPERTY_PIN_CATEGORY,
                               NULL, 0, &category, sizeof(GUID), &r2) != S_OK)
            goto next;
        if (!IsEqualGUID(&category, &PIN_CATEGORY_CAPTURE))
            goto next;
        name_buf = dup_wchar_to_utf8(info.achName);

        r = IPin_QueryId(pin, &pin_id);
        if (r != S_OK) {
            av_log(avctx, AV_LOG_ERROR, "Could not query pin id\n");
            return AVERROR(EIO);
        }
        pin_buf = dup_wchar_to_utf8(pin_id);

        if (!ppin) {
            av_log(avctx, AV_LOG_INFO, " Pin \"%s\" (alternative pin name \"%s\")\n", name_buf, pin_buf);
            dshow_cycle_formats(avctx, devtype, pin, NULL, NULL, AV_DEV_CAP_QUERY_NONE);
            goto next;
        }

        if (desired_pin_name) {
            if(strcmp(name_buf, desired_pin_name) && strcmp(pin_buf, desired_pin_name)) {
                av_log(avctx, AV_LOG_DEBUG, "skipping pin \"%s\" (\"%s\") != requested \"%s\"\n",
                    name_buf, pin_buf, desired_pin_name);
                goto next;
            }
        }

        // will either try to find format matching options supplied by user
        // or try to open default format. Successful if returns with format_set==1
        // if ranges is non-NULL, will iterate over all formats and return info
        // about all the valid ones. If any valid found, format_set==1, else
        // format_set will be 0
        dshow_cycle_formats(avctx, devtype, pin, &format_set, ranges, query_type);
        if (!format_set) {
            goto next;
        }

        if (devtype == AudioDevice && ctx->audio_buffer_size && !ranges) {
            if (dshow_set_audio_buffer_size(avctx, pin) < 0) {
                av_log(avctx, AV_LOG_ERROR, "unable to set audio buffer size %d to pin, using pin anyway...", ctx->audio_buffer_size);
            }
        }

        if (format_set) {
            device_pin = pin;
            av_log(avctx, AV_LOG_DEBUG, "Selecting pin %s on %s\n", name_buf, devtypename);
        }
next:
        if (p)
            IKsPropertySet_Release(p);
        if (device_pin != pin) {
            IPin_Release(pin);
            // remove any option ranges info we just added, wrong pin
            if (ranges && nb_ranges>0) {
                int nb_original_entries = nb_ranges * ranges->nb_components;
                for (int i = nb_original_entries; i < ranges->nb_ranges * ranges->nb_components; i++) {
                    AVOptionRange *range = ranges->range[i];
                    if (range) {
                        av_freep(&range->str);
                        av_freep(&ranges->range[i]);
                    }
                }
                if (av_reallocp_array(&ranges->range, nb_original_entries, sizeof(*ranges->range)) < 0)
                    ranges->nb_ranges = 0;
                else
                    ranges->nb_ranges = nb_ranges;
            }
            device_pin = NULL;
        }
        av_free(name_buf);
        av_free(pin_buf);
        if (pin_id)
            CoTaskMemFree(pin_id);
    }

    IEnumPins_Release(pins);

    if (ppin) {
        if (set_format && !format_set) {
            av_log(avctx, AV_LOG_ERROR, "Could not set %s options\n", devtypename);
            return AVERROR(EIO);
        }
        if (!device_pin) {
            av_log(avctx, AV_LOG_ERROR,
                "Could not find output pin from %s capture device.\n", devtypename);
            return AVERROR(EIO);
        }
        *ppin = device_pin;
    }

    return 0;
}

/**
 * List options for device with type devtype, source filter type sourcetype
 *
 * @param devenum device enumerator used for accessing the device
 */
static int
dshow_list_device_options(AVFormatContext *avctx, ICreateDevEnum *devenum,
                          enum dshowDeviceType devtype, enum dshowSourceFilterType sourcetype,
                          AVOptionRanges *ranges, enum AVDeviceCapabilitiesQueryType query_type)
{
    struct dshow_ctx *ctx = avctx->priv_data;
    IBaseFilter *device_filter = NULL;
    IPin *device_pin = NULL;
    char *device_unique_name = NULL;
    int r;

    if ((r = dshow_cycle_devices(avctx, devenum, devtype, sourcetype, &device_filter, &device_unique_name, NULL)) < 0)
        return r;
    ctx->device_filter[devtype] = device_filter;
    ctx->device_unique_name[devtype] = device_unique_name;
    if ((r = dshow_cycle_pins(avctx, devtype, sourcetype, device_filter, ranges ? &device_pin : NULL, ranges, query_type)) < 0)
        return r;
    return 0;
}

static int
dshow_open_device(AVFormatContext *avctx, ICreateDevEnum *devenum,
                  enum dshowDeviceType devtype, enum dshowSourceFilterType sourcetype)
{
    struct dshow_ctx *ctx = avctx->priv_data;
    IBaseFilter *device_filter = NULL;
    char *device_filter_unique_name = NULL;
    IGraphBuilder *graph = ctx->graph;
    IPin *device_pin = NULL;
    DShowPin *capture_pin = NULL;
    DShowFilter *capture_filter = NULL;
    ICaptureGraphBuilder2 *graph_builder2 = NULL;
    int ret = AVERROR(EIO);
    int r;
    IStream *ifile_stream = NULL;
    IStream *ofile_stream = NULL;
    IPersistStream *pers_stream = NULL;
    enum dshowDeviceType otherDevType = (devtype == VideoDevice) ? AudioDevice : VideoDevice;

    const wchar_t *filter_name[2] = { L"Audio capture filter", L"Video capture filter" };


    if ( ((ctx->audio_filter_load_file) && (strlen(ctx->audio_filter_load_file)>0) && (sourcetype == AudioSourceDevice)) ||
            ((ctx->video_filter_load_file) && (strlen(ctx->video_filter_load_file)>0) && (sourcetype == VideoSourceDevice)) ) {
        HRESULT hr;
        char *filename = NULL;

        if (sourcetype == AudioSourceDevice)
            filename = ctx->audio_filter_load_file;
        else
            filename = ctx->video_filter_load_file;

        hr = SHCreateStreamOnFile ((LPCSTR) filename, STGM_READ, &ifile_stream);
        if (S_OK != hr) {
            av_log(avctx, AV_LOG_ERROR, "Could not open capture filter description file.\n");
            goto error;
        }

        hr = OleLoadFromStream(ifile_stream, &IID_IBaseFilter, (void **) &device_filter);
        if (hr != S_OK) {
            av_log(avctx, AV_LOG_ERROR, "Could not load capture filter from file.\n");
            goto error;
        }

        if (sourcetype == AudioSourceDevice)
            av_log(avctx, AV_LOG_INFO, "Audio-");
        else
            av_log(avctx, AV_LOG_INFO, "Video-");
        av_log(avctx, AV_LOG_INFO, "Capture filter loaded successfully from file \"%s\".\n", filename);
    } else {

        if ((r = dshow_cycle_devices(avctx, devenum, devtype, sourcetype, &device_filter, &device_filter_unique_name, NULL)) < 0) {
            ret = r;
            goto error;
        }
    }

    if (ctx->device_filter[otherDevType]) {
        // avoid adding add two instances of the same device to the graph, one for video, one for audio
        // a few devices don't support this (could also do this check earlier to avoid double crossbars, etc. but they seem OK)
        if (strcmp(device_filter_unique_name, ctx->device_unique_name[otherDevType]) == 0) {
          av_log(avctx, AV_LOG_DEBUG, "reusing previous graph capture filter... %s\n", device_filter_unique_name);
          IBaseFilter_Release(device_filter);
          device_filter = ctx->device_filter[otherDevType];
          IBaseFilter_AddRef(ctx->device_filter[otherDevType]);
        } else {
            av_log(avctx, AV_LOG_DEBUG, "not reusing previous graph capture filter %s != %s\n", device_filter_unique_name, ctx->device_unique_name[otherDevType]);
        }
    }

    ctx->device_filter [devtype] = device_filter;
    ctx->device_unique_name [devtype] = device_filter_unique_name;

    r = IGraphBuilder_AddFilter(graph, device_filter, NULL);
    if (r != S_OK) {
        av_log(avctx, AV_LOG_ERROR, "Could not add device filter to graph.\n");
        goto error;
    }

    if ((r = dshow_cycle_pins(avctx, devtype, sourcetype, device_filter, &device_pin, NULL, AV_DEV_CAP_QUERY_NONE)) < 0) {
        ret = r;
        goto error;
    }

    ctx->device_pin[devtype] = device_pin;

    capture_filter = ff_dshow_filter_Create(avctx, callback, devtype);
    if (!capture_filter) {
        av_log(avctx, AV_LOG_ERROR, "Could not create grabber filter.\n");
        goto error;
    }
    ctx->capture_filter[devtype] = capture_filter;

    if ( ((ctx->audio_filter_save_file) && (strlen(ctx->audio_filter_save_file)>0) && (sourcetype == AudioSourceDevice)) ||
            ((ctx->video_filter_save_file) && (strlen(ctx->video_filter_save_file)>0) && (sourcetype == VideoSourceDevice)) ) {

        HRESULT hr;
        char *filename = NULL;

        if (sourcetype == AudioSourceDevice)
            filename = ctx->audio_filter_save_file;
        else
            filename = ctx->video_filter_save_file;

        hr = SHCreateStreamOnFile ((LPCSTR) filename, STGM_CREATE | STGM_READWRITE, &ofile_stream);
        if (S_OK != hr) {
            av_log(avctx, AV_LOG_ERROR, "Could not create capture filter description file.\n");
            goto error;
        }

        hr  = IBaseFilter_QueryInterface(device_filter, &IID_IPersistStream, (void **) &pers_stream);
        if (hr != S_OK) {
            av_log(avctx, AV_LOG_ERROR, "Query for IPersistStream failed.\n");
            goto error;
        }

        hr = OleSaveToStream(pers_stream, ofile_stream);
        if (hr != S_OK) {
            av_log(avctx, AV_LOG_ERROR, "Could not save capture filter \n");
            goto error;
        }

        hr = IStream_Commit(ofile_stream, STGC_DEFAULT);
        if (S_OK != hr) {
            av_log(avctx, AV_LOG_ERROR, "Could not commit capture filter data to file.\n");
            goto error;
        }

        if (sourcetype == AudioSourceDevice)
            av_log(avctx, AV_LOG_INFO, "Audio-");
        else
            av_log(avctx, AV_LOG_INFO, "Video-");
        av_log(avctx, AV_LOG_INFO, "Capture filter saved successfully to file \"%s\".\n", filename);
    }

    r = IGraphBuilder_AddFilter(graph, (IBaseFilter *) capture_filter,
                                filter_name[devtype]);
    if (r != S_OK) {
        av_log(avctx, AV_LOG_ERROR, "Could not add capture filter to graph\n");
        goto error;
    }

    ff_dshow_pin_AddRef(capture_filter->pin);
    capture_pin = capture_filter->pin;
    ctx->capture_pin[devtype] = capture_pin;

    r = CoCreateInstance(&CLSID_CaptureGraphBuilder2, NULL, CLSCTX_INPROC_SERVER,
                         &IID_ICaptureGraphBuilder2, (void **) &graph_builder2);
    if (r != S_OK) {
        av_log(avctx, AV_LOG_ERROR, "Could not create CaptureGraphBuilder2\n");
        goto error;
    }
    ctx->graph_builder2[devtype] = graph_builder2;
    ICaptureGraphBuilder2_SetFiltergraph(graph_builder2, graph);
    if (r != S_OK) {
        av_log(avctx, AV_LOG_ERROR, "Could not set graph for CaptureGraphBuilder2\n");
        goto error;
    }

    r = ICaptureGraphBuilder2_RenderStream(graph_builder2, NULL, NULL, (IUnknown *) device_pin, NULL /* no intermediate filter */,
        (IBaseFilter *) capture_filter); /* connect pins, optionally insert intermediate filters like crossbar if necessary */

    if (r != S_OK) {
        av_log(avctx, AV_LOG_ERROR, "Could not RenderStream to connect pins\n");
        goto error;
    }

    r = ff_dshow_try_setup_crossbar_options(graph_builder2, device_filter, devtype, avctx);

    if (r != S_OK) {
        av_log(avctx, AV_LOG_ERROR, "Could not setup CrossBar\n");
        goto error;
    }

    ret = 0;

error:
    if (pers_stream)
        IPersistStream_Release(pers_stream);

    if (ifile_stream)
        IStream_Release(ifile_stream);

    if (ofile_stream)
        IStream_Release(ofile_stream);

    return ret;
}

static int dshow_control_message(AVFormatContext *avctx, int type, void *data, size_t data_size)
{
    struct dshow_ctx *ctx = avctx->priv_data;
    int run_state = ctx->is_running;
    HRESULT hr;
    int ret = 0;

    switch (type) {
    case AV_APP_TO_DEV_PAUSE:
        run_state = 0;
        break;
    case AV_APP_TO_DEV_PLAY:
        run_state = 1;
        break;
    case AV_APP_TO_DEV_TOGGLE_PAUSE:
        run_state = !run_state;
        break;
    case AV_APP_TO_DEV_CONFIG: {
        /* For documentation of dialog variable, see ffmpeg-devices.html in docs */
        int dialog;
        enum dshowDeviceType devtype;
        
        if (!data)
            av_log(avctx, AV_LOG_ERROR, "Use the data argument to indicate which dialog should be shown.");
        dialog = *(int *) data;
        devtype = (dialog & 1) ? AudioDevice : VideoDevice;
        
        if (dialog & 1<<1) {
            // device_dialog
            if (ctx->device_filter[devtype])
                ff_dshow_show_filter_properties(ctx->device_filter[devtype], avctx);
        } else if (dialog & 1<<2) {
            // crossbar_connection_dialog
            if (ctx->device_filter[devtype] && ctx->graph_builder2[devtype]) {
                IAMCrossbar *cross_bar = NULL;
                IBaseFilter *cross_bar_base_filter = NULL;
                hr = ff_dshow_get_crossbar_and_filter(ctx->graph_builder2[devtype], ctx->device_filter[devtype], cross_bar, &cross_bar_base_filter);

                if (hr == S_OK && cross_bar_base_filter)
                    ff_dshow_show_filter_properties(cross_bar_base_filter, avctx);

                if (cross_bar)
                    IAMCrossbar_Release(cross_bar);
                if (cross_bar_base_filter)
                    IBaseFilter_Release(cross_bar_base_filter);
            }
        } else if (dialog & 1<<3) {
            // tv_tuner_dialog
            if (ctx->device_filter[devtype] && ctx->graph_builder2[devtype]) {
                if (devtype == VideoDevice) {
                    IAMTVTuner *tv_tuner_filter = NULL;
                    IBaseFilter *tv_tuner_base_filter = NULL;
                    hr = ff_dshow_get_tvtuner_and_filter(ctx->graph_builder2[devtype], ctx->device_filter[devtype], tv_tuner_filter, tv_tuner_base_filter);

                    if (hr == S_OK && tv_tuner_base_filter)
                        ff_dshow_show_filter_properties(tv_tuner_base_filter, avctx);

                    if (tv_tuner_filter)
                        IAMTVTuner_Release(tv_tuner_filter);
                    if (tv_tuner_base_filter)
                        IBaseFilter_Release(tv_tuner_base_filter);
                } else {
                    IAMAudioInputMixer *tv_audio_filter = NULL;
                    IBaseFilter *tv_audio_base_filter = NULL;
                    hr = ff_dshow_get_audiomixer_and_filter(ctx->graph_builder2[devtype], ctx->device_filter[devtype], tv_audio_filter, tv_audio_base_filter);

                    if (hr == S_OK && tv_audio_base_filter)
                        ff_dshow_show_filter_properties(tv_audio_base_filter, avctx);

                    if (tv_audio_filter)
                        IAMAudioInputMixer_Release(tv_audio_filter);
                    if (tv_audio_base_filter)
                        IBaseFilter_Release(tv_audio_base_filter);
                }
            }
        }
        break;
    }

    default:
        ret = AVERROR(ENOSYS);
    }

    // if play state change requested, apply
    if (run_state != ctx->is_running) {
        if (run_state)
            hr = IMediaControl_Run(ctx->control);
        else
            hr = IMediaControl_Pause(ctx->control);

        if (hr == S_FALSE) {
            OAFilterState pfs;
            hr = IMediaControl_GetState(ctx->control, 0, &pfs);
        }
        if (hr != S_OK) {
            av_log(avctx, AV_LOG_ERROR, "Could not run/pause graph\n");
            return AVERROR(EIO);
        }
        ctx->is_running = run_state;
    }

    return ret;
}

static int
dshow_add_device(AVFormatContext *avctx,
                 enum dshowDeviceType devtype)
{
    struct dshow_ctx *ctx = avctx->priv_data;
    AM_MEDIA_TYPE type;
    AVCodecParameters *par;
    AVStream *st;
    struct dshow_format_info *fmt_info = NULL;
    int ret = AVERROR(EIO);

    type.pbFormat = NULL;

    st = avformat_new_stream(avctx, NULL);
    if (!st) {
        ret = AVERROR(ENOMEM);
        goto error;
    }
    st->id = devtype;

    ctx->capture_filter[devtype]->stream_index = st->index;

    ff_dshow_pin_ConnectionMediaType(ctx->capture_pin[devtype], &type);
    fmt_info = dshow_get_format_info(&type);
    if (!fmt_info) {
        ret = AVERROR(EIO);
        goto error;
    }

    par = st->codecpar;
    if (devtype == VideoDevice) {
        BITMAPINFOHEADER *bih = NULL;
        AVRational time_base;

        if (IsEqualGUID(&type.formattype, &FORMAT_VideoInfo)) {
            VIDEOINFOHEADER *v = (void *) type.pbFormat;
            time_base = (AVRational) { v->AvgTimePerFrame, 10000000 };
            bih = &v->bmiHeader;
        } else if (IsEqualGUID(&type.formattype, &FORMAT_VideoInfo2)) {
            VIDEOINFOHEADER2 *v = (void *) type.pbFormat;
            time_base = (AVRational) { v->AvgTimePerFrame, 10000000 };
            bih = &v->bmiHeader;
        }
        if (!bih) {
            av_log(avctx, AV_LOG_ERROR, "Could not get media type.\n");
            goto error;
        }

        st->avg_frame_rate = av_inv_q(time_base);
        st->r_frame_rate = av_inv_q(time_base);

        par->codec_type = AVMEDIA_TYPE_VIDEO;
        par->width      = fmt_info->width;
        par->height     = fmt_info->height;
        par->codec_tag  = bih->biCompression;
        par->format     = fmt_info->pix_fmt;
        if (bih->biCompression == MKTAG('H', 'D', 'Y', 'C')) {
            av_log(avctx, AV_LOG_DEBUG, "attempt to use full range for HDYC...\n");
            par->color_range = AVCOL_RANGE_MPEG; // just in case it needs this...
        }
        par->color_range = fmt_info->col_range;
        par->color_space = fmt_info->col_space;
        par->color_primaries = fmt_info->col_prim;
        par->color_trc = fmt_info->col_trc;
        par->chroma_location = fmt_info->chroma_loc;
        par->codec_id = fmt_info->codec_id;
        if (par->codec_id == AV_CODEC_ID_RAWVIDEO) {
            if (bih->biCompression == BI_RGB || bih->biCompression == BI_BITFIELDS) {
                par->bits_per_coded_sample = bih->biBitCount;
                if (par->height < 0) {
                    par->height *= -1;
                } else {
                    par->extradata = av_malloc(9 + AV_INPUT_BUFFER_PADDING_SIZE);
                    if (par->extradata) {
                        par->extradata_size = 9;
                        memcpy(par->extradata, "BottomUp", 9);
                    }
                }
            }
        } else {
            if (par->codec_id == AV_CODEC_ID_NONE) {
                av_log(avctx, AV_LOG_ERROR, "Unknown compression type. "
                                 "Please report type 0x%X.\n", (int) bih->biCompression);
                ret = AVERROR_PATCHWELCOME;
                goto error;
            }
            par->bits_per_coded_sample = bih->biBitCount;
        }
    } else {
        if (!IsEqualGUID(&type.formattype, &FORMAT_WaveFormatEx)) {
            av_log(avctx, AV_LOG_ERROR, "Could not get media type.\n");
            goto error;
        }

        par->codec_type  = AVMEDIA_TYPE_AUDIO;
        par->format      = sample_fmt_bits_per_sample(fmt_info->sample_size);
        par->codec_id    = waveform_codec_id(par->format);
        par->sample_rate = fmt_info->sample_rate;
        par->ch_layout.nb_channels = fmt_info->channels;
    }

    avpriv_set_pts_info(st, 64, 1, 10000000);

    ret = 0;

error:
    av_freep(&fmt_info);
    if (type.pbFormat)
        CoTaskMemFree(type.pbFormat);
    return ret;
}

static int parse_device_name(AVFormatContext *avctx)
{
    struct dshow_ctx *ctx = avctx->priv_data;
    char **device_name = ctx->device_name;
    char *name = av_strdup(avctx->url);
    char *tmp = name;
    int ret = 1;
    char *type;

    while ((type = strtok(tmp, "="))) {
        char *token = strtok(NULL, ":");
        tmp = NULL;

        if        (!strcmp(type, "video")) {
            device_name[0] = token;
        } else if (!strcmp(type, "audio")) {
            device_name[1] = token;
        } else {
            device_name[0] = NULL;
            device_name[1] = NULL;
            break;
        }
    }

    if (!device_name[0] && !device_name[1]) {
        ret = 0;
    } else {
        if (device_name[0])
            device_name[0] = av_strdup(device_name[0]);
        if (device_name[1])
            device_name[1] = av_strdup(device_name[1]);
    }

    av_free(name);
    return ret;
}

static int dshow_read_header(AVFormatContext *avctx)
{
    struct dshow_ctx *ctx = avctx->priv_data;
    IGraphBuilder *graph = NULL;
    ICreateDevEnum *devenum = NULL;
    IMediaControl *control = NULL;
    IMediaEvent *media_event = NULL;
    HANDLE media_event_handle;
    HANDLE proc;
    int ret = AVERROR(EIO);
    int r;

    CoInitialize(0);

    if (!ctx->list_devices && !parse_device_name(avctx)) {
        av_log(avctx, AV_LOG_ERROR, "Malformed dshow input string.\n");
        goto error;
    }

    ctx->video_codec_id = avctx->video_codec_id ? avctx->video_codec_id
                                                : AV_CODEC_ID_RAWVIDEO;
    if (ctx->pixel_format != AV_PIX_FMT_NONE) {
        if (ctx->video_codec_id != AV_CODEC_ID_RAWVIDEO) {
            av_log(avctx, AV_LOG_ERROR, "Pixel format may only be set when "
                              "video codec is not set or set to rawvideo\n");
            ret = AVERROR(EINVAL);
            goto error;
        }
    }
    if (ctx->framerate) {
        r = av_parse_video_rate(&ctx->requested_framerate, ctx->framerate);
        if (r < 0) {
            av_log(avctx, AV_LOG_ERROR, "Could not parse framerate '%s'.\n", ctx->framerate);
            goto error;
        }
    }

    r = CoCreateInstance(&CLSID_FilterGraph, NULL, CLSCTX_INPROC_SERVER,
                         &IID_IGraphBuilder, (void **) &graph);
    if (r != S_OK) {
        av_log(avctx, AV_LOG_ERROR, "Could not create capture graph.\n");
        goto error;
    }
    ctx->graph = graph;

    r = CoCreateInstance(&CLSID_SystemDeviceEnum, NULL, CLSCTX_INPROC_SERVER,
                         &IID_ICreateDevEnum, (void **) &devenum);
    if (r != S_OK) {
        av_log(avctx, AV_LOG_ERROR, "Could not enumerate system devices.\n");
        goto error;
    }

    if (ctx->list_devices) {
        dshow_cycle_devices(avctx, devenum, VideoDevice, VideoSourceDevice, NULL, NULL, NULL);
        dshow_cycle_devices(avctx, devenum, AudioDevice, AudioSourceDevice, NULL, NULL, NULL);
        ret = AVERROR_EXIT;
        goto error;
    }
    if (ctx->list_options) {
        if (ctx->device_name[VideoDevice])
            if ((r = dshow_list_device_options(avctx, devenum, VideoDevice, VideoSourceDevice, NULL, AV_DEV_CAP_QUERY_NONE))) {
                ret = r;
                goto error;
            }
        if (ctx->device_name[AudioDevice]) {
            if (dshow_list_device_options(avctx, devenum, AudioDevice, AudioSourceDevice, NULL, AV_DEV_CAP_QUERY_NONE)) {
                /* show audio options from combined video+audio sources as fallback */
                if ((r = dshow_list_device_options(avctx, devenum, AudioDevice, VideoSourceDevice, NULL, AV_DEV_CAP_QUERY_NONE))) {
                    ret = r;
                    goto error;
                }
            }
        }
        // don't exit yet, allow it to list crossbar options in dshow_open_device
    }
    ctx->is_running = 0;
    if (ctx->device_name[VideoDevice]) {
        if ((r = dshow_open_device(avctx, devenum, VideoDevice, VideoSourceDevice)) < 0 ||
            (r = dshow_add_device(avctx, VideoDevice)) < 0) {
            ret = r;
            goto error;
        }
    }
    if (ctx->device_name[AudioDevice]) {
        if ((r = dshow_open_device(avctx, devenum, AudioDevice, AudioSourceDevice)) < 0 ||
            (r = dshow_add_device(avctx, AudioDevice)) < 0) {
            av_log(avctx, AV_LOG_INFO, "Searching for audio device within video devices for %s\n", ctx->device_name[AudioDevice]);
            /* see if there's a video source with an audio pin with the given audio name */
            if ((r = dshow_open_device(avctx, devenum, AudioDevice, VideoSourceDevice)) < 0 ||
                (r = dshow_add_device(avctx, AudioDevice)) < 0) {
                ret = r;
                goto error;
            }
        }
    }
    if (ctx->list_options) {
        /* allow it to list crossbar options in dshow_open_device */
        ret = AVERROR_EXIT;
        goto error;
    }
    ctx->curbufsize[0] = 0;
    ctx->curbufsize[1] = 0;
    ctx->mutex = CreateMutex(NULL, 0, NULL);
    if (!ctx->mutex) {
        av_log(avctx, AV_LOG_ERROR, "Could not create Mutex\n");
        goto error;
    }
    ctx->event[1] = CreateEvent(NULL, 1, 0, NULL);
    if (!ctx->event[1]) {
        av_log(avctx, AV_LOG_ERROR, "Could not create Event\n");
        goto error;
    }

    r = IGraphBuilder_QueryInterface(graph, &IID_IMediaControl, (void **) &control);
    if (r != S_OK) {
        av_log(avctx, AV_LOG_ERROR, "Could not get media control.\n");
        goto error;
    }
    ctx->control = control;

    r = IGraphBuilder_QueryInterface(graph, &IID_IMediaEvent, (void **) &media_event);
    if (r != S_OK) {
        av_log(avctx, AV_LOG_ERROR, "Could not get media event.\n");
        goto error;
    }
    ctx->media_event = media_event;

    r = IMediaEvent_GetEventHandle(media_event, (void *) &media_event_handle);
    if (r != S_OK) {
        av_log(avctx, AV_LOG_ERROR, "Could not get media event handle.\n");
        goto error;
    }
    proc = GetCurrentProcess();
    r = DuplicateHandle(proc, media_event_handle, proc, &ctx->event[0],
                        0, 0, DUPLICATE_SAME_ACCESS);
    if (!r) {
        av_log(avctx, AV_LOG_ERROR, "Could not duplicate media event handle.\n");
        goto error;
    }

    r = IMediaControl_Run(control);
    if (r == S_FALSE) {
        OAFilterState pfs;
        r = IMediaControl_GetState(control, 0, &pfs);
    }
    if (r != S_OK) {
        av_log(avctx, AV_LOG_ERROR, "Could not run graph (sometimes caused by a device already in use by other application)\n");
        goto error;
    }
    ctx->is_running = 1;

    ret = 0;

error:

    if (devenum)
        ICreateDevEnum_Release(devenum);

    if (ret < 0)
        dshow_read_close(avctx);

    return ret;
}

/**
 * Checks media events from DirectShow and returns -1 on error or EOF. Also
 * purges all events that might be in the event queue to stop the trigger
 * of event notification.
 */
static int dshow_check_event_queue(IMediaEvent *media_event)
{
    LONG_PTR p1, p2;
    long code;
    int ret = 0;

    while (IMediaEvent_GetEvent(media_event, &code, &p1, &p2, 0) != E_ABORT) {
        if (code == EC_COMPLETE || code == EC_DEVICE_LOST || code == EC_ERRORABORT)
            ret = -1;
        IMediaEvent_FreeEventParams(media_event, code, p1, p2);
    }

    return ret;
}

static int dshow_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    struct dshow_ctx *ctx = s->priv_data;
    PacketListEntry *pktl = NULL;

    while (!ctx->eof && !pktl) {
        WaitForSingleObject(ctx->mutex, INFINITE);
        pktl = ctx->pktl;
        if (pktl) {
            *pkt = pktl->pkt;
            ctx->pktl = ctx->pktl->next;
            av_free(pktl);
            ctx->curbufsize[pkt->stream_index] -= pkt->size;
        }
        ResetEvent(ctx->event[1]);
        ReleaseMutex(ctx->mutex);
        if (!pktl) {
            if (dshow_check_event_queue(ctx->media_event) < 0) {
                ctx->eof = 1;
            } else if (s->flags & AVFMT_FLAG_NONBLOCK) {
                return AVERROR(EAGAIN);
            } else {
                WaitForMultipleObjects(2, ctx->event, 0, INFINITE);
            }
        }
    }

    return ctx->eof ? AVERROR(EIO) : pkt->size;
}

// TODO: consider if and how to expose extra info we have about formats, such as color_range
static int dshow_query_ranges(AVOptionRanges **ranges_arg, void *obj, const char *key, int flags)
{
    AVDeviceCapabilitiesQuery *caps = obj;
    const AVFormatContext *avctx = caps->device_context;
    struct dshow_ctx *ctx = avctx->priv_data;

    int backup_sample_size;
    int backup_sample_rate;
    int backup_channels;
    enum AVCodecID backup_video_codec_id;
    enum AVPixelFormat backup_pixel_format;
    int backup_requested_width;
    int backup_requested_height;
    char* backup_framerate = NULL;

    enum AVDeviceCapabilitiesQueryType query_type = AV_DEV_CAP_QUERY_NONE;

    AVOptionRanges *ranges = av_mallocz(sizeof(**ranges_arg));
    const AVOption *field = av_opt_find(obj, key, NULL, 0, flags);
    int ret;

    ICreateDevEnum *devenum = NULL;

    *ranges_arg = NULL;

    if (!ranges) {
        ret = AVERROR(ENOMEM);
        goto fail1;
    }

    if (!field) {
        ret = AVERROR_OPTION_NOT_FOUND;
        goto fail1;
    }

    // turn option name into cap query
    query_type = ff_device_get_query_type(field->name);

    if (query_type == AV_DEV_CAP_QUERY_CHANNEL_LAYOUT || query_type == AV_DEV_CAP_QUERY_WINDOW_SIZE) {
        av_log(avctx, AV_LOG_ERROR, "Querying the option %s is not supported for a dshow device\n", field->name);
        ret = AVERROR(EINVAL);
        goto fail1;
    }

    // take backup of dshow parameters/options
    // audio
    backup_sample_size = ctx->sample_size;
    backup_sample_rate = ctx->sample_rate;
    backup_channels = ctx->channels;
    // video
    backup_video_codec_id = ctx->video_codec_id;
    backup_pixel_format = ctx->pixel_format;
    backup_requested_width = ctx->requested_width;
    backup_requested_height= ctx->requested_height;
    backup_framerate = ctx->framerate;


    // set format constraints set in AVDeviceCapabilitiesQuery
    // audio (NB: channel_layout not used)
    ctx->sample_size = av_get_bytes_per_sample(caps->sample_format) << 3;
    ctx->sample_rate = (caps->sample_rate == -1) ? 0 : caps->sample_rate;
    ctx->channels = (caps->channels == -1) ? 0 : caps->channels;
    // video (NB: window_width and window_height not used)
    ctx->video_codec_id = caps->codec;
    ctx->pixel_format = caps->pixel_format;
    ctx->requested_width = caps->frame_width;
    ctx->requested_height = caps->frame_height;
    // dshow checks whether requested framerate is set by means of !ctx->framerate.
    // fill with something
    if (!isnan(caps->fps)) {
        ctx->requested_framerate = av_d2q(caps->fps, INT_MAX);
        ctx->framerate = av_strdup("dummy");    // just make sure its non-zero
        if (!ctx->framerate) {
            ret = AVERROR(ENOMEM);
            goto fail2;
        }
    }
    else
        ctx->framerate = NULL;  // make sure its NULL (if it wasn't, we already have a backup of the pointer to restore later)

    // now iterate matching format of pin that would be selected when device
    // is opened with options currently in effect.
    // for each matching format, output its parameter range, also if that same
    // range already returned for another format. That way, user can reconstruct
    // possible valid combinations by av_opt_query_ranges() for each of the
    // format options and matching returned values by sequence number.
    if (CoCreateInstance(&CLSID_SystemDeviceEnum, NULL, CLSCTX_INPROC_SERVER,
                         &IID_ICreateDevEnum, (void **) &devenum)) {
        av_log(avctx, AV_LOG_ERROR, "Could not enumerate system devices.\n");
        ret = AVERROR(EIO);
        goto fail2;
    }

    ctx->video_codec_id = ctx->video_codec_id ? ctx->video_codec_id
                                              : AV_CODEC_ID_RAWVIDEO;

    ranges->nb_components = (field->type == AV_OPT_TYPE_IMAGE_SIZE && (flags & AV_OPT_MULTI_COMPONENT_RANGE)) ? 3 : 1;
    if (ctx->device_name[VideoDevice])
        if ((ret = dshow_list_device_options(avctx, devenum, VideoDevice, VideoSourceDevice, ranges, query_type)) < 0)
            goto fail2;
    if (ctx->device_name[AudioDevice]) {
        if (dshow_list_device_options(avctx, devenum, AudioDevice, AudioSourceDevice, ranges, query_type) < 0) {
            /* show audio options from combined video+audio sources as fallback */
            if ((ret = dshow_list_device_options(avctx, devenum, AudioDevice, VideoSourceDevice, ranges, query_type)) < 0)
                goto fail2;
        }
    }
    ret = ranges->nb_ranges ? ranges->nb_components : 0;

    // when dealing with a multi-component item (regardless of whether
    // AV_OPT_MULTI_COMPONENT_RANGE is set or not), we need to reorganize the
    // output range array from [r1_c1 r1_c2 r1_c3 r2_c1 r2_c2 r2_c3 ...] to
    // [r1_c1 r2_c1 ... r1_c2 r2_c2 ... r1_c3 r2_c3 ...] to be consistent with
    // documentation of AVOptionRanges in libavutil/opt.h
    if (ranges->nb_ranges && ranges->nb_components > 1) {
        AVOptionRange **new_range = av_malloc_array(ranges->nb_components * ranges->nb_ranges, sizeof(*ranges->range));
        AVOptionRange **old_range = ranges->range;
        if (!new_range) {
            ret = AVERROR(ENOMEM);
            goto fail2;
        }
        ranges->nb_ranges /= ranges->nb_components;
        for (int n = 0; n < ranges->nb_components * ranges->nb_ranges; n++) {
            int i = n / ranges->nb_components;
            int j = n % ranges->nb_components;
            new_range[ranges->nb_ranges * j + i] = old_range[n];
        }
        ranges->range = new_range;
        av_freep(&old_range);
    }

    // success, set output
    *ranges_arg = ranges;

fail2:
    // set dshow parameters/options back to original values
    // audio
    ctx->sample_size = backup_sample_size;
    ctx->sample_rate = backup_sample_rate;
    ctx->channels = backup_channels;
    // video
    ctx->video_codec_id = backup_video_codec_id;
    ctx->pixel_format = backup_pixel_format;
    ctx->requested_width = backup_requested_width;
    ctx->requested_height = backup_requested_height;
    if (ctx->framerate)
        av_free(ctx->framerate);
    ctx->framerate = backup_framerate;

    if (devenum)
        ICreateDevEnum_Release(devenum);

fail1:
    if (ret < 0)
        av_opt_freep_ranges(&ranges);

    return ret;
}

// fake class to point av_opt_query_ranges to our query_ranges function
static const AVClass dshow_dev_caps_class = {
    .class_name   = "",
    .item_name    = av_default_item_name,
    .option       = ff_device_capabilities,
    .version      = LIBAVUTIL_VERSION_INT,
    .query_ranges = dshow_query_ranges,
};

static int dshow_create_device_capabilities(struct AVFormatContext *avctx, AVDeviceCapabilitiesQuery *caps)
{
    struct dshow_ctx *ctx = avctx->priv_data;
    int ret = 0;
    ICreateDevEnum *devenum = NULL;

    // set class so queries work
    caps->av_class = &dshow_dev_caps_class;

    if (ctx->device_name[0] || ctx->device_name[1]) {
        av_log(avctx, AV_LOG_ERROR, "You cannot query device capabilities on an opened device\n");
        ret = AVERROR(EIO);
        goto fail;
    }

    if (!parse_device_name(avctx)) {
        av_log(avctx, AV_LOG_ERROR, "You must set a device name (AVFormatContext url) to specify which device to query capabilities from\n");
        ret = AVERROR(EINVAL);
        goto fail;
    }

    CoInitialize(0);
    if (CoCreateInstance(&CLSID_SystemDeviceEnum, NULL, CLSCTX_INPROC_SERVER,
                         &IID_ICreateDevEnum, (void **) &devenum)) {
        av_log(avctx, AV_LOG_ERROR, "Could not enumerate system devices.\n");
        ret = AVERROR(EIO);
        goto fail;
    }

    // check devices can be found
    if (ctx->device_name[VideoDevice]) {
        IBaseFilter *device_filter = NULL;
        char *device_unique_name = NULL;
        if ((ret = dshow_cycle_devices(avctx, devenum, VideoDevice, VideoSourceDevice, &device_filter, &device_unique_name, NULL)) < 0)
            return ret;

        if (ret >= 0) {
            ctx->device_filter[VideoDevice] = device_filter;
            ctx->device_unique_name[VideoDevice] = device_unique_name;
        }
    }
    if (ctx->device_name[AudioDevice]) {
        IBaseFilter *device_filter = NULL;
        char *device_unique_name = NULL;
        if (dshow_cycle_devices(avctx, devenum, AudioDevice, AudioSourceDevice, &device_filter, &device_unique_name, NULL) < 0) {
            /* try to access audio from combined video+audio sources as fallback */
            if ((ret = dshow_cycle_devices(avctx, devenum, AudioDevice, VideoSourceDevice, &device_filter, &device_unique_name, NULL)) < 0)
                goto fail;
        }
        if (ret >= 0) {
            ctx->device_filter[AudioDevice] = device_filter;
            ctx->device_unique_name[AudioDevice] = device_unique_name;
        }
    }

fail:
    if (devenum)
        ICreateDevEnum_Release(devenum);

    if (ret < 0)
        return ret;
    else
        return 0;
}

static int dshow_free_device_capabilities(struct AVFormatContext *avctx, AVDeviceCapabilitiesQuery *caps)
{
    // clear state variables that may have been set during the querying process
    // (e.g. frees device names, removes device_filters, etc)
    dshow_read_close(avctx);

    return 0;
}

#define OFFSET(x) offsetof(struct dshow_ctx, x)
#define DEC AV_OPT_FLAG_DECODING_PARAM
static const AVOption options[] = {
    { "video_size", "set video size given a string such as 640x480 or hd720.", OFFSET(requested_width), AV_OPT_TYPE_IMAGE_SIZE, {.str = NULL}, 0, 0, DEC },
    { "pixel_format", "set video pixel format", OFFSET(pixel_format), AV_OPT_TYPE_PIXEL_FMT, {.i64 = AV_PIX_FMT_NONE}, -1, INT_MAX, DEC },
    { "framerate", "set video frame rate", OFFSET(framerate), AV_OPT_TYPE_STRING, {.str = NULL}, 0, 0, DEC },
    { "sample_rate", "set audio sample rate", OFFSET(sample_rate), AV_OPT_TYPE_INT, {.i64 = 0}, 0, INT_MAX, DEC },
    { "sample_size", "set audio sample size", OFFSET(sample_size), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 16, DEC },
    { "channels", "set number of audio channels, such as 1 or 2", OFFSET(channels), AV_OPT_TYPE_INT, {.i64 = 0}, 0, INT_MAX, DEC },
    { "audio_buffer_size", "set audio device buffer latency size in milliseconds (default is the device's default)", OFFSET(audio_buffer_size), AV_OPT_TYPE_INT, {.i64 = 0}, 0, INT_MAX, DEC },
    { "list_devices", "list available devices",                      OFFSET(list_devices), AV_OPT_TYPE_BOOL, {.i64=0}, 0, 1, DEC },
    { "list_options", "list available options for specified device", OFFSET(list_options), AV_OPT_TYPE_BOOL, {.i64=0}, 0, 1, DEC },
    { "video_device_number", "set video device number for devices with same name (starts at 0)", OFFSET(video_device_number), AV_OPT_TYPE_INT, {.i64 = 0}, 0, INT_MAX, DEC },
    { "audio_device_number", "set audio device number for devices with same name (starts at 0)", OFFSET(audio_device_number), AV_OPT_TYPE_INT, {.i64 = 0}, 0, INT_MAX, DEC },
    { "video_pin_name", "select video capture pin by name", OFFSET(video_pin_name),AV_OPT_TYPE_STRING, {.str = NULL},  0, 0, AV_OPT_FLAG_ENCODING_PARAM },
    { "audio_pin_name", "select audio capture pin by name", OFFSET(audio_pin_name),AV_OPT_TYPE_STRING, {.str = NULL},  0, 0, AV_OPT_FLAG_ENCODING_PARAM },
    { "crossbar_video_input_pin_number", "set video input pin number for crossbar device", OFFSET(crossbar_video_input_pin_number), AV_OPT_TYPE_INT, {.i64 = -1}, -1, INT_MAX, DEC },
    { "crossbar_audio_input_pin_number", "set audio input pin number for crossbar device", OFFSET(crossbar_audio_input_pin_number), AV_OPT_TYPE_INT, {.i64 = -1}, -1, INT_MAX, DEC },
    { "show_video_device_dialog",              "display property dialog for video capture device",                            OFFSET(show_video_device_dialog),              AV_OPT_TYPE_BOOL, {.i64 = 0}, 0, 1, DEC },
    { "show_audio_device_dialog",              "display property dialog for audio capture device",                            OFFSET(show_audio_device_dialog),              AV_OPT_TYPE_BOOL, {.i64 = 0}, 0, 1, DEC },
    { "show_video_crossbar_connection_dialog", "display property dialog for crossbar connecting pins filter on video device", OFFSET(show_video_crossbar_connection_dialog), AV_OPT_TYPE_BOOL, {.i64 = 0}, 0, 1, DEC },
    { "show_audio_crossbar_connection_dialog", "display property dialog for crossbar connecting pins filter on audio device", OFFSET(show_audio_crossbar_connection_dialog), AV_OPT_TYPE_BOOL, {.i64 = 0}, 0, 1, DEC },
    { "show_analog_tv_tuner_dialog",           "display property dialog for analog tuner filter",                             OFFSET(show_analog_tv_tuner_dialog),           AV_OPT_TYPE_BOOL, {.i64 = 0}, 0, 1, DEC },
    { "show_analog_tv_tuner_audio_dialog",     "display property dialog for analog tuner audio filter",                       OFFSET(show_analog_tv_tuner_audio_dialog),     AV_OPT_TYPE_BOOL, {.i64 = 0}, 0, 1, DEC },
    { "audio_device_load", "load audio capture filter device (and properties) from file", OFFSET(audio_filter_load_file), AV_OPT_TYPE_STRING, {.str = NULL}, 0, 0, DEC },
    { "audio_device_save", "save audio capture filter device (and properties) to file", OFFSET(audio_filter_save_file), AV_OPT_TYPE_STRING, {.str = NULL}, 0, 0, DEC },
    { "video_device_load", "load video capture filter device (and properties) from file", OFFSET(video_filter_load_file), AV_OPT_TYPE_STRING, {.str = NULL}, 0, 0, DEC },
    { "video_device_save", "save video capture filter device (and properties) to file", OFFSET(video_filter_save_file), AV_OPT_TYPE_STRING, {.str = NULL}, 0, 0, DEC },
    { "use_video_device_timestamps", "use device instead of wallclock timestamps for video frames", OFFSET(use_video_device_timestamps), AV_OPT_TYPE_BOOL, {.i64 = 1}, 0, 1, DEC },
    { NULL },
};

static const AVClass dshow_class = {
    .class_name = "dshow indev",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
    .category   = AV_CLASS_CATEGORY_DEVICE_VIDEO_INPUT,
};

const AVInputFormat ff_dshow_demuxer = {
    .name                       = "dshow",
    .long_name                  = NULL_IF_CONFIG_SMALL("DirectShow capture"),
    .priv_data_size             = sizeof(struct dshow_ctx),
    .read_header                = dshow_read_header,
    .read_packet                = dshow_read_packet,
    .read_close                 = dshow_read_close,
    .get_device_list            = dshow_get_device_list,
    .control_message            = dshow_control_message,
    .create_device_capabilities = dshow_create_device_capabilities,
    .free_device_capabilities   = dshow_free_device_capabilities,
    .flags                      = AVFMT_NOFILE | AVFMT_NOBINSEARCH | AVFMT_NOGENSEARCH | AVFMT_NO_BYTE_SEEK,
    .priv_class                 = &dshow_class,
};
