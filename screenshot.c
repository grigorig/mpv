/*
 * This file is part of mplayer2.
 *
 * mplayer2 is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * mplayer2 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with mplayer2; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <setjmp.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <libswscale/swscale.h>
#include <libavcodec/avcodec.h>

#include "config.h"

#ifdef CONFIG_JPEG
#include <jpeglib.h>
#endif

#include "talloc.h"
#include "screenshot.h"
#include "mp_core.h"
#include "mp_msg.h"
#include "libmpcodecs/img_format.h"
#include "libmpcodecs/mp_image.h"
#include "libmpcodecs/dec_video.h"
#include "libmpcodecs/vf.h"
#include "libvo/video_out.h"

#include "fmt-conversion.h"

//for sws_getContextFromCmdLine_hq and mp_sws_set_colorspace
#include "libmpcodecs/vf_scale.h"
#include "libvo/csputils.h"

typedef struct screenshot_ctx {
    struct MPContext *mpctx;

    int full_window;
    int each_frame;
    int using_vf_screenshot;

    int frameno;
    char fname[102];
} screenshot_ctx;

struct img_writer {
    const char *file_ext;
    int (*write)(screenshot_ctx *ctx, mp_image_t *image);
};

static screenshot_ctx *screenshot_get_ctx(MPContext *mpctx)
{
    if (!mpctx->screenshot_ctx) {
        mpctx->screenshot_ctx = talloc(mpctx, screenshot_ctx);
        *mpctx->screenshot_ctx = (screenshot_ctx) {
            .mpctx = mpctx,
        };
    }
    return mpctx->screenshot_ctx;
}

static FILE *open_file(screenshot_ctx *ctx, char *fname) {
    FILE *fp = fopen(fname, "wb");
    if (fp == NULL)
        mp_msg(MSGT_CPLAYER, MSGL_ERR, "\nPNG Error opening %s for writing!\n",
               fname);
    return fp;
}

static int write_png(screenshot_ctx *ctx, struct mp_image *image)
{
    char *fname = ctx->fname;
    FILE *fp = NULL;
    void *outbuffer = NULL;
    int success = 0;

    AVCodecContext *avctx = avcodec_alloc_context();
    if (!avctx)
        goto error_exit;

    if (avcodec_open(avctx, avcodec_find_encoder(CODEC_ID_PNG))) {
        mp_msg(MSGT_CPLAYER, MSGL_INFO, "Could not open libavcodec PNG encoder"
               " for saving screenshot\n");
        goto error_exit;
    }

    avctx->width = image->width;
    avctx->height = image->height;
    avctx->pix_fmt = PIX_FMT_RGB24;
    avctx->compression_level = 0;

    size_t outbuffer_size = image->width * image->height * 3 * 2;
    outbuffer = malloc(outbuffer_size);
    if (!outbuffer)
        goto error_exit;

    AVFrame pic;
    pic.data[0] = image->planes[0];
    pic.linesize[0] = image->stride[0];
    int size = avcodec_encode_video(avctx, outbuffer, outbuffer_size, &pic);
    if (size < 1)
        goto error_exit;

    fp = open_file(ctx, fname);
    if (fp == NULL)
        goto error_exit;

    fwrite(outbuffer, size, 1, fp);
    fflush(fp);

    if (ferror(fp))
        goto error_exit;

    success = 1;
error_exit:
    if (avctx)
        avcodec_close(avctx);
    if (fp)
        fclose(fp);
    free(outbuffer);
    return success;
}

#ifdef CONFIG_JPEG

static void write_jpeg_error_exit(j_common_ptr cinfo)
{
  // NOTE: do not write error message, too much effort to connect the libjpeg
  //       log callbacks with mplayer's log function mp_msp()

  // Return control to the setjmp point
  longjmp(*(jmp_buf*)cinfo->client_data, 1);
}

static int write_jpeg(screenshot_ctx *ctx, mp_image_t *image)
{
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    FILE *outfile = open_file(ctx, ctx->fname);

    if (!outfile)
        return 0;

    cinfo.err = jpeg_std_error(&jerr);
    jerr.error_exit = write_jpeg_error_exit;

    jmp_buf error_return_jmpbuf;
    cinfo.client_data = &error_return_jmpbuf;
    if (setjmp(cinfo.client_data)) {
        jpeg_destroy_compress(&cinfo);
        fclose(outfile);
        return 0;
    }

    jpeg_create_compress(&cinfo);
    jpeg_stdio_dest(&cinfo, outfile);

    cinfo.image_width = image->width;
    cinfo.image_height = image->height;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;

    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, ctx->mpctx->opts.screenshot_jpeg_quality, 1);

    jpeg_start_compress(&cinfo, TRUE);

    while (cinfo.next_scanline < cinfo.image_height) {
        JSAMPROW row_pointer[1];
        row_pointer[0] = image->planes[0] + cinfo.next_scanline * image->stride[0];
        jpeg_write_scanlines(&cinfo, row_pointer,1);
    }

    jpeg_finish_compress(&cinfo);

    jpeg_destroy_compress(&cinfo);
    fclose(outfile);

    return 1;
}

#endif

static const struct img_writer img_writers[] = {
    { "png", write_png },
#ifdef CONFIG_JPEG
    { "jpg", write_jpeg },
    { "jpeg", write_jpeg },
#endif
};

static const struct img_writer *get_writer(screenshot_ctx *ctx)
{
    const char *type = ctx->mpctx->opts.screenshot_filetype;

    for (size_t n = 0; n < sizeof(img_writers) / sizeof(img_writers[0]); n++) {
        const struct img_writer *writer = &img_writers[n];
        if (type && strcmp(type, writer->file_ext) == 0)
            return writer;
    }

    return &img_writers[0];
}

static int fexists(char *fname)
{
    struct stat dummy;
    if (stat(fname, &dummy) == 0)
        return 1;
    else
        return 0;
}

static void gen_fname(screenshot_ctx *ctx, const char *ext)
{
    do {
        snprintf(ctx->fname, 100, "shot%04d.%s", ++ctx->frameno, ext);
    } while (fexists(ctx->fname) && ctx->frameno < 100000);
    if (fexists(ctx->fname)) {
        ctx->fname[0] = '\0';
        return;
    }

    mp_msg(MSGT_CPLAYER, MSGL_INFO, "*** screenshot '%s' ***\n", ctx->fname);

}

void screenshot_save(struct MPContext *mpctx, struct mp_image *image)
{
    screenshot_ctx *ctx = screenshot_get_ctx(mpctx);
    const struct img_writer *writer = get_writer(ctx);
    struct mp_image *dst = alloc_mpi(image->w, image->h, IMGFMT_RGB24);

    struct SwsContext *sws = sws_getContextFromCmdLine_hq(image->width,
                                                          image->height,
                                                          image->imgfmt,
                                                          dst->width,
                                                          dst->height,
                                                          dst->imgfmt);

    struct mp_csp_details colorspace;
    get_detected_video_colorspace(mpctx->sh_video, &colorspace);
    // this is a property of the output device; images always use full-range RGB
    colorspace.levels_out = MP_CSP_LEVELS_PC;
    mp_sws_set_colorspace(sws, &colorspace);

    sws_scale(sws, (const uint8_t **)image->planes, image->stride, 0,
              image->height, dst->planes, dst->stride);

    gen_fname(ctx, writer->file_ext);
    writer->write(ctx, dst);

    sws_freeContext(sws);
    free_mp_image(dst);
}

static void vf_screenshot_callback(void *pctx, struct mp_image *image)
{
    struct MPContext *mpctx = (struct MPContext *)pctx;
    screenshot_ctx *ctx = screenshot_get_ctx(mpctx);
    screenshot_save(mpctx, image);
    if (ctx->each_frame)
        screenshot_request(mpctx, 0, ctx->full_window);
}

void screenshot_request(struct MPContext *mpctx, bool each_frame,
                        bool full_window)
{
    if (mpctx->video_out && mpctx->video_out->config_ok) {
        screenshot_ctx *ctx = screenshot_get_ctx(mpctx);

        ctx->using_vf_screenshot = 0;

        if (each_frame) {
            ctx->each_frame = !ctx->each_frame;
            ctx->full_window = full_window;
            if (!ctx->each_frame)
                return;
        }

        struct voctrl_screenshot_args args = { .full_window = full_window };
        if (vo_control(mpctx->video_out, VOCTRL_SCREENSHOT, &args) == true) {
            screenshot_save(mpctx, args.out_image);
            free_mp_image(args.out_image);
        } else {
            mp_msg(MSGT_CPLAYER, MSGL_INFO, "No VO support for taking"
                   " screenshots, trying VFCTRL_SCREENSHOT!\n");
            ctx->using_vf_screenshot = 1;
            struct vf_ctrl_screenshot cmd = {
                .image_callback = vf_screenshot_callback,
                .image_callback_ctx = mpctx,
            };
            struct vf_instance *vfilter = mpctx->sh_video->vfilter;
            if (vfilter->control(vfilter, VFCTRL_SCREENSHOT, &cmd) !=
                    CONTROL_OK)
                mp_msg(MSGT_CPLAYER, MSGL_INFO,
                       "...failed (need --vf=screenshot?)\n");
        }
    }
}

void screenshot_flip(struct MPContext *mpctx)
{
    screenshot_ctx *ctx = screenshot_get_ctx(mpctx);

    if (!ctx->each_frame)
        return;

    // screenshot_flip is called when the VO presents a new frame. vf_screenshot
    // can behave completely different (consider filters inserted between
    // vf_screenshot and vf_vo, that add or remove frames), so handle this case
    // somewhere else.
    if (ctx->using_vf_screenshot)
        return;

    screenshot_request(mpctx, 0, ctx->full_window);
}