﻿/*
 * Copyright (C) 2017 Bouncyball
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the
 * Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "llrawproc.h"
#include "pixelproc.h"
#include "stripes.h"
#include "patternnoise.h"
#include "dualiso.h"
#include "hist.h"

#include "../mlv_object.h"

#define MIN(a,b) (((a)<(b))?(a):(b))
#define MAX(a,b) (((a)>(b))?(a):(b))
#define COERCE(x,lo,hi) MAX(MIN((x),(hi)),(lo))

static void deflicker(mlvObject_t * video, uint16_t * raw_image_buff, size_t raw_image_size)
{
    uint16_t black = video->RAWI.raw_info.black_level;
    uint16_t white = (1 << video->RAWI.raw_info.bits_per_pixel) - 1;

    struct histogram * hist = hist_create(white);
    hist_add(hist, raw_image_buff + 1, (uint32_t)((raw_image_size - 1) / 2), 1);
    uint16_t median = hist_median(hist);
    double correction = log2((double) (video->llrawproc->deflicker_target - black) / (median - black));
    video->RAWI.raw_info.exposure_bias[0] = correction * 10000;
    video->RAWI.raw_info.exposure_bias[1] = 10000;
}

static void subtract_darkframe(mlvObject_t * video, uint16_t * raw_image_buff, size_t raw_image_size)
{
    if( !video->llrawproc->dark_frame_data || (raw_image_size != video->llrawproc->dark_frame_size) )
    {
#ifndef STDOUT_SILENT
        printf("Subtracting dark frame is impossible, invalid dark frame'\n\n");
#endif
        return;
    }
#ifndef STDOUT_SILENT
    printf("Subtracting dark frame...'\n\n");
#endif
    uint16_t * dark_frame_data = video->llrawproc->dark_frame_data;
    uint32_t black_level = video->RAWI.raw_info.black_level;
    uint16_t white_level = (1 << video->RAWI.raw_info.bits_per_pixel) - 1;

    uint32_t pixel_count = raw_image_size / 2;
    for(uint32_t i = 0; i < pixel_count; i++)
    {
        int32_t orig_val = raw_image_buff[i];
        int32_t dark_val = dark_frame_data[i];

        raw_image_buff[i] = COERCE( orig_val - dark_val + black_level, 0, white_level );
    }
}

/* initialise low level raw processing struct */
llrawprocObject_t * initLLRawProcObject()
{
    llrawprocObject_t * llrawproc = calloc(1, sizeof(llrawprocObject_t));

    /* set defaults */
    llrawproc->vertical_stripes = 1;
    llrawproc->focus_pixels = 1;
    llrawproc->fpi_method = 1;
    llrawproc->bad_pixels = 1;
    llrawproc->bps_method = 0;
    llrawproc->bpi_method = 1;
    llrawproc->chroma_smooth = 0;
    llrawproc->pattern_noise = 0;
    llrawproc->deflicker_target = 0;
    llrawproc->fpm_status = 0;
    llrawproc->bpm_status = 0;
    llrawproc->compute_stripes = 1;
    llrawproc->first_time = 1;
    llrawproc->dual_iso = 0;
    llrawproc->is_dual_iso = 0;
    llrawproc->diso_averaging = 0;
    llrawproc->diso_alias_map = 1;
    llrawproc->diso_frblending = 1;
    llrawproc->dark_frame = 0;

    llrawproc->dark_frame_filename = NULL;
    llrawproc->dark_frame_data = NULL;
    llrawproc->dark_frame_size = 0;

    llrawproc->raw2ev = NULL;
    llrawproc->ev2raw = NULL;

    llrawproc->focus_pixel_map.type = PIX_FOCUS;
    llrawproc->focus_pixel_map.pixels = NULL;
    llrawproc->bad_pixel_map.type = PIX_BAD;
    llrawproc->bad_pixel_map.pixels = NULL;

    return llrawproc;
}

void freeLLRawProcObject(llrawprocObject_t * llrawproc)
{
    if(llrawproc->dark_frame_filename) free(llrawproc->dark_frame_filename);
    if(llrawproc->dark_frame_data) free(llrawproc->dark_frame_data);
    free_luts(llrawproc->raw2ev, llrawproc->ev2raw);
    free_pixel_maps(&(llrawproc->focus_pixel_map), &(llrawproc->bad_pixel_map));
    free(llrawproc);
}

/* all low level raw processing takes place here */
void applyLLRawProcObject(mlvObject_t * video, uint16_t * raw_image_buff, size_t raw_image_size)
{
    /* on fix_raw=0 skip raw processing alltogether */
    if(!video->llrawproc->fix_raw) return;

    /* do first time stuff */
    if(video->llrawproc->first_time)
    {
        /* check dual iso */
        video->llrawproc->is_dual_iso = diso_get_preview(raw_image_buff,
                                                         video->RAWI.xRes,
                                                         video->RAWI.yRes,
                                                         video->RAWI.raw_info.black_level,
                                                         video->RAWI.raw_info.white_level,
                                                         1); // dual iso check mode is on
        /* initialise LUTs */
        video->llrawproc->raw2ev = get_raw2ev(video->RAWI.raw_info.black_level);
        video->llrawproc->ev2raw = get_ev2raw(video->RAWI.raw_info.black_level);

        video->llrawproc->first_time = 0;
    }

    /* deflicker RAW data */
    if (video->llrawproc->deflicker_target)
    {
#ifndef STDOUT_SILENT
        printf("Per-frame exposure compensation: 'ON'\nDeflicker target: '%d'\n\n", video->llrawproc->deflicker_target);
#endif
        deflicker(video, raw_image_buff, raw_image_size);
    }

    /* subtruct dark frame */
    if (video->llrawproc->dark_frame)
    {
        printf("DF Processing...\n");
        subtract_darkframe(video, raw_image_buff, raw_image_size);
    }

    /* fix pattern noise */
    if (video->llrawproc->pattern_noise)
    {
#ifndef STDOUT_SILENT
        printf("Fixing pattern noise... ");
#endif
        fix_pattern_noise((int16_t *)raw_image_buff, video->RAWI.xRes, video->RAWI.yRes, video->RAWI.raw_info.white_level, 0);
#ifndef STDOUT_SILENT
        printf("Done\n\n");
#endif
    }

    /* fix focus pixels */
    if (video->llrawproc->focus_pixels && video->llrawproc->fpm_status < 3)
    {
        /* detect crop_rec mode */
        int crop_rec = (llrpDetectFocusDotFixMode(video) == 2) ? 1 : (video->llrawproc->focus_pixels == 2);
        /* detect if raw data is restricted to 8-12bit lossless mode */
        int restricted_lossless = ( (video->MLVI.videoClass & MLV_VIDEO_CLASS_FLAG_LJ92) && (video->RAWI.raw_info.white_level < 15000) );
        fix_focus_pixels(&video->llrawproc->focus_pixel_map,
                         &video->llrawproc->fpm_status,
                         raw_image_buff,
                         video->IDNT.cameraModel,
                         video->RAWI.xRes,
                         video->RAWI.yRes,
                         video->VIDF.panPosX,
                         video->VIDF.panPosY,
                         video->RAWI.raw_info.width,
                         video->RAWI.raw_info.height,
                         crop_rec,
                         (!restricted_lossless ? 0 : 5),
                         video->llrawproc->fpi_method,
                         video->llrawproc->is_dual_iso,
                         video->llrawproc->raw2ev,
                         video->llrawproc->ev2raw);
    }

    /* fix bad pixels */
    if (video->llrawproc->bad_pixels && video->llrawproc->bpm_status < 3)
    {
        fix_bad_pixels(&video->llrawproc->bad_pixel_map,
                       &video->llrawproc->bpm_status,
                       raw_image_buff,
                       video->IDNT.cameraModel,
                       video->RAWI.xRes,
                       video->RAWI.yRes,
                       video->VIDF.panPosX,
                       video->VIDF.panPosY,
                       video->RAWI.raw_info.width,
                       video->RAWI.raw_info.height,
                       video->RAWI.raw_info.black_level,
                       (video->llrawproc->bad_pixels == 2),
                       video->llrawproc->bps_method,
                       video->llrawproc->bpi_method,
                       video->llrawproc->is_dual_iso,
                       video->llrawproc->raw2ev,
                       video->llrawproc->ev2raw);
    }

    /* dual iso processing */
    if(video->llrawproc->is_dual_iso)
    {
        switch(video->llrawproc->dual_iso)
        {
            case 1: // full 20bit processing
            {
                struct raw_info raw_info = video->RAWI.raw_info;
                raw_info.width = video->RAWI.xRes;
                raw_info.height = video->RAWI.yRes;
                raw_info.pitch = video->RAWI.xRes;
                raw_info.active_area.x1 = 0;
                raw_info.active_area.y1 = 0;
                raw_info.active_area.x2 = raw_info.width;
                raw_info.active_area.y2 = raw_info.height;
                raw_info.black_level = video->RAWI.raw_info.black_level;
                raw_info.white_level = video->RAWI.raw_info.white_level;
                diso_get_full20bit(raw_info,
                                   raw_image_buff,
                                   video->llrawproc->diso_averaging,
                                   video->llrawproc->diso_alias_map,
                                   video->llrawproc->diso_frblending,
                                   video->llrawproc->chroma_smooth);
                break;
            }
            case 2: // preview mode
            {
                diso_get_preview(raw_image_buff,
                                 video->RAWI.xRes,
                                 video->RAWI.yRes,
                                 video->RAWI.raw_info.black_level,
                                 video->RAWI.raw_info.white_level,
                                 0); // dual iso check mode is off
                break;
            }
        }
    }


    /* do chroma smoothing */
    if (video->llrawproc->chroma_smooth && (video->llrawproc->dual_iso != 1 || !video->llrawproc->is_dual_iso)) // do not smooth 20bit dualiso raw
    {
#ifndef STDOUT_SILENT
            printf("\nUsing chroma smooth method: '%dx%d'\n\n", video->llrawproc->chroma_smooth, video->llrawproc->chroma_smooth);
#endif
        chroma_smooth(video->llrawproc->chroma_smooth,
                      raw_image_buff,
                      video->RAWI.xRes,
                      video->RAWI.yRes,
                      video->RAWI.raw_info.black_level,
                      video->RAWI.raw_info.white_level,
                      video->llrawproc->raw2ev,
                      video->llrawproc->ev2raw);
    }

    /* fix vertical stripes */
    if (video->llrawproc->vertical_stripes)
    {
        fix_vertical_stripes(&video->llrawproc->stripe_corrections,
                             raw_image_buff,
                             raw_image_size / 2,
                             (video->llrawproc->dual_iso == 1 && video->llrawproc->is_dual_iso) ? video->RAWI.raw_info.black_level * 4 : video->RAWI.raw_info.black_level,
                             (video->llrawproc->dual_iso == 1 && video->llrawproc->is_dual_iso) ? video->RAWI.raw_info.white_level * 4 : video->RAWI.raw_info.white_level,
                             video->RAWI.raw_info.frame_size,
                             video->RAWI.xRes,
                             video->RAWI.yRes,
                             video->llrawproc->vertical_stripes,
                             &video->llrawproc->compute_stripes);
    }
}

/* Detect focus dot fix mode according to RAWC block info (binning + skipping) and camera ID
   Return value 0 = off, 1 = On, 2 = CropRec */
int llrpDetectFocusDotFixMode(mlvObject_t * video)
{
    switch(video->IDNT.cameraModel)
    {
        case 0x80000331: // EOSM
        case 0x80000346: // 100D
        case 0x80000301: // 650D
        case 0x80000326: // 700D
            if(video->RAWC.blockType[0])
            {
                int sampling_x = video->RAWC.binning_x + video->RAWC.skipping_x;
                int sampling_y = video->RAWC.binning_y + video->RAWC.skipping_y;
                if( (video->RAWI.raw_info.height < 900) && !(sampling_y == 5 && sampling_x == 3) )
                {
                    return 2;
                }
            }
            return 1;

        default: // All other cameras
            return 0;
    }
}

/* LLRawProcObject variable handling */
int llrpGetFixRawMode(mlvObject_t * video)
{
    return video->llrawproc->fix_raw;
}

void llrpSetFixRawMode(mlvObject_t * video, int value)
{
    video->llrawproc->fix_raw = value;
}

int llrpGetVerticalStripeMode(mlvObject_t * video)
{
    return video->llrawproc->vertical_stripes;
}

void llrpSetVerticalStripeMode(mlvObject_t * video, int value)
{
    video->llrawproc->vertical_stripes = value;
}

void llrpComputeStripesOn(mlvObject_t * video)
{
    video->llrawproc->compute_stripes = 1;
}

int llrpGetFocusPixelMode(mlvObject_t * video)
{
    return video->llrawproc->focus_pixels;
}

void llrpSetFocusPixelMode(mlvObject_t * video, int value)
{
    video->llrawproc->focus_pixels = value;
}

int llrpGetFocusPixelInterpolationMethod(mlvObject_t * video)
{
    return video->llrawproc->fpi_method;
}

void llrpSetFocusPixelInterpolationMethod(mlvObject_t * video, int value)
{
    video->llrawproc->fpi_method = value;
}

int llrpGetBadPixelMode(mlvObject_t * video)
{
    return video->llrawproc->bad_pixels;
}

void llrpSetBadPixelMode(mlvObject_t * video, int value)
{
    video->llrawproc->bad_pixels = value;
}

int llrpGetBadPixelSearchMethod(mlvObject_t *video)
{
    return video->llrawproc->bps_method;
}

void llrpSetBadPixelSearchMethod(mlvObject_t * video, int value)
{
    video->llrawproc->bps_method = value;
}

int llrpGetBadPixelInterpolationMethod(mlvObject_t * video)
{
    return video->llrawproc->bpi_method;
}

void llrpSetBadPixelInterpolationMethod(mlvObject_t * video, int value)
{
    video->llrawproc->bpi_method = value;
}

int llrpGetChromaSmoothMode(mlvObject_t * video)
{
    return video->llrawproc->chroma_smooth;
}

void llrpSetChromaSmoothMode(mlvObject_t * video, int value)
{
    video->llrawproc->chroma_smooth = value;
}

int llrpGetPatternNoiseMode(mlvObject_t * video)
{
    return video->llrawproc->pattern_noise;
}

void llrpSetPatternNoiseMode(mlvObject_t * video, int value)
{
    video->llrawproc->pattern_noise = value;
}

int llrpGetDeflickerTarget(mlvObject_t * video)
{
    return video->llrawproc->deflicker_target;
}

void llrpSetDeflickerTarget(mlvObject_t * video, int value)
{
    video->llrawproc->deflicker_target = value;
}

int llrpGetDualIsoMode(mlvObject_t * video)
{
    return video->llrawproc->dual_iso;
}

void llrpSetDualIsoMode(mlvObject_t * video, int value)
{
    video->llrawproc->dual_iso = value;
}

int llrpGetDualIsoInterpolationMethod(mlvObject_t * video)
{
    return video->llrawproc->diso_averaging;
}

void llrpSetDualIsoInterpolationMethod(mlvObject_t * video, int value)
{
    video->llrawproc->diso_averaging = value;
}

int llrpGetDualIsoAliasMapMode(mlvObject_t * video)
{
    return video->llrawproc->diso_alias_map;
}

void llrpSetDualIsoAliasMapMode(mlvObject_t * video, int value)
{
    video->llrawproc->diso_alias_map = value;
}

int llrpGetDualIsoFullResBlendingMode(mlvObject_t * video)
{
    return video->llrawproc->diso_frblending;
}

void llrpSetDualIsoFullResBlendingMode(mlvObject_t * video, int value)
{
    video->llrawproc->diso_frblending = value;
}

int llrpIsDualIso(mlvObject_t * video)
{
    return video->llrawproc->is_dual_iso;
}

int llrpHQDualIso(mlvObject_t * video)
{
    return ((video->llrawproc->dual_iso == 1) && video->llrawproc->is_dual_iso);
}

void llrpResetFpmStatus(mlvObject_t * video)
{
    reset_fpm_status(&(video->llrawproc->focus_pixel_map), &(video->llrawproc->fpm_status));
}

void llrpResetBpmStatus(mlvObject_t * video)
{
    reset_bpm_status(&(video->llrawproc->bad_pixel_map), &(video->llrawproc->bpm_status));
}

int llrpGetDarkFrameMode(mlvObject_t * video)
{
    return video->llrawproc->dark_frame;
}

void llrpSetDarkFrameMode(mlvObject_t * video, int value)
{
    video->llrawproc->dark_frame = value;
}

int llrpGetDarkFrameExtStatus(mlvObject_t * video)
{
    if(video->llrawproc->dark_frame_filename) return 1;
    return 0;
}

int llrpGetDarkFrameIntStatus(mlvObject_t * video)
{
    if(video->DARK.blockType[0]) return 1;
    return 0;
}

void llrpInitDarkFrameExtFileName(mlvObject_t * video, char * df_filename)
{
    size_t df_filename_size = strlen(df_filename);
    video->llrawproc->dark_frame_filename = calloc(df_filename_size + 1, 1);
    memcpy(video->llrawproc->dark_frame_filename, df_filename, df_filename_size);
}

void llrpFreeDarkFrameExtFileName(mlvObject_t * video)
{
    if(video->llrawproc->dark_frame_filename) free(video->llrawproc->dark_frame_filename);
    video->llrawproc->dark_frame_filename = NULL;
}

int llrpInitDarkFrame(mlvObject_t * video, int df_mode)
{
    switch(df_mode)
    {
        case DF_EXT:
            if(llrpGetDarkFrameExtStatus(video)) return loadDarkFrameExt(video);
            break;
        case DF_INT:
            //if(llrpGetDarkFrameIntStatus(video)) return loadDarkFrameInt(video);
            break;
        default:
            llrpFreeDarkFrame(video);
            break;
    }
    return 0;
}

void llrpFreeDarkFrame(mlvObject_t * video)
{
    if(video->llrawproc->dark_frame_data)
    {
        free(video->llrawproc->dark_frame_data);
        video->llrawproc->dark_frame_data = NULL;
        video->llrawproc->dark_frame_size = 0;
    }
}
