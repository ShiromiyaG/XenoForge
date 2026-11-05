/* The bitstream filters built (FFmpeg's configure writes this list): the
   null one the decode path uses. */
static const FFBitStreamFilter * const bitstream_filters[] = {
    &ff_null_bsf,
    NULL };
