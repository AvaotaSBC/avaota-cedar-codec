#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "memoryAdapter.h"
#include "sc_interface.h"
#include "vencoder.h"
#include "avaota_cedar_bridge.h"

struct cedar_h264 {
    VideoEncoder *encoder;
    struct ScMemOpsS *memops;
    unsigned char *header;
    size_t header_len;
    int width;
    int height;
    int aligned_height;
    int bitrate;
    int fps;
    int gop;
    unsigned int frames;
    int input_allocated;
};

int avaota_cedar_api_version(void)
{
    return 1;
}

static void cedar_destroy(struct cedar_h264 *ctx)
{
    if (!ctx)
        return;
    if (ctx->encoder && ctx->input_allocated)
        ReleaseAllocInputBuffer(ctx->encoder);
    if (ctx->encoder)
        VideoEncDestroy(ctx->encoder);
    if (ctx->memops)
        CdcMemClose(ctx->memops);
    free(ctx->header);
    free(ctx);
}

void avaota_cedar_h264_close(struct cedar_h264 *ctx)
{
    cedar_destroy(ctx);
}

struct cedar_h264 *avaota_cedar_h264_open(int width, int height, int fps,
                                          int bitrate, int gop)
{
    struct cedar_h264 *ctx;
    VencBaseConfig base = {0};
    VencH264Param param = {0};
    VencH264VideoSignal signal = {0};
    VencHeaderData header = {0};
    VencAllocateBufferParam alloc = {0};
    unsigned int vbv = 12 * 1024 * 1024;

    if (width < 16 || height < 16 || width > 4096 || height > 4096 ||
        (width & 1) || (height & 1) || fps < 1 || bitrate < 1 || gop < 1)
        return NULL;
    ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return NULL;
    ctx->width = width;
    ctx->height = height;
    ctx->aligned_height = (height + 15) & ~15;
    ctx->fps = fps;
    ctx->bitrate = bitrate;
    ctx->gop = gop;
    ctx->memops = MemAdapterGetOpsS();
    if (!ctx->memops || CdcMemOpen(ctx->memops)) {
        ctx->memops = NULL;
        goto fail;
    }
    ctx->encoder = VideoEncCreate(VENC_CODEC_H264);
    if (!ctx->encoder)
        goto fail;
    param.bEntropyCodingCABAC = 1;
    param.nBitrate = bitrate;
    param.nFramerate = fps;
    param.nMaxKeyInterval = gop;
    param.nCodingMode = VENC_FRAME_CODING;
    param.sProfileLevel.nProfile = VENC_H264ProfileHigh;
    param.sProfileLevel.nLevel = VENC_H264Level51;
    param.sQPRange.nMinqp = 10;
    param.sQPRange.nMaxqp = 50;
    if (VideoEncSetParameter(ctx->encoder, VENC_IndexParamH264Param, &param))
        goto fail;
    signal.video_format = DEFAULT;
    signal.full_range_flag = 1;
    signal.transfer_characteristics = 1;
    signal.matrix_coefficients = 1;
    signal.src_colour_primaries = VENC_BT709;
    signal.dst_colour_primaries = VENC_BT709;
    if (VideoEncSetParameter(ctx->encoder, VENC_IndexParamH264VideoSignal,
                             &signal))
        goto fail;
    VideoEncSetParameter(ctx->encoder, VENC_IndexParamSetVbvSize, &vbv);
    base.memops = ctx->memops;
    base.nInputWidth = width;
    base.nInputHeight = ctx->aligned_height;
    base.nStride = width;
    base.nDstWidth = width;
    base.nDstHeight = height;
    base.eInputFormat = VENC_PIXEL_YUV420SP;
    if (VideoEncInit(ctx->encoder, &base))
        goto fail;
    if (VideoEncGetParameter(ctx->encoder, VENC_IndexParamH264SPSPPS,
                             &header) || !header.pBuffer || !header.nLength)
        goto fail;
    ctx->header = malloc(header.nLength);
    if (!ctx->header)
        goto fail;
    memcpy(ctx->header, header.pBuffer, header.nLength);
    ctx->header_len = header.nLength;
    alloc.nBufferNum = 1;
    alloc.nSizeY = (unsigned int)(width * ctx->aligned_height);
    alloc.nSizeC = alloc.nSizeY / 2;
    if (AllocInputBuffer(ctx->encoder, &alloc))
        goto fail;
    ctx->input_allocated = 1;
    return ctx;
fail:
    cedar_destroy(ctx);
    return NULL;
}

int avaota_cedar_h264_set_bitrate(struct cedar_h264 *ctx, int bitrate)
{
    if (!ctx || bitrate < 1)
        return -EINVAL;
    if (VideoEncSetParameter(ctx->encoder, VENC_IndexParamBitrate, &bitrate))
        return -EIO;
    ctx->bitrate = bitrate;
    return 0;
}

int avaota_cedar_h264_force_keyframe(struct cedar_h264 *ctx)
{
    int force = 1;
    if (!ctx)
        return -EINVAL;
    return VideoEncSetParameter(ctx->encoder, VENC_IndexParamForceKeyFrame,
                                &force) ? -EIO : 0;
}

static int has_idr(const unsigned char *data, size_t len)
{
    size_t i;
    for (i = 0; i + 4 < len; i++) {
        if (data[i] || data[i + 1] || data[i + 2] != 1)
            continue;
        if ((data[i + 3] & 0x1f) == 5)
            return 1;
    }
    return 0;
}

int avaota_cedar_h264_encode(struct cedar_h264 *ctx, const unsigned char *nv12,
                              size_t len, int64_t pts_ms, unsigned char *dst,
                              size_t capacity, size_t *written, int *keyframe)
{
    VencInputBuffer input = {0};
    VencOutputBuffer output = {0};
    size_t image_size;
    size_t total;
    int row;
    int result = -EIO;
    int input_owned = 0;
    int output_owned = 0;
    int key;

    if (!ctx || !nv12 || !dst || !written || !keyframe)
        return -EINVAL;
    image_size = (size_t)ctx->width * ctx->height;
    if (len < image_size * 3 / 2)
        return -EINVAL;
    *written = 0;
    *keyframe = 0;
    if (GetOneAllocInputBuffer(ctx->encoder, &input))
        return -EIO;
    input_owned = 1;
    input.nPts = pts_ms;
    memcpy(input.pAddrVirY, nv12, image_size);
    for (row = ctx->height; row < ctx->aligned_height; row++)
        memcpy(input.pAddrVirY + row * ctx->width,
               input.pAddrVirY + (ctx->height - 1) * ctx->width, ctx->width);
    memcpy(input.pAddrVirC, nv12 + image_size, image_size / 2);
    for (row = ctx->height / 2; row < ctx->aligned_height / 2; row++)
        memcpy(input.pAddrVirC + row * ctx->width,
               input.pAddrVirC + (ctx->height / 2 - 1) * ctx->width, ctx->width);
    FlushCacheAllocInputBuffer(ctx->encoder, &input);
    if (AddOneInputBuffer(ctx->encoder, &input) ||
        VideoEncodeOneFrame(ctx->encoder))
        goto done;
    if (GetOneBitstreamFrame(ctx->encoder, &output))
        goto done;
    output_owned = 1;
    key = has_idr(output.pData0, output.nSize0) ||
          has_idr(output.pData1, output.nSize1) || ctx->frames == 0;
    total = output.nSize0 + output.nSize1 + (key ? ctx->header_len : 0);
    if (total > capacity) {
        result = -ENOSPC;
        goto done;
    }
    total = 0;
    if (key) {
        memcpy(dst, ctx->header, ctx->header_len);
        total += ctx->header_len;
    }
    memcpy(dst + total, output.pData0, output.nSize0);
    total += output.nSize0;
    if (output.nSize1) {
        memcpy(dst + total, output.pData1, output.nSize1);
        total += output.nSize1;
    }
    *written = total;
    *keyframe = key;
    ctx->frames++;
    result = 0;
done:
    if (output_owned)
        FreeOneBitStreamFrame(ctx->encoder, &output);
    if (input_owned) {
        AlreadyUsedInputBuffer(ctx->encoder, &input);
        ReturnOneAllocInputBuffer(ctx->encoder, &input);
    }
    return result;
}
