/*
* Interface to the Android Stagefright library for
* H/W accelerated H.264 decoding
*
* Copyright (C) 2011 Mohamed Naufal
* Copyright (C) 2011 Martin Storsjö
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

#include <binder/ProcessState.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/MediaBufferGroup.h>
#include <media/stagefright/MediaDebug.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/OMXClient.h>
#include <media/stagefright/OMXCodec.h>
#include <media/stagefright/openmax/OMX_IVCommon.h>
#include <new>
#include <map>
#include <list>

#include <android/log.h>

extern "C" {
#include "avcodec.h"
#include "libavutil/imgutils.h"
#include "internal.h"
}

using namespace android;

struct Frame {
    int64_t frameTime;
    int isKeyFrame;
    AVFrame *ffmpegFrame;
    uint8_t *buffer;
    size_t bufferSize;
};

static void freeFrame(Frame* frame, bool retainAVFrame) {
    if(frame == NULL) return;
    if(frame->ffmpegFrame && !retainAVFrame) av_frame_free(&(frame->ffmpegFrame));
    if(frame->buffer) av_freep(&(frame->buffer));;
    av_freep(&frame);
}

static Frame* allocFrame(int bufferSize) {
    Frame* newFrame = (Frame*)av_mallocz(sizeof(Frame));
    if(newFrame == NULL) return NULL;

    newFrame->ffmpegFrame = NULL;
    newFrame->buffer = bufferSize > 0 ? (uint8_t*)av_malloc(bufferSize) : NULL;
    if(bufferSize > 0 && newFrame->buffer == NULL) {
        freeFrame(newFrame, false);
        return NULL;
    }
    newFrame->bufferSize = bufferSize;
    return newFrame;

}

struct TimeStamp {
    int64_t pts;
    int64_t reordered_opaque;
};

class CustomSource;

struct StagefrightContext {
    AVCodecContext *mVideo;
    AVBitStreamFilterContext *mConverter;
    uint8_t* frameExtraData;
    int frameExtraDataSize;
    std::list<Frame*> *inputFrameQueue, *outputFrameQueue;

    pthread_mutex_t inputQueueMutex, outputQueueMutex;
    pthread_cond_t frameAvailableCondition;
    pthread_t decoderThreadId;
    volatile sig_atomic_t decoderThreadStarted, decoderThreadExited, stopDecoderThread, stopReadThread;

    int64_t currentFrameIndex;
    std::map<int64_t, TimeStamp> *frameIndexToTimestampMap;

    OMXClient *omxClient;
    bool isOmxConnected;
    sp<MediaSource> mediaSource;
    sp<MediaSource> decoder;
    sp<MetaData> decoderOutputFormat;
    const char *decoderName;

    bool source_done;
};

class CustomSource : public MediaSource {
public:
    CustomSource(AVCodecContext *avctx, sp<MetaData> meta) : MediaSource() {
        stageFrightContext = (StagefrightContext*)avctx->priv_data;
        sourceMetaData = meta;
        frameSize  = (avctx->width * avctx->height * 3) / 2;
        bufferGroup.add_buffer(new MediaBuffer(frameSize));
    }

    virtual sp<MetaData> getFormat() {
        return sourceMetaData;
    }

    virtual status_t start(MetaData *params) {
        return OK;
    }

    virtual status_t stop() {
        return OK;
    }

    virtual status_t read(MediaBuffer **buffer, const MediaSource::ReadOptions *options) {
        status_t ret;

        //av_log(NULL, AV_LOG_DEBUG, "CustomSource::read\n");
        //for (;;) {
        while (stageFrightContext->inputFrameQueue->empty()) {
            if (stageFrightContext->stopReadThread) {
                //av_log(NULL, AV_LOG_DEBUG, "CustomSource::read: thread exiting\n");
                stageFrightContext->stopReadThread = true;
                return ERROR_END_OF_STREAM;
            }
            //av_log(NULL, AV_LOG_DEBUG, "CustomSource::read locking input queue\n");
            pthread_mutex_lock(&stageFrightContext->inputQueueMutex);
            //av_log(NULL, AV_LOG_DEBUG, "CustomSource::read waiting for frameAvailableCondition\n");
            pthread_cond_wait(&stageFrightContext->frameAvailableCondition, &stageFrightContext->inputQueueMutex);
        }

        Frame *frame = *stageFrightContext->inputFrameQueue->begin();

        //av_log(NULL, AV_LOG_DEBUG, "CustomSource::read: got frame from input queue of %d\n", frame->bufferSize);

        ret = bufferGroup.acquire_buffer(buffer);
        if (ret == OK) {
            memcpy((uint8_t *)(*buffer)->data() + (*buffer)->range_offset(), frame->buffer, frame->bufferSize);
            (*buffer)->set_range(0, frame->bufferSize);
            (*buffer)->meta_data()->clear();
            (*buffer)->meta_data()->setInt32(kKeyIsSyncFrame, frame->isKeyFrame);
            (*buffer)->meta_data()->setInt64(kKeyTime, frame->frameTime);
        } else {
            av_log(NULL, AV_LOG_ERROR, "Failed to acquire MediaBuffer\n");
        }

        stageFrightContext->inputFrameQueue->remove(frame);
        //av_log(NULL, AV_LOG_DEBUG, "CustomSource::read freeFrame\n");
        freeFrame(frame, false);
        pthread_mutex_unlock(&stageFrightContext->inputQueueMutex);

        //av_log(NULL, AV_LOG_DEBUG, "CustomSource::read returning OK\n");
        return OK;
        //}
    }

private:
    MediaBufferGroup bufferGroup;
    sp<MetaData> sourceMetaData;
    StagefrightContext *stageFrightContext;
    int frameSize;
};

static void pgm_save(unsigned char *buf, int wrap, int xsize, int ysize, char *filename)
{
    FILE *f;
    int i;
    f = fopen(filename,"w");
    fprintf(f, "P5\n%d %d\n%d\n", xsize, ysize, 255);
    for (i = 0; i < ysize; i++)
        fwrite(buf + i * wrap, 1, xsize, f);
    fclose(f);
}

void* decoderThread(void *arg)
{
    AVCodecContext *avctx = (AVCodecContext*)arg;
    StagefrightContext *stagefrightContext = (StagefrightContext*)avctx->priv_data;
    const AVPixFmtDescriptor *pixelDescriptor = av_pix_fmt_desc_get(avctx->pix_fmt);
    Frame* frame;
    MediaBuffer *mediaBuffer;
    int32_t width, height;
    int status;
    int imageLinesize[3];
    const uint8_t *imageData[3];
    int64_t frameTimestamp = 0;

    do {
        mediaBuffer = NULL;
        frame = NULL;

        //av_log(avctx, AV_LOG_DEBUG, "decoderThread: Pushing buffer to decoder\n");
        status = stagefrightContext->decoder->read(&mediaBuffer);

        //av_log(avctx, AV_LOG_DEBUG, "decoderThread: Decoder read returned %d\n", status);
        switch(status) {
        case OK: {
            sp<MetaData> imageFormat = stagefrightContext->decoder->getFormat();
            imageFormat->findInt32(kKeyWidth , &width);
            imageFormat->findInt32(kKeyHeight, &height);
            //av_log(avctx, AV_LOG_DEBUG, "decoderThread: Output format is %d x %d\n", width, height);

            frame = allocFrame(0);
            if (!frame) {
                av_log(avctx, AV_LOG_ERROR, "decoderThread: Can't allocate frame in decoder thread\n");
                break;
            }
            frame->ffmpegFrame = av_frame_alloc();
            if(frame->ffmpegFrame == NULL) {
                av_log(avctx, AV_LOG_ERROR, "decoderThread: Can't allocate AVFrame in decoder thread\n");
                freeFrame(frame, false);
                break;
            }

            // The OMX.SEC decoder doesn't signal the modified width/height
            if (stagefrightContext->decoderName                             &&
                    !strncmp(stagefrightContext->decoderName, "OMX.SEC", 7) &&
                    (width & 15 || height & 15)                             &&
                    ((width + 15)&~15) * ((height + 15)&~15) * 3/2 == mediaBuffer->range_length()) {

                width = (width + 15)&~15;
                height = (height + 15)&~15;
            }

            if (!avctx->width || !avctx->height || avctx->width > width || avctx->height > height) {
                avctx->width  = width;
                avctx->height = height;
            }

            if(avpicture_fill((AVPicture *)(frame->ffmpegFrame), (uint8_t*)mediaBuffer->data(), avctx->pix_fmt, width, height) < 0) {
                av_log(avctx, AV_LOG_ERROR, "decoderThread: Can't do avpicture_fill\n");
                freeFrame(frame, false);
                break;
            }

            frame->ffmpegFrame->format = avctx->pix_fmt;
            frame->ffmpegFrame->height = height;
            frame->ffmpegFrame->width = width;
            frame->ffmpegFrame->channel_layout = 0;

//            V/ffmpeg  (10410): ret_frame: format -1
//            V/ffmpeg  (10410): ret_frame: 0 x 0
//            V/ffmpeg  (10410): ret_frame: channels 0
//            V/ffmpeg  (10410): ret_frame: channel_layout 0
//            V/ffmpeg  (10410): ret_frame: format -1
//            V/ffmpeg  (10410): ret_frame: nb_samples 0

            //pgm_save(frame->ffmpegFrame->data[0], frame->ffmpegFrame->linesize[0], width, height, "/sdcard/decoded.pgm");

            //av_log(avctx, AV_LOG_DEBUG, "decoderThread: Hurry up into the output :-)\n");
            if (mediaBuffer->meta_data()->findInt64(kKeyTime, &frameTimestamp) &&
                    stagefrightContext->frameIndexToTimestampMap->count(frameTimestamp) > 0) {

                //av_log(avctx, AV_LOG_DEBUG, "decoderThread: writing pts\n");
                frame->ffmpegFrame->pts = (*(stagefrightContext->frameIndexToTimestampMap))[frameTimestamp].pts;
                //av_log(avctx, AV_LOG_DEBUG, "decoderThread: writing reorder opaque\n");
                frame->ffmpegFrame->reordered_opaque = (*stagefrightContext->frameIndexToTimestampMap)[frameTimestamp].reordered_opaque;
                //av_log(avctx, AV_LOG_DEBUG, "decoderThread: erasing timestamp from map\n");
                stagefrightContext->frameIndexToTimestampMap->erase(frameTimestamp);
            }

            //av_log(avctx, AV_LOG_DEBUG, "decoderThread: Waiting for a slot in the output\n");
            while (true) {
                pthread_mutex_lock(&stagefrightContext->outputQueueMutex);
                if (stagefrightContext->outputFrameQueue->size() >= 10) {
                    pthread_mutex_unlock(&stagefrightContext->outputQueueMutex);
                    usleep(10000);
                    continue;
                }
                break;
            }
            //av_log(avctx, AV_LOG_DEBUG, "decoderThread: pushing frame to output queue\n");
            stagefrightContext->outputFrameQueue->push_back(frame);
            pthread_mutex_unlock(&stagefrightContext->outputQueueMutex);
            //av_log(avctx, AV_LOG_DEBUG, "decoderThread: Pushed decoded frame to output queue\n");
            mediaBuffer->release();
            break;
        }
        case INFO_FORMAT_CHANGED:
            //av_log(avctx, AV_LOG_DEBUG, "decoderThread: format has changed\n");
            if(mediaBuffer) mediaBuffer->release();
            freeFrame(frame, false);
            continue;
        default: {
            //av_log(avctx, AV_LOG_DEBUG, "decoderThread: Decoder status unknown. Exiting\n");
            if(mediaBuffer) mediaBuffer->release();
            freeFrame(frame, false);
            goto decoder_exit;
        }
        }
    } while (!stagefrightContext->stopDecoderThread);
decoder_exit:
    //av_log(avctx, AV_LOG_DEBUG, "decoderThread: return 0\n");
    stagefrightContext->decoderThreadExited = true;
    return 0;
}

static AVPixelFormat findPixelFormat(int omxColorFormat) {
    switch(omxColorFormat) {
    case OMX_COLOR_FormatMonochrome:
        return AV_PIX_FMT_MONOBLACK;
    case OMX_COLOR_Format24bitRGB888:
        return AV_PIX_FMT_RGB24;
    case OMX_COLOR_Format24bitBGR888:
        return AV_PIX_FMT_BGR24;
    case OMX_COLOR_Format32bitBGRA8888:
        return AV_PIX_FMT_BGRA;
    case OMX_COLOR_Format32bitARGB8888:
        return AV_PIX_FMT_ARGB;
    case OMX_COLOR_FormatYUV411Planar:
        return AV_PIX_FMT_YUV411P;
    case OMX_COLOR_FormatYUV411PackedPlanar:
        return AV_PIX_FMT_UYYVYY411;
    case OMX_COLOR_FormatYUV420Planar:
        return AV_PIX_FMT_YUV420P;
    case OMX_QCOM_COLOR_FormatYVU420SemiPlanar:
    case OMX_COLOR_FormatYUV420SemiPlanar:
        //Not sure about the order of U and V
        return AV_PIX_FMT_NV21;
    case OMX_TI_COLOR_FormatYUV420PackedSemiPlanar:
        return PIX_FMT_NV12;
    case OMX_COLOR_FormatYUV422Planar:
        return AV_PIX_FMT_YUV422P;
    case OMX_COLOR_FormatYUV422PackedPlanar:
        return AV_PIX_FMT_YUYV422;
    case OMX_COLOR_FormatYUV422SemiPlanar:
        return AV_PIX_FMT_NV16;
    case OMX_COLOR_FormatYCbYCr:
        return AV_PIX_FMT_YUYV422;
    case OMX_COLOR_FormatCbYCrY:
        return AV_PIX_FMT_UYVY422;
    case OMX_COLOR_FormatL8:
        return AV_PIX_FMT_GRAY8;
    case OMX_COLOR_Format16bitARGB4444:
        return AV_PIX_FMT_RGB444;
    case OMX_COLOR_Format16bitARGB1555:
        return AV_PIX_FMT_RGB555;
    case OMX_COLOR_Format16bitRGB565:
        return AV_PIX_FMT_RGB565;
    case OMX_COLOR_Format16bitBGR565:
        return AV_PIX_FMT_BGR565;
    case OMX_COLOR_FormatL16:
        return AV_PIX_FMT_GRAY16;
    default:
        return AV_PIX_FMT_NONE;
    }
}

static int filter_packet(AVPacket *avPacket, AVCodecContext *avCodecContext, AVBitStreamFilterContext *avBitStreamFilterContext) {
    int ret = 0;

    while (avBitStreamFilterContext) {
        AVPacket maybeFiltereAvPacket = *avPacket;
        ret = av_bitstream_filter_filter(avBitStreamFilterContext,
                                         avCodecContext,
                                         NULL,
                                         &maybeFiltereAvPacket.data,
                                         &maybeFiltereAvPacket.size,
                                         avPacket->data,
                                         avPacket->size,
                                         avPacket->flags & AV_PKT_FLAG_KEY);

        if (ret == 0 && maybeFiltereAvPacket.data != avPacket->data && maybeFiltereAvPacket.destruct) {
            if ((ret = av_copy_packet(&maybeFiltereAvPacket, avPacket)) < 0) break;
            ret = 1;
        }

        if (ret > 0) {
            maybeFiltereAvPacket.buf = av_buffer_create(maybeFiltereAvPacket.data,
                                                        maybeFiltereAvPacket.size,
                                                        av_buffer_default_free,
                                                        NULL,
                                                        0);

            if (!maybeFiltereAvPacket.buf) break;
        }
        *avPacket = maybeFiltereAvPacket;

        avBitStreamFilterContext = avBitStreamFilterContext->next;
    }

    if (ret < 0) {
        av_log(avCodecContext,
               AV_LOG_ERROR,
               "Failed to filter bitstream with filter %s for stream %d with codec %s\n",
               avBitStreamFilterContext->filter->name,
               avPacket->stream_index,
               avcodec_get_name(avCodecContext->codec_id));
    }

    return ret;
}

static int Stagefright_decode_frame(AVCodecContext *avctx, void *returnData, int *gotFrame, AVPacket *avPacket) {
    StagefrightContext *stagefrightContext = (StagefrightContext*)avctx->priv_data;
    Frame *frame;
    int orig_size = avPacket->size;
    AVFrame *returnFrame;

    if (stagefrightContext->decoderThreadExited) return ERROR_END_OF_STREAM;

    if (avPacket && avPacket->data) filter_packet(avPacket, avctx, stagefrightContext->mConverter);

    if (!stagefrightContext->source_done) {
        frame = allocFrame(avPacket->size);
        if(frame == NULL) {
            av_free_packet(avPacket);
            return AVERROR(ENOMEM);
        }

        //av_log(avctx, AV_LOG_DEBUG, "Allocated frame of size\n");
        if (avPacket->data) {
            //av_log(avctx, AV_LOG_DEBUG, "Got data\n");
            frame->isKeyFrame = avPacket->flags & AV_PKT_FLAG_KEY ? 1 : 0;
            //av_log(avctx, AV_LOG_DEBUG, "Allocating frame buffer\n");
            uint8_t *ptr = avPacket->data;
            // The OMX.SEC decoder fails without this.
            if (avPacket->size == orig_size + avctx->extradata_size) {
                ptr += avctx->extradata_size;
                frame->bufferSize = orig_size;
            }
            memcpy(frame->buffer, ptr, orig_size);

            frame->frameTime = ++stagefrightContext->currentFrameIndex;
            (*stagefrightContext->frameIndexToTimestampMap)[stagefrightContext->currentFrameIndex].pts = avPacket->pts;
            (*stagefrightContext->frameIndexToTimestampMap)[stagefrightContext->currentFrameIndex].reordered_opaque = avctx->reordered_opaque;
        } else {
            //av_log(avctx, AV_LOG_DEBUG, "Stagefright decode: End of stream\n");
            stagefrightContext->source_done = true;
        }

        while (true) {
            if (stagefrightContext->decoderThreadExited) {
                stagefrightContext->source_done = true;
                break;
            }
            //av_log(avctx, AV_LOG_DEBUG, "Stagefright decode: locking input queue\n");
            pthread_mutex_lock(&stagefrightContext->inputQueueMutex);
            if (stagefrightContext->inputFrameQueue->size() >= 10) {
                //av_log(avctx, AV_LOG_DEBUG, "Stagefright decode: input queue already contains max frames. Retryin insert in a bit\n");
                pthread_mutex_unlock(&stagefrightContext->inputQueueMutex);
                usleep(10000);
                continue;
            }
            //av_log(avctx, AV_LOG_DEBUG, "Stagefright decode: pushing frame to input queue\n");
            stagefrightContext->inputFrameQueue->push_back(frame);
            //av_log(avctx, AV_LOG_DEBUG, "Stagefright decode: broadcasting frameAvailableCondition\n");
            pthread_cond_broadcast(&stagefrightContext->frameAvailableCondition);
            //av_log(avctx, AV_LOG_DEBUG, "Stagefright decode: unlocking input queue\n");
            pthread_mutex_unlock(&stagefrightContext->inputQueueMutex);
            break;
        }
    }
    while (true) {
        if (stagefrightContext->decoderThreadExited) return ERROR_END_OF_STREAM;

        //av_log(avctx, AV_LOG_DEBUG, "Stagefright decode: locking output queue\n");
        pthread_mutex_lock(&stagefrightContext->outputQueueMutex);
        if (!stagefrightContext->outputFrameQueue->empty()) break;
        //av_log(avctx, AV_LOG_DEBUG, "Stagefright decode: unlocking empty output queue\n");
        pthread_mutex_unlock(&stagefrightContext->outputQueueMutex);
        if (stagefrightContext->source_done) {
            usleep(10000);
            continue;
        } else {
            av_free_packet(avPacket);
            return orig_size;
        }
    }

    frame = *(stagefrightContext->outputFrameQueue->begin());
    //av_log(avctx, AV_LOG_DEBUG, "Stagefright decode: got decoded frame\n");
    stagefrightContext->outputFrameQueue->erase(stagefrightContext->outputFrameQueue->begin());
    //av_log(avctx, AV_LOG_DEBUG, "Stagefright decode: unlocking output queue\n");
    pthread_mutex_unlock(&stagefrightContext->outputQueueMutex);

    returnFrame = frame->ffmpegFrame;
    //av_log(avctx, AV_LOG_DEBUG, "Stagefright decode: frame->ffmpegFrame addr is %d\n", (int)(returnFrame));
    *gotFrame = sizeof(AVFrame);
    int err;
    //returnFrame->extended_data = returnFrame->data;

    //av_log(avctx, AV_LOG_DEBUG, "ret_frame: format %d\n", returnFrame->format);
    //av_log(avctx, AV_LOG_DEBUG, "ret_frame: %d x %d\n", returnFrame->width, returnFrame->height);
    //av_log(avctx, AV_LOG_DEBUG, "ret_frame: channels %d\n", returnFrame->channels);
    //av_log(avctx, AV_LOG_DEBUG, "ret_frame: channel_layout %d\n", returnFrame->channel_layout);
    //av_log(avctx, AV_LOG_DEBUG, "ret_frame: nb_samples %d\n", returnFrame->nb_samples);

    if ((err = av_frame_ref((AVFrame*)returnData, returnFrame)) < 0) {
        av_log(avctx, AV_LOG_ERROR, "Stagefright_decode_frame: av_frame_ref failed with %d\n", err);
        freeFrame(frame, true);
        av_free_packet(avPacket);
        return -1;
    }

    freeFrame(frame, false);
    av_free_packet(avPacket);

    //av_log(avctx, AV_LOG_DEBUG, "Consumed %d\n", orig_size);
    return orig_size;
}

static void closeDecoderThread(AVCodecContext *avctx, StagefrightContext *stagefrightContext)
{
    Frame *frame;
    stagefrightContext->stopReadThread = true;
    if (stagefrightContext->decoderThreadStarted) {
        //av_log(avctx, AV_LOG_DEBUG, "Stagefright close decoder started\n");
        if (!stagefrightContext->decoderThreadExited) {
            stagefrightContext->stopDecoderThread = 1;

            // Make sure decode_thread() doesn't get stuck
            pthread_mutex_lock(&stagefrightContext->inputQueueMutex);
            pthread_cond_broadcast(&stagefrightContext->frameAvailableCondition);
            pthread_mutex_unlock(&stagefrightContext->inputQueueMutex);

            pthread_mutex_lock(&stagefrightContext->outputQueueMutex);
            while (!stagefrightContext->outputFrameQueue->empty()) {
                frame = *stagefrightContext->outputFrameQueue->begin();
                stagefrightContext->outputFrameQueue->remove(frame);
                av_log(NULL, AV_LOG_DEBUG, "Stagefrigh close decoder thread freeFrame\n");
                freeFrame(frame, false);

            }
            pthread_mutex_unlock(&stagefrightContext->outputQueueMutex);

            pthread_mutex_lock(&stagefrightContext->inputQueueMutex);
            pthread_cond_broadcast(&stagefrightContext->frameAvailableCondition);
            pthread_mutex_unlock(&stagefrightContext->inputQueueMutex);
        }

        //av_log(avctx, AV_LOG_DEBUG, "Stagefright close decoder waiting for decoder thread to finish\n");
        pthread_join(stagefrightContext->decoderThreadId, NULL);
        //av_log(avctx, AV_LOG_DEBUG, "Stagefright close decoder successful\n");
        stagefrightContext->decoderThreadStarted = false;
    }
}

static void closeOmxClient(StagefrightContext *stagefrightContext)
{
    stagefrightContext->decoder->stop();
    stagefrightContext->decoder.clear();

    if(stagefrightContext->isOmxConnected) {
        stagefrightContext->omxClient->disconnect();
        stagefrightContext->isOmxConnected = false;
    }
    if(stagefrightContext->omxClient != NULL) delete stagefrightContext->omxClient;
}

void closeQueues(StagefrightContext *stagefrightContext)
{
    Frame *frame;
    if(stagefrightContext->inputFrameQueue != NULL) {
        while (!stagefrightContext->inputFrameQueue->empty()) {
            frame = *stagefrightContext->inputFrameQueue->begin();
            stagefrightContext->inputFrameQueue->erase(stagefrightContext->inputFrameQueue->begin());
            //av_log(NULL, AV_LOG_DEBUG, "closeQueues freeFrame\n");
            freeFrame(frame, false);
        }
        delete stagefrightContext->inputFrameQueue;
    }
    if(stagefrightContext->outputFrameQueue != NULL) {
        while (!stagefrightContext->outputFrameQueue->empty()) {
            frame = *stagefrightContext->outputFrameQueue->begin();
            stagefrightContext->outputFrameQueue->erase(stagefrightContext->outputFrameQueue->begin());
            //av_log(NULL, AV_LOG_DEBUG, "closeQueues freeFrame\n");
            freeFrame(frame, false);
        }
        delete stagefrightContext->outputFrameQueue;
    }
}

static av_cold int Stagefright_init(AVCodecContext *avctx)
{
    StagefrightContext *stagefrightContext = (StagefrightContext*)avctx->priv_data;
    stagefrightContext->frameExtraData = NULL;
    stagefrightContext->mConverter = NULL;
    stagefrightContext->mediaSource = NULL;
    stagefrightContext->inputFrameQueue = NULL;
    stagefrightContext->outputFrameQueue = NULL;
    stagefrightContext->frameIndexToTimestampMap = NULL;
    stagefrightContext->omxClient = NULL;
    stagefrightContext->isOmxConnected = false;
    stagefrightContext->decoderOutputFormat = NULL;
    stagefrightContext->source_done = false;

    sp<MetaData> metaData = NULL;
    int colorFormat = 0;
    int status;
    int threadStarted;
    AVPixelFormat pixelFormat = AV_PIX_FMT_NONE;

    //av_log(avctx, AV_LOG_DEBUG, "Stagefright init\n");

    if (!avctx->extradata || !avctx->extradata_size || avctx->extradata[0] != 1) {
        av_log(avctx, AV_LOG_ERROR, "No extradata found\n");
        return -1;
    }

    stagefrightContext->mVideo = avctx;
    stagefrightContext->mConverter  = av_bitstream_filter_init("h264_mp4toannexb");
    if (!stagefrightContext->mConverter) {
        av_log(avctx, AV_LOG_ERROR, "Cannot open the h264_mp4toannexb BSF!\n");
        return -1;
    }

    stagefrightContext->frameExtraDataSize = avctx->extradata_size;
    stagefrightContext->frameExtraData = (uint8_t*) av_mallocz(avctx->extradata_size + FF_INPUT_BUFFER_PADDING_SIZE);
    if (!stagefrightContext->frameExtraData) {
        status = AVERROR(ENOMEM);
        goto fail;
    }
    memcpy(stagefrightContext->frameExtraData, avctx->extradata, avctx->extradata_size);

    metaData = new MetaData;
    if (metaData == NULL) {
        status = AVERROR(ENOMEM);
        goto fail;
    }
    metaData->setCString(kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_AVC);
    metaData->setInt32(kKeyWidth, avctx->width);
    metaData->setInt32(kKeyHeight, avctx->height);
    metaData->setInt32(kKeyColorFormat, OMX_COLOR_FormatYUV420SemiPlanar);
    if(avctx->extradata[0] == 1) metaData->setData(kKeyAVCC, kTypeAVCC, avctx->extradata, avctx->extradata_size);

    android::ProcessState::self()->startThreadPool();

    stagefrightContext->mediaSource      = new  CustomSource(avctx, metaData);
    if(stagefrightContext->mediaSource == NULL) {
        status = AVERROR(ENOMEM);
        goto fail;
    }
    stagefrightContext->inputFrameQueue  = new std::list<Frame*>;
    if(stagefrightContext->inputFrameQueue == NULL) {
        status = AVERROR(ENOMEM);
        goto fail;
    }
    stagefrightContext->outputFrameQueue = new std::list<Frame*>;
    if(stagefrightContext->outputFrameQueue == NULL) {
        status = AVERROR(ENOMEM);
        goto fail;
    }
    stagefrightContext->frameIndexToTimestampMap    = new std::map<int64_t, TimeStamp>;
    if(stagefrightContext->frameIndexToTimestampMap == NULL) {
        status = AVERROR(ENOMEM);
        goto fail;
    }
    stagefrightContext->omxClient    = new OMXClient;
    if(stagefrightContext->omxClient == NULL) {
        status = AVERROR(ENOMEM);
        goto fail;
    }

    if (stagefrightContext->omxClient->connect() !=  OK) {
        av_log(avctx, AV_LOG_ERROR, "Cannot connect OMX client\n");
        status = -1;
        goto fail;
    } else stagefrightContext->isOmxConnected = true;

    stagefrightContext->decoder = OMXCodec::Create(stagefrightContext->omxClient->interface(), metaData,
                                                   false, stagefrightContext->mediaSource, NULL,
                                                   OMXCodec::kClientNeedsFramebuffer);
    if (stagefrightContext->decoder->start() !=  OK) {
        av_log(avctx, AV_LOG_ERROR, "Cannot start decoder\n");
        status = -1;
        goto fail;
    }

    stagefrightContext->decoderOutputFormat = stagefrightContext->decoder->getFormat();
    stagefrightContext->decoderOutputFormat->findInt32(kKeyColorFormat, &colorFormat);

    pixelFormat = findPixelFormat(colorFormat);

    if(pixelFormat == AV_PIX_FMT_NONE) {
        av_log(avctx, AV_LOG_ERROR, "Decoder output format is unknown\n");
        status = colorFormat;
        goto fail;
    }
    avctx->pix_fmt = pixelFormat;

    stagefrightContext->decoderOutputFormat->findCString(kKeyDecoderComponent, &stagefrightContext->decoderName);
    if (stagefrightContext->decoderName) stagefrightContext->decoderName = av_strdup(stagefrightContext->decoderName);

    pthread_mutex_init(&stagefrightContext->inputQueueMutex, NULL);
    pthread_mutex_init(&stagefrightContext->outputQueueMutex, NULL);
    pthread_cond_init(&stagefrightContext->frameAvailableCondition, NULL);

    threadStarted = pthread_create(&stagefrightContext->decoderThreadId, NULL, &decoderThread, avctx);
    if(threadStarted == 0) {
        stagefrightContext->decoderThreadStarted = true;
    } else {
        stagefrightContext->decoderThreadStarted = false;
        goto fail;
    }

    //av_log(avctx, AV_LOG_DEBUG, "Init successful\n");
    return 0;
fail:
    av_log(avctx, AV_LOG_ERROR, "Failed with status %d\n", status);

    closeDecoderThread(avctx, stagefrightContext);
    closeOmxClient(stagefrightContext);
    closeQueues(stagefrightContext);

    if (stagefrightContext->mConverter != NULL) {
        av_bitstream_filter_close(stagefrightContext->mConverter);
        stagefrightContext->mConverter = NULL;
    }
    //if (stagefrightContext->frameExtraData != NULL) av_freep(&stagefrightContext->frameExtraData);
    if (stagefrightContext->frameIndexToTimestampMap != NULL) {
        delete stagefrightContext->frameIndexToTimestampMap;
        stagefrightContext->frameIndexToTimestampMap = NULL;
    }
    if (stagefrightContext->decoderName) av_freep(&stagefrightContext->decoderName);

    pthread_mutex_destroy(&stagefrightContext->inputQueueMutex);
    pthread_mutex_destroy(&stagefrightContext->outputQueueMutex);
    pthread_cond_destroy(&stagefrightContext->frameAvailableCondition);

    return status;
}

static av_cold int Stagefright_close(AVCodecContext *avctx)
{
    StagefrightContext *stagefrightContext = (StagefrightContext*)avctx->priv_data;
    Frame *frame;

    //av_log(avctx, AV_LOG_DEBUG, "Stagefright close initiated\n");

    closeDecoderThread(avctx, stagefrightContext);
    closeOmxClient(stagefrightContext);
    closeQueues(stagefrightContext);

    // Reset the extradata back to the original mp4 format, so that
    // the next invocation (both when decoding and when called from
    // av_find_stream_info) get the original mp4 format extradata.
    av_freep(&avctx->extradata);
    avctx->extradata = stagefrightContext->frameExtraData;
    avctx->extradata_size = stagefrightContext->frameExtraDataSize;

    if (stagefrightContext->mConverter != NULL) {
        av_bitstream_filter_close(stagefrightContext->mConverter);
        stagefrightContext->mConverter = NULL;
    }
    //if (stagefrightContext->frameExtraData != NULL) av_freep(&stagefrightContext->frameExtraData);
    if (stagefrightContext->frameIndexToTimestampMap != NULL) {
        delete stagefrightContext->frameIndexToTimestampMap;
        stagefrightContext->frameIndexToTimestampMap = NULL;
    }
    if (stagefrightContext->decoderName) av_freep(&stagefrightContext->decoderName);

    pthread_mutex_destroy(&stagefrightContext->inputQueueMutex);
    pthread_mutex_destroy(&stagefrightContext->outputQueueMutex);
    pthread_cond_destroy(&stagefrightContext->frameAvailableCondition);
    return 0;
}

AVCodec ff_libstagefright_h264_decoder = {
    "libstagefright_h264",
    NULL_IF_CONFIG_SMALL("libstagefright H.264"),
    AVMEDIA_TYPE_VIDEO,
    AV_CODEC_ID_H264,
    CODEC_CAP_DELAY,
    NULL, //supported_framerates
    NULL, //pix_fmts
    NULL, //supported_samplerates
    NULL, //sample_fmts
    NULL, //channel_layouts
    0,    //max_lowres
    NULL, //priv_class
    NULL, //profiles
    sizeof(StagefrightContext),
    NULL, //next
    NULL, //init_thread_copy
    NULL, //update_thread_context
    NULL, //defaults
    NULL, //init_static_data
    Stagefright_init,
    NULL, //encode
    NULL, //encode2
    Stagefright_decode_frame,
    Stagefright_close,
};
