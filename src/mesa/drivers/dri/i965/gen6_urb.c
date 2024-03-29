/*
 * Copyright © 2009 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 *    Eric Anholt <eric@anholt.net>
 *
 */

#include "main/macros.h"
#include "intel_batchbuffer.h"
#include "brw_context.h"
#include "brw_state.h"
#include "brw_defines.h"

static void
gen6_upload_urb( struct brw_context *brw )
{
   struct intel_context *intel = &brw->intel;
   int nr_vs_entries;

   /* CACHE_NEW_VS_PROG */
   brw->urb.vs_size = MAX2(brw->vs.prog_data->urb_entry_size, 1);

   /* Calculate how many VS URB entries fit in the total URB size */
   nr_vs_entries = (brw->urb.size * 1024) / (brw->urb.vs_size * 128);

   if (nr_vs_entries > brw->urb.max_vs_entries)
      nr_vs_entries = brw->urb.max_vs_entries;

   /* According to volume 2a, nr_vs_entries must be a multiple of 4. */
   brw->urb.nr_vs_entries = ROUND_DOWN_TO(nr_vs_entries, 4);

   /* Since we currently don't support Geometry Shaders, we always put the
    * GS unit in passthrough mode and don't allocate it any URB space.
    */
   brw->urb.nr_gs_entries = 0;
   brw->urb.gs_size = 1; /* Incorrect, but with 0 GS entries it doesn't matter. */

   assert(brw->urb.nr_vs_entries >= 24);
   assert(brw->urb.nr_vs_entries % 4 == 0);
   assert(brw->urb.nr_gs_entries % 4 == 0);
   /* GS requirement */
   assert(!brw->gs.prog_active || brw->urb.vs_size < 5);

   BEGIN_BATCH(3);
   OUT_BATCH(_3DSTATE_URB << 16 | (3 - 2));
   OUT_BATCH(((brw->urb.vs_size - 1) << GEN6_URB_VS_SIZE_SHIFT) |
	     ((brw->urb.nr_vs_entries) << GEN6_URB_VS_ENTRIES_SHIFT));
   OUT_BATCH(((brw->urb.gs_size - 1) << GEN6_URB_GS_SIZE_SHIFT) |
	     ((brw->urb.nr_gs_entries) << GEN6_URB_GS_ENTRIES_SHIFT));
   ADVANCE_BATCH();
}

const struct brw_tracked_state gen6_urb = {
   .dirty = {
      .mesa = 0,
      .brw = BRW_NEW_CONTEXT,
      .cache = (CACHE_NEW_VS_PROG | CACHE_NEW_GS_PROG),
   },
   .emit = gen6_upload_urb,
};
