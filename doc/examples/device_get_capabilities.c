/*
 * Copyright (c) 2021 Diederick Niehorster
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/**
 * @file
 * avdevice getting capabilities example.
 *
 * Shows how to use the avdevice capabilities API to probe
 * device capabilities (supported codecs, pixel formats, sample
 * formats, resolutions, channel counts, etc)
 * @example device_get_capabilities.c
 */

#include <libavformat/avformat.h>
#include <libavdevice/avdevice.h>
#include <libavutil/avstring.h>
#include <libavutil/log.h>
#include <libavutil/bprint.h>

static int print_option_ranges(enum AVOptionType type, AVOptionRanges *ranges)
{
    for (int range_index = 0; range_index < ranges->nb_ranges; range_index++) {
        int ret;
        char *out_val = NULL;
        AVBPrint bp;
        av_bprint_init(&bp, 0, AV_BPRINT_SIZE_UNLIMITED);
        for (int component_index = 0; component_index < ranges->nb_components; component_index++)
        {
            AVOptionRange* range = ranges->range[ranges->nb_ranges * component_index + range_index];
            if (component_index > 0)
                av_bprintf(&bp, ", ");
            av_bprintf(&bp, "%s: ", range->str);
            if (range->value_min > range->value_max)
                av_bprintf(&bp, "<value not available>");
            else {
                avdevice_capabilities_bprint_num(&bp, range->str, range->value_min);
                if (range->is_range) {
                    av_bprintf(&bp, " -- ");
                    avdevice_capabilities_bprint_num(&bp, range->str, range->value_max);
                }
            }
        }
        if (!av_bprint_is_complete(&bp))
            return AVERROR(ENOMEM);
        if ((ret = av_bprint_finalize(&bp, &out_val)) < 0)
            return ret;
        printf("%s\n", out_val);
        av_freep(&out_val);
    }

    return 0;
}

static void list_queries(void)
{
    const AVOption *opt = NULL;
    const AVClass *class = avdevice_capabilities_get_class();
    while (opt = av_opt_next(&class, opt))
        fprintf(stderr, "  %s\n", opt->name);
}

static void list_device_sources(const AVInputFormat *fmt)
{
    int ret;
    AVDeviceInfoList *device_list = NULL;

    if (!fmt || !fmt->priv_class || !AV_IS_INPUT_DEVICE(fmt->priv_class->category))
        return;

    if (!fmt->get_device_list) {
        ret = AVERROR(ENOSYS);
        fprintf(stderr, "  Cannot list sources. Not implemented.\n");
        return;
    }

    if ((ret = avdevice_list_input_sources(fmt, NULL, NULL, &device_list)) < 0) {
        fprintf(stderr, "  Cannot list sources.\n");
        return;
    }

    for (int i = 0; i < device_list->nb_devices; i++) {
        const AVDeviceInfo *device = device_list->devices[i];
        fprintf(stderr, "  %s %s (", device_list->default_device == i ? "*" : " ", device->device_name);
        if (device->nb_media_types > 0 && device->media_types) {
            for (int j = 0; j < device->nb_media_types; ++j) {
                const char* media_type = av_get_media_type_string(device->media_types[j]);
                if (j > 0)
                    fprintf(stderr, ", ");
                fprintf(stderr, "%s", media_type ? media_type : "unknown");
            }
        }
        else {
            fprintf(stderr, "none");
        }
        fprintf(stderr, ")\n");
    }

    avdevice_free_list_devices(&device_list);
}



int main (int argc, char **argv)
{
    int ret = 0;
    const char *device_name = NULL;
    const char *input_name = NULL;
    const char *query_cap = NULL;
    const char *set_cap_name = NULL;
    const char *set_cap_value = NULL;

    const AVInputFormat *fmt = NULL;
    AVFormatContext *fmt_ctx = NULL;
    AVDeviceCapabilitiesQuery *caps = NULL;
    AVOptionRanges *ranges = NULL;
    const AVOption *opt = NULL;

    if (argc != 6) {
        fprintf(stderr, "usage: %s  device_name input_name query_cap set_cap_name set_cap_value\n"
                "API example program to show how to use the avdevice\n"
                "capabilities API to probe device capabilities \n"
                "(supported codecs, pixel formats, sample formats,\n"
                "resolutions, channel counts, etc).\n\n"
                "example invocation: "
                "%s dshow video=\"Integrated Webcam\" frame_size pixel_format yuyv422",
                argv[0], argv[0]);
        exit(1);
    }
    device_name = argv[1];
    input_name = argv[2];
    query_cap = argv[3];
    set_cap_name = argv[4];
    set_cap_value = argv[5];

    // make sure avdevices can be found among input and output formats
    avdevice_register_all();
    // find specified device
    fmt = av_find_input_format(device_name);
    if (!fmt) {
        fprintf(stderr, "Could not find the device '%s'\n",device_name);
        ret = AVERROR(EINVAL);
        goto end;
    }

    // prepare device format context, and set device to query, 
    ret = avformat_alloc_input_context(&fmt_ctx, fmt, NULL);
    if (ret < 0) {
        fprintf(stderr, "Cannot allocate input format context\n");
        goto end;
    }
    fmt_ctx->url = av_strdup(input_name);

    // prepare query object, setting device options
    ret = avdevice_capabilities_create(&caps, fmt_ctx, NULL);
    if (ret < 0) {
        fprintf(stderr, "avdevice_capabilities_create() failed. Possibly the input name you specified ('%s') is not available for this device ('%s').\n%s can access the following sources:\n", input_name, device_name, device_name);
        list_device_sources(fmt);
        goto end;
    }

    // check capability to query, and get info about the return type
    opt = av_opt_find(caps, query_cap, NULL, 0, 0);
    if (!opt) {
        fprintf(stderr, "Capability '%s' you wish to query is not available.\nYou can query the following capabilities:\n", query_cap);
        list_queries();
        ret = AVERROR_OPTION_NOT_FOUND;
        goto end;
    }

    // query the capability without any filter set
    ret = av_opt_query_ranges(&ranges, caps, opt->name, AV_OPT_MULTI_COMPONENT_RANGE);
    if (ret < 0) {
        fprintf(stderr, "av_opt_query_ranges() failed\n");
        goto end;
    }

    // print results
    ret = print_option_ranges(opt->type, ranges);
    if (ret < 0) {
        fprintf(stderr, "printing the AVOptionRanges failed\n");
        goto end;
    }
    av_opt_freep_ranges(&ranges);

    printf("=============\n");

    // set one capability, which may filter out some returned capabilities
    // (or all, if set to an invalid value)
    ret = av_opt_set(caps, set_cap_name, set_cap_value, 0);
    if (ret < 0) {
        fprintf(stderr, "av_opt_set() failed when trying to set the capability '%s'. Possibly it is not available.\nYou can set the following capabilities:\n", set_cap_name);
        list_queries();
        goto end;
    }

    // query again
    ret = av_opt_query_ranges(&ranges, caps, opt->name, AV_OPT_MULTI_COMPONENT_RANGE);
    if (ret < 0) {
        fprintf(stderr, "av_opt_query_ranges() failed\n");
        goto end;
    }

    // print results
    print_option_ranges(opt->type, ranges);


end:
    if (ranges)
        av_opt_freep_ranges(&ranges);
    if (caps)
        avdevice_capabilities_free(&caps, fmt_ctx);

    if (fmt_ctx)
        avformat_free_context(fmt_ctx);

    if (ret < 0) {
        char a[AV_ERROR_MAX_STRING_SIZE] = { 0 };
        av_make_error_string(a, AV_ERROR_MAX_STRING_SIZE, ret);

        printf("Error: %s\n", a);
    }

    return ret < 0;
}
