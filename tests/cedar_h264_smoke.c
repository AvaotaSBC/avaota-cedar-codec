#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vencoder.h"
#include "memoryAdapter.h"
#include "sc_interface.h"

int main(int argc, char **argv)
{
    int w = 1920, h = 1080, aligned_h;
    struct ScMemOpsS *memops = NULL;
    VideoEncoder *enc = NULL;
    VencBaseConfig base = {0};
    VencH264Param param = {0};
    VencH264VideoSignal signal = {0};
    VencAllocateBufferParam alloc = {0};
    VencInputBuffer input = {0};
    VencOutputBuffer output = {0};
    VencHeaderData header = {0};
    FILE *in = NULL, *out = NULL;
    int rc = 1;
    if (argc != 3 && argc != 5) {
        fprintf(stderr, "usage: %s frame.nv12 frame.h264 [width height]\n", argv[0]);
        return 2;
    }
    if (argc == 5) {
        w = atoi(argv[3]); h = atoi(argv[4]);
        if (w <= 0 || h <= 0 || (w & 1) || (h & 1)) return 2;
    }
    aligned_h = (h + 15) & ~15;
    in = fopen(argv[1], "rb");
    out = fopen(argv[2], "wb");
    if (!in || !out) { perror("open"); goto done; }
    memops = MemAdapterGetOpsS();
    if (!memops || CdcMemOpen(memops)) { fprintf(stderr, "CdcMemOpen failed\n"); goto done; }
    base.memops = memops;
    base.nInputWidth = w; base.nInputHeight = aligned_h; base.nStride = w;
    base.nDstWidth = w; base.nDstHeight = h;
    base.eInputFormat = VENC_PIXEL_YUV420SP;
    enc = VideoEncCreate(VENC_CODEC_H264);
    if (!enc) { fprintf(stderr, "VideoEncCreate failed\n"); goto done; }
    param.bEntropyCodingCABAC = 1;
    param.nBitrate = 4000000; param.nFramerate = 30;
    param.nCodingMode = VENC_FRAME_CODING;
    param.nMaxKeyInterval = 30;
    param.sProfileLevel.nProfile = VENC_H264ProfileHigh;
    param.sProfileLevel.nLevel = VENC_H264Level51;
    param.sQPRange.nMinqp = 10; param.sQPRange.nMaxqp = 50;
    if (VideoEncSetParameter(enc, VENC_IndexParamH264Param, &param)) { fprintf(stderr, "set H264 failed\n"); goto done; }
    signal.video_format = DEFAULT;
    signal.full_range_flag = 1;
    signal.transfer_characteristics = 1;
    signal.matrix_coefficients = 1;
    signal.src_colour_primaries = VENC_BT709;
    signal.dst_colour_primaries = VENC_BT709;
    if (VideoEncSetParameter(enc, VENC_IndexParamH264VideoSignal, &signal)) { fprintf(stderr, "set color signal failed\n"); goto done; }
    unsigned int vbv = 12 * 1024 * 1024;
    VideoEncSetParameter(enc, VENC_IndexParamSetVbvSize, &vbv);
    if (VideoEncInit(enc, &base)) { fprintf(stderr, "VideoEncInit failed\n"); goto done; }
    if (VideoEncGetParameter(enc, VENC_IndexParamH264SPSPPS, &header) || !header.pBuffer) { fprintf(stderr, "header failed\n"); goto done; }
    fwrite(header.pBuffer, 1, header.nLength, out);
    alloc.nBufferNum = 1; alloc.nSizeY = w * aligned_h; alloc.nSizeC = w * aligned_h / 2;
    if (AllocInputBuffer(enc, &alloc) || GetOneAllocInputBuffer(enc, &input)) { fprintf(stderr, "alloc input failed\n"); goto done; }
    if (fread(input.pAddrVirY, 1, w * h, in) != (size_t)(w * h)) { fprintf(stderr, "Y input too short\n"); goto done; }
    for (int row = h; row < aligned_h; row++)
        memcpy(input.pAddrVirY + row * w, input.pAddrVirY + (h - 1) * w, w);
    if (fread(input.pAddrVirC, 1, w * h / 2, in) != (size_t)(w * h / 2)) { fprintf(stderr, "UV input too short\n"); goto done; }
    for (int row = h / 2; row < aligned_h / 2; row++)
        memcpy(input.pAddrVirC + row * w, input.pAddrVirC + (h / 2 - 1) * w, w);
    FlushCacheAllocInputBuffer(enc, &input);
    if (AddOneInputBuffer(enc, &input) || VideoEncodeOneFrame(enc)) { fprintf(stderr, "encode failed\n"); goto done; }
    if (GetOneBitstreamFrame(enc, &output)) { fprintf(stderr, "output failed\n"); goto done; }
    fwrite(output.pData0, 1, output.nSize0, out);
    fwrite(output.pData1, 1, output.nSize1, out);
    fprintf(stderr, "Cedar H264 frame: header=%u output=%u+%u\n", header.nLength, output.nSize0, output.nSize1);
    FreeOneBitStreamFrame(enc, &output);
    AlreadyUsedInputBuffer(enc, &input);
    ReturnOneAllocInputBuffer(enc, &input);
    rc = 0;
done:
    if (enc) VideoEncDestroy(enc);
    if (memops) CdcMemClose(memops);
    if (in) fclose(in);
    if (out) fclose(out);
    return rc;
}
