#ifndef AVAOTA_CEDAR_BRIDGE_H
#define AVAOTA_CEDAR_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct cedar_h264;

int avaota_cedar_api_version(void);
struct cedar_h264 *avaota_cedar_h264_open(int width, int height, int fps,
                                          int bitrate, int gop);
struct cedar_h264 *avaota_cedar_h264_open_scaled(int input_width, int input_height,
                                                 int output_width, int output_height,
                                                 int fps, int bitrate, int gop);
struct cedar_h264 *avaota_cedar_h265_open_scaled(int input_width, int input_height,
                                                 int output_width, int output_height,
                                                 int fps, int bitrate, int gop);
struct cedar_h264 *avaota_cedar_jpeg_open_scaled(int input_width, int input_height,
                                                 int output_width, int output_height,
                                                 int fps, int bitrate, int quality);
void avaota_cedar_h264_close(struct cedar_h264 *ctx);
int avaota_cedar_h264_set_bitrate(struct cedar_h264 *ctx, int bitrate);
int avaota_cedar_h264_force_keyframe(struct cedar_h264 *ctx);
int avaota_cedar_h264_encode(struct cedar_h264 *ctx, const unsigned char *nv12,
                              size_t len, int64_t pts_ms, unsigned char *dst,
                              size_t capacity, size_t *written, int *keyframe);
int avaota_cedar_h264_import(struct cedar_h264 *ctx, int dmabuf_fd,
                             uint32_t *ve_address);
int avaota_cedar_h264_unimport(struct cedar_h264 *ctx, int dmabuf_fd);
int avaota_cedar_h264_encode_imported(struct cedar_h264 *ctx, uint32_t ve_address,
                                      int64_t pts_ms, unsigned char *dst,
                                      size_t capacity, size_t *written, int *keyframe);

#ifdef __cplusplus
}
#endif

#endif
