/**************************************************************************
 * 
 * Copyright 2003 Tungsten Graphics, Inc., Cedar Park, Texas.
 * Copyright 2011 Intel Corporation
 * All Rights Reserved.
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 * 
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL TUNGSTEN GRAPHICS AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 * 
 * Authors:
 *     Chad Versace <chad@chad-versace.us>
 *
 **************************************************************************/

#include <stdbool.h>
#include <stdint.h>
#include "main/glheader.h"
#include "main/macros.h"
#include "main/mtypes.h"
#include "main/colormac.h"
#include "main/renderbuffer.h"

#include "intel_buffers.h"
#include "intel_fbo.h"
#include "intel_mipmap_tree.h"
#include "intel_screen.h"
#include "intel_span.h"
#include "intel_regions.h"
#include "intel_tex.h"

#include "swrast/swrast.h"

static void
intel_set_span_functions(struct intel_context *intel,
			 struct gl_renderbuffer *rb);

#undef DBG
#define DBG 0

#define LOCAL_VARS							\
   struct intel_renderbuffer *irb = intel_renderbuffer(rb);		\
   int minx = 0, miny = 0;						\
   int maxx = rb->Width;						\
   int maxy = rb->Height;						\
   int pitch = rb->RowStride * irb->mt->region->cpp;			\
   void *buf = rb->Data;						\
   GLuint p;								\
   (void) p;

#define HW_CLIPLOOP()
#define HW_ENDCLIPLOOP()

#define Y_FLIP(_y) (_y)

#define HW_LOCK()

#define HW_UNLOCK()

/* r5g6b5 color span and pixel functions */
#define SPANTMP_PIXEL_FMT GL_RGB
#define SPANTMP_PIXEL_TYPE GL_UNSIGNED_SHORT_5_6_5
#define TAG(x) intel_##x##_RGB565
#define TAG2(x,y) intel_##x##y_RGB565
#include "spantmp2.h"

/* a4r4g4b4 color span and pixel functions */
#define SPANTMP_PIXEL_FMT GL_BGRA
#define SPANTMP_PIXEL_TYPE GL_UNSIGNED_SHORT_4_4_4_4_REV
#define TAG(x) intel_##x##_ARGB4444
#define TAG2(x,y) intel_##x##y_ARGB4444
#include "spantmp2.h"

/* a1r5g5b5 color span and pixel functions */
#define SPANTMP_PIXEL_FMT GL_BGRA
#define SPANTMP_PIXEL_TYPE GL_UNSIGNED_SHORT_1_5_5_5_REV
#define TAG(x) intel_##x##_ARGB1555
#define TAG2(x,y) intel_##x##y##_ARGB1555
#include "spantmp2.h"

/* a8r8g8b8 color span and pixel functions */
#define SPANTMP_PIXEL_FMT GL_BGRA
#define SPANTMP_PIXEL_TYPE GL_UNSIGNED_INT_8_8_8_8_REV
#define TAG(x) intel_##x##_ARGB8888
#define TAG2(x,y) intel_##x##y##_ARGB8888
#include "spantmp2.h"

/* x8r8g8b8 color span and pixel functions */
#define SPANTMP_PIXEL_FMT GL_BGR
#define SPANTMP_PIXEL_TYPE GL_UNSIGNED_INT_8_8_8_8_REV
#define TAG(x) intel_##x##_xRGB8888
#define TAG2(x,y) intel_##x##y##_xRGB8888
#include "spantmp2.h"

/* a8 color span and pixel functions */
#define SPANTMP_PIXEL_FMT GL_ALPHA
#define SPANTMP_PIXEL_TYPE GL_UNSIGNED_BYTE
#define TAG(x) intel_##x##_A8
#define TAG2(x,y) intel_##x##y##_A8
#include "spantmp2.h"

/**
 * \brief Get pointer offset into stencil buffer.
 *
 * The stencil buffer is W tiled. Since the GTT is incapable of W fencing, we
 * must decode the tile's layout in software.
 *
 * See
 *   - PRM, 2011 Sandy Bridge, Volume 1, Part 2, Section 4.5.2.1 W-Major Tile
 *     Format.
 *   - PRM, 2011 Sandy Bridge, Volume 1, Part 2, Section 4.5.3 Tiling Algorithm
 *
 * Even though the returned offset is always positive, the return type is
 * signed due to
 *    commit e8b1c6d6f55f5be3bef25084fdd8b6127517e137
 *    mesa: Fix return type of  _mesa_get_format_bytes() (#37351)
 */
intptr_t
intel_offset_S8(uint32_t stride, uint32_t x, uint32_t y)
{
   uint32_t tile_size = 4096;
   uint32_t tile_width = 64;
   uint32_t tile_height = 64;
   uint32_t row_size = 64 * stride;

   uint32_t tile_x = x / tile_width;
   uint32_t tile_y = y / tile_height;

   /* The byte's address relative to the tile's base addres. */
   uint32_t byte_x = x % tile_width;
   uint32_t byte_y = y % tile_height;

   uintptr_t u = tile_y * row_size
               + tile_x * tile_size
               + 512 * (byte_x / 8)
               +  64 * (byte_y / 8)
               +  32 * ((byte_y / 4) % 2)
               +  16 * ((byte_x / 4) % 2)
               +   8 * ((byte_y / 2) % 2)
               +   4 * ((byte_x / 2) % 2)
               +   2 * (byte_y % 2)
               +   1 * (byte_x % 2);

   /*
    * Errata for Gen5:
    *
    * An additional offset is needed which is not documented in the PRM.
    *
    * if ((byte_x / 8) % 2 == 1) {
    *    if ((byte_y / 8) % 2) == 0) {
    *       u += 64;
    *    } else {
    *       u -= 64;
    *    }
    * }
    *
    * The offset is expressed more tersely as
    * u += ((int) x & 0x8) * (8 - (((int) y & 0x8) << 1));
    */

   return u;
}

void
intel_renderbuffer_map(struct intel_context *intel, struct gl_renderbuffer *rb)
{
   struct gl_context *ctx = &intel->ctx;
   struct intel_renderbuffer *irb = intel_renderbuffer(rb);
   GLubyte *map;
   int stride;

   if (!irb)
      return;

   if (rb->Data) {
      /* Renderbuffer is already mapped. This usually happens when a single
       * buffer is attached to the framebuffer's depth and stencil attachment
       * points.
       */
      return;
   }

   ctx->Driver.MapRenderbuffer(ctx, rb, 0, 0, rb->Width, rb->Height,
			       GL_MAP_READ_BIT | GL_MAP_WRITE_BIT,
			       &map, &stride);
   rb->Data = map;
   rb->RowStride = stride / _mesa_get_format_bytes(rb->Format);

   intel_set_span_functions(intel, rb);
}

void
intel_renderbuffer_unmap(struct intel_context *intel,
			 struct gl_renderbuffer *rb)
{
   struct gl_context *ctx = &intel->ctx;
   struct intel_renderbuffer *irb = intel_renderbuffer(rb);

   if (!irb)
      return;

   if (!rb->Data) {
      /* Renderbuffer is already unmapped. This usually happens when a single
       * buffer is attached to the framebuffer's depth and stencil attachment
       * points.
       */
      return;
   }

   ctx->Driver.UnmapRenderbuffer(ctx, rb);

   rb->GetRow = NULL;
   rb->PutRow = NULL;
   rb->Data = NULL;
   rb->RowStride = 0;
}

static void
intel_framebuffer_map(struct intel_context *intel, struct gl_framebuffer *fb)
{
   int i;

   for (i = 0; i < BUFFER_COUNT; i++) {
      intel_renderbuffer_map(intel, fb->Attachment[i].Renderbuffer);
   }

   intel_check_front_buffer_rendering(intel);
}

static void
intel_framebuffer_unmap(struct intel_context *intel, struct gl_framebuffer *fb)
{
   int i;

   for (i = 0; i < BUFFER_COUNT; i++) {
      intel_renderbuffer_unmap(intel, fb->Attachment[i].Renderbuffer);
   }
}

/**
 * Resolve all buffers that will be mapped by intelSpanRenderStart().
 *
 * Resolve the depth buffer of each enabled texture and of the read and draw
 * buffers.
 *
 * (Note: In the future this will also perform MSAA resolves.)
 */
static void
intel_span_resolve_buffers(struct intel_context *intel)
{
   struct gl_context *ctx = &intel->ctx;
   struct intel_renderbuffer *draw_irb;
   struct intel_renderbuffer *read_irb;
   struct intel_texture_object *tex_obj;

   /* Resolve depth buffer of each enabled texture. */
   for (int i = 0; i < ctx->Const.MaxTextureImageUnits; i++) {
      if (!ctx->Texture.Unit[i]._ReallyEnabled)
	 continue;
      tex_obj = intel_texture_object(ctx->Texture.Unit[i]._Current);
      intel_finalize_mipmap_tree(intel, i);
      if (!tex_obj || !tex_obj->mt)
	 continue;
      intel_miptree_all_slices_resolve_depth(intel, tex_obj->mt);
   }

   /* Resolve each attached depth buffer. */
   draw_irb = intel_get_renderbuffer(ctx->DrawBuffer, BUFFER_DEPTH);
   read_irb = intel_get_renderbuffer(ctx->ReadBuffer, BUFFER_DEPTH);
   if (draw_irb)
      intel_renderbuffer_resolve_depth(intel, draw_irb);
   if (read_irb != draw_irb && read_irb)
      intel_renderbuffer_resolve_depth(intel, read_irb);
}

/**
 * Map the regions needed by intelSpanRenderStart().
 */
static void
intel_span_map_buffers(struct intel_context *intel)
{
   struct gl_context *ctx = &intel->ctx;
   struct intel_texture_object *tex_obj;

   for (int i = 0; i < ctx->Const.MaxTextureImageUnits; i++) {
      if (!ctx->Texture.Unit[i]._ReallyEnabled)
	 continue;
      tex_obj = intel_texture_object(ctx->Texture.Unit[i]._Current);
      intel_finalize_mipmap_tree(intel, i);
      intel_tex_map_images(intel, tex_obj,
			   GL_MAP_READ_BIT | GL_MAP_WRITE_BIT);
   }

   intel_framebuffer_map(intel, ctx->DrawBuffer);
   if (ctx->ReadBuffer != ctx->DrawBuffer) {
      intel_framebuffer_map(intel, ctx->ReadBuffer);
   }
}

/**
 * Prepare for software rendering.  Map current read/draw framebuffers'
 * renderbuffes and all currently bound texture objects.
 *
 * Old note: Moved locking out to get reasonable span performance.
 */
void
intelSpanRenderStart(struct gl_context * ctx)
{
   struct intel_context *intel = intel_context(ctx);

   intel_flush(ctx);
   intel_prepare_render(intel);
   intel_span_resolve_buffers(intel);
   intel_flush(ctx);
   intel_span_map_buffers(intel);
}

/**
 * Called when done software rendering.  Unmap the buffers we mapped in
 * the above function.
 */
void
intelSpanRenderFinish(struct gl_context * ctx)
{
   struct intel_context *intel = intel_context(ctx);
   GLuint i;

   _swrast_flush(ctx);

   for (i = 0; i < ctx->Const.MaxTextureImageUnits; i++) {
      if (ctx->Texture.Unit[i]._ReallyEnabled) {
         struct gl_texture_object *texObj = ctx->Texture.Unit[i]._Current;
         intel_tex_unmap_images(intel, intel_texture_object(texObj));
      }
   }

   intel_framebuffer_unmap(intel, ctx->DrawBuffer);
   if (ctx->ReadBuffer != ctx->DrawBuffer) {
      intel_framebuffer_unmap(intel, ctx->ReadBuffer);
   }
}


void
intelInitSpanFuncs(struct gl_context * ctx)
{
   struct swrast_device_driver *swdd = _swrast_GetDeviceDriverReference(ctx);
   swdd->SpanRenderStart = intelSpanRenderStart;
   swdd->SpanRenderFinish = intelSpanRenderFinish;
}

void
intel_map_vertex_shader_textures(struct gl_context *ctx)
{
   struct intel_context *intel = intel_context(ctx);
   int i;

   if (ctx->VertexProgram._Current == NULL)
      return;

   for (i = 0; i < ctx->Const.MaxTextureImageUnits; i++) {
      if (ctx->Texture.Unit[i]._ReallyEnabled &&
	  ctx->VertexProgram._Current->Base.TexturesUsed[i] != 0) {
         struct gl_texture_object *texObj = ctx->Texture.Unit[i]._Current;

         intel_tex_map_images(intel, intel_texture_object(texObj),
                              GL_MAP_READ_BIT | GL_MAP_WRITE_BIT);
      }
   }
}

void
intel_unmap_vertex_shader_textures(struct gl_context *ctx)
{
   struct intel_context *intel = intel_context(ctx);
   int i;

   if (ctx->VertexProgram._Current == NULL)
      return;

   for (i = 0; i < ctx->Const.MaxTextureImageUnits; i++) {
      if (ctx->Texture.Unit[i]._ReallyEnabled &&
	  ctx->VertexProgram._Current->Base.TexturesUsed[i] != 0) {
         struct gl_texture_object *texObj = ctx->Texture.Unit[i]._Current;

         intel_tex_unmap_images(intel, intel_texture_object(texObj));
      }
   }
}

typedef void (*span_init_func)(struct gl_renderbuffer *rb);

static span_init_func intel_span_init_funcs[MESA_FORMAT_COUNT] =
{
   [MESA_FORMAT_A8] = intel_InitPointers_A8,
   [MESA_FORMAT_RGB565] = intel_InitPointers_RGB565,
   [MESA_FORMAT_ARGB4444] = intel_InitPointers_ARGB4444,
   [MESA_FORMAT_ARGB1555] = intel_InitPointers_ARGB1555,
   [MESA_FORMAT_XRGB8888] = intel_InitPointers_xRGB8888,
   [MESA_FORMAT_ARGB8888] = intel_InitPointers_ARGB8888,
   [MESA_FORMAT_SARGB8] = intel_InitPointers_ARGB8888,
   [MESA_FORMAT_Z16] = _mesa_set_renderbuffer_accessors,
   [MESA_FORMAT_X8_Z24] = _mesa_set_renderbuffer_accessors,
   [MESA_FORMAT_S8_Z24] = _mesa_set_renderbuffer_accessors,
   [MESA_FORMAT_S8] = _mesa_set_renderbuffer_accessors,
   [MESA_FORMAT_R8] = _mesa_set_renderbuffer_accessors,
   [MESA_FORMAT_GR88] = _mesa_set_renderbuffer_accessors,
   [MESA_FORMAT_R16] = _mesa_set_renderbuffer_accessors,
   [MESA_FORMAT_RG1616] = _mesa_set_renderbuffer_accessors,
   [MESA_FORMAT_RGBA_FLOAT32] = _mesa_set_renderbuffer_accessors,
   [MESA_FORMAT_RG_FLOAT32] = _mesa_set_renderbuffer_accessors,
   [MESA_FORMAT_R_FLOAT32] = _mesa_set_renderbuffer_accessors,
   [MESA_FORMAT_INTENSITY_FLOAT32] = _mesa_set_renderbuffer_accessors,
   [MESA_FORMAT_LUMINANCE_FLOAT32] = _mesa_set_renderbuffer_accessors,
};

bool
intel_span_supports_format(gl_format format)
{
   /* Rendering to/from integer textures will be done using MapRenderbuffer,
    * rather than coding up new paths through GetRow/PutRow(), so claim support
    * for those formats in here for now.
    */
   return (intel_span_init_funcs[format] != NULL ||
	   _mesa_is_format_integer_color(format));
}

/**
 * Plug in appropriate span read/write functions for the given renderbuffer.
 * These are used for the software fallbacks.
 */
static void
intel_set_span_functions(struct intel_context *intel,
			 struct gl_renderbuffer *rb)
{
   struct intel_renderbuffer *irb = (struct intel_renderbuffer *) rb;

   assert(intel_span_init_funcs[irb->Base.Format]);
   intel_span_init_funcs[irb->Base.Format](rb);

   if (rb->DataType == GL_NONE) {
      _mesa_problem(NULL,
		    "renderbuffer format %s is missing "
		    "intel_mesa_format_to_rb_datatype() support.",
		    _mesa_get_format_name(rb->Format));
   }
}
