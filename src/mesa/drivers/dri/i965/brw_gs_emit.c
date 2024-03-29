/*
 Copyright (C) Intel Corp.  2006.  All Rights Reserved.
 Intel funded Tungsten Graphics (http://www.tungstengraphics.com) to
 develop this 3D driver.
 
 Permission is hereby granted, free of charge, to any person obtaining
 a copy of this software and associated documentation files (the
 "Software"), to deal in the Software without restriction, including
 without limitation the rights to use, copy, modify, merge, publish,
 distribute, sublicense, and/or sell copies of the Software, and to
 permit persons to whom the Software is furnished to do so, subject to
 the following conditions:
 
 The above copyright notice and this permission notice (including the
 next paragraph) shall be included in all copies or substantial
 portions of the Software.
 
 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 IN NO EVENT SHALL THE COPYRIGHT OWNER(S) AND/OR ITS SUPPLIERS BE
 LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 
 **********************************************************************/
 /*
  * Authors:
  *   Keith Whitwell <keith@tungstengraphics.com>
  */
 

#include "main/glheader.h"
#include "main/macros.h"
#include "main/enums.h"

#include "program/program.h"
#include "intel_batchbuffer.h"

#include "brw_defines.h"
#include "brw_context.h"
#include "brw_eu.h"
#include "brw_gs.h"

static void brw_gs_alloc_regs( struct brw_gs_compile *c,
			       GLuint nr_verts )
{
   GLuint i = 0,j;

   /* Register usage is static, precompute here:
    */
   c->reg.R0 = retype(brw_vec8_grf(i, 0), BRW_REGISTER_TYPE_UD); i++;

   /* Payload vertices plus space for more generated vertices:
    */
   for (j = 0; j < nr_verts; j++) {
      c->reg.vertex[j] = brw_vec4_grf(i, 0);
      i += c->nr_regs;
   }

   c->reg.temp = brw_vec8_grf(i, 0);

   c->prog_data.urb_read_length = c->nr_regs; 
   c->prog_data.total_grf = i;
}


static void brw_gs_emit_vue(struct brw_gs_compile *c, 
			    struct brw_reg vert,
			    bool last,
			    GLuint header)
{
   struct brw_compile *p = &c->func;
   struct intel_context *intel = &c->func.brw->intel;
   bool allocate = !last;
   struct brw_reg temp;

   if (intel->gen < 6)
      temp = c->reg.R0;
   else {
      temp = c->reg.temp;
      brw_MOV(p, retype(temp, BRW_REGISTER_TYPE_UD),
	      retype(c->reg.R0, BRW_REGISTER_TYPE_UD));
   }

   /* Overwrite PrimType and PrimStart in the message header, for
    * each vertex in turn:
    */
   brw_MOV(p, get_element_ud(temp, 2), brw_imm_ud(header));

   /* Copy the vertex from vertn into m1..mN+1:
    */
   brw_copy8(p, brw_message_reg(1), vert, c->nr_regs);

   /* Send each vertex as a seperate write to the urb.  This is
    * different to the concept in brw_sf_emit.c, where subsequent
    * writes are used to build up a single urb entry.  Each of these
    * writes instantiates a seperate urb entry, and a new one must be
    * allocated each time.
    */
   brw_urb_WRITE(p, 
		 allocate ? temp : retype(brw_null_reg(), BRW_REGISTER_TYPE_UD),
		 0,
		 temp,
		 allocate,
		 1,		/* used */
		 c->nr_regs + 1, /* msg length */
		 allocate ? 1 : 0, /* response length */
		 allocate ? 0 : 1, /* eot */
		 1,		/* writes_complete */
		 0,		/* urb offset */
		 BRW_URB_SWIZZLE_NONE);

   if (intel->gen >= 6 && allocate)
       brw_MOV(p, get_element_ud(c->reg.R0, 0), get_element_ud(temp, 0));
}

static void brw_gs_ff_sync(struct brw_gs_compile *c, int num_prim)
{
   struct brw_compile *p = &c->func;
   struct intel_context *intel = &c->func.brw->intel;

   if (intel->gen < 6) {
      brw_MOV(p, get_element_ud(c->reg.R0, 1), brw_imm_ud(num_prim));
      brw_ff_sync(p,
		  c->reg.R0,
		  0,
		  c->reg.R0,
		  1, /* allocate */
		  1, /* response length */
		  0 /* eot */);
   } else {
      brw_MOV(p, retype(c->reg.temp, BRW_REGISTER_TYPE_UD),
	      retype(c->reg.R0, BRW_REGISTER_TYPE_UD));
      brw_MOV(p, get_element_ud(c->reg.temp, 1), brw_imm_ud(num_prim));
      brw_ff_sync(p,
		  c->reg.temp,
		  0,
		  c->reg.temp,
		  1, /* allocate */
		  1, /* response length */
		  0 /* eot */);
      brw_MOV(p, get_element_ud(c->reg.R0, 0),
      get_element_ud(c->reg.temp, 0));
   }
}


void brw_gs_quads( struct brw_gs_compile *c, struct brw_gs_prog_key *key )
{
   struct intel_context *intel = &c->func.brw->intel;

   brw_gs_alloc_regs(c, 4);
   
   /* Use polygons for correct edgeflag behaviour. Note that vertex 3
    * is the PV for quads, but vertex 0 for polygons:
    */
   if (intel->needs_ff_sync)
      brw_gs_ff_sync(c, 1);
   if (key->pv_first) {
      brw_gs_emit_vue(c, c->reg.vertex[0], 0, ((_3DPRIM_POLYGON << 2) | R02_PRIM_START));
      brw_gs_emit_vue(c, c->reg.vertex[1], 0, (_3DPRIM_POLYGON << 2));
      brw_gs_emit_vue(c, c->reg.vertex[2], 0, (_3DPRIM_POLYGON << 2));
      brw_gs_emit_vue(c, c->reg.vertex[3], 1, ((_3DPRIM_POLYGON << 2) | R02_PRIM_END));
   }
   else {
      brw_gs_emit_vue(c, c->reg.vertex[3], 0, ((_3DPRIM_POLYGON << 2) | R02_PRIM_START));
      brw_gs_emit_vue(c, c->reg.vertex[0], 0, (_3DPRIM_POLYGON << 2));
      brw_gs_emit_vue(c, c->reg.vertex[1], 0, (_3DPRIM_POLYGON << 2));
      brw_gs_emit_vue(c, c->reg.vertex[2], 1, ((_3DPRIM_POLYGON << 2) | R02_PRIM_END));
   }
}

void brw_gs_quad_strip( struct brw_gs_compile *c, struct brw_gs_prog_key *key )
{
   struct intel_context *intel = &c->func.brw->intel;

   brw_gs_alloc_regs(c, 4);
   
   if (intel->needs_ff_sync)
      brw_gs_ff_sync(c, 1);
   if (key->pv_first) {
      brw_gs_emit_vue(c, c->reg.vertex[0], 0, ((_3DPRIM_POLYGON << 2) | R02_PRIM_START));
      brw_gs_emit_vue(c, c->reg.vertex[1], 0, (_3DPRIM_POLYGON << 2));
      brw_gs_emit_vue(c, c->reg.vertex[2], 0, (_3DPRIM_POLYGON << 2));
      brw_gs_emit_vue(c, c->reg.vertex[3], 1, ((_3DPRIM_POLYGON << 2) | R02_PRIM_END));
   }
   else {
      brw_gs_emit_vue(c, c->reg.vertex[2], 0, ((_3DPRIM_POLYGON << 2) | R02_PRIM_START));
      brw_gs_emit_vue(c, c->reg.vertex[3], 0, (_3DPRIM_POLYGON << 2));
      brw_gs_emit_vue(c, c->reg.vertex[0], 0, (_3DPRIM_POLYGON << 2));
      brw_gs_emit_vue(c, c->reg.vertex[1], 1, ((_3DPRIM_POLYGON << 2) | R02_PRIM_END));
   }
}

void brw_gs_lines( struct brw_gs_compile *c )
{
   struct intel_context *intel = &c->func.brw->intel;

   brw_gs_alloc_regs(c, 2);

   if (intel->needs_ff_sync)
      brw_gs_ff_sync(c, 1);
   brw_gs_emit_vue(c, c->reg.vertex[0], 0, ((_3DPRIM_LINESTRIP << 2) | R02_PRIM_START));
   brw_gs_emit_vue(c, c->reg.vertex[1], 1, ((_3DPRIM_LINESTRIP << 2) | R02_PRIM_END));
}
