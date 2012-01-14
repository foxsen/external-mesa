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


#include "main/imports.h"
#include "main/macros.h"
#include "main/simple_list.h"

#include "vbo/vbo_context.h"

#include "brw_context.h"
#include "brw_defines.h"
#include "brw_draw.h"
#include "brw_state.h"

#include "gen6_hiz.h"

#include "intel_fbo.h"
#include "intel_mipmap_tree.h"
#include "intel_regions.h"
#include "intel_span.h"
#include "intel_tex.h"
#include "intel_tex_obj.h"

#include "tnl/t_pipeline.h"
#include "glsl/ralloc.h"

/***************************************
 * Mesa's Driver Functions
 ***************************************/

/**
 * \brief Prepare for entry into glBegin/glEnd block.
 *
 * Resolve buffers before entering a glBegin/glEnd block. This is
 * necessary to prevent recursive calls to FLUSH_VERTICES.
 *
 * This resolves the depth buffer of each enabled depth texture and the HiZ
 * buffer of the attached depth renderbuffer.
 *
 * Details
 * -------
 * When vertices are queued during a glBegin/glEnd block, those vertices must
 * be drawn before any rendering state changes. To ensure this, Mesa calls
 * FLUSH_VERTICES as a prehook to such state changes. Therefore,
 * FLUSH_VERTICES itself cannot change rendering state without falling into a
 * recursive trap.
 *
 * This precludes meta-ops, namely buffer resolves, from occurring while any
 * vertices are queued. To prevent that situation, we resolve some buffers on
 * entering a glBegin/glEnd
 *
 * \see brwCleanupExecEnd()
 */
static void brwPrepareExecBegin(struct gl_context *ctx)
{
   struct brw_context *brw = brw_context(ctx);
   struct intel_context *intel = &brw->intel;
   struct intel_renderbuffer *draw_irb;
   struct intel_texture_object *tex_obj;

   if (!intel->has_hiz) {
      /* The context uses no feature that requires buffer resolves. */
      return;
   }

   /* Resolve each enabled texture. */
   for (int i = 0; i < ctx->Const.MaxTextureImageUnits; i++) {
      if (!ctx->Texture.Unit[i]._ReallyEnabled)
	 continue;
      tex_obj = intel_texture_object(ctx->Texture.Unit[i]._Current);
      if (!tex_obj || !tex_obj->mt)
	 continue;
      intel_miptree_all_slices_resolve_depth(intel, tex_obj->mt);
   }

   /* Resolve the attached depth buffer. */
   draw_irb = intel_get_renderbuffer(ctx->DrawBuffer, BUFFER_DEPTH);
   if (draw_irb) {
      intel_renderbuffer_resolve_hiz(intel, draw_irb);
   }
}

static void brwInitDriverFunctions( struct dd_function_table *functions )
{
   intelInitDriverFunctions( functions );

   brwInitFragProgFuncs( functions );
   brw_init_queryobj_functions(functions);

   functions->PrepareExecBegin = brwPrepareExecBegin;
}

bool
brwCreateContext(int api,
	         const struct gl_config *mesaVis,
		 __DRIcontext *driContextPriv,
	         void *sharedContextPrivate)
{
   struct dd_function_table functions;
   struct brw_context *brw = rzalloc(NULL, struct brw_context);
   struct intel_context *intel = &brw->intel;
   struct gl_context *ctx = &intel->ctx;
   unsigned i;

   if (!brw) {
      printf("%s: failed to alloc context\n", __FUNCTION__);
      return false;
   }

   brwInitDriverFunctions( &functions );

   if (!intelInitContext( intel, api, mesaVis, driContextPriv,
			  sharedContextPrivate, &functions )) {
      printf("%s: failed to init intel context\n", __FUNCTION__);
      FREE(brw);
      return false;
   }

   brwInitVtbl( brw );

   brw_init_surface_formats(brw);

   /* Initialize swrast, tnl driver tables: */
   intelInitSpanFuncs(ctx);

   TNL_CONTEXT(ctx)->Driver.RunPipeline = _tnl_run_pipeline;

   ctx->Const.MaxDrawBuffers = BRW_MAX_DRAW_BUFFERS;
   ctx->Const.MaxTextureImageUnits = BRW_MAX_TEX_UNIT;
   ctx->Const.MaxTextureCoordUnits = 8; /* Mesa limit */
   ctx->Const.MaxTextureUnits = MIN2(ctx->Const.MaxTextureCoordUnits,
                                     ctx->Const.MaxTextureImageUnits);
   ctx->Const.MaxVertexTextureImageUnits = 0; /* no vertex shader textures */
   ctx->Const.MaxCombinedTextureImageUnits =
      ctx->Const.MaxVertexTextureImageUnits +
      ctx->Const.MaxTextureImageUnits;

   ctx->Const.MaxTextureLevels = 14; /* 8192 */
   if (ctx->Const.MaxTextureLevels > MAX_TEXTURE_LEVELS)
	   ctx->Const.MaxTextureLevels = MAX_TEXTURE_LEVELS;
   ctx->Const.Max3DTextureLevels = 9;
   ctx->Const.MaxCubeTextureLevels = 12;
   /* minimum maximum.  Users are likely to run into memory problems
    * even at this size, since 64 * 2048 * 2048 * 4 = 1GB and we can't
    * address that much.
    */
   ctx->Const.MaxArrayTextureLayers = 64;
   ctx->Const.MaxTextureRectSize = (1<<12);
   
   ctx->Const.MaxTextureMaxAnisotropy = 16.0;

   /* if conformance mode is set, swrast can handle any size AA point */
   ctx->Const.MaxPointSizeAA = 255.0;

   /* We want the GLSL compiler to emit code that uses condition codes */
   for (i = 0; i <= MESA_SHADER_FRAGMENT; i++) {
      ctx->ShaderCompilerOptions[i].MaxIfDepth = intel->gen < 6 ? 16 : UINT_MAX;
      ctx->ShaderCompilerOptions[i].EmitCondCodes = true;
      ctx->ShaderCompilerOptions[i].EmitNVTempInitialization = true;
      ctx->ShaderCompilerOptions[i].EmitNoNoise = true;
      ctx->ShaderCompilerOptions[i].EmitNoMainReturn = true;
      ctx->ShaderCompilerOptions[i].EmitNoIndirectInput = true;
      ctx->ShaderCompilerOptions[i].EmitNoIndirectOutput = true;

      ctx->ShaderCompilerOptions[i].EmitNoIndirectUniform =
	 (i == MESA_SHADER_FRAGMENT);
      ctx->ShaderCompilerOptions[i].EmitNoIndirectTemp =
	 (i == MESA_SHADER_FRAGMENT);
      ctx->ShaderCompilerOptions[i].LowerClipDistance = true;
   }

   ctx->Const.VertexProgram.MaxNativeInstructions = (16 * 1024);
   ctx->Const.VertexProgram.MaxAluInstructions = 0;
   ctx->Const.VertexProgram.MaxTexInstructions = 0;
   ctx->Const.VertexProgram.MaxTexIndirections = 0;
   ctx->Const.VertexProgram.MaxNativeAluInstructions = 0;
   ctx->Const.VertexProgram.MaxNativeTexInstructions = 0;
   ctx->Const.VertexProgram.MaxNativeTexIndirections = 0;
   ctx->Const.VertexProgram.MaxNativeAttribs = 16;
   ctx->Const.VertexProgram.MaxNativeTemps = 256;
   ctx->Const.VertexProgram.MaxNativeAddressRegs = 1;
   ctx->Const.VertexProgram.MaxNativeParameters = 1024;
   ctx->Const.VertexProgram.MaxEnvParams =
      MIN2(ctx->Const.VertexProgram.MaxNativeParameters,
	   ctx->Const.VertexProgram.MaxEnvParams);

   ctx->Const.FragmentProgram.MaxNativeInstructions = (16 * 1024);
   ctx->Const.FragmentProgram.MaxNativeAluInstructions = (16 * 1024);
   ctx->Const.FragmentProgram.MaxNativeTexInstructions = (16 * 1024);
   ctx->Const.FragmentProgram.MaxNativeTexIndirections = (16 * 1024);
   ctx->Const.FragmentProgram.MaxNativeAttribs = 12;
   ctx->Const.FragmentProgram.MaxNativeTemps = 256;
   ctx->Const.FragmentProgram.MaxNativeAddressRegs = 0;
   ctx->Const.FragmentProgram.MaxNativeParameters = 1024;
   ctx->Const.FragmentProgram.MaxEnvParams =
      MIN2(ctx->Const.FragmentProgram.MaxNativeParameters,
	   ctx->Const.FragmentProgram.MaxEnvParams);

   /* Fragment shaders use real, 32-bit twos-complement integers for all
    * integer types.
    */
   ctx->Const.FragmentProgram.LowInt.RangeMin = 31;
   ctx->Const.FragmentProgram.LowInt.RangeMax = 30;
   ctx->Const.FragmentProgram.LowInt.Precision = 0;
   ctx->Const.FragmentProgram.HighInt = ctx->Const.FragmentProgram.MediumInt
      = ctx->Const.FragmentProgram.LowInt;

   /* Gen6 converts quads to polygon in beginning of 3D pipeline,
      but we're not sure how it's actually done for vertex order,
      that affect provoking vertex decision. Always use last vertex
      convention for quad primitive which works as expected for now. */
   if (intel->gen >= 6)
       ctx->Const.QuadsFollowProvokingVertexConvention = false;

   if (intel->is_g4x || intel->gen >= 5) {
      brw->CMD_VF_STATISTICS = GM45_3DSTATE_VF_STATISTICS;
      brw->CMD_PIPELINE_SELECT = CMD_PIPELINE_SELECT_GM45;
      brw->has_surface_tile_offset = true;
      if (intel->gen < 6)
	  brw->has_compr4 = true;
      brw->has_aa_line_parameters = true;
      brw->has_pln = true;
  } else {
      brw->CMD_VF_STATISTICS = GEN4_3DSTATE_VF_STATISTICS;
      brw->CMD_PIPELINE_SELECT = CMD_PIPELINE_SELECT_965;
   }

   /* WM maximum threads is number of EUs times number of threads per EU. */
   if (intel->gen >= 7) {
      if (intel->gt == 1) {
	 brw->max_wm_threads = 86;
	 brw->max_vs_threads = 36;
	 brw->max_gs_threads = 36;
	 brw->urb.size = 128;
	 brw->urb.max_vs_entries = 512;
	 brw->urb.max_gs_entries = 192;
      } else if (intel->gt == 2) {
	 brw->max_wm_threads = 86;
	 brw->max_vs_threads = 128;
	 brw->max_gs_threads = 128;
	 brw->urb.size = 256;
	 brw->urb.max_vs_entries = 704;
	 brw->urb.max_gs_entries = 320;
      } else {
	 assert(!"Unknown gen7 device.");
      }
   } else if (intel->gen == 6) {
      if (intel->gt == 2) {
	 /* This could possibly be 80, but is supposed to require
	  * disabling of WIZ hashing (bit 6 of GT_MODE, 0x20d0) and a
	  * GPU reset to change.
	  */
	 brw->max_wm_threads = 40;
	 brw->max_vs_threads = 60;
	 brw->max_gs_threads = 60;
	 brw->urb.size = 64;            /* volume 5c.5 section 5.1 */
	 brw->urb.max_vs_entries = 256; /* volume 2a (see 3DSTATE_URB) */
      } else {
	 brw->max_wm_threads = 40;
	 brw->max_vs_threads = 24;
	 brw->max_gs_threads = 21; /* conservative; 24 if rendering disabled */
	 brw->urb.size = 32;            /* volume 5c.5 section 5.1 */
	 brw->urb.max_vs_entries = 128; /* volume 2a (see 3DSTATE_URB) */
      }
   } else if (intel->gen == 5) {
      brw->urb.size = 1024;
      brw->max_vs_threads = 72;
      brw->max_gs_threads = 32;
      brw->max_wm_threads = 12 * 6;
   } else if (intel->is_g4x) {
      brw->urb.size = 384;
      brw->max_vs_threads = 32;
      brw->max_gs_threads = 2;
      brw->max_wm_threads = 10 * 5;
   } else if (intel->gen < 6) {
      brw->urb.size = 256;
      brw->max_vs_threads = 16;
      brw->max_gs_threads = 2;
      brw->max_wm_threads = 8 * 4;
      brw->has_negative_rhw_bug = true;
   }

   brw_init_state( brw );

   brw->curbe.last_buf = calloc(1, 4096);
   brw->curbe.next_buf = calloc(1, 4096);

   brw->state.dirty.mesa = ~0;
   brw->state.dirty.brw = ~0;

   brw->emit_state_always = 0;

   intel->batch.need_workaround_flush = true;

   ctx->VertexProgram._MaintainTnlProgram = true;
   ctx->FragmentProgram._MaintainTexEnvProgram = true;

   brw_draw_init( brw );

   brw->new_vs_backend = (getenv("INTEL_OLD_VS") == NULL);
   brw->precompile = driQueryOptionb(&intel->optionCache, "shader_precompile");

   /* If we're using the new shader backend, we require integer uniforms
    * stored as actual integers.
    */
   if (brw->new_vs_backend) {
      ctx->Const.NativeIntegers = true;
      ctx->Const.UniformBooleanTrue = 1;
   }

   return true;
}

