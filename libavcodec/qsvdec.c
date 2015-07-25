/*
 * Intel MediaSDK QSV codec-independent code
 *
 * copyright (c) 2013 Luca Barbato
 * copyright (c) 2015 Anton Khirnov <anton@khirnov.net>
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

#include <string.h>
#include <sys/types.h>

#include <mfx/mfxvideo.h>

#include "libavutil/common.h"
#include "libavutil/mem.h"
#include "libavutil/log.h"
#include "libavutil/pixfmt.h"
#include "libavutil/time.h"

#include "avcodec.h"
#include "internal.h"
#include "qsv.h"
#include "qsv_internal.h"
#include "qsvdec.h"

int ff_qsv_map_pixfmt(enum AVPixelFormat format)
{
    switch (format) {
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUVJ420P:
        return AV_PIX_FMT_NV12;
    default:
        return AVERROR(ENOSYS);
    }
}

int ff_qsv_decode_init(AVCodecContext *avctx, QSVContext *q, AVPacket *avpkt)
{
    mfxVideoParam param = { { 0 } };
    mfxBitstream bs   = { { { 0 } } };
    int ret;

    q->iopattern  = MFX_IOPATTERN_OUT_SYSTEM_MEMORY;
    if (!q->session) {
        if (avctx->hwaccel_context) {
            AVQSVContext *qsv = avctx->hwaccel_context;

            q->session        = qsv->session;
            q->iopattern      = qsv->iopattern;
            q->ext_buffers    = qsv->ext_buffers;
            q->nb_ext_buffers = qsv->nb_ext_buffers;
        }
        if (!q->session) {
            ret = ff_qsv_init_internal_session(avctx, &q->internal_qs, NULL);
            if (ret < 0)
                return ret;

            q->session = q->internal_qs.session;
        }
    }

    if (avpkt->size) {
        bs.Data       = avpkt->data;
        bs.DataLength = avpkt->size;
        bs.MaxLength  = bs.DataLength;
        bs.TimeStamp  = avpkt->pts;
    } else
        return AVERROR_INVALIDDATA;

    ret = ff_qsv_codec_id_to_mfx(avctx->codec_id);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Unsupported codec_id %08x\n", avctx->codec_id);
        return ret;
    }

    param.mfx.CodecId = ret;

    ret = MFXVideoDECODE_DecodeHeader(q->session, &bs, &param);
    if (MFX_ERR_MORE_DATA==ret) {
        /* this code means that header not found so we return packet size to skip
           a current packet
         */
        return avpkt->size;
    } else if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Decode header error %d\n", ret);
        return ff_qsv_error(ret);
    }
    param.IOPattern   = q->iopattern;
    param.AsyncDepth  = q->async_depth;
    param.ExtParam    = q->ext_buffers;
    param.NumExtParam = q->nb_ext_buffers;
    param.mfx.FrameInfo.BitDepthLuma   = 8;
    param.mfx.FrameInfo.BitDepthChroma = 8;

    ret = MFXVideoDECODE_Init(q->session, &param);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Error initializing the MFX video decoder\n");
        return ff_qsv_error(ret);
    }

    avctx->pix_fmt      = AV_PIX_FMT_NV12;
    avctx->profile      = param.mfx.CodecProfile;
    avctx->level        = param.mfx.CodecLevel;
    avctx->coded_width  = param.mfx.FrameInfo.Width;
    avctx->coded_height = param.mfx.FrameInfo.Height;
    avctx->width        = param.mfx.FrameInfo.CropW - param.mfx.FrameInfo.CropX;
    avctx->height       = param.mfx.FrameInfo.CropH - param.mfx.FrameInfo.CropY;

    /* maximum decoder latency should be not exceed max DPB size for h.264 and
       HEVC which is 16 for both cases.
       So weare  pre-allocating fifo big enough for 17 elements:
     */
    q->async_fifo = av_fifo_alloc((1 + 16) *
                                  (sizeof(mfxSyncPoint) + sizeof(QSVFrame*)));
    if (!q->async_fifo)
        return AVERROR(ENOMEM);

    q->input_fifo = av_fifo_alloc(1024*16);
    if (!q->input_fifo)
        return AVERROR(ENOMEM);

    q->engine_ready = 1;

    return 0;
}

static int alloc_frame(AVCodecContext *avctx, QSVFrame *frame)
{
    int ret;

    ret = ff_get_buffer(avctx, frame->frame, AV_GET_BUFFER_FLAG_REF);
    if (ret < 0)
        return ret;

    if (frame->frame->format == AV_PIX_FMT_QSV) {
        frame->surface = (mfxFrameSurface1*)frame->frame->data[3];
    } else {
        frame->surface_internal.Info.BitDepthLuma   = 8;
        frame->surface_internal.Info.BitDepthChroma = 8;
        frame->surface_internal.Info.FourCC         = MFX_FOURCC_NV12;
        frame->surface_internal.Info.Width          = avctx->coded_width;
        frame->surface_internal.Info.Height         = avctx->coded_height;
        frame->surface_internal.Info.ChromaFormat   = MFX_CHROMAFORMAT_YUV420;

        frame->surface_internal.Data.PitchLow = frame->frame->linesize[0];
        frame->surface_internal.Data.Y        = frame->frame->data[0];
        frame->surface_internal.Data.UV       = frame->frame->data[1];

        frame->surface = &frame->surface_internal;
    }

    return 0;
}

static void qsv_clear_unused_frames(QSVContext *q)
{
    QSVFrame *cur = q->work_frames;
    while (cur) {
        if (cur->surface && !cur->surface->Data.Locked && !cur->queued) {
            cur->surface = NULL;
            av_frame_unref(cur->frame);
        }
        cur = cur->next;
    }
}

static int get_surface(AVCodecContext *avctx, QSVContext *q, mfxFrameSurface1 **surf)
{
    QSVFrame *frame, **last;
    int ret;

    qsv_clear_unused_frames(q);

    frame = q->work_frames;
    last  = &q->work_frames;
    while (frame) {
        if (!frame->surface) {
            ret = alloc_frame(avctx, frame);
            if (ret < 0)
                return ret;
            *surf = frame->surface;
            return 0;
        }

        last  = &frame->next;
        frame = frame->next;
    }

    frame = av_mallocz(sizeof(*frame));
    if (!frame)
        return AVERROR(ENOMEM);
    frame->frame = av_frame_alloc();
    if (!frame->frame) {
        av_freep(&frame);
        return AVERROR(ENOMEM);
    }
    *last = frame;

    ret = alloc_frame(avctx, frame);
    if (ret < 0)
        return ret;

    *surf = frame->surface;

    return 0;
}

static QSVFrame *find_frame(QSVContext *q, mfxFrameSurface1 *surf)
{
    QSVFrame *cur = q->work_frames;
    while (cur) {
        if (surf == cur->surface)
            return cur;
        cur = cur->next;
    }
    return NULL;
}

/*  This function uses for 'smart' releasing of consumed data
    from the input bitstream fifo.
    Since the input fifo mapped to mfxBitstream which does not understand
    a wrapping of data over fifo end, we should also to relocate a possible
    data rest to fifo begin. If rest of data is absent then we just reset fifo's
    pointers to initial positions.
    NOTE the case when fifo does contain unconsumed data is rare and typical
    amount of such data is 1..4 bytes.
*/
static void qsv_fifo_relocate(AVFifoBuffer *f, int bytes_to_free)
{
    int data_size;
    int data_rest = 0;

    av_fifo_drain(f, bytes_to_free);

    data_size = av_fifo_size(f);
    if (data_size > 0) {
        if (f->buffer!=f->rptr) {
            if ( (f->end - f->rptr) < data_size) {
                data_rest = data_size - (f->end - f->rptr);
                data_size-=data_rest;
                memmove(f->buffer+data_size, f->buffer, data_rest);
            }
            memmove(f->buffer, f->rptr, data_size);
            data_size+= data_rest;
        }
    }
    f->rptr = f->buffer;
    f->wptr = f->buffer + data_size;
    f->wndx = data_size;
    f->rndx = 0;
}

int ff_qsv_decode(AVCodecContext *avctx, QSVContext *q,
                  AVFrame *frame, int *got_frame,
                  AVPacket *avpkt)
{
    QSVFrame *out_frame;
    mfxFrameSurface1 *insurf;
    mfxFrameSurface1 *outsurf;
    mfxSyncPoint sync;
    mfxBitstream bs = { { { 0 } } };
    int ret;
    int n_out_frames;
    int buffered = 0;

    if (!q->engine_ready) {
        ret = ff_qsv_decode_init(avctx, q, avpkt);
        if (ret)
            return ret;
    }

    if (avpkt->size ) {
        if (av_fifo_size(q->input_fifo)) {
            /* we have got rest of previous packet into buffer */
            if (av_fifo_space(q->input_fifo) < avpkt->size) {
                ret = av_fifo_grow(q->input_fifo, avpkt->size);
                if (ret < 0)
                    return ret;
            }
            av_fifo_generic_write(q->input_fifo, avpkt->data, avpkt->size, NULL);
            bs.Data       = q->input_fifo->rptr;
            bs.DataLength = av_fifo_size(q->input_fifo);
            buffered = 1;
        } else {
            bs.Data       = avpkt->data;
            bs.DataLength = avpkt->size;
        }
        bs.MaxLength  = bs.DataLength;
        bs.TimeStamp  = avpkt->pts;
    }

    while (1) {
        ret = get_surface(avctx, q, &insurf);
        if (ret < 0)
            return ret;
        do {
            ret = MFXVideoDECODE_DecodeFrameAsync(q->session, avpkt->size ? &bs : NULL,
                                                  insurf, &outsurf, &sync);
            if (ret != MFX_WRN_DEVICE_BUSY)
                break;
            av_usleep(1);
        } while (1);

        if (MFX_WRN_VIDEO_PARAM_CHANGED==ret) {
            /* TODO: handle here sequence header changing */
        }

        if (sync) {
            QSVFrame *out_frame = find_frame(q, outsurf);

            if (!out_frame) {
                av_log(avctx, AV_LOG_ERROR,
                       "The returned surface does not correspond to any frame\n");
                return AVERROR_BUG;
            }

            out_frame->queued = 1;
            av_fifo_generic_write(q->async_fifo, &out_frame, sizeof(out_frame), NULL);
            av_fifo_generic_write(q->async_fifo, &sync,      sizeof(sync),      NULL);

            continue;
        }
        if (MFX_ERR_MORE_SURFACE != ret && ret < 0)
            break;
    }

    /* make sure we do not enter an infinite loop if the SDK
     * did not consume any data and did not return anything */
    if (!sync && !bs.DataOffset) {
        av_log(avctx, AV_LOG_WARNING, "A decode call did not consume any data\n");
        bs.DataOffset = avpkt->size;
    }

    if (buffered) {
        qsv_fifo_relocate(q->input_fifo, bs.DataOffset);
    } else if (bs.DataOffset!=avpkt->size) {
        /* some data of packet was not consumed. store it to local buffer */
        av_fifo_generic_write(q->input_fifo, avpkt->data+bs.DataOffset,
                              avpkt->size - bs.DataOffset, NULL);
    }

    if (MFX_ERR_MORE_DATA!=ret && ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Error %d during QSV decoding.\n", ret);
        return ff_qsv_error(ret);
    }
    n_out_frames = av_fifo_size(q->async_fifo) / (sizeof(out_frame)+sizeof(sync));

    if (n_out_frames > q->async_depth || (!avpkt->size && n_out_frames) ) {
        AVFrame *src_frame;

        av_fifo_generic_read(q->async_fifo, &out_frame, sizeof(out_frame), NULL);
        av_fifo_generic_read(q->async_fifo, &sync,      sizeof(sync),      NULL);
        out_frame->queued = 0;

        MFXVideoCORE_SyncOperation(q->session, sync, 60000);

        src_frame = out_frame->frame;

        ret = av_frame_ref(frame, src_frame);
        if (ret < 0)
            return ret;

        outsurf = out_frame->surface;

        frame->pkt_pts = frame->pts = outsurf->Data.TimeStamp;

        frame->repeat_pict =
            outsurf->Info.PicStruct & MFX_PICSTRUCT_FRAME_TRIPLING ? 4 :
            outsurf->Info.PicStruct & MFX_PICSTRUCT_FRAME_DOUBLING ? 2 :
            outsurf->Info.PicStruct & MFX_PICSTRUCT_FIELD_REPEATED ? 1 : 0;
        frame->top_field_first =
            outsurf->Info.PicStruct & MFX_PICSTRUCT_FIELD_TFF;
        frame->interlaced_frame =
            !(outsurf->Info.PicStruct & MFX_PICSTRUCT_PROGRESSIVE);

        *got_frame = 1;
    }

    return avpkt->size;
}

int ff_qsv_decode_close(QSVContext *q)
{
    QSVFrame *cur = q->work_frames;

    while (cur) {
        q->work_frames = cur->next;
        av_frame_free(&cur->frame);
        av_freep(&cur);
        cur = q->work_frames;
    }

    av_fifo_free(q->async_fifo);
    q->async_fifo = NULL;

    av_fifo_free(q->input_fifo);
    q->input_fifo = NULL;

    MFXVideoDECODE_Close(q->session);
    q->session = NULL;

    ff_qsv_close_internal_session(&q->internal_qs);

    q->engine_ready = 0;

    return 0;
}
