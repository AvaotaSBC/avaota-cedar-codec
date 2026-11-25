#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

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
    int ve_fd;
    int codec;
};

struct cedar_dma_buf {
    int fd;
    uint32_t phy_addr;
};

#define CEDAR_MAP_DMA_BUF 0x504
#define CEDAR_UNMAP_DMA_BUF 0x505

int avaota_cedar_api_version(void)
{
    return 4;
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
    if (ctx->ve_fd >= 0)
        close(ctx->ve_fd);
    free(ctx->header);
    free(ctx);
}

void avaota_cedar_h264_close(struct cedar_h264 *ctx)
{
    cedar_destroy(ctx);
}

static struct cedar_h264 *cedar_open_scaled(int codec, int width, int height,
                                                 int output_width, int output_height,
                                                 int fps, int bitrate, int gop)
{
    struct cedar_h264 *ctx;
    VencBaseConfig base = {0};
    VencH264Param param = {0};
    VencH265Param h265 = {0};
    VencH264VideoSignal signal = {0};
    VencHeaderData header = {0};
    VencAllocateBufferParam alloc = {0};
    unsigned int vbv = 12 * 1024 * 1024;

    if (width < 16 || height < 16 || width > 4096 || height > 4096 ||
        output_width < 16 || output_height < 16 ||
        output_width > width || output_height > height ||
        (width & 1) || (height & 1) ||
        (output_width & 1) || (output_height & 1) ||
        fps < 1 || bitrate < 1 || gop < 1)
        return NULL;
    ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return NULL;
    ctx->ve_fd = -1;
    ctx->codec = codec;
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
    ctx->encoder = VideoEncCreate(codec);
    if (!ctx->encoder)
        goto fail;
    if (codec == VENC_CODEC_JPEG) {
        int quality = 85;
        int mode = 0;
        if (VideoEncSetParameter(ctx->encoder, VENC_IndexParamJpegQuality, &quality) ||
            VideoEncSetParameter(ctx->encoder, VENC_IndexParamJpegEncMode, &mode))
            goto fail;
    } else if (codec == VENC_CODEC_H265) {
        h265.nBitrate = bitrate;
        h265.nFramerate = fps;
        h265.idr_period = gop;
        h265.nIntraPeriod = gop;
        h265.nGopSize = gop;
        h265.nQPInit = 30;
        h265.sProfileLevel.nProfile = VENC_H265ProfileMain;
        h265.sProfileLevel.nLevel = VENC_H265Level41;
        h265.sQPRange.nMinqp = 10;
        h265.sQPRange.nMaxqp = 52;
        if (VideoEncSetParameter(ctx->encoder, VENC_IndexParamH265Param, &h265))
            goto fail;
    } else {
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
    if (!getenv("AVAOTA_CEDAR_FAST_ENC") ||
        strcmp(getenv("AVAOTA_CEDAR_FAST_ENC"), "0") != 0) {
        int fast = 1;
        VideoEncSetParameter(ctx->encoder, VENC_IndexParamFastEnc, &fast);
    }
    }
    VideoEncSetParameter(ctx->encoder, VENC_IndexParamSetVbvSize, &vbv);
    base.memops = ctx->memops;
    base.nInputWidth = width;
    // VIN's NV12 payload is tightly packed at the visible height. Its MMAP
    // allocation is larger, but the UV plane starts at width * height.
    base.nInputHeight = height;
    base.nStride = width;
    base.nDstWidth = output_width;
    base.nDstHeight = output_height;
    base.eInputFormat = VENC_PIXEL_YUV420SP;
    if (VideoEncInit(ctx->encoder, &base))
        goto fail;
    if (codec != VENC_CODEC_JPEG) {
        if (VideoEncGetParameter(ctx->encoder,
                                 codec == VENC_CODEC_H265 ? VENC_IndexParamH265Header : VENC_IndexParamH264SPSPPS,
                                 &header) || !header.pBuffer || !header.nLength)
            goto fail;
        ctx->header = malloc(header.nLength);
        if (!ctx->header)
            goto fail;
        memcpy(ctx->header, header.pBuffer, header.nLength);
        ctx->header_len = header.nLength;
    }
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

struct cedar_h264 *avaota_cedar_h264_open_scaled(int width, int height,
                                                 int output_width, int output_height,
                                                 int fps, int bitrate, int gop)
{
    return cedar_open_scaled(VENC_CODEC_H264, width, height, output_width,
                             output_height, fps, bitrate, gop);
}

struct cedar_h264 *avaota_cedar_h265_open_scaled(int width, int height,
                                                 int output_width, int output_height,
                                                 int fps, int bitrate, int gop)
{
    return cedar_open_scaled(VENC_CODEC_H265, width, height, output_width,
                             output_height, fps, bitrate, gop);
}

struct cedar_h264 *avaota_cedar_jpeg_open_scaled(int width, int height,
                                                 int output_width, int output_height,
                                                 int fps, int bitrate, int quality)
{
    struct cedar_h264 *ctx = cedar_open_scaled(VENC_CODEC_JPEG, width, height,
                                                output_width, output_height, fps,
                                                bitrate, 1);
    if (ctx && VideoEncSetParameter(ctx->encoder, VENC_IndexParamJpegQuality, &quality)) {
        cedar_destroy(ctx);
        return NULL;
    }
    return ctx;
}

struct cedar_h264 *avaota_cedar_h264_open(int width, int height, int fps,
                                          int bitrate, int gop)
{
    return avaota_cedar_h264_open_scaled(width, height, width, height,
                                          fps, bitrate, gop);
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

static int has_idr(const unsigned char *data, size_t len, int codec)
{
    size_t i;
    for (i = 0; i + 4 < len; i++) {
        if (data[i] || data[i + 1] || data[i + 2] != 1)
            continue;
        if (codec == VENC_CODEC_H264 ? (data[i + 3] & 0x1f) == 5 :
            ((data[i + 3] >> 1) & 0x3f) >= 16 && ((data[i + 3] >> 1) & 0x3f) <= 23)
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
    key = ctx->codec == VENC_CODEC_JPEG || has_idr(output.pData0, output.nSize0, ctx->codec) ||
          has_idr(output.pData1, output.nSize1, ctx->codec) || ctx->frames == 0;
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

int avaota_cedar_h264_import(struct cedar_h264 *ctx, int dmabuf_fd,
                             uint32_t *ve_address)
{
    struct cedar_dma_buf mapping = { .fd = dmabuf_fd };
    if (!ctx || dmabuf_fd < 0 || !ve_address)
        return -EINVAL;
    if (ctx->ve_fd < 0) {
        ctx->ve_fd = open("/dev/cedar_dev", O_RDWR | O_CLOEXEC);
        if (ctx->ve_fd < 0)
            return -errno;
    }
    if (ioctl(ctx->ve_fd, CEDAR_MAP_DMA_BUF, &mapping) < 0)
        return -errno;
    if (!mapping.phy_addr) {
        ioctl(ctx->ve_fd, CEDAR_UNMAP_DMA_BUF, &mapping);
        return -EIO;
    }
    *ve_address = mapping.phy_addr;
    return 0;
}

int avaota_cedar_h264_unimport(struct cedar_h264 *ctx, int dmabuf_fd)
{
    struct cedar_dma_buf mapping = { .fd = dmabuf_fd };
    if (!ctx || ctx->ve_fd < 0 || dmabuf_fd < 0)
        return -EINVAL;
    return ioctl(ctx->ve_fd, CEDAR_UNMAP_DMA_BUF, &mapping) < 0 ? -errno : 0;
}

int avaota_cedar_h264_encode_imported(struct cedar_h264 *ctx, uint32_t ve_address,
                                      int64_t pts_ms, unsigned char *dst,
                                      size_t capacity, size_t *written, int *keyframe)
{
    VencInputBuffer input = {0};
    VencOutputBuffer output = {0};
    size_t image_size, total;
    int key, result = -EIO, input_owned = 0, output_owned = 0;

    if (!ctx || !ve_address || !dst || !written || !keyframe)
        return -EINVAL;
    image_size = (size_t)ctx->width * ctx->height;
    *written = 0;
    *keyframe = 0;
    input.nPts = pts_ms;
    input.pAddrPhyY = (unsigned char *)(uintptr_t)ve_address;
    input.pAddrPhyC = (unsigned char *)(uintptr_t)(ve_address + image_size);
    input.nWidth = ctx->width;
    input.nHeight = ctx->height;
    if (AddOneInputBuffer(ctx->encoder, &input))
        return -EIO;
    input_owned = 1;
    if (VideoEncodeOneFrame(ctx->encoder))
        goto done;
    if (GetOneBitstreamFrame(ctx->encoder, &output))
        goto done;
    output_owned = 1;
    key = ctx->codec == VENC_CODEC_JPEG || has_idr(output.pData0, output.nSize0, ctx->codec) ||
          has_idr(output.pData1, output.nSize1, ctx->codec) || ctx->frames == 0;
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
    if (input_owned)
        AlreadyUsedInputBuffer(ctx->encoder, &input);
    return result;
}
