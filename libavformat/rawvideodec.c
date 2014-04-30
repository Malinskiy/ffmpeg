/*
 * RAW video demuxer
 * Copyright (c) 2001 Fabrice Bellard
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

#include "libavutil/parseutils.h"
#include "libavutil/pixdesc.h"
#include "libavutil/opt.h"
#include "internal.h"
#include "avformat.h"

/* an arbitrarily chosen "sane" max packet size -- 50M */
#define SANE_CHUNK_SIZE (50000000)

static int ffio_limit(AVIOContext *s, int size)
{
    if (s->maxsize>= 0) {
        int64_t remaining= s->maxsize - avio_tell(s);
        if (remaining < size) {
            int64_t newsize = avio_size(s);
            if (!s->maxsize || s->maxsize<newsize)
                s->maxsize = newsize - !newsize;
            remaining= s->maxsize - avio_tell(s);
            remaining= FFMAX(remaining, 0);
        }

        if (s->maxsize>= 0 && remaining+1 < size) {
            av_log(NULL, remaining ? AV_LOG_ERROR : AV_LOG_DEBUG, "Truncating packet of size %d to %"PRId64"\n", size, remaining+1);
            size = remaining+1;
        }
    }
    return size;
}

typedef struct RawVideoDemuxerContext {
    const AVClass *class;     /**< Class for private options. */
    int width, height;        /**< Integers describing video size, set by a private option. */
    char *pixel_format;       /**< Set by a private option. */
    int explicit_pts;
    AVRational framerate;     /**< AVRational describing framerate, set by a private option. */
} RawVideoDemuxerContext;


static int rawvideo_read_header(AVFormatContext *ctx)
{
    RawVideoDemuxerContext *s = ctx->priv_data;
    enum AVPixelFormat pix_fmt;
    AVStream *st;

    st = avformat_new_stream(ctx, NULL);
    if (!st)
        return AVERROR(ENOMEM);

    st->codec->codec_type = AVMEDIA_TYPE_VIDEO;

    st->codec->codec_id = ctx->iformat->raw_codec_id;

    if ((pix_fmt = av_get_pix_fmt(s->pixel_format)) == AV_PIX_FMT_NONE) {
        av_log(ctx, AV_LOG_ERROR, "No such pixel format: %s.\n",
               s->pixel_format);
        return AVERROR(EINVAL);
    }

    if(s->explicit_pts) {
        avpriv_set_pts_info(st, 64, 1, 1000);
        st->time_base = (AVRational){1, 1000};
    } else {
        avpriv_set_pts_info(st, 64, s->framerate.den, s->framerate.num);
    }

    st->codec->width  = s->width;
    st->codec->height = s->height;
    st->codec->pix_fmt = pix_fmt;
    st->codec->bit_rate = av_rescale_q(avpicture_get_size(st->codec->pix_fmt, s->width, s->height),
                                       (AVRational){8,1}, st->time_base);

    return 0;
}

static int append_packet_chunked_with_pts(AVIOContext *s, AVPacket *pkt, int explicit_pts, int64_t *pts, int size)
{
    int64_t orig_pos   = pkt->pos; // av_grow_packet might reset pos
    int orig_size      = pkt->size;
    int ret;

    do {
        int prev_size = pkt->size;
        int read_size;

        /* When the caller requests a lot of data, limit it to the amount
         * left in file or SANE_CHUNK_SIZE when it is not known. */
        read_size = size;
        if (read_size > SANE_CHUNK_SIZE/10) {
            read_size = ffio_limit(s, read_size);
            // If filesize/maxsize is unknown, limit to SANE_CHUNK_SIZE
            if (s->maxsize < 0)
                read_size = FFMIN(read_size, SANE_CHUNK_SIZE);
        }

        ret = av_grow_packet(pkt, read_size);
        if (ret < 0)
            break;

        ret = avio_read(s, (unsigned char*)pts, sizeof(int64_t));
        if (ret != sizeof(int64_t)) {
            av_shrink_packet(pkt, prev_size + FFMAX(ret, 0));
            break;
        }

        ret = avio_read(s, pkt->data + prev_size, read_size);
        if (ret != read_size) {
            av_shrink_packet(pkt, prev_size + FFMAX(ret, 0));
            break;
        }

        size -= read_size;
    } while (size > 0);
    if (size > 0)
        pkt->flags |= AV_PKT_FLAG_CORRUPT;

    pkt->pos = orig_pos;
    if (!pkt->size)
        av_free_packet(pkt);
    return pkt->size > orig_size ? pkt->size - orig_size : ret;
}

static int av_get_packet_with_pts(AVIOContext *s, AVPacket *pkt, int explicit_pts, int64_t *pts, int size)
{
    av_init_packet(pkt);
    pkt->data = NULL;
    pkt->size = 0;
    pkt->pos  = avio_tell(s);

    return append_packet_chunked_with_pts(s, pkt, explicit_pts, pts, size);
}

static int rawvideo_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    int packet_size, ret, width, height;
    int explicit_pts = ((RawVideoDemuxerContext*)s->priv_data)->explicit_pts;
    int64_t pts = 0;
    AVStream *st = s->streams[0];

    width = st->codec->width;
    height = st->codec->height;

    packet_size = avpicture_get_size(st->codec->pix_fmt, width, height);
    if (packet_size < 0)
        return -1;

    ret = av_get_packet_with_pts(s->pb, pkt, explicit_pts, &pts, packet_size);

    if(explicit_pts > 0) {
        pkt->pts = pkt->dts = pts;
        av_log(NULL, AV_LOG_DEBUG, "Got pts from stream %" PRId64 "\n", pts);
    } else {
        pkt->pts = pkt->dts = pkt->pos / packet_size;
    }

    pkt->stream_index = 0;
    if (ret < 0)
        return ret;
    return 0;
}

#define OFFSET(x) offsetof(RawVideoDemuxerContext, x)
#define DEC AV_OPT_FLAG_DECODING_PARAM
static const AVOption rawvideo_options[] = {
    { "video_size", "set frame size", OFFSET(width), AV_OPT_TYPE_IMAGE_SIZE, {.str = NULL}, 0, 0, DEC },
    { "pixel_format", "set pixel format", OFFSET(pixel_format), AV_OPT_TYPE_STRING, {.str = "yuv420p"}, 0, 0, DEC },
    { "framerate", "set frame rate", OFFSET(framerate), AV_OPT_TYPE_VIDEO_RATE, {.str = "25"}, 0, 0, DEC },
    { "explicit_pts", "use preceeding pts in packet", OFFSET(explicit_pts), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 1, DEC },
    { NULL },
};

static const AVClass rawvideo_demuxer_class = {
    .class_name = "rawvideo demuxer",
    .item_name  = av_default_item_name,
    .option     = rawvideo_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

AVInputFormat ff_rawvideo_demuxer = {
    .name           = "rawvideo",
    .long_name      = NULL_IF_CONFIG_SMALL("raw video"),
    .priv_data_size = sizeof(RawVideoDemuxerContext),
    .read_header    = rawvideo_read_header,
    .read_packet    = rawvideo_read_packet,
    .flags          = AVFMT_GENERIC_INDEX,
    .extensions     = "yuv,cif,qcif,rgb",
    .raw_codec_id   = AV_CODEC_ID_RAWVIDEO,
    .priv_class     = &rawvideo_demuxer_class,
};
