#include "H264StreamDecoder.hpp"

#include <cstring>
#include <cstdio>
#include <vector>
#include <cstdarg>
#include <switch.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
}


// Without an explicit get_format picking AV_PIX_FMT_NVTEGRA the hwaccel is not properly attached
// (no hw frames context), and freeing a frame later derefs a NULL nvtegra ctx.

static enum AVPixelFormat PickNvtegraFormat(AVCodecContext *, const enum AVPixelFormat *fmts)
{
    for (const enum AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; p++)
        if (*p == AV_PIX_FMT_NVTEGRA)
            return *p;
    return AV_PIX_FMT_NONE;
}

bool H264StreamDecoder::EnsureInitialized()
{
    if (codecContext || initFailed)
        return codecContext != nullptr;

    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec)
    {
        initFailed = true;
        return false;
    }

    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    if (!ctx)
    {
        initFailed = true;
        return false;
    }

    AVBufferRef *hwRef = nullptr;
    if (av_hwdevice_ctx_create(&hwRef, AV_HWDEVICE_TYPE_NVTEGRA, nullptr, nullptr, 0) < 0)
    {
        avcodec_free_context(&ctx);
        initFailed = true;
        return false;
    }
    ctx->hw_device_ctx = av_buffer_ref(hwRef);
    ctx->get_format = PickNvtegraFormat;
    ctx->thread_count = 1;

    ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;

    if (avcodec_open2(ctx, codec, nullptr) < 0)
    {
        av_buffer_unref(&hwRef);
        avcodec_free_context(&ctx);
        initFailed = true;
        return false;
    }

    codecContext = ctx;
    hwDeviceRef = hwRef;
    packet = av_packet_alloc();
    hwFrame = av_frame_alloc();
    swFrame = av_frame_alloc();
    return true;
}

bool H264StreamDecoder::DecodeAccessUnit(const uint8_t *data, size_t size,
                                         void (*onFrame)(const uint8_t *, int, const uint8_t *, int, int, int, void *),
                                         void *userdata, bool present)
{
    if (!EnsureInitialized())
        return false;

    AVCodecContext *ctx = (AVCodecContext *)codecContext;
    AVPacket *pkt = (AVPacket *)packet;
    AVFrame *hwF = (AVFrame *)hwFrame;
    AVFrame *swF = (AVFrame *)swFrame;

    // Padded, owned copy: raw vector memory has no AV_INPUT_BUFFER_PADDING_SIZE.
    if (av_new_packet(pkt, (int)size) < 0)
        return false;
    memcpy(pkt->data, data, size);

    int sendRc = avcodec_send_packet(ctx, pkt);
    av_packet_unref(pkt);
    if (sendRc < 0)
    {
        hadError = true;
        return false;
    }

    bool gotFrame = false;
    int recvRc;
    while ((recvRc = avcodec_receive_frame(ctx, hwF)) == 0)
    {
        if (!present)
        {
            av_frame_unref(hwF); // backlog catch-up: decoded for reference only, not shown
            continue;
        }
        if (av_hwframe_transfer_data(swF, hwF, 0) == 0)
        {
            if (onFrame)
            {
                onFrame(swF->data[0], swF->linesize[0], swF->data[1], swF->linesize[1], swF->width, swF->height,
                        userdata);
            }
            gotFrame = true;
        }
        av_frame_unref(swF);
        av_frame_unref(hwF);
    }
    if (recvRc != AVERROR(EAGAIN) && recvRc != AVERROR_EOF)
        hadError = true;

    return gotFrame;
}

bool H264StreamDecoder::TakeError()
{
    const bool had = hadError;
    hadError = false;
    return had;
}

void H264StreamDecoder::Flush()
{
    if (codecContext)
        avcodec_flush_buffers((AVCodecContext *)codecContext);
}

H264StreamDecoder::~H264StreamDecoder()
{
    if (hwFrame)
        av_frame_free((AVFrame **)&hwFrame);
    if (swFrame)
        av_frame_free((AVFrame **)&swFrame);
    if (packet)
        av_packet_free((AVPacket **)&packet);
    if (codecContext)
        avcodec_free_context((AVCodecContext **)&codecContext);
    if (hwDeviceRef)
        av_buffer_unref((AVBufferRef **)&hwDeviceRef);
}

