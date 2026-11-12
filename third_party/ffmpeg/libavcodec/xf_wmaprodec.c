/*
 * The WMA Pro / XMA decoders (libavcodec/wmaprodec.c of has207's fork, the
 * file itself, unchanged) with one function added by XenoForge. This file is
 * compiled into ffmpeg-xma in wmaprodec.c's place (ffmpeg.cmake).
 *
 * This file is part of FFmpeg's build in XenoForge and under the same licence
 * as wmaprodec.c: the GNU Lesser General Public License version 2.1 or later.
 */

#include "wmaprodec.c"

/*
 * The size of an xmaframes decoder's private state (AVCodecContext's
 * priv_data). Everything its decoding changes is inside that structure (the
 * output and overlap buffers, the bit reservoir, the frame state; the pointers
 * in it point into it, at FFmpeg's static tables, or at the transforms its
 * init made, whose results do not depend on earlier calls), so a copy taken
 * right after avcodec_open2 and written back later gives the decoder exactly
 * the state of a new one opened with the same rate and channels, without the
 * transforms' tables made again. XenoForge keeps opened decoders that way
 * (src/xma_decoder.cpp). Not part of FFmpeg's API: a build of FFmpeg without
 * this file lacks the symbol, and XenoForge then opens a new decoder each time.
 */
int xf_xmaframes_state_size(void)
{
    return sizeof(WMAProDecodeCtx);
}
