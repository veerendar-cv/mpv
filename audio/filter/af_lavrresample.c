/*
 * Copyright (c) 2004 Michael Niedermayer <michaelni@gmx.at>
 * Copyright (c) 2013 Stefano Pigozzi <stefano.pigozzi@gmail.com>
 *
 * This file is part of mpv.
 *
 * MPlayer is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * MPlayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with MPlayer; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <libavutil/opt.h>
#include <libavutil/audioconvert.h>
#include <libavutil/common.h>
#include <libavutil/samplefmt.h>
#include <libavutil/mathematics.h>

#include "talloc.h"
#include "config.h"

#if defined(CONFIG_LIBAVRESAMPLE)
#include <libavresample/avresample.h>
#define USE_SET_CHANNEL_MAPPING HAVE_AVRESAMPLE_SET_CHANNEL_MAPPING
#elif defined(CONFIG_LIBSWRESAMPLE)
#include <libswresample/swresample.h>
#define AVAudioResampleContext SwrContext
#define avresample_alloc_context swr_alloc
#define avresample_open swr_init
#define avresample_close(x) do { } while(0)
#define avresample_available(x) 0
#define avresample_convert(ctx, out, out_planesize, out_samples, in, in_planesize, in_samples) \
    swr_convert(ctx, out, out_samples, (const uint8_t**)(in), in_samples)
#define avresample_set_channel_mapping swr_set_channel_mapping
#define USE_SET_CHANNEL_MAPPING 1
#else
#error "config.h broken"
#endif

#include "mpvcore/mp_msg.h"
#include "mpvcore/m_option.h"
#include "mpvcore/av_opts.h"
#include "audio/filter/af.h"
#include "audio/fmt-conversion.h"
#include "audio/reorder_ch.h"

struct af_resample_opts {
    int filter_size;
    int phase_shift;
    int linear;
    double cutoff;

    int in_rate;
    int in_format;
    struct mp_chmap in_channels;
    int out_rate;
    int out_format;
    struct mp_chmap out_channels;
};

struct af_resample {
    int allow_detach;
    char *avopts;
    struct AVAudioResampleContext *avrctx;
    struct AVAudioResampleContext *avrctx_out; // for output channel reordering
    struct af_resample_opts ctx;   // opts in the context
    struct af_resample_opts opts;  // opts requested by the user
    // At least libswresample keeps a pointer around for this:
    int reorder_in[MP_NUM_CHANNELS];
    int reorder_out[MP_NUM_CHANNELS];
    uint8_t *reorder_buffer;
};

#ifdef CONFIG_LIBAVRESAMPLE
static int get_delay(struct af_resample *s)
{
    return avresample_get_delay(s->avrctx);
}
#else
static int get_delay(struct af_resample *s)
{
    return swr_get_delay(s->avrctx, s->ctx.in_rate);
}
#endif

static double af_resample_default_cutoff(int filter_size)
{
    return FFMAX(1.0 - 6.5 / (filter_size + 8), 0.80);
}

static bool needs_lavrctx_reconfigure(struct af_resample *s,
                                      struct mp_audio *in,
                                      struct mp_audio *out)
{
    return s->ctx.in_rate     != in->rate ||
           s->ctx.in_format   != in->format ||
           !mp_chmap_equals(&s->ctx.in_channels, &in->channels) ||
           s->ctx.out_rate    != out->rate ||
           s->ctx.out_format  != out->format ||
           !mp_chmap_equals(&s->ctx.out_channels, &out->channels) ||
           s->ctx.filter_size != s->opts.filter_size ||
           s->ctx.phase_shift != s->opts.phase_shift ||
           s->ctx.linear      != s->opts.linear ||
           s->ctx.cutoff      != s->opts.cutoff;

}

static bool test_conversion(int src_format, int dst_format)
{
    return af_to_avformat(src_format) != AV_SAMPLE_FMT_NONE &&
           af_to_avformat(dst_format) != AV_SAMPLE_FMT_NONE;
}

#define ctx_opt_set_int(a,b) av_opt_set_int(s->avrctx, (a), (b), 0)
#define ctx_opt_set_dbl(a,b) av_opt_set_double(s->avrctx, (a), (b), 0)

static int control(struct af_instance *af, int cmd, void *arg)
{
    struct af_resample *s = (struct af_resample *) af->priv;
    struct mp_audio *in   = (struct mp_audio *) arg;
    struct mp_audio *out  = (struct mp_audio *) af->data;

    switch (cmd) {
    case AF_CONTROL_REINIT: {
        struct mp_audio orig_in = *in;

        if (((out->rate    == in->rate) || (out->rate == 0)) &&
            (out->format   == in->format) &&
            (mp_chmap_equals(&out->channels, &in->channels) || out->nch == 0) &&
            s->allow_detach)
            return AF_DETACH;

        if (out->rate == 0)
            out->rate = in->rate;

        if (mp_chmap_is_empty(&out->channels))
            mp_audio_set_channels(out, &in->channels);

        enum AVSampleFormat in_samplefmt = af_to_avformat(in->format);
        if (in_samplefmt == AV_SAMPLE_FMT_NONE) {
            mp_audio_set_format(in, AF_FORMAT_FLOAT_NE);
            in_samplefmt = af_to_avformat(in->format);
        }
        enum AVSampleFormat out_samplefmt = af_to_avformat(out->format);
        if (out_samplefmt == AV_SAMPLE_FMT_NONE) {
            mp_audio_set_format(out, in->format);
            out_samplefmt = in_samplefmt;
        }

        af->mul     = (double) (out->rate * out->nch) / (in->rate * in->nch);
        af->delay   = out->nch * s->opts.filter_size / FFMIN(af->mul, 1);

        if (needs_lavrctx_reconfigure(s, in, out)) {
            avresample_close(s->avrctx);
            avresample_close(s->avrctx_out);

            s->ctx.out_rate    = out->rate;
            s->ctx.in_rate     = in->rate;
            s->ctx.out_format  = out->format;
            s->ctx.in_format   = in->format;
            s->ctx.out_channels= out->channels;
            s->ctx.in_channels = in->channels;
            s->ctx.filter_size = s->opts.filter_size;
            s->ctx.phase_shift = s->opts.phase_shift;
            s->ctx.linear      = s->opts.linear;
            s->ctx.cutoff      = s->opts.cutoff;

            ctx_opt_set_int("filter_size",        s->ctx.filter_size);
            ctx_opt_set_int("phase_shift",        s->ctx.phase_shift);
            ctx_opt_set_int("linear_interp",      s->ctx.linear);

            ctx_opt_set_dbl("cutoff",             s->ctx.cutoff);

            if (parse_avopts(s->avrctx, s->avopts) < 0) {
                mp_msg(MSGT_VFILTER, MSGL_FATAL,
                       "af_lavrresample: could not set opts: '%s'\n", s->avopts);
                return AF_ERROR;
            }

            struct mp_chmap map_in = in->channels;
            struct mp_chmap map_out = out->channels;

            // Try not to do any remixing if at least one is "unknown".
            if (mp_chmap_is_unknown(&map_in) || mp_chmap_is_unknown(&map_out)) {
                mp_chmap_set_unknown(&map_in, map_in.num);
                mp_chmap_set_unknown(&map_out, map_out.num);
            }

            // unchecked: don't take any channel reordering into account
            uint64_t in_ch_layout = mp_chmap_to_lavc_unchecked(&map_in);
            uint64_t out_ch_layout = mp_chmap_to_lavc_unchecked(&map_out);

            ctx_opt_set_int("in_channel_layout",  in_ch_layout);
            ctx_opt_set_int("out_channel_layout", out_ch_layout);

            ctx_opt_set_int("in_sample_rate",     s->ctx.in_rate);
            ctx_opt_set_int("out_sample_rate",    s->ctx.out_rate);

            ctx_opt_set_int("in_sample_fmt",      in_samplefmt);
            ctx_opt_set_int("out_sample_fmt",     out_samplefmt);

            struct mp_chmap in_lavc;
            mp_chmap_from_lavc(&in_lavc, in_ch_layout);
            mp_chmap_get_reorder(s->reorder_in, &map_in, &in_lavc);

            struct mp_chmap out_lavc;
            mp_chmap_from_lavc(&out_lavc, out_ch_layout);
            mp_chmap_get_reorder(s->reorder_out, &out_lavc, &map_out);

            // Same configuration; we just reorder.
            av_opt_set_int(s->avrctx_out, "in_channel_layout", out_ch_layout, 0);
            av_opt_set_int(s->avrctx_out, "out_channel_layout", out_ch_layout, 0);
            av_opt_set_int(s->avrctx_out, "in_sample_fmt", out_samplefmt, 0);
            av_opt_set_int(s->avrctx_out, "out_sample_fmt", out_samplefmt, 0);
            av_opt_set_int(s->avrctx_out, "in_sample_rate", s->ctx.out_rate, 0);
            av_opt_set_int(s->avrctx_out, "out_sample_rate", s->ctx.out_rate, 0);

#if USE_SET_CHANNEL_MAPPING
            // API has weird requirements, quoting avresample.h:
            //  * This function can only be called when the allocated context is not open.
            //  * Also, the input channel layout must have already been set.
            avresample_set_channel_mapping(s->avrctx, s->reorder_in);
            avresample_set_channel_mapping(s->avrctx_out, s->reorder_out);
#endif

            if (avresample_open(s->avrctx) < 0 ||
                avresample_open(s->avrctx_out) < 0)
            {
                mp_msg(MSGT_AFILTER, MSGL_ERR, "[lavrresample] Cannot open "
                       "Libavresample Context. \n");
                return AF_ERROR;
            }
        }

        return ((in->format == orig_in.format) &&
                mp_chmap_equals(&in->channels, &orig_in.channels))
               ? AF_OK : AF_FALSE;
    }
    case AF_CONTROL_FORMAT_FMT | AF_CONTROL_SET: {
        if (af_to_avformat(*(int*)arg) == AV_SAMPLE_FMT_NONE)
            return AF_FALSE;

        mp_audio_set_format(af->data, *(int*)arg);
        return AF_OK;
    }
    case AF_CONTROL_CHANNELS | AF_CONTROL_SET: {
        mp_audio_set_channels(af->data, (struct mp_chmap *)arg);
        return AF_OK;
    }
    case AF_CONTROL_RESAMPLE_RATE | AF_CONTROL_SET:
        out->rate = *(int *)arg;
        return AF_OK;
    }
    return AF_UNKNOWN;
}

#undef ctx_opt_set_int
#undef ctx_opt_set_dbl

static void uninit(struct af_instance *af)
{
    struct af_resample *s = af->priv;
    if (s->avrctx)
        avresample_close(s->avrctx);
    if (s->avrctx_out)
        avresample_close(s->avrctx_out);
}

static bool needs_reorder(int *reorder, int num_ch)
{
    for (int n = 0; n < num_ch; n++) {
        if (reorder[n] != n)
            return true;
    }
    return false;
}

static struct mp_audio *play(struct af_instance *af, struct mp_audio *data)
{
    struct af_resample *s = af->priv;
    struct mp_audio *in   = data;
    struct mp_audio *out  = af->data;


    int in_size     = data->len;
    int in_samples  = in_size / (data->bps * data->nch);
    int out_samples = avresample_available(s->avrctx) +
        av_rescale_rnd(get_delay(s) + in_samples,
                       s->ctx.out_rate, s->ctx.in_rate, AV_ROUND_UP);
    int out_size    = out->bps * out_samples * out->nch;

    if (talloc_get_size(out->audio) < out_size)
        out->audio = talloc_realloc_size(out, out->audio, out_size);

    af->delay = out->bps * av_rescale_rnd(get_delay(s),
                                          s->ctx.out_rate, s->ctx.in_rate,
                                          AV_ROUND_UP);

#if !USE_SET_CHANNEL_MAPPING
    reorder_channels(data->audio, s->reorder_in, data->bps, data->nch, in_samples);
#endif

    out_samples = avresample_convert(s->avrctx,
            (uint8_t **) &out->audio, out_size, out_samples,
            (uint8_t **) &in->audio,  in_size,  in_samples);

    *data = *out;

#if USE_SET_CHANNEL_MAPPING
    if (needs_reorder(s->reorder_out, out->nch)) {
        if (talloc_get_size(s->reorder_buffer) < out_size)
            s->reorder_buffer = talloc_realloc_size(s, s->reorder_buffer, out_size);
        data->audio = s->reorder_buffer;
        out_samples = avresample_convert(s->avrctx_out,
                (uint8_t **) &data->audio, out_size, out_samples,
                (uint8_t **) &out->audio, out_size, out_samples);
    }
#else
    reorder_channels(data->audio, s->reorder_out, out->bps, out->nch, out_samples);
#endif

    data->len = out->bps * out_samples * out->nch;
    return data;
}

static int af_open(struct af_instance *af)
{
    struct af_resample *s = af->priv;

    af->control = control;
    af->uninit  = uninit;
    af->play    = play;
    af->mul     = 1;
    af->data    = talloc_zero(s, struct mp_audio);

    af->data->rate   = 0;

    if (s->opts.cutoff <= 0.0)
        s->opts.cutoff = af_resample_default_cutoff(s->opts.filter_size);

    s->avrctx = avresample_alloc_context();
    s->avrctx_out = avresample_alloc_context();

    if (s->avrctx && s->avrctx_out) {
        return AF_OK;
    } else {
        mp_msg(MSGT_AFILTER, MSGL_ERR, "[lavrresample] Cannot initialize "
               "Libavresample Context. \n");
        uninit(af);
        return AF_ERROR;
    }
}

#define OPT_BASE_STRUCT struct af_resample

struct af_info af_info_lavrresample = {
    "Sample frequency conversion using libavresample",
    "lavrresample",
    "Stefano Pigozzi (based on Michael Niedermayer's lavcresample)",
    "",
    AF_FLAGS_REENTRANT,
    af_open,
    .test_conversion = test_conversion,
    .priv_size = sizeof(struct af_resample),
    .priv_defaults = &(const struct af_resample) {
        .opts = {
            .filter_size = 16,
            .cutoff      = 0.0,
            .phase_shift = 10,
        },
        .allow_detach = 1,
    },
    .options = (const struct m_option[]) {
        OPT_INTRANGE("filter-size", opts.filter_size, 0, 0, 32),
        OPT_INTRANGE("phase-shift", opts.phase_shift, 0, 0, 30),
        OPT_FLAG("linear", opts.linear, 0),
        OPT_DOUBLE("cutoff", opts.cutoff, M_OPT_RANGE, .min = 0, .max = 1),
        OPT_FLAG("detach", allow_detach, 0),
        OPT_STRING("o", avopts, 0),
        {0}
    },
};
