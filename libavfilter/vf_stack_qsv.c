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

/**
 * @file
 * A hardware accelerated hstack and vstack filters based on Intel Quick Sync Video VPP
 */

#include "libavutil/opt.h"
#include "libavutil/common.h"
#include "libavutil/pixdesc.h"
#include "libavutil/eval.h"
#include "libavutil/hwcontext.h"
#include "libavutil/avstring.h"
#include "libavutil/avassert.h"
#include "libavutil/imgutils.h"
#include "libavutil/mathematics.h"

#include "internal.h"
#include "avfilter.h"
#include "filters.h"
#include "formats.h"
#include "video.h"

#include "framesync.h"
#include "qsvvpp.h"

#define OFFSET(x) offsetof(QSVStackContext, x)
#define FLAGS (AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_FILTERING_PARAM)

typedef struct StackItem {
    int x, y, w, h;
} StackItem;

typedef struct QSVStackContext {
    QSVVPPContext qsv;
    QSVVPPParam qsv_param;
    mfxExtVPPComposite comp_conf;
    int nb_inputs;
    int shortest;
    int is_horizontal;

    StackItem *items;
    AVFrame **frames;
    FFFrameSync fs;
} QSVStackContext;

static int process_frame(FFFrameSync *fs)
{
    AVFilterContext  *ctx = fs->parent;
    QSVVPPContext    *qsv = fs->opaque;
    AVFrame        *frame = NULL;
    int               ret = 0, i;

    for (i = 0; i < ctx->nb_inputs; i++) {
        ret = ff_framesync_get_frame(fs, i, &frame, 0);
        if (ret == 0)
            ret = ff_qsvvpp_filter_frame(qsv, ctx->inputs[i], frame);
        if (ret < 0 && ret != AVERROR(EAGAIN))
            break;
    }

    if (ret == 0 && qsv->got_frame == 0) {
        for (i = 0; i < ctx->nb_inputs; i++)
            FF_FILTER_FORWARD_WANTED(ctx->outputs[0], ctx->inputs[i]);

        ret = FFERROR_NOT_READY;
    }

    return ret;
}

static int init_framesync(AVFilterContext *ctx)
{
    QSVStackContext *vpp = ctx->priv;
    int ret, i;

    vpp->fs.on_event = process_frame;
    vpp->fs.opaque = vpp;
    ret = ff_framesync_init(&vpp->fs, ctx, ctx->nb_inputs);

    if (ret < 0)
        return ret;

    for (i = 0; i < ctx->nb_inputs; i++) {
        FFFrameSyncIn *in = &vpp->fs.in[i];
        in->before = EXT_STOP;
        in->after = vpp->shortest ? EXT_STOP : EXT_INFINITY;
        in->sync = i ? 1 : 2;
        in->time_base = ctx->inputs[i]->time_base;
    }

    return ff_framesync_configure(&vpp->fs);
}

static int config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    QSVStackContext *vpp = ctx->priv;
    AVFilterLink *inlink0 = ctx->inputs[0];
    int width, height, i, ret;

    av_log(ctx, AV_LOG_DEBUG, "Output is of %s.\n", av_get_pix_fmt_name(outlink->format));

    for (i = 1; i < vpp->nb_inputs; i++) {
        AVFilterLink *inlink = ctx->inputs[i];

        if (inlink0->format == AV_PIX_FMT_QSV) {
            if (inlink0->format != inlink->format) {
                av_log(ctx, AV_LOG_ERROR, "Mixing hardware and software pixel formats is not supported.\n");

                return AVERROR(EINVAL);
            } else {
                AVHWFramesContext *hwfc0 = (AVHWFramesContext *)inlink0->hw_frames_ctx->data;
                AVHWFramesContext *hwfc = (AVHWFramesContext *)inlink->hw_frames_ctx->data;

                if (hwfc0->device_ctx != hwfc->device_ctx) {
                    av_log(ctx, AV_LOG_ERROR, "Inputs with different underlying QSV devices are forbidden.\n");

                    return AVERROR(EINVAL);
                }
            }
        }
    }

    if (vpp->is_horizontal) {
        height = inlink0->h;
        width = 0;

        for (i = 0; i < vpp->nb_inputs; i++) {
            AVFilterLink *inlink = ctx->inputs[i];
            mfxVPPCompInputStream *is = &vpp->comp_conf.InputStream[i];

            if (inlink0->h != inlink->h) {
                av_log(ctx, AV_LOG_ERROR, "Input %d height %d does not match input %d height %d.\n", i, inlink->h, 0, inlink0->h);
                return AVERROR(EINVAL);
            }

            is->DstX = width;
            is->DstY = 0;
            is->DstW = inlink->w;
            is->DstH = inlink->h;
            is->GlobalAlpha = 255;
            is->GlobalAlphaEnable = 1;
            is->PixelAlphaEnable = 0;

            width += inlink->w;
        }
    }

    outlink->w = width;
    outlink->h = height;
    outlink->frame_rate = inlink0->frame_rate;
    outlink->time_base = av_inv_q(outlink->frame_rate);

    ret = init_framesync(ctx);

    if (ret < 0)
        return ret;

    return ff_qsvvpp_init(ctx, &vpp->qsv_param);
}

/*
 * Callback for qsvvpp
 * @Note: qsvvpp composition does not generate PTS for result frame.
 *        so we assign the PTS from framesync to the output frame.
 */

static int filter_callback(AVFilterLink *outlink, AVFrame *frame)
{
    QSVStackContext *vpp = outlink->src->priv;
    frame->pts = av_rescale_q(vpp->fs.pts,
                              vpp->fs.time_base, outlink->time_base);
    return ff_filter_frame(outlink, frame);
}


static int stack_qsv_init(AVFilterContext *ctx)
{
    QSVStackContext *vpp = ctx->priv;
    int i, ret;

    if (!strcmp(ctx->filter->name, "hstack_qsv"))
        vpp->is_horizontal = 1;
    else {
        av_assert0(strcmp(ctx->filter->name, "vstack_qsv") == 0);
        vpp->is_horizontal = 0;
    }

    vpp->frames = av_calloc(vpp->nb_inputs, sizeof(*vpp->frames));

    if (!vpp->frames)
        return AVERROR(ENOMEM);

    vpp->items = av_calloc(vpp->nb_inputs, sizeof(*vpp->items));

    if (!vpp->items)
        return AVERROR(ENOMEM);

    for (i = 0; i < vpp->nb_inputs; i++) {
        AVFilterPad pad = { 0 };

        pad.type = AVMEDIA_TYPE_VIDEO;
        pad.name = av_asprintf("input%d", i);

        if (!pad.name)
            return AVERROR(ENOMEM);

        if ((ret = ff_insert_inpad(ctx, i, &pad)) < 0) {
            av_freep(&pad.name);

            return ret;
        }
    }

    /* fill composite config */
    vpp->comp_conf.Header.BufferId = MFX_EXTBUFF_VPP_COMPOSITE;
    vpp->comp_conf.Header.BufferSz = sizeof(vpp->comp_conf);
    vpp->comp_conf.NumInputStream = vpp->nb_inputs;
    vpp->comp_conf.InputStream = av_mallocz_array(vpp->nb_inputs,
                                                  sizeof(*vpp->comp_conf.InputStream));
    if (!vpp->comp_conf.InputStream)
        return AVERROR(ENOMEM);

    /* initialize QSVVPP params */
    vpp->qsv_param.filter_frame = filter_callback;
    vpp->qsv_param.ext_buf      = av_mallocz(sizeof(*vpp->qsv_param.ext_buf));

    if (!vpp->qsv_param.ext_buf)
        return AVERROR(ENOMEM);

    vpp->qsv_param.ext_buf[0]    = (mfxExtBuffer *)&vpp->comp_conf;
    vpp->qsv_param.num_ext_buf   = 1;
    vpp->qsv_param.out_sw_format = AV_PIX_FMT_NV12;
    vpp->qsv_param.num_crop      = 0;

    return 0;
}

static av_cold void stack_qsv_uninit(AVFilterContext *ctx)
{
    QSVStackContext *vpp = ctx->priv;
    int i;

    ff_qsvvpp_close(ctx);
    ff_framesync_uninit(&vpp->fs);
    av_freep(&vpp->comp_conf.InputStream);
    av_freep(&vpp->qsv_param.ext_buf);
    av_freep(&vpp->frames);
    av_freep(&vpp->items);

    for (i = 0; i < ctx->nb_inputs; i++)
        av_freep(&ctx->input_pads[i].name);
}

static int stack_qsv_activate(AVFilterContext *ctx)
{
    QSVStackContext *vpp = ctx->priv;
    return ff_framesync_activate(&vpp->fs);
}

static int stack_qsv_query_formats(AVFilterContext *ctx)
{
    int i;
    int ret;

    static const enum AVPixelFormat in_fmts[] = {
        AV_PIX_FMT_NV12,
        AV_PIX_FMT_QSV,
        AV_PIX_FMT_NONE
    };

    static const enum AVPixelFormat out_fmts[] = {
        AV_PIX_FMT_NV12,
        AV_PIX_FMT_QSV,
        AV_PIX_FMT_NONE
    };

    for (i = 0; i < ctx->nb_inputs; i++) {
        ret = ff_formats_ref(ff_make_format_list(in_fmts), &ctx->inputs[i]->outcfg.formats);

        if (ret < 0)
            return ret;
    }

    ret = ff_formats_ref(ff_make_format_list(out_fmts), &ctx->outputs[0]->incfg.formats);

    return ret;
}

static const AVFilterPad stack_qsv_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .config_props  = config_output,
    },

    { NULL }
};

static const AVOption stack_qsv_options[] = {
    { "inputs", "set number of inputs", OFFSET(nb_inputs), AV_OPT_TYPE_INT, { .i64 = 2 }, 2, 64, .flags = FLAGS },
    { "shortest", "force termination when the shortest input terminates", OFFSET(shortest), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, FLAGS },
    { NULL }
};

#define hstack_qsv_options stack_qsv_options
AVFILTER_DEFINE_CLASS(hstack_qsv);

const AVFilter ff_vf_hstack_qsv = {
    .name           = "hstack_qsv",
    .description    = NULL_IF_CONFIG_SMALL("Quick Sync Video hstack."),
    .priv_size      = sizeof(QSVStackContext),
    .priv_class     = &hstack_qsv_class,
    .query_formats  = stack_qsv_query_formats,
    .outputs        = stack_qsv_outputs,
    .init           = stack_qsv_init,
    .uninit         = stack_qsv_uninit,
    .activate       = stack_qsv_activate,
    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
    .flags          = AVFILTER_FLAG_DYNAMIC_INPUTS,
};

#define vstack_qsv_options stack_qsv_options
AVFILTER_DEFINE_CLASS(vstack_qsv);

const AVFilter ff_vf_vstack_qsv = {
    .name           = "vstack_qsv",
    .description    = NULL_IF_CONFIG_SMALL("Quick Sync Video vstack."),
    .priv_size      = sizeof(QSVStackContext),
    .priv_class     = &vstack_qsv_class,
    .query_formats  = stack_qsv_query_formats,
    .outputs        = stack_qsv_outputs,
    .init           = stack_qsv_init,
    .uninit         = stack_qsv_uninit,
    .activate       = stack_qsv_activate,
    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
    .flags          = AVFILTER_FLAG_DYNAMIC_INPUTS,
};
