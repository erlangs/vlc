/*****************************************************************************
 * h264.c: h264/avc video packetizer
 *****************************************************************************
 * Copyright (C) 2001, 2002, 2006 VLC authors and VideoLAN
 * $Id$
 *
 * Authors: Laurent Aimar <fenrir@via.ecp.fr>
 *          Eric Petit <titer@videolan.org>
 *          Gildas Bazin <gbazin@videolan.org>
 *          Derk-Jan Hartman <hartman at videolan dot org>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_sout.h>
#include <vlc_codec.h>
#include <vlc_block.h>

#include <vlc_block_helper.h>
#include <vlc_bits.h>
#include "h264_nal.h"
#include "hxxx_nal.h"
#include "hxxx_sei.h"
#include "hxxx_common.h"
#include "packetizer_helper.h"
#include "startcode_helper.h"

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
static int  Open ( vlc_object_t * );
static void Close( vlc_object_t * );

vlc_module_begin ()
    set_category( CAT_SOUT )
    set_subcategory( SUBCAT_SOUT_PACKETIZER )
    set_description( N_("H.264 video packetizer") )
    set_capability( "packetizer", 50 )
    set_callbacks( Open, Close )
vlc_module_end ()


/****************************************************************************
 * Local prototypes
 ****************************************************************************/
typedef struct
{
    int i_nal_type;
    int i_nal_ref_idc;

    int i_frame_type;
    int i_pic_parameter_set_id;
    int i_frame_num;

    int i_field_pic_flag;
    int i_bottom_field_flag;

    int i_idr_pic_id;

    int i_pic_order_cnt_type;
    int i_pic_order_cnt_lsb;
    int i_delta_pic_order_cnt_bottom;

    int i_delta_pic_order_cnt0;
    int i_delta_pic_order_cnt1;
} slice_t;

struct decoder_sys_t
{
    /* */
    packetizer_t packetizer;

    /* */
    bool    b_slice;
    block_t *p_frame;
    block_t **pp_frame_last;
    block_t *p_sei;
    block_t **pp_sei_last;
    /* a new sps/pps can be transmitted outside of iframes */
    bool    b_new_sps;
    bool    b_new_pps;

    bool   b_header;

    struct
    {
        block_t *p_block;
        h264_sequence_parameter_set_t *p_sps;
    } sps[H264_SPS_ID_MAX + 1];
    struct
    {
        block_t *p_block;
        h264_picture_parameter_set_t *p_pps;
    } pps[H264_PPS_ID_MAX + 1];
    const h264_sequence_parameter_set_t *p_active_sps;
    const h264_picture_parameter_set_t *p_active_pps;

    int    i_recovery_frames;  /* -1 = no recovery */

    /* avcC data */
    uint8_t i_avcC_length_size;

    /* From SEI for current frame */
    uint8_t i_pic_struct;
    uint8_t i_dpb_output_delay;

    /* Useful values of the Slice Header */
    slice_t slice;

    /* */
    bool b_even_frame;
    bool b_discontinuity;

    mtime_t i_frame_pts;
    mtime_t i_frame_dts;
    mtime_t i_prev_pts;

    date_t dts;

    /* */
    cc_storage_t *p_ccs;
};

#define BLOCK_FLAG_PRIVATE_AUD (1 << BLOCK_FLAG_PRIVATE_SHIFT)

static block_t *Packetize( decoder_t *, block_t ** );
static block_t *PacketizeAVC1( decoder_t *, block_t ** );
static block_t *GetCc( decoder_t *p_dec, bool pb_present[4] );
static void PacketizeFlush( decoder_t * );

static void PacketizeReset( void *p_private, bool b_broken );
static block_t *PacketizeParse( void *p_private, bool *pb_ts_used, block_t * );
static int PacketizeValidate( void *p_private, block_t * );

static block_t *ParseNALBlock( decoder_t *, bool *pb_ts_used, block_t * );

static block_t *OutputPicture( decoder_t *p_dec );
static void PutSPS( decoder_t *p_dec, block_t *p_frag );
static void PutPPS( decoder_t *p_dec, block_t *p_frag );
static bool ParseSliceHeader( decoder_t *p_dec, int i_nal_ref_idc, int i_nal_type,
                              const block_t *p_frag, slice_t *p_slice );
static bool ParseSeiCallback( const hxxx_sei_data_t *, void * );


static const uint8_t p_h264_startcode[3] = { 0x00, 0x00, 0x01 };

/*****************************************************************************
 * Helpers
 *****************************************************************************/

static void StoreSPS( decoder_sys_t *p_sys, uint8_t i_id,
                      block_t *p_block, h264_sequence_parameter_set_t *p_sps )
{
    if( p_sys->sps[i_id].p_block )
        block_Release( p_sys->sps[i_id].p_block );
    if( p_sys->sps[i_id].p_sps )
        h264_release_sps( p_sys->sps[i_id].p_sps );
    if( p_sys->sps[i_id].p_sps == p_sys->p_active_sps )
        p_sys->p_active_sps = NULL;
    p_sys->sps[i_id].p_block = p_block;
    p_sys->sps[i_id].p_sps = p_sps;
}

static void StorePPS( decoder_sys_t *p_sys, uint8_t i_id,
                      block_t *p_block, h264_picture_parameter_set_t *p_pps )
{
    if( p_sys->pps[i_id].p_block )
        block_Release( p_sys->pps[i_id].p_block );
    if( p_sys->pps[i_id].p_pps )
        h264_release_pps( p_sys->pps[i_id].p_pps );
    if( p_sys->pps[i_id].p_pps == p_sys->p_active_pps )
        p_sys->p_active_pps = NULL;
    p_sys->pps[i_id].p_block = p_block;
    p_sys->pps[i_id].p_pps = p_pps;
}

static void ActivateSets( decoder_t *p_dec, const h264_sequence_parameter_set_t *p_sps,
                                            const h264_picture_parameter_set_t *p_pps )
{
    decoder_sys_t *p_sys = p_dec->p_sys;

    p_sys->p_active_pps = p_pps;
    p_sys->p_active_sps = p_sps;

    if( p_sps )
    {
        p_dec->fmt_out.i_profile = p_sps->i_profile;
        p_dec->fmt_out.i_level = p_sps->i_level;

        (void) h264_get_picture_size( p_sps, &p_dec->fmt_out.video.i_width,
                                      &p_dec->fmt_out.video.i_height,
                                      &p_dec->fmt_out.video.i_visible_width,
                                      &p_dec->fmt_out.video.i_visible_height );

        if( p_sps->vui.i_sar_num != 0 && p_sps->vui.i_sar_den != 0 )
        {
            p_dec->fmt_out.video.i_sar_num = p_sps->vui.i_sar_num;
            p_dec->fmt_out.video.i_sar_den = p_sps->vui.i_sar_den;
        }

        if( p_sps->vui.b_valid )
        {
            if( p_sps->vui.b_fixed_frame_rate && !p_dec->fmt_out.video.i_frame_rate_base )
            {
                p_dec->fmt_out.video.i_frame_rate_base = p_sps->vui.i_num_units_in_tick;
                p_dec->fmt_out.video.i_frame_rate = p_sps->vui.i_time_scale >> 1 /* num_clock_ts == 2 */;
                if( p_sps->vui.i_num_units_in_tick > 0 )
                    date_Change( &p_sys->dts, p_sps->vui.i_time_scale, p_sps->vui.i_num_units_in_tick );
            }
            if( p_dec->fmt_out.video.primaries == COLOR_PRIMARIES_UNDEF )
            {
                p_dec->fmt_out.video.primaries =
                        hxxx_colour_primaries_to_vlc( p_sps->vui.colour.i_colour_primaries );
                p_dec->fmt_out.video.transfer =
                        hxxx_transfer_characteristics_to_vlc( p_sps->vui.colour.i_transfer_characteristics );
                p_dec->fmt_out.video.space =
                        hxxx_matrix_coeffs_to_vlc( p_sps->vui.colour.i_matrix_coefficients );
                p_dec->fmt_out.video.b_color_range_full = p_sps->vui.colour.b_full_range;
            }
        }
    }
}

static void SliceInit( slice_t *p_slice )
{
    p_slice->i_nal_type = -1;
    p_slice->i_nal_ref_idc = -1;
    p_slice->i_idr_pic_id = -1;
    p_slice->i_frame_num = -1;
    p_slice->i_frame_type = 0;
    p_slice->i_pic_parameter_set_id = -1;
    p_slice->i_field_pic_flag = 0;
    p_slice->i_bottom_field_flag = -1;
    p_slice->i_pic_order_cnt_type = -1;
    p_slice->i_pic_order_cnt_lsb = -1;
    p_slice->i_delta_pic_order_cnt_bottom = -1;
    p_slice->i_delta_pic_order_cnt0 = 0;
    p_slice->i_delta_pic_order_cnt1 = 0;
}

static bool IsFirstVCLNALUnit( const slice_t *p_prev, const slice_t *p_cur )
{
    /* Detection of the first VCL NAL unit of a primary coded picture
     * (cf. 7.4.1.2.4) */
    if( p_cur->i_frame_num != p_prev->i_frame_num ||
        p_cur->i_pic_parameter_set_id != p_prev->i_pic_parameter_set_id ||
        p_cur->i_field_pic_flag != p_prev->i_field_pic_flag ||
       !p_cur->i_nal_ref_idc != !p_prev->i_nal_ref_idc )
        return true;
    if( (p_cur->i_bottom_field_flag != -1) &&
        (p_prev->i_bottom_field_flag != -1) &&
        (p_cur->i_bottom_field_flag != p_prev->i_bottom_field_flag) )
        return true;
    if( p_cur->i_pic_order_cnt_type == 0 &&
       ( p_cur->i_pic_order_cnt_lsb != p_prev->i_pic_order_cnt_lsb ||
         p_cur->i_delta_pic_order_cnt_bottom != p_prev->i_delta_pic_order_cnt_bottom ) )
        return true;
    else if( p_cur->i_pic_order_cnt_type == 1 &&
           ( p_cur->i_delta_pic_order_cnt0 != p_prev->i_delta_pic_order_cnt0 ||
             p_cur->i_delta_pic_order_cnt1 != p_prev->i_delta_pic_order_cnt1 ) )
        return true;
    if( ( p_cur->i_nal_type == H264_NAL_SLICE_IDR || p_prev->i_nal_type == H264_NAL_SLICE_IDR ) &&
        ( p_cur->i_nal_type != p_prev->i_nal_type || p_cur->i_idr_pic_id != p_prev->i_idr_pic_id ) )
        return true;

    return false;
}

/*****************************************************************************
 * Open: probe the packetizer and return score
 * When opening after demux, the packetizer is only loaded AFTER the decoder
 * That means that what you set in fmt_out is ignored by the decoder in this special case
 *****************************************************************************/
static int Open( vlc_object_t *p_this )
{
    decoder_t     *p_dec = (decoder_t*)p_this;
    decoder_sys_t *p_sys;
    int i;

    const bool b_avc = (p_dec->fmt_in.i_original_fourcc == VLC_FOURCC( 'a', 'v', 'c', '1' ));

    if( p_dec->fmt_in.i_codec != VLC_CODEC_H264 )
        return VLC_EGENERIC;
    if( b_avc && p_dec->fmt_in.i_extra < 7 )
        return VLC_EGENERIC;

    /* Allocate the memory needed to store the decoder's structure */
    if( ( p_dec->p_sys = p_sys = malloc( sizeof(decoder_sys_t) ) ) == NULL )
    {
        return VLC_ENOMEM;
    }

    p_sys->p_ccs = cc_storage_new();
    if( unlikely(!p_sys->p_ccs) )
    {
        free( p_dec->p_sys );
        return VLC_ENOMEM;
    }

    packetizer_Init( &p_sys->packetizer,
                     p_h264_startcode, sizeof(p_h264_startcode), startcode_FindAnnexB,
                     p_h264_startcode, 1, 5,
                     PacketizeReset, PacketizeParse, PacketizeValidate, p_dec );

    p_sys->b_slice = false;
    p_sys->p_frame = NULL;
    p_sys->pp_frame_last = &p_sys->p_frame;
    p_sys->p_sei = NULL;
    p_sys->pp_sei_last = &p_sys->p_sei;
    p_sys->b_new_sps = false;
    p_sys->b_new_pps = false;

    p_sys->b_header= false;
    for( i = 0; i <= H264_SPS_ID_MAX; i++ )
    {
        p_sys->sps[i].p_sps = NULL;
        p_sys->sps[i].p_block = NULL;
    }
    p_sys->p_active_sps = NULL;
    for( i = 0; i <= H264_PPS_ID_MAX; i++ )
    {
        p_sys->pps[i].p_pps = NULL;
        p_sys->pps[i].p_block = NULL;
    }
    p_sys->p_active_pps = NULL;
    p_sys->i_recovery_frames = -1;

    SliceInit( &p_sys->slice );

    p_sys->b_even_frame = false;
    p_sys->b_discontinuity = false;
    p_sys->i_frame_dts = VLC_TS_INVALID;
    p_sys->i_frame_pts = VLC_TS_INVALID;
    p_sys->i_prev_pts = VLC_TS_INVALID;
    p_sys->i_dpb_output_delay = 0;

    date_Init( &p_sys->dts, 1, 1 );
    date_Set( &p_sys->dts, VLC_TS_INVALID );

    /* Setup properties */
    es_format_Copy( &p_dec->fmt_out, &p_dec->fmt_in );
    p_dec->fmt_out.i_codec = VLC_CODEC_H264;
    p_dec->fmt_out.b_packetized = true;

    if( p_dec->fmt_in.video.i_frame_rate_base > 0 )
    {
        date_Change( &p_sys->dts, p_dec->fmt_in.video.i_frame_rate * 2,
                                  p_dec->fmt_in.video.i_frame_rate_base );
    }

    if( b_avc )
    {
        /* This type of stream is produced by mp4 and matroska
         * when we want to store it in another streamformat, you need to convert
         * The fmt_in.p_extra should ALWAYS contain the avcC
         * The fmt_out.p_extra should contain all the SPS and PPS with 4 byte startcodes */
        if( h264_isavcC( p_dec->fmt_in.p_extra, p_dec->fmt_in.i_extra ) )
        {
            free( p_dec->fmt_out.p_extra );
            size_t i_size;
            p_dec->fmt_out.p_extra = h264_avcC_to_AnnexB_NAL( p_dec->fmt_in.p_extra,
                                                              p_dec->fmt_in.i_extra,
                                                             &i_size,
                                                             &p_sys->i_avcC_length_size );
            p_dec->fmt_out.i_extra = i_size;
            p_sys->b_header = !!p_dec->fmt_out.i_extra;

            if(!p_dec->fmt_out.p_extra)
            {
                msg_Err( p_dec, "Invalid AVC extradata");
                Close( p_this );
                return VLC_EGENERIC;
            }
        }
        else
        {
            msg_Err( p_dec, "Invalid or missing AVC extradata");
            Close( p_this );
            return VLC_EGENERIC;
        }

        /* Set callback */
        p_dec->pf_packetize = PacketizeAVC1;
    }
    else
    {
        /* This type of stream contains data with 3 of 4 byte startcodes
         * The fmt_in.p_extra MAY contain SPS/PPS with 4 byte startcodes
         * The fmt_out.p_extra should be the same */

        /* Set callback */
        p_dec->pf_packetize = Packetize;
    }

    /* */
    if( p_dec->fmt_out.i_extra > 0 )
    {
        packetizer_Header( &p_sys->packetizer,
                           p_dec->fmt_out.p_extra, p_dec->fmt_out.i_extra );
    }

    if( b_avc )
    {
        /* FIXME: that's not correct for every AVC */
        if( !p_sys->b_new_pps || !p_sys->b_new_sps )
        {
            msg_Err( p_dec, "Invalid or missing SPS %d or PPS %d in AVC extradata",
                     p_sys->b_new_sps, p_sys->b_new_pps );
            Close( p_this );
            return VLC_EGENERIC;
        }

        msg_Dbg( p_dec, "Packetizer fed with AVC, nal length size=%d",
                         p_sys->i_avcC_length_size );
    }

    /* CC are the same for H264/AVC in T35 sections (ETSI TS 101 154)  */
    p_dec->pf_get_cc = GetCc;
    p_dec->pf_flush = PacketizeFlush;

    return VLC_SUCCESS;
}

/*****************************************************************************
 * Close: clean up the packetizer
 *****************************************************************************/
static void Close( vlc_object_t *p_this )
{
    decoder_t *p_dec = (decoder_t*)p_this;
    decoder_sys_t *p_sys = p_dec->p_sys;
    int i;

    block_ChainRelease( p_sys->p_frame );
    block_ChainRelease( p_sys->p_sei );
    for( i = 0; i <= H264_SPS_ID_MAX; i++ )
        StoreSPS( p_sys, i, NULL, NULL );
    for( i = 0; i <= H264_PPS_ID_MAX; i++ )
        StorePPS( p_sys, i, NULL, NULL );

    packetizer_Clean( &p_sys->packetizer );

    cc_storage_delete( p_sys->p_ccs );

    free( p_sys );
}

static void PacketizeFlush( decoder_t *p_dec )
{
    decoder_sys_t *p_sys = p_dec->p_sys;

    packetizer_Flush( &p_sys->packetizer );
}

/****************************************************************************
 * Packetize: the whole thing
 * Search for the startcodes 3 or more bytes
 * Feed ParseNALBlock ALWAYS with 4 byte startcode prepended NALs
 ****************************************************************************/
static block_t *Packetize( decoder_t *p_dec, block_t **pp_block )
{
    decoder_sys_t *p_sys = p_dec->p_sys;

    return packetizer_Packetize( &p_sys->packetizer, pp_block );
}

/****************************************************************************
 * PacketizeAVC1: Takes VCL blocks of data and creates annexe B type NAL stream
 * Will always use 4 byte 0 0 0 1 startcodes
 * Will prepend a SPS and PPS before each keyframe
 ****************************************************************************/
static block_t *PacketizeAVC1( decoder_t *p_dec, block_t **pp_block )
{
    decoder_sys_t *p_sys = p_dec->p_sys;

    return PacketizeXXC1( p_dec, p_sys->i_avcC_length_size,
                          pp_block, ParseNALBlock );
}

/*****************************************************************************
 * GetCc:
 *****************************************************************************/
static block_t *GetCc( decoder_t *p_dec, bool pb_present[4] )
{
    return cc_storage_get_current( p_dec->p_sys->p_ccs, pb_present );
}

/****************************************************************************
 * Helpers
 ****************************************************************************/
static void PacketizeReset( void *p_private, bool b_broken )
{
    decoder_t *p_dec = p_private;
    decoder_sys_t *p_sys = p_dec->p_sys;

    if( b_broken || !p_sys->b_slice )
    {
        block_ChainRelease( p_sys->p_frame );
        block_ChainRelease( p_sys->p_sei );
        p_sys->p_frame = NULL;
        p_sys->pp_frame_last = &p_sys->p_frame;
        p_sys->p_sei = NULL;
        p_sys->pp_sei_last = &p_sys->p_sei;
        p_sys->b_new_sps = false;
        p_sys->b_new_pps = false;
        p_sys->p_active_pps = NULL;
        p_sys->p_active_sps = NULL;
        p_sys->i_frame_pts = VLC_TS_INVALID;
        p_sys->i_frame_dts = VLC_TS_INVALID;
        p_sys->i_prev_pts = VLC_TS_INVALID;
        p_sys->b_even_frame = false;
        p_sys->slice.i_frame_type = 0;
        p_sys->b_slice = false;
        /* From SEI */
        p_sys->i_dpb_output_delay = 0;
        p_sys->i_pic_struct = 0;
    }
    p_sys->b_discontinuity = true;
    date_Set( &p_sys->dts, VLC_TS_INVALID );
}
static block_t *PacketizeParse( void *p_private, bool *pb_ts_used, block_t *p_block )
{
    decoder_t *p_dec = p_private;

    /* Remove trailing 0 bytes */
    while( p_block->i_buffer > 5 && p_block->p_buffer[p_block->i_buffer-1] == 0x00 )
        p_block->i_buffer--;

    return ParseNALBlock( p_dec, pb_ts_used, p_block );
}
static int PacketizeValidate( void *p_private, block_t *p_au )
{
    VLC_UNUSED(p_private);
    VLC_UNUSED(p_au);
    return VLC_SUCCESS;
}

/*****************************************************************************
 * ParseNALBlock: parses annexB type NALs
 * All p_frag blocks are required to start with 0 0 0 1 4-byte startcode
 *****************************************************************************/
static block_t *ParseNALBlock( decoder_t *p_dec, bool *pb_ts_used, block_t *p_frag )
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    block_t *p_pic = NULL;
    bool b_new_picture = false;

    const int i_nal_ref_idc = (p_frag->p_buffer[4] >> 5)&0x03;
    const int i_nal_type = p_frag->p_buffer[4]&0x1f;
    const mtime_t i_frag_dts = p_frag->i_dts;
    const mtime_t i_frag_pts = p_frag->i_pts;

    if( p_sys->b_slice && (!p_sys->p_active_pps || !p_sys->p_active_sps) )
    {
        block_ChainRelease( p_sys->p_frame );
        block_ChainRelease( p_sys->p_sei );
        msg_Warn( p_dec, "waiting for SPS/PPS" );

        /* Reset context */
        p_sys->slice.i_frame_type = 0;
        p_sys->p_frame = NULL;
        p_sys->pp_frame_last = &p_sys->p_frame;
        p_sys->p_sei = NULL;
        p_sys->pp_sei_last = &p_sys->p_sei;
        p_sys->b_slice = false;
        /* From SEI */
        p_sys->i_dpb_output_delay = 0;
        p_sys->i_pic_struct = 0;
        cc_storage_reset( p_sys->p_ccs );
    }

    if( i_nal_type >= H264_NAL_SLICE && i_nal_type <= H264_NAL_SLICE_IDR )
    {
        slice_t newslice;

        if( ParseSliceHeader( p_dec, i_nal_ref_idc, i_nal_type, p_frag, &newslice ) )
        {
            /* Only IDR carries the id, to be propagated */
            if( newslice.i_idr_pic_id == -1 )
                newslice.i_idr_pic_id = p_sys->slice.i_idr_pic_id;

            b_new_picture = IsFirstVCLNALUnit( &p_sys->slice, &newslice );
            if( b_new_picture )
            {
                /* Parse SEI for that frame now we should have matched SPS/PPS */
                for( block_t *p_sei = p_sys->p_sei; p_sei; p_sei = p_sei->p_next )
                {
                    HxxxParse_AnnexB_SEI( p_sei->p_buffer, p_sei->i_buffer,
                                          1 /* nal header */, ParseSeiCallback, p_dec );
                }

                if( p_sys->b_slice )
                    p_pic = OutputPicture( p_dec );
            }

            /* */
            p_sys->slice = newslice;
        }
        else
        {
            p_sys->p_active_pps = NULL;
            /* Fragment will be discarded later on */
        }
        p_sys->b_slice = true;
    }
    else if( i_nal_type == H264_NAL_SPS )
    {
        if( p_sys->b_slice )
            p_pic = OutputPicture( p_dec );

        PutSPS( p_dec, p_frag );
        p_sys->b_new_sps = true;

        /* Do not append the SPS because we will insert it on keyframes */
        p_frag = NULL;
    }
    else if( i_nal_type == H264_NAL_PPS )
    {
        if( p_sys->b_slice )
            p_pic = OutputPicture( p_dec );

        PutPPS( p_dec, p_frag );
        p_sys->b_new_pps = true;

        /* Do not append the PPS because we will insert it on keyframes */
        p_frag = NULL;
    }
    else if( i_nal_type == H264_NAL_SEI )
    {
        if( p_sys->b_slice )
            p_pic = OutputPicture( p_dec );

        block_ChainLastAppend( &p_sys->pp_sei_last, p_frag );
        p_frag = NULL;
    }
    else if( i_nal_type == H264_NAL_AU_DELIMITER ||
             ( i_nal_type >= H264_NAL_PREFIX && i_nal_type <= H264_NAL_RESERVED_18 ) )
    {
        if( p_sys->b_slice )
            p_pic = OutputPicture( p_dec );

        if( i_nal_type == H264_NAL_AU_DELIMITER )
        {
            if( p_sys->p_frame && (p_sys->p_frame->i_flags & BLOCK_FLAG_PRIVATE_AUD) )
            {
                block_Release( p_frag );
                p_frag = NULL;
            }
            else
            {
                p_frag->i_flags |= BLOCK_FLAG_PRIVATE_AUD;
            }
        }
    }

    /* Append the block */
    if( p_frag )
        block_ChainLastAppend( &p_sys->pp_frame_last, p_frag );

    *pb_ts_used = false;
    if( p_sys->i_frame_dts <= VLC_TS_INVALID &&
        p_sys->i_frame_pts <= VLC_TS_INVALID && b_new_picture )
    {
        p_sys->i_frame_dts = i_frag_dts;
        p_sys->i_frame_pts = i_frag_pts;
        *pb_ts_used = true;
        if( i_frag_dts > VLC_TS_INVALID )
            date_Set( &p_sys->dts, i_frag_dts );
    }
    return p_pic;
}

static block_t *OutputPicture( decoder_t *p_dec )
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    block_t *p_pic = NULL;
    block_t **pp_pic_last = &p_pic;

    if ( !p_sys->b_header && p_sys->i_recovery_frames != -1 )
    {
        if( p_sys->i_recovery_frames == 0 )
        {
            msg_Dbg( p_dec, "Recovery from SEI recovery point complete" );
            p_sys->b_header = true;
        }
        --p_sys->i_recovery_frames;
    }

    if( unlikely(!p_sys->p_frame) )
    {
        assert( p_sys->p_frame );
        return NULL;
    }

    if( !p_sys->b_header && p_sys->i_recovery_frames == -1 &&
         p_sys->slice.i_frame_type != BLOCK_FLAG_TYPE_I)
        return NULL;

    /* Bind matched/referred PPS and SPS */
    const h264_picture_parameter_set_t *p_pps = p_sys->p_active_pps;
    const h264_sequence_parameter_set_t *p_sps = p_sys->p_active_sps;
    if( !p_pps || !p_sps )
        return NULL;

    /* Gather PPS/SPS if required */
    block_t *p_xpsnal = NULL;
    block_t **pp_xpsnal_tail = &p_xpsnal;
    const bool b_sps_pps_i = p_sys->slice.i_frame_type == BLOCK_FLAG_TYPE_I &&
                             p_sys->p_active_pps &&
                             p_sys->p_active_sps;
    if( b_sps_pps_i || p_sys->b_new_sps || p_sys->b_new_pps )
    {
        for( int i = 0; i <= H264_SPS_ID_MAX && (b_sps_pps_i || p_sys->b_new_sps); i++ )
        {
            if( p_sys->sps[i].p_block )
                block_ChainLastAppend( &pp_xpsnal_tail, block_Duplicate( p_sys->sps[i].p_block ) );
        }
        for( int i = 0; i < H264_PPS_ID_MAX && (b_sps_pps_i || p_sys->b_new_pps); i++ )
        {
            if( p_sys->pps[i].p_block )
                block_ChainLastAppend( &pp_xpsnal_tail, block_Duplicate( p_sys->pps[i].p_block ) );
        }
        if( b_sps_pps_i && p_xpsnal )
            p_sys->b_header = true;
    }

    /* Now rebuild NAL Sequence, inserting PPS/SPS if any */
    if( p_sys->p_frame->i_flags & BLOCK_FLAG_PRIVATE_AUD )
    {
        block_t *p_au = p_sys->p_frame;
        p_sys->p_frame = p_au->p_next;
        p_au->p_next = NULL;
        p_au->i_flags &= ~BLOCK_FLAG_PRIVATE_AUD;
        block_ChainLastAppend( &pp_pic_last, p_au );
    }

    if( p_xpsnal )
        block_ChainLastAppend( &pp_pic_last, p_xpsnal );

    if( p_sys->p_sei )
        block_ChainLastAppend( &pp_pic_last, p_sys->p_sei );

    assert( p_sys->p_frame );
    if( p_sys->p_frame )
        block_ChainLastAppend( &pp_pic_last, p_sys->p_frame );

    /* Reset chains, now empty */
    p_sys->p_frame = NULL;
    p_sys->pp_frame_last = &p_sys->p_frame;
    p_sys->p_sei = NULL;
    p_sys->pp_sei_last = &p_sys->p_sei;

    p_pic = block_ChainGather( p_pic );

    if( !p_pic )
        return NULL;

    unsigned i_num_clock_ts = 2;
    if( p_sps->frame_mbs_only_flag == 0 )
    {
        if( p_sps->vui.b_pic_struct_present_flag && p_sys->i_pic_struct < 9 )
        {
            const uint8_t rgi_numclock[9] = { 1, 1, 1, 2, 2, 3, 3, 2, 3 };
            i_num_clock_ts = rgi_numclock[ p_sys->i_pic_struct ];
        }
        else if( p_sys->slice.i_field_pic_flag ) /* See D-1 and E-6 */
        {
            i_num_clock_ts = 1;
        }
    }

    if( p_sps->vui.i_time_scale && p_pic->i_length == 0 )
    {
        p_pic->i_length = CLOCK_FREQ * i_num_clock_ts *
                          p_sps->vui.i_num_units_in_tick / p_sps->vui.i_time_scale;
    }

    mtime_t i_field_pts_diff = -1;
    if( p_sps->frame_mbs_only_flag == 0 && p_sps->vui.b_pic_struct_present_flag )
    {
        switch( p_sys->i_pic_struct )
        {
        /* Top and Bottom field slices */
        case 1:
        case 2:
            if( !p_sys->b_even_frame )
            {
                p_pic->i_flags |= (p_sys->i_pic_struct == 1) ? BLOCK_FLAG_TOP_FIELD_FIRST
                                                             : BLOCK_FLAG_BOTTOM_FIELD_FIRST;
            }
            else if( p_pic->i_pts <= VLC_TS_INVALID &&
                     date_Get( &p_sys->dts ) > VLC_TS_INVALID && p_pic->i_length )
            {
                /* interpolate from even frame */
                i_field_pts_diff = p_pic->i_length;
            }

            p_sys->b_even_frame = !p_sys->b_even_frame;
            break;
        /* Each of the following slices contains multiple fields */
        case 3:
            p_pic->i_flags |= BLOCK_FLAG_TOP_FIELD_FIRST;
            p_sys->b_even_frame = false;
            break;
        case 4:
            p_pic->i_flags |= BLOCK_FLAG_BOTTOM_FIELD_FIRST;
            p_sys->b_even_frame = false;
            break;
        case 5:
            p_pic->i_flags |= BLOCK_FLAG_TOP_FIELD_FIRST;
            break;
        case 6:
            p_pic->i_flags |= BLOCK_FLAG_BOTTOM_FIELD_FIRST;
            break;
        default:
            p_sys->b_even_frame = false;
            break;
        }
    }

    /* set dts/pts to current block timestamps */
    p_pic->i_dts = p_sys->i_frame_dts;
    p_pic->i_pts = p_sys->i_frame_pts;

    /* Fixup missing timestamps after split (multiple AU/block)*/
    if( p_pic->i_dts <= VLC_TS_INVALID )
        p_pic->i_dts = date_Get( &p_sys->dts );

    /* PTS Fixup, interlaced fields (multiple AU/block) */
    if( p_pic->i_pts <= VLC_TS_INVALID && p_sps->vui.i_time_scale &&
        p_sps->vui.b_valid && p_sps->vui.b_hrd_parameters_present_flag )
    {
        mtime_t i_pts_delay = CLOCK_FREQ * p_sys->i_dpb_output_delay *
                              p_sps->vui.i_num_units_in_tick / p_sps->vui.i_time_scale;
        p_pic->i_pts = p_pic->i_dts + i_pts_delay;
        if( i_field_pts_diff >= 0 )
            p_pic->i_pts += i_field_pts_diff;
    }

    if( !p_sys->b_discontinuity )
    {
        /* save for next pic fixups */
        date_Increment( &p_sys->dts, i_num_clock_ts );
        p_sys->i_prev_pts = p_pic->i_pts;
    }
    else
    {
        p_sys->b_discontinuity = false;
        p_sys->i_prev_pts = VLC_TS_INVALID;
        date_Set( &p_sys->dts, VLC_TS_INVALID );
        if( p_pic )
            p_pic->i_flags |= BLOCK_FLAG_DISCONTINUITY;
    }

    p_pic->i_flags |= p_sys->slice.i_frame_type;
    p_pic->i_flags &= ~BLOCK_FLAG_PRIVATE_AUD;
    if( !p_sys->b_header )
        p_pic->i_flags |= BLOCK_FLAG_PREROLL;

    /* reset after output */
    p_sys->i_frame_dts = VLC_TS_INVALID;
    p_sys->i_frame_pts = VLC_TS_INVALID;
    p_sys->i_dpb_output_delay = 0;
    p_sys->i_pic_struct = 0;
    p_sys->slice.i_frame_type = 0;
    p_sys->p_sei = NULL;
    p_sys->pp_sei_last = &p_sys->p_sei;
    p_sys->b_new_sps = false;
    p_sys->b_new_pps = false;
    p_sys->b_slice = false;

    /* CC */
    cc_storage_commit( p_sys->p_ccs, p_pic );

    return p_pic;
}

static void PutSPS( decoder_t *p_dec, block_t *p_frag )
{
    decoder_sys_t *p_sys = p_dec->p_sys;

    const uint8_t *p_buffer = p_frag->p_buffer;
    size_t i_buffer = p_frag->i_buffer;

    if( !hxxx_strip_AnnexB_startcode( &p_buffer, &i_buffer ) )
    {
        block_Release( p_frag );
        return;
    }

    h264_sequence_parameter_set_t *p_sps = h264_decode_sps( p_buffer, i_buffer, true );
    if( !p_sps )
    {
        msg_Warn( p_dec, "invalid SPS" );
        block_Release( p_frag );
        return;
    }

    /* We have a new SPS */
    if( !p_sys->sps[p_sps->i_id].p_sps )
        msg_Dbg( p_dec, "found NAL_SPS (sps_id=%d)", p_sps->i_id );

    StoreSPS( p_sys, p_sps->i_id, p_frag, p_sps );
}

static void PutPPS( decoder_t *p_dec, block_t *p_frag )
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    const uint8_t *p_buffer = p_frag->p_buffer;
    size_t i_buffer = p_frag->i_buffer;

    if( !hxxx_strip_AnnexB_startcode( &p_buffer, &i_buffer ) )
    {
        block_Release( p_frag );
        return;
    }

    h264_picture_parameter_set_t *p_pps = h264_decode_pps( p_buffer, i_buffer, true );
    if( !p_pps )
    {
        msg_Warn( p_dec, "invalid PPS" );
        block_Release( p_frag );
        return;
    }

    /* We have a new PPS */
    if( !p_sys->pps[p_pps->i_id].p_pps )
        msg_Dbg( p_dec, "found NAL_PPS (pps_id=%d sps_id=%d)", p_pps->i_id, p_pps->i_sps_id );

    StorePPS( p_sys, p_pps->i_id, p_frag, p_pps );
}

static bool ParseSliceHeader( decoder_t *p_dec, int i_nal_ref_idc, int i_nal_type,
                              const block_t *p_frag, slice_t *p_slice )
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    int i_slice_type;
    SliceInit( p_slice );
    bs_t s;
    unsigned i_bitflow = 0;

    const uint8_t *p_stripped = p_frag->p_buffer;
    size_t i_stripped = p_frag->i_buffer;

    if( !hxxx_strip_AnnexB_startcode( &p_stripped, &i_stripped ) || i_stripped < 2 )
        return false;

    bs_init( &s, p_stripped, i_stripped );
    s.p_fwpriv = &i_bitflow;
    s.pf_forward = hxxx_bsfw_ep3b_to_rbsp;  /* Does the emulated 3bytes conversion to rbsp */
    bs_skip( &s, 8 ); /* nal unit header */

    /* first_mb_in_slice */
    /* int i_first_mb = */ bs_read_ue( &s );

    /* slice_type */
    switch( (i_slice_type = bs_read_ue( &s )) )
    {
    case 0: case 5:
        p_slice->i_frame_type = BLOCK_FLAG_TYPE_P;
        break;
    case 1: case 6:
        p_slice->i_frame_type = BLOCK_FLAG_TYPE_B;
        break;
    case 2: case 7:
        p_slice->i_frame_type = BLOCK_FLAG_TYPE_I;
        break;
    case 3: case 8: /* SP */
        p_slice->i_frame_type = BLOCK_FLAG_TYPE_P;
        break;
    case 4: case 9:
        p_slice->i_frame_type = BLOCK_FLAG_TYPE_I;
        break;
    default:
        p_slice->i_frame_type = 0;
        break;
    }

    /* */
    p_slice->i_nal_type = i_nal_type;
    p_slice->i_nal_ref_idc = i_nal_ref_idc;

    p_slice->i_pic_parameter_set_id = bs_read_ue( &s );
    if( p_slice->i_pic_parameter_set_id > H264_PPS_ID_MAX )
        return false;

    /* Bind matched/referred PPS and SPS */
    const h264_picture_parameter_set_t *p_pps =
            p_sys->p_active_pps = p_sys->pps[p_slice->i_pic_parameter_set_id].p_pps;
    if( p_pps == NULL )
        return false;

    const h264_sequence_parameter_set_t *p_sps =
            p_sys->p_active_sps = p_sys->sps[p_pps->i_sps_id].p_sps;
    if( p_sps == NULL )
        return false;

    ActivateSets( p_dec, p_sps, p_pps );

    p_slice->i_frame_num = bs_read( &s, p_sps->i_log2_max_frame_num + 4 );

    if( !p_sps->frame_mbs_only_flag )
    {
        /* field_pic_flag */
        p_slice->i_field_pic_flag = bs_read( &s, 1 );
        if( p_slice->i_field_pic_flag )
            p_slice->i_bottom_field_flag = bs_read( &s, 1 );
    }

    if( p_slice->i_nal_type == H264_NAL_SLICE_IDR )
        p_slice->i_idr_pic_id = bs_read_ue( &s );

    p_slice->i_pic_order_cnt_type = p_sps->i_pic_order_cnt_type;
    if( p_sps->i_pic_order_cnt_type == 0 )
    {
        p_slice->i_pic_order_cnt_lsb = bs_read( &s, p_sps->i_log2_max_pic_order_cnt_lsb + 4 );
        if( p_pps->i_pic_order_present_flag && !p_slice->i_field_pic_flag )
            p_slice->i_delta_pic_order_cnt_bottom = bs_read_se( &s );
    }
    else if( (p_sps->i_pic_order_cnt_type == 1) &&
             (!p_sps->i_delta_pic_order_always_zero_flag) )
    {
        p_slice->i_delta_pic_order_cnt0 = bs_read_se( &s );
        if( p_pps->i_pic_order_present_flag && !p_slice->i_field_pic_flag )
            p_slice->i_delta_pic_order_cnt1 = bs_read_se( &s );
    }

    return true;
}

static bool ParseSeiCallback( const hxxx_sei_data_t *p_sei_data, void *cbdata )
{
    decoder_t *p_dec = (decoder_t *) cbdata;
    decoder_sys_t *p_sys = p_dec->p_sys;

    switch( p_sei_data->i_type )
    {
        /* Look for pic timing */
        case HXXX_SEI_PIC_TIMING:
        {
            const h264_sequence_parameter_set_t *p_sps = p_sys->p_active_sps;
            if( unlikely( p_sps == NULL ) )
            {
                assert( p_sps );
                break;
            }

            if( p_sps->vui.b_valid )
            {
                if( p_sps->vui.b_hrd_parameters_present_flag )
                {
                    bs_read( p_sei_data->p_bs, p_sps->vui.i_cpb_removal_delay_length_minus1 + 1 );
                    p_sys->i_dpb_output_delay =
                            bs_read( p_sei_data->p_bs, p_sps->vui.i_dpb_output_delay_length_minus1 + 1 );
                }

                if( p_sps->vui.b_pic_struct_present_flag )
                    p_sys->i_pic_struct = bs_read( p_sei_data->p_bs, 4 );
                /* + unparsed remains */
            }
        } break;

            /* Look for user_data_registered_itu_t_t35 */
        case HXXX_SEI_USER_DATA_REGISTERED_ITU_T_T35:
        {
            if( p_sei_data->itu_t35.type == HXXX_ITU_T35_TYPE_CC )
            {
                cc_storage_append( p_sys->p_ccs, true, p_sei_data->itu_t35.u.cc.p_data,
                                                       p_sei_data->itu_t35.u.cc.i_data );
            }
        } break;

            /* Look for SEI recovery point */
        case HXXX_SEI_RECOVERY_POINT:
        {
            if( !p_sys->b_header )
            {
                msg_Dbg( p_dec, "Seen SEI recovery point, %d recovery frames", p_sei_data->recovery.i_frames );
                if ( p_sys->i_recovery_frames == -1 || p_sei_data->recovery.i_frames < p_sys->i_recovery_frames )
                    p_sys->i_recovery_frames = p_sei_data->recovery.i_frames;
            }
        } break;

        default:
            /* Will skip */
            break;
    }

    return true;
}

