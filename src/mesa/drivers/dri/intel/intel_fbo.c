/**************************************************************************
 * 
 * Copyright 2006 Tungsten Graphics, Inc., Cedar Park, Texas.
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
 **************************************************************************/


#include "main/enums.h"
#include "main/imports.h"
#include "main/macros.h"
#include "main/mfeatures.h"
#include "main/mtypes.h"
#include "main/fbobject.h"
#include "main/framebuffer.h"
#include "main/renderbuffer.h"
#include "main/context.h"
#include "main/teximage.h"
#include "main/image.h"

#include "swrast/swrast.h"
#include "drivers/common/meta.h"

#include "intel_context.h"
#include "intel_batchbuffer.h"
#include "intel_buffers.h"
#include "intel_blit.h"
#include "intel_fbo.h"
#include "intel_mipmap_tree.h"
#include "intel_regions.h"
#include "intel_tex.h"
#include "intel_span.h"
#ifndef I915
#include "brw_context.h"
#endif

#define FILE_DEBUG_FLAG DEBUG_FBO


bool
intel_framebuffer_has_hiz(struct gl_framebuffer *fb)
{
   struct intel_renderbuffer *rb = NULL;
   if (fb)
      rb = intel_get_renderbuffer(fb, BUFFER_DEPTH);
   return rb && rb->mt && rb->mt->hiz_mt;
}

struct intel_region*
intel_get_rb_region(struct gl_framebuffer *fb, GLuint attIndex)
{
   struct intel_renderbuffer *irb = intel_get_renderbuffer(fb, attIndex);
   if (irb && irb->mt)
      return irb->mt->region;
   else
      return NULL;
}

/**
 * Create a new framebuffer object.
 */
static struct gl_framebuffer *
intel_new_framebuffer(struct gl_context * ctx, GLuint name)
{
   /* Only drawable state in intel_framebuffer at this time, just use Mesa's
    * class
    */
   return _mesa_new_framebuffer(ctx, name);
}


/** Called by gl_renderbuffer::Delete() */
static void
intel_delete_renderbuffer(struct gl_renderbuffer *rb)
{
   struct intel_renderbuffer *irb = intel_renderbuffer(rb);

   ASSERT(irb);

   intel_miptree_release(&irb->mt);

   _mesa_reference_renderbuffer(&irb->wrapped_depth, NULL);
   _mesa_reference_renderbuffer(&irb->wrapped_stencil, NULL);

   free(irb);
}

/**
 * \brief Map a renderbuffer through the GTT.
 *
 * \see intel_map_renderbuffer()
 */
static void
intel_map_renderbuffer_gtt(struct gl_context *ctx,
                           struct gl_renderbuffer *rb,
                           GLuint x, GLuint y, GLuint w, GLuint h,
                           GLbitfield mode,
                           GLubyte **out_map,
                           GLint *out_stride)
{
   struct intel_context *intel = intel_context(ctx);
   struct intel_renderbuffer *irb = intel_renderbuffer(rb);
   GLubyte *map;
   int stride, flip_stride;

   assert(irb->mt);

   intel_renderbuffer_resolve_depth(intel, irb);
   if (mode & GL_MAP_WRITE_BIT) {
      intel_renderbuffer_set_needs_hiz_resolve(irb);
   }

   irb->map_mode = mode;
   irb->map_x = x;
   irb->map_y = y;
   irb->map_w = w;
   irb->map_h = h;

   stride = irb->mt->region->pitch * irb->mt->region->cpp;

   if (rb->Name == 0) {
      y = irb->mt->region->height - 1 - y;
      flip_stride = -stride;
   } else {
      x += irb->draw_x;
      y += irb->draw_y;
      flip_stride = stride;
   }

   if (drm_intel_bo_references(intel->batch.bo, irb->mt->region->bo)) {
      intel_batchbuffer_flush(intel);
   }

   drm_intel_gem_bo_map_gtt(irb->mt->region->bo);

   map = irb->mt->region->bo->virtual;
   map += x * irb->mt->region->cpp;
   map += (int)y * stride;

   *out_map = map;
   *out_stride = flip_stride;

   DBG("%s: rb %d (%s) gtt mapped: (%d, %d) (%dx%d) -> %p/%d\n",
       __FUNCTION__, rb->Name, _mesa_get_format_name(rb->Format),
       x, y, w, h, *out_map, *out_stride);
}

/**
 * \brief Map a renderbuffer by blitting it to a temporary gem buffer.
 *
 * On gen6+, we have LLC sharing, which means we can get high-performance
 * access to linear-mapped buffers.
 *
 * This function allocates a temporary gem buffer at
 * intel_renderbuffer::map_bo, then blits the renderbuffer into it, and
 * returns a map of that. (Note: Only X tiled buffers can be blitted).
 *
 * \see intel_renderbuffer::map_bo
 * \see intel_map_renderbuffer()
 */
static void
intel_map_renderbuffer_blit(struct gl_context *ctx,
			    struct gl_renderbuffer *rb,
			    GLuint x, GLuint y, GLuint w, GLuint h,
			    GLbitfield mode,
			    GLubyte **out_map,
			    GLint *out_stride)
{
   struct intel_context *intel = intel_context(ctx);
   struct intel_renderbuffer *irb = intel_renderbuffer(rb);

   int src_x, src_y;
   int dst_stride;

   assert(irb->mt->region);
   assert(intel->gen >= 6);
   assert(!(mode & GL_MAP_WRITE_BIT));
   assert(irb->mt->region->tiling == I915_TILING_X);

   irb->map_mode = mode;
   irb->map_x = x;
   irb->map_y = y;
   irb->map_w = w;
   irb->map_h = h;

   dst_stride = ALIGN(w * irb->mt->region->cpp, 4);

   if (rb->Name) {
      src_x = x + irb->draw_x;
      src_y = y + irb->draw_y;
   } else {
      src_x = x;
      src_y = irb->mt->region->height - y - h;
   }

   irb->map_bo = drm_intel_bo_alloc(intel->bufmgr, "MapRenderbuffer() temp",
				    dst_stride * h, 4096);

   /* We don't do the flip in the blit, because it's always so tricky to get
    * right.
    */
   if (irb->map_bo &&
       intelEmitCopyBlit(intel,
			 irb->mt->region->cpp,
			 irb->mt->region->pitch, irb->mt->region->bo,
			 0, irb->mt->region->tiling,
			 dst_stride / irb->mt->region->cpp, irb->map_bo,
			 0, I915_TILING_NONE,
			 src_x, src_y,
			 0, 0,
			 w, h,
			 GL_COPY)) {
      intel_batchbuffer_flush(intel);
      drm_intel_bo_map(irb->map_bo, false);

      if (rb->Name) {
	 *out_map = irb->map_bo->virtual;
	 *out_stride = dst_stride;
      } else {
	 *out_map = irb->map_bo->virtual + (h - 1) * dst_stride;
	 *out_stride = -dst_stride;
      }

      DBG("%s: rb %d (%s) blit mapped: (%d, %d) (%dx%d) -> %p/%d\n",
	  __FUNCTION__, rb->Name, _mesa_get_format_name(rb->Format),
	  src_x, src_y, w, h, *out_map, *out_stride);
   } else {
      /* Fallback to GTT mapping. */
      drm_intel_bo_unreference(irb->map_bo);
      irb->map_bo = NULL;
      intel_map_renderbuffer_gtt(ctx, rb,
				 x, y, w, h,
				 mode,
				 out_map, out_stride);
   }
}

/**
 * \brief Map a stencil renderbuffer.
 *
 * Stencil buffers are W-tiled. Since the GTT has no W fence, we must detile
 * the buffer in software.
 *
 * This function allocates a temporary malloc'd buffer at
 * intel_renderbuffer::map_buffer, detiles the stencil buffer into it, then
 * returns the temporary buffer as the map.
 *
 * \see intel_renderbuffer::map_buffer
 * \see intel_map_renderbuffer()
 * \see intel_unmap_renderbuffer_s8()
 */
static void
intel_map_renderbuffer_s8(struct gl_context *ctx,
			  struct gl_renderbuffer *rb,
			  GLuint x, GLuint y, GLuint w, GLuint h,
			  GLbitfield mode,
			  GLubyte **out_map,
			  GLint *out_stride)
{
   struct intel_context *intel = intel_context(ctx);
   struct intel_renderbuffer *irb = intel_renderbuffer(rb);
   uint8_t *tiled_s8_map;
   uint8_t *untiled_s8_map;

   assert(rb->Format == MESA_FORMAT_S8);
   assert(irb->mt);

   irb->map_mode = mode;
   irb->map_x = x;
   irb->map_y = y;
   irb->map_w = w;
   irb->map_h = h;

   /* Flip the Y axis for the default framebuffer. */
   int y_flip = (rb->Name == 0) ? -1 : 1;
   int y_bias = (rb->Name == 0) ? (rb->Height - 1) : 0;

   irb->map_buffer = malloc(w * h);
   untiled_s8_map = irb->map_buffer;
   tiled_s8_map = intel_region_map(intel, irb->mt->region, mode);

   for (uint32_t pix_y = 0; pix_y < h; pix_y++) {
      for (uint32_t pix_x = 0; pix_x < w; pix_x++) {
	 uint32_t flipped_y = y_flip * (int32_t)(y + pix_y) + y_bias;
	 ptrdiff_t offset = intel_offset_S8(irb->mt->region->pitch,
	                                    x + pix_x,
	                                    flipped_y);
	 untiled_s8_map[pix_y * w + pix_x] = tiled_s8_map[offset];
      }
   }

   *out_map = untiled_s8_map;
   *out_stride = w;

   DBG("%s: rb %d (%s) s8 detiled mapped: (%d, %d) (%dx%d) -> %p/%d\n",
       __FUNCTION__, rb->Name, _mesa_get_format_name(rb->Format),
       x, y, w, h, *out_map, *out_stride);
}

/**
 * \brief Map a depthstencil buffer with separate stencil.
 *
 * A depthstencil renderbuffer, if using separate stencil, consists of a depth
 * renderbuffer and a hidden stencil renderbuffer.  This function maps the
 * depth buffer, whose format is MESA_FORMAT_X8_Z24, through the GTT and
 * returns that as the mapped pointer. The caller need not be aware of the
 * hidden stencil buffer and may safely assume that the mapped pointer points
 * to a MESA_FORMAT_S8_Z24 buffer
 *
 * The consistency between the depth buffer's S8 bits and the hidden stencil
 * buffer is managed within intel_map_renderbuffer() and
 * intel_unmap_renderbuffer() by scattering or gathering the stencil bits
 * according to the map mode.
 *
 * \see intel_map_renderbuffer()
 * \see intel_unmap_renderbuffer_separate_s8z24()
 */
static void
intel_map_renderbuffer_separate_s8z24(struct gl_context *ctx,
				      struct gl_renderbuffer *rb,
				      GLuint x, GLuint y, GLuint w, GLuint h,
				      GLbitfield mode,
				      GLubyte **out_map,
				      GLint *out_stride)
{
   struct intel_context *intel = intel_context(ctx);
   struct intel_renderbuffer *irb = intel_renderbuffer(rb);

   uint8_t *s8z24_map;
   int32_t s8z24_stride;

   struct intel_renderbuffer *s8_irb;
   uint8_t *s8_map;

   assert(rb->Name != 0);
   assert(rb->Format == MESA_FORMAT_S8_Z24);
   assert(irb->wrapped_depth != NULL);
   assert(irb->wrapped_stencil != NULL);

   irb->map_mode = mode;
   irb->map_x = x;
   irb->map_y = y;
   irb->map_w = w;
   irb->map_h = h;

   /* Map with write mode for the gather below. */
   intel_map_renderbuffer_gtt(ctx, irb->wrapped_depth,
			       x, y, w, h, mode | GL_MAP_WRITE_BIT,
			       &s8z24_map, &s8z24_stride);

   s8_irb = intel_renderbuffer(irb->wrapped_stencil);
   s8_map = intel_region_map(intel, s8_irb->mt->region, GL_MAP_READ_BIT);

   /* Gather the stencil buffer into the depth buffer. */
   for (uint32_t pix_y = 0; pix_y < h; ++pix_y) {
      for (uint32_t pix_x = 0; pix_x < w; ++pix_x) {
	 ptrdiff_t s8_offset = intel_offset_S8(s8_irb->mt->region->pitch,
					       x + pix_x,
					       y + pix_y);
	 ptrdiff_t s8z24_offset = pix_y * s8z24_stride
				+ pix_x * 4
				+ 3;
	 s8z24_map[s8z24_offset] = s8_map[s8_offset];
      }
   }

   intel_region_unmap(intel, s8_irb->mt->region);

   *out_map = s8z24_map;
   *out_stride = s8z24_stride;
}

/**
 * \see dd_function_table::MapRenderbuffer
 */
static void
intel_map_renderbuffer(struct gl_context *ctx,
		       struct gl_renderbuffer *rb,
		       GLuint x, GLuint y, GLuint w, GLuint h,
		       GLbitfield mode,
		       GLubyte **out_map,
		       GLint *out_stride)
{
   struct intel_context *intel = intel_context(ctx);
   struct intel_renderbuffer *irb = intel_renderbuffer(rb);

   /* We sometimes get called with this by our intel_span.c usage. */
   if (!irb->mt && !irb->wrapped_depth) {
      *out_map = NULL;
      *out_stride = 0;
      return;
   }

   if (rb->Format == MESA_FORMAT_S8) {
      intel_map_renderbuffer_s8(ctx, rb, x, y, w, h, mode,
			        out_map, out_stride);
   } else if (irb->wrapped_depth) {
      intel_map_renderbuffer_separate_s8z24(ctx, rb, x, y, w, h, mode,
					    out_map, out_stride);
   } else if (intel->gen >= 6 &&
	      !(mode & GL_MAP_WRITE_BIT) &&
	      irb->mt->region->tiling == I915_TILING_X) {
      intel_map_renderbuffer_blit(ctx, rb, x, y, w, h, mode,
				  out_map, out_stride);
   } else {
      intel_map_renderbuffer_gtt(ctx, rb, x, y, w, h, mode,
				 out_map, out_stride);
   }
}

/**
 * \see intel_map_renderbuffer_s8()
 */
static void
intel_unmap_renderbuffer_s8(struct gl_context *ctx,
			    struct gl_renderbuffer *rb)
{
   struct intel_context *intel = intel_context(ctx);
   struct intel_renderbuffer *irb = intel_renderbuffer(rb);

   DBG("%s: rb %d (%s)\n", __FUNCTION__,
       rb->Name, _mesa_get_format_name(rb->Format));

   assert(rb->Format == MESA_FORMAT_S8);

   if (!irb->map_buffer)
      return;

   if (irb->map_mode & GL_MAP_WRITE_BIT) {
      /* The temporary buffer was written to, so we must copy its pixels into
       * the real buffer.
       */
      uint8_t *untiled_s8_map = irb->map_buffer;
      uint8_t *tiled_s8_map = irb->mt->region->bo->virtual;

      /* Flip the Y axis for the default framebuffer. */
      int y_flip = (rb->Name == 0) ? -1 : 1;
      int y_bias = (rb->Name == 0) ? (rb->Height - 1) : 0;

      for (uint32_t pix_y = 0; pix_y < irb->map_h; pix_y++) {
	 for (uint32_t pix_x = 0; pix_x < irb->map_w; pix_x++) {
	    uint32_t flipped_y = y_flip * (int32_t)(pix_y + irb->map_y) + y_bias;
	    ptrdiff_t offset = intel_offset_S8(irb->mt->region->pitch,
	                                       pix_x + irb->map_x,
	                                       flipped_y);
	    tiled_s8_map[offset] =
	       untiled_s8_map[pix_y * irb->map_w + pix_x];
	 }
      }
   }

   intel_region_unmap(intel, irb->mt->region);
   free(irb->map_buffer);
   irb->map_buffer = NULL;
}

/**
 * \brief Unmap a depthstencil renderbuffer with separate stencil.
 *
 * \see intel_map_renderbuffer_separate_s8z24()
 * \see intel_unmap_renderbuffer()
 */
static void
intel_unmap_renderbuffer_separate_s8z24(struct gl_context *ctx,
				        struct gl_renderbuffer *rb)
{
   struct intel_context *intel = intel_context(ctx);
   struct intel_renderbuffer *irb = intel_renderbuffer(rb);
   struct intel_renderbuffer *s8z24_irb;

   assert(rb->Name != 0);
   assert(rb->Format == MESA_FORMAT_S8_Z24);
   assert(irb->wrapped_depth != NULL);
   assert(irb->wrapped_stencil != NULL);

   s8z24_irb = intel_renderbuffer(irb->wrapped_depth);

   if (irb->map_mode & GL_MAP_WRITE_BIT) {
      /* Copy the stencil bits from the depth buffer into the stencil buffer.
       */
      uint32_t map_x = irb->map_x;
      uint32_t map_y = irb->map_y;
      uint32_t map_w = irb->map_w;
      uint32_t map_h = irb->map_h;

      struct intel_renderbuffer *s8_irb;
      uint8_t *s8_map;
      
      s8_irb = intel_renderbuffer(irb->wrapped_stencil);
      s8_map = intel_region_map(intel, s8_irb->mt->region, GL_MAP_WRITE_BIT);

      int32_t s8z24_stride = 4 * s8z24_irb->mt->region->pitch;
      uint8_t *s8z24_map = s8z24_irb->mt->region->bo->virtual
			 + map_y * s8z24_stride
			 + map_x * 4;

      for (uint32_t pix_y = 0; pix_y < map_h; ++pix_y) {
	 for (uint32_t pix_x = 0; pix_x < map_w; ++pix_x) {
	    ptrdiff_t s8_offset = intel_offset_S8(s8_irb->mt->region->pitch,
						  map_x + pix_x,
						  map_y + pix_y);
	    ptrdiff_t s8z24_offset = pix_y * s8z24_stride
				   + pix_x * 4
				   + 3;
	    s8_map[s8_offset] = s8z24_map[s8z24_offset];
	 }
      }

      intel_region_unmap(intel, s8_irb->mt->region);
   }

   drm_intel_gem_bo_unmap_gtt(s8z24_irb->mt->region->bo);
}

/**
 * \see dd_function_table::UnmapRenderbuffer
 */
static void
intel_unmap_renderbuffer(struct gl_context *ctx,
			 struct gl_renderbuffer *rb)
{
   struct intel_renderbuffer *irb = intel_renderbuffer(rb);

   DBG("%s: rb %d (%s)\n", __FUNCTION__,
       rb->Name, _mesa_get_format_name(rb->Format));

   if (rb->Format == MESA_FORMAT_S8) {
      intel_unmap_renderbuffer_s8(ctx, rb);
   } else if (irb->wrapped_depth) {
      intel_unmap_renderbuffer_separate_s8z24(ctx, rb);
   } else if (irb->map_bo) {
      /* Paired with intel_map_renderbuffer_blit(). */
      drm_intel_bo_unmap(irb->map_bo);
      drm_intel_bo_unreference(irb->map_bo);
      irb->map_bo = 0;
   } else {
      /* Paired with intel_map_renderbuffer_gtt(). */
      if (irb->mt) {
	 /* The miptree may be null when intel_map_renderbuffer() is
	  * called from intel_span.c.
	  */
	 drm_intel_gem_bo_unmap_gtt(irb->mt->region->bo);
      }
   }
}

/**
 * Return a pointer to a specific pixel in a renderbuffer.
 */
static void *
intel_get_pointer(struct gl_context * ctx, struct gl_renderbuffer *rb,
                  GLint x, GLint y)
{
   /* By returning NULL we force all software rendering to go through
    * the span routines.
    */
   return NULL;
}


/**
 * Called via glRenderbufferStorageEXT() to set the format and allocate
 * storage for a user-created renderbuffer.
 */
GLboolean
intel_alloc_renderbuffer_storage(struct gl_context * ctx, struct gl_renderbuffer *rb,
                                 GLenum internalFormat,
                                 GLuint width, GLuint height)
{
   struct intel_context *intel = intel_context(ctx);
   struct intel_renderbuffer *irb = intel_renderbuffer(rb);
   int cpp, tiling;

   ASSERT(rb->Name != 0);

   switch (internalFormat) {
   default:
      /* Use the same format-choice logic as for textures.
       * Renderbuffers aren't any different from textures for us,
       * except they're less useful because you can't texture with
       * them.
       */
      rb->Format = intel->ctx.Driver.ChooseTextureFormat(ctx, internalFormat,
							 GL_NONE, GL_NONE);
      break;
   case GL_STENCIL_INDEX:
   case GL_STENCIL_INDEX1_EXT:
   case GL_STENCIL_INDEX4_EXT:
   case GL_STENCIL_INDEX8_EXT:
   case GL_STENCIL_INDEX16_EXT:
      /* These aren't actual texture formats, so force them here. */
      if (intel->has_separate_stencil) {
	 rb->Format = MESA_FORMAT_S8;
      } else {
	 assert(!intel->must_use_separate_stencil);
	 rb->Format = MESA_FORMAT_S8_Z24;
      }
      break;
   }

   rb->Width = width;
   rb->Height = height;
   rb->_BaseFormat = _mesa_base_fbo_format(ctx, internalFormat);
   rb->DataType = intel_mesa_format_to_rb_datatype(rb->Format);
   cpp = _mesa_get_format_bytes(rb->Format);

   intel_flush(ctx);

   intel_miptree_release(&irb->mt);

   DBG("%s: %s: %s (%dx%d)\n", __FUNCTION__,
       _mesa_lookup_enum_by_nr(internalFormat),
       _mesa_get_format_name(rb->Format), width, height);

   tiling = I915_TILING_NONE;
   if (intel->use_texture_tiling) {
      GLenum base_format = _mesa_get_format_base_format(rb->Format);

      if (intel->gen >= 4 && (base_format == GL_DEPTH_COMPONENT ||
			      base_format == GL_STENCIL_INDEX ||
			      base_format == GL_DEPTH_STENCIL))
	 tiling = I915_TILING_Y;
      else
	 tiling = I915_TILING_X;
   }

   if (irb->Base.Format == MESA_FORMAT_S8) {
      /*
       * The stencil buffer is W tiled. However, we request from the kernel a
       * non-tiled buffer because the GTT is incapable of W fencing.
       *
       * The stencil buffer has quirky pitch requirements.  From Vol 2a,
       * 11.5.6.2.1 3DSTATE_STENCIL_BUFFER, field "Surface Pitch":
       *    The pitch must be set to 2x the value computed based on width, as
       *    the stencil buffer is stored with two rows interleaved.
       * To accomplish this, we resort to the nasty hack of doubling the drm
       * region's cpp and halving its height.
       *
       * If we neglect to double the pitch, then render corruption occurs.
       */
      irb->mt = intel_miptree_create_for_renderbuffer(
		  intel,
		  rb->Format,
		  I915_TILING_NONE,
		  cpp * 2,
		  ALIGN(width, 64),
		  ALIGN((height + 1) / 2, 64));
      if (!irb->mt)
	 return false;

   } else if (irb->Base.Format == MESA_FORMAT_S8_Z24
	      && intel->has_separate_stencil) {

      bool ok = true;
      struct gl_renderbuffer *depth_rb;
      struct gl_renderbuffer *stencil_rb;

      depth_rb = intel_create_wrapped_renderbuffer(ctx, width, height,
						   MESA_FORMAT_X8_Z24);
      stencil_rb = intel_create_wrapped_renderbuffer(ctx, width, height,
						     MESA_FORMAT_S8);
      ok = depth_rb && stencil_rb;
      ok = ok && intel_alloc_renderbuffer_storage(ctx, depth_rb,
						  depth_rb->InternalFormat,
						  width, height);
      ok = ok && intel_alloc_renderbuffer_storage(ctx, stencil_rb,
						  stencil_rb->InternalFormat,
						  width, height);

      if (!ok) {
	 if (depth_rb) {
	    intel_delete_renderbuffer(depth_rb);
	 }
	 if (stencil_rb) {
	    intel_delete_renderbuffer(stencil_rb);
	 }
	 return false;
      }

      depth_rb->Wrapped = rb;
      stencil_rb->Wrapped = rb;
      _mesa_reference_renderbuffer(&irb->wrapped_depth, depth_rb);
      _mesa_reference_renderbuffer(&irb->wrapped_stencil, stencil_rb);

   } else {
      irb->mt = intel_miptree_create_for_renderbuffer(intel, rb->Format,
                                                      tiling, cpp,
                                                      width, height);
      if (!irb->mt)
	 return false;

      if (intel->vtbl.is_hiz_depth_format(intel, rb->Format)) {
	 bool ok = intel_miptree_alloc_hiz(intel, irb->mt);
	 if (!ok) {
	    intel_miptree_release(&irb->mt);
	    return false;
	 }
      }
   }

   return true;
}


#if FEATURE_OES_EGL_image
static void
intel_image_target_renderbuffer_storage(struct gl_context *ctx,
					struct gl_renderbuffer *rb,
					void *image_handle)
{
   struct intel_context *intel = intel_context(ctx);
   struct intel_renderbuffer *irb;
   __DRIscreen *screen;
   __DRIimage *image;

   screen = intel->intelScreen->driScrnPriv;
   image = screen->dri2.image->lookupEGLImage(screen, image_handle,
					      screen->loaderPrivate);
   if (image == NULL)
      return;

   /* __DRIimage is opaque to the core so it has to be checked here */
   switch (image->format) {
   case MESA_FORMAT_RGBA8888_REV:
      _mesa_error(&intel->ctx, GL_INVALID_OPERATION,
            "glEGLImageTargetRenderbufferStorage(unsupported image format");
      return;
      break;
   default:
      break;
   }

   irb = intel_renderbuffer(rb);
   intel_miptree_release(&irb->mt);
   irb->mt = intel_miptree_create_for_region(intel,
                                             GL_TEXTURE_2D,
                                             image->format,
                                             image->region);
   if (!irb->mt)
      return;

   rb->InternalFormat = image->internal_format;
   rb->Width = image->region->width;
   rb->Height = image->region->height;
   rb->Format = image->format;
   rb->DataType = image->data_type;
   rb->_BaseFormat = _mesa_base_fbo_format(&intel->ctx,
					   image->internal_format);
}
#endif

/**
 * Called for each hardware renderbuffer when a _window_ is resized.
 * Just update fields.
 * Not used for user-created renderbuffers!
 */
static GLboolean
intel_alloc_window_storage(struct gl_context * ctx, struct gl_renderbuffer *rb,
                           GLenum internalFormat, GLuint width, GLuint height)
{
   ASSERT(rb->Name == 0);
   rb->Width = width;
   rb->Height = height;
   rb->InternalFormat = internalFormat;

   return true;
}


static void
intel_resize_buffers(struct gl_context *ctx, struct gl_framebuffer *fb,
		     GLuint width, GLuint height)
{
   int i;

   _mesa_resize_framebuffer(ctx, fb, width, height);

   fb->Initialized = true; /* XXX remove someday */

   if (fb->Name != 0) {
      return;
   }


   /* Make sure all window system renderbuffers are up to date */
   for (i = BUFFER_FRONT_LEFT; i <= BUFFER_BACK_RIGHT; i++) {
      struct gl_renderbuffer *rb = fb->Attachment[i].Renderbuffer;

      /* only resize if size is changing */
      if (rb && (rb->Width != width || rb->Height != height)) {
	 rb->AllocStorage(ctx, rb, rb->InternalFormat, width, height);
      }
   }
}


/** Dummy function for gl_renderbuffer::AllocStorage() */
static GLboolean
intel_nop_alloc_storage(struct gl_context * ctx, struct gl_renderbuffer *rb,
                        GLenum internalFormat, GLuint width, GLuint height)
{
   _mesa_problem(ctx, "intel_op_alloc_storage should never be called.");
   return false;
}

/**
 * Create a new intel_renderbuffer which corresponds to an on-screen window,
 * not a user-created renderbuffer.
 */
struct intel_renderbuffer *
intel_create_renderbuffer(gl_format format)
{
   GET_CURRENT_CONTEXT(ctx);

   struct intel_renderbuffer *irb;

   irb = CALLOC_STRUCT(intel_renderbuffer);
   if (!irb) {
      _mesa_error(ctx, GL_OUT_OF_MEMORY, "creating renderbuffer");
      return NULL;
   }

   _mesa_init_renderbuffer(&irb->Base, 0);
   irb->Base.ClassID = INTEL_RB_CLASS;
   irb->Base._BaseFormat = _mesa_get_format_base_format(format);
   irb->Base.Format = format;
   irb->Base.InternalFormat = irb->Base._BaseFormat;
   irb->Base.DataType = intel_mesa_format_to_rb_datatype(format);

   /* intel-specific methods */
   irb->Base.Delete = intel_delete_renderbuffer;
   irb->Base.AllocStorage = intel_alloc_window_storage;
   irb->Base.GetPointer = intel_get_pointer;

   return irb;
}


struct gl_renderbuffer*
intel_create_wrapped_renderbuffer(struct gl_context * ctx,
				  int width, int height,
				  gl_format format)
{
   /*
    * The name here is irrelevant, as long as its nonzero, because the
    * renderbuffer never gets entered into Mesa's renderbuffer hash table.
    */
   GLuint name = ~0;

   struct intel_renderbuffer *irb = CALLOC_STRUCT(intel_renderbuffer);
   if (!irb) {
      _mesa_error(ctx, GL_OUT_OF_MEMORY, "creating renderbuffer");
      return NULL;
   }

   struct gl_renderbuffer *rb = &irb->Base;
   _mesa_init_renderbuffer(rb, name);
   rb->ClassID = INTEL_RB_CLASS;
   rb->_BaseFormat = _mesa_get_format_base_format(format);
   rb->Format = format;
   rb->InternalFormat = rb->_BaseFormat;
   rb->DataType = intel_mesa_format_to_rb_datatype(format);
   rb->Width = width;
   rb->Height = height;

   return rb;
}


/**
 * Create a new renderbuffer object.
 * Typically called via glBindRenderbufferEXT().
 */
static struct gl_renderbuffer *
intel_new_renderbuffer(struct gl_context * ctx, GLuint name)
{
   /*struct intel_context *intel = intel_context(ctx); */
   struct intel_renderbuffer *irb;

   irb = CALLOC_STRUCT(intel_renderbuffer);
   if (!irb) {
      _mesa_error(ctx, GL_OUT_OF_MEMORY, "creating renderbuffer");
      return NULL;
   }

   _mesa_init_renderbuffer(&irb->Base, name);
   irb->Base.ClassID = INTEL_RB_CLASS;

   /* intel-specific methods */
   irb->Base.Delete = intel_delete_renderbuffer;
   irb->Base.AllocStorage = intel_alloc_renderbuffer_storage;
   irb->Base.GetPointer = intel_get_pointer;
   /* span routines set in alloc_storage function */

   return &irb->Base;
}


/**
 * Called via glBindFramebufferEXT().
 */
static void
intel_bind_framebuffer(struct gl_context * ctx, GLenum target,
                       struct gl_framebuffer *fb, struct gl_framebuffer *fbread)
{
   if (target == GL_FRAMEBUFFER_EXT || target == GL_DRAW_FRAMEBUFFER_EXT) {
      intel_draw_buffer(ctx);
   }
   else {
      /* don't need to do anything if target == GL_READ_FRAMEBUFFER_EXT */
   }
}


/**
 * Called via glFramebufferRenderbufferEXT().
 */
static void
intel_framebuffer_renderbuffer(struct gl_context * ctx,
                               struct gl_framebuffer *fb,
                               GLenum attachment, struct gl_renderbuffer *rb)
{
   DBG("Intel FramebufferRenderbuffer %u %u\n", fb->Name, rb ? rb->Name : 0);

   intel_flush(ctx);

   _mesa_framebuffer_renderbuffer(ctx, fb, attachment, rb);
   intel_draw_buffer(ctx);
}

static struct intel_renderbuffer*
intel_renderbuffer_wrap_miptree(struct intel_context *intel,
                                struct intel_mipmap_tree *mt,
                                uint32_t level,
                                uint32_t layer,
                                gl_format format,
                                GLenum internal_format);

/**
 * \par Special case for separate stencil
 *
 *     When wrapping a depthstencil texture that uses separate stencil, this
 *     function is recursively called twice: once to create \c
 *     irb->wrapped_depth and again to create \c irb->wrapped_stencil.  On the
 *     call to create \c irb->wrapped_depth, the \c format and \c
 *     internal_format parameters do not match \c mt->format. In that case, \c
 *     mt->format is MESA_FORMAT_S8_Z24 and \c format is \c
 *     MESA_FORMAT_X8_Z24.
 *
 * @return true on success
 */
static bool
intel_renderbuffer_update_wrapper(struct intel_context *intel,
                                  struct intel_renderbuffer *irb,
                                  struct intel_mipmap_tree *mt,
                                  uint32_t level,
                                  uint32_t layer,
                                  gl_format format,
                                  GLenum internal_format)
{
   struct gl_renderbuffer *rb = &irb->Base;

   rb->Format = format;
   rb->InternalFormat = internal_format;
   rb->DataType = intel_mesa_format_to_rb_datatype(rb->Format);
   rb->_BaseFormat = _mesa_get_format_base_format(rb->Format);
   rb->Width = mt->level[level].width;
   rb->Height = mt->level[level].height;

   irb->Base.Delete = intel_delete_renderbuffer;
   irb->Base.AllocStorage = intel_nop_alloc_storage;

   intel_miptree_check_level_layer(mt, level, layer);
   irb->mt_level = level;
   irb->mt_layer = layer;

   if (mt->stencil_mt && _mesa_is_depthstencil_format(rb->InternalFormat)) {
      assert((irb->wrapped_depth == NULL) == (irb->wrapped_stencil == NULL));

      struct intel_renderbuffer *depth_irb;
      struct intel_renderbuffer *stencil_irb;

      if (!irb->wrapped_depth) {
	 depth_irb = intel_renderbuffer_wrap_miptree(intel,
	                                             mt, level, layer,
	                                             MESA_FORMAT_X8_Z24,
	                                             GL_DEPTH_COMPONENT24);
	 stencil_irb = intel_renderbuffer_wrap_miptree(intel,
	                                               mt->stencil_mt,
	                                               level, layer,
	                                               MESA_FORMAT_S8,
	                                               GL_STENCIL_INDEX8);
	 _mesa_reference_renderbuffer(&irb->wrapped_depth, &depth_irb->Base);
	 _mesa_reference_renderbuffer(&irb->wrapped_stencil, &stencil_irb->Base);

	 if (!irb->wrapped_depth || !irb->wrapped_stencil)
	    return false;
      } else {
	 bool ok = true;

	 depth_irb = intel_renderbuffer(irb->wrapped_depth);
	 stencil_irb = intel_renderbuffer(irb->wrapped_stencil);

	 ok &= intel_renderbuffer_update_wrapper(intel,
	                                         depth_irb,
	                                         mt,
	                                         level, layer,
	                                         MESA_FORMAT_X8_Z24,
	                                         GL_DEPTH_COMPONENT24);
	 ok &= intel_renderbuffer_update_wrapper(intel,
	                                         stencil_irb,
	                                         mt->stencil_mt,
	                                         level, layer,
	                                         MESA_FORMAT_S8,
	                                         GL_STENCIL_INDEX8);
	 if (!ok)
	    return false;
      }
   } else {
      intel_miptree_reference(&irb->mt, mt);
      intel_renderbuffer_set_draw_offset(irb);

      if (mt->hiz_mt == NULL &&
	  intel->vtbl.is_hiz_depth_format(intel, rb->Format)) {
	 intel_miptree_alloc_hiz(intel, mt);
         if (!mt->hiz_mt)
            return false;
      }
   }

   return true;
}

/**
 * \brief Wrap a renderbuffer around a single slice of a miptree.
 *
 * Called by glFramebufferTexture*(). This just allocates a
 * ``struct intel_renderbuffer`` then calls
 * intel_renderbuffer_update_wrapper() to do the real work.
 *
 * \see intel_renderbuffer_update_wrapper()
 */
static struct intel_renderbuffer*
intel_renderbuffer_wrap_miptree(struct intel_context *intel,
                                struct intel_mipmap_tree *mt,
                                uint32_t level,
                                uint32_t layer,
                                gl_format format,
                                GLenum internal_format)

{
   const GLuint name = ~0;   /* not significant, but distinct for debugging */
   struct gl_context *ctx = &intel->ctx;
   struct intel_renderbuffer *irb;

   intel_miptree_check_level_layer(mt, level, layer);

   irb = CALLOC_STRUCT(intel_renderbuffer);
   if (!irb) {
      _mesa_error(ctx, GL_OUT_OF_MEMORY, "glFramebufferTexture");
      return NULL;
   }

   _mesa_init_renderbuffer(&irb->Base, name);
   irb->Base.ClassID = INTEL_RB_CLASS;

   if (!intel_renderbuffer_update_wrapper(intel, irb,
                                          mt, level, layer,
                                          format, internal_format)) {
      free(irb);
      return NULL;
   }

   return irb;
}

void
intel_renderbuffer_set_draw_offset(struct intel_renderbuffer *irb)
{
   unsigned int dst_x, dst_y;

   /* compute offset of the particular 2D image within the texture region */
   intel_miptree_get_image_offset(irb->mt,
				  irb->mt_level,
				  0, /* face, which we ignore */
				  irb->mt_layer,
				  &dst_x, &dst_y);

   irb->draw_x = dst_x;
   irb->draw_y = dst_y;
}

/**
 * Rendering to tiled buffers requires that the base address of the
 * buffer be aligned to a page boundary.  We generally render to
 * textures by pointing the surface at the mipmap image level, which
 * may not be aligned to a tile boundary.
 *
 * This function returns an appropriately-aligned base offset
 * according to the tiling restrictions, plus any required x/y offset
 * from there.
 */
uint32_t
intel_renderbuffer_tile_offsets(struct intel_renderbuffer *irb,
				uint32_t *tile_x,
				uint32_t *tile_y)
{
   struct intel_region *region = irb->mt->region;
   int cpp = region->cpp;
   uint32_t pitch = region->pitch * cpp;

   if (region->tiling == I915_TILING_NONE) {
      *tile_x = 0;
      *tile_y = 0;
      return irb->draw_x * cpp + irb->draw_y * pitch;
   } else if (region->tiling == I915_TILING_X) {
      *tile_x = irb->draw_x % (512 / cpp);
      *tile_y = irb->draw_y % 8;
      return ((irb->draw_y / 8) * (8 * pitch) +
	      (irb->draw_x - *tile_x) / (512 / cpp) * 4096);
   } else {
      assert(region->tiling == I915_TILING_Y);
      *tile_x = irb->draw_x % (128 / cpp);
      *tile_y = irb->draw_y % 32;
      return ((irb->draw_y / 32) * (32 * pitch) +
	      (irb->draw_x - *tile_x) / (128 / cpp) * 4096);
   }
}

#ifndef I915
static bool
need_tile_offset_workaround(struct brw_context *brw,
			    struct intel_renderbuffer *irb)
{
   uint32_t tile_x, tile_y;

   if (brw->has_surface_tile_offset)
      return false;

   intel_renderbuffer_tile_offsets(irb, &tile_x, &tile_y);

   return tile_x != 0 || tile_y != 0;
}
#endif

/**
 * Called by glFramebufferTexture[123]DEXT() (and other places) to
 * prepare for rendering into texture memory.  This might be called
 * many times to choose different texture levels, cube faces, etc
 * before intel_finish_render_texture() is ever called.
 */
static void
intel_render_texture(struct gl_context * ctx,
                     struct gl_framebuffer *fb,
                     struct gl_renderbuffer_attachment *att)
{
   struct intel_context *intel = intel_context(ctx);
   struct gl_texture_image *image = _mesa_get_attachment_teximage(att);
   struct intel_renderbuffer *irb = intel_renderbuffer(att->Renderbuffer);
   struct intel_texture_image *intel_image = intel_texture_image(image);
   struct intel_mipmap_tree *mt = intel_image->mt;

   (void) fb;

   int layer;
   if (att->CubeMapFace > 0) {
      assert(att->Zoffset == 0);
      layer = att->CubeMapFace;
   } else {
      layer = att->Zoffset;
   }

   if (!intel_image->mt) {
      /* Fallback on drawing to a texture that doesn't have a miptree
       * (has a border, width/height 0, etc.)
       */
      _mesa_reference_renderbuffer(&att->Renderbuffer, NULL);
      _swrast_render_texture(ctx, fb, att);
      return;
   }
   else if (!irb) {
      irb = intel_renderbuffer_wrap_miptree(intel,
                                            mt,
                                            att->TextureLevel,
                                            layer,
                                            image->TexFormat,
                                            image->InternalFormat);

      if (irb) {
         /* bind the wrapper to the attachment point */
         _mesa_reference_renderbuffer(&att->Renderbuffer, &irb->Base);
      }
      else {
         /* fallback to software rendering */
         _swrast_render_texture(ctx, fb, att);
         return;
      }
   }

   if (!intel_renderbuffer_update_wrapper(intel, irb,
                                          mt, att->TextureLevel, layer,
                                          image->TexFormat,
                                          image->InternalFormat)) {
       _mesa_reference_renderbuffer(&att->Renderbuffer, NULL);
       _swrast_render_texture(ctx, fb, att);
       return;
   }

   DBG("Begin render %s texture tex=%u w=%d h=%d refcount=%d\n",
       _mesa_get_format_name(image->TexFormat),
       att->Texture->Name, image->Width, image->Height,
       irb->Base.RefCount);

   intel_image->used_as_render_target = true;

#ifndef I915
   if (need_tile_offset_workaround(brw_context(ctx), irb)) {
      /* Original gen4 hardware couldn't draw to a non-tile-aligned
       * destination in a miptree unless you actually setup your
       * renderbuffer as a miptree and used the fragile
       * lod/array_index/etc. controls to select the image.  So,
       * instead, we just make a new single-level miptree and render
       * into that.
       */
      struct intel_context *intel = intel_context(ctx);
      struct intel_mipmap_tree *new_mt;
      int width, height, depth;

      intel_miptree_get_dimensions_for_image(image, &width, &height, &depth);

      new_mt = intel_miptree_create(intel, image->TexObject->Target,
				    intel_image->base.Base.TexFormat,
				    intel_image->base.Base.Level,
				    intel_image->base.Base.Level,
                                    width, height, depth,
				    true);

      intel_miptree_copy_teximage(intel, intel_image, new_mt);
      intel_renderbuffer_set_draw_offset(irb);

      intel_miptree_reference(&irb->mt, intel_image->mt);
      intel_miptree_release(&new_mt);
   }
#endif
   /* update drawing region, etc */
   intel_draw_buffer(ctx);
}


/**
 * Called by Mesa when rendering to a texture is done.
 */
static void
intel_finish_render_texture(struct gl_context * ctx,
                            struct gl_renderbuffer_attachment *att)
{
   struct intel_context *intel = intel_context(ctx);
   struct gl_texture_object *tex_obj = att->Texture;
   struct gl_texture_image *image =
      tex_obj->Image[att->CubeMapFace][att->TextureLevel];
   struct intel_texture_image *intel_image = intel_texture_image(image);

   DBG("Finish render %s texture tex=%u\n",
       _mesa_get_format_name(image->TexFormat), att->Texture->Name);

   /* Flag that this image may now be validated into the object's miptree. */
   if (intel_image)
      intel_image->used_as_render_target = false;

   /* Since we've (probably) rendered to the texture and will (likely) use
    * it in the texture domain later on in this batchbuffer, flush the
    * batch.  Once again, we wish for a domain tracker in libdrm to cover
    * usage inside of a batchbuffer like GEM does in the kernel.
    */
   intel_batchbuffer_emit_mi_flush(intel);
}

/**
 * Do additional "completeness" testing of a framebuffer object.
 */
static void
intel_validate_framebuffer(struct gl_context *ctx, struct gl_framebuffer *fb)
{
   struct intel_context *intel = intel_context(ctx);
   const struct intel_renderbuffer *depthRb =
      intel_get_renderbuffer(fb, BUFFER_DEPTH);
   const struct intel_renderbuffer *stencilRb =
      intel_get_renderbuffer(fb, BUFFER_STENCIL);
   int i;

   /*
    * The depth and stencil renderbuffers are the same renderbuffer or wrap
    * the same texture.
    */
   if (depthRb && stencilRb) {
      bool depth_stencil_are_same;
      if (depthRb == stencilRb)
	 depth_stencil_are_same = true;
      else if ((fb->Attachment[BUFFER_DEPTH].Type == GL_TEXTURE) &&
	       (fb->Attachment[BUFFER_STENCIL].Type == GL_TEXTURE) &&
	       (fb->Attachment[BUFFER_DEPTH].Texture->Name ==
		fb->Attachment[BUFFER_STENCIL].Texture->Name))
	 depth_stencil_are_same = true;
      else
	 depth_stencil_are_same = false;

      if (!intel->has_separate_stencil && !depth_stencil_are_same) {
	 fb->_Status = GL_FRAMEBUFFER_UNSUPPORTED_EXT;
      }
   }

   for (i = 0; i < Elements(fb->Attachment); i++) {
      struct gl_renderbuffer *rb;
      struct intel_renderbuffer *irb;

      if (fb->Attachment[i].Type == GL_NONE)
	 continue;

      /* A supported attachment will have a Renderbuffer set either
       * from being a Renderbuffer or being a texture that got the
       * intel_wrap_texture() treatment.
       */
      rb = fb->Attachment[i].Renderbuffer;
      if (rb == NULL) {
	 DBG("attachment without renderbuffer\n");
	 fb->_Status = GL_FRAMEBUFFER_UNSUPPORTED_EXT;
	 continue;
      }

      irb = intel_renderbuffer(rb);
      if (irb == NULL) {
	 DBG("software rendering renderbuffer\n");
	 fb->_Status = GL_FRAMEBUFFER_UNSUPPORTED_EXT;
	 continue;
      }

      if (!intel->vtbl.render_target_supported(intel, irb->Base.Format)) {
	 DBG("Unsupported HW texture/renderbuffer format attached: %s\n",
	     _mesa_get_format_name(irb->Base.Format));
	 fb->_Status = GL_FRAMEBUFFER_UNSUPPORTED_EXT;
      }

#ifdef I915
      if (!intel_span_supports_format(irb->Base.Format)) {
	 DBG("Unsupported swrast texture/renderbuffer format attached: %s\n",
	     _mesa_get_format_name(irb->Base.Format));
	 fb->_Status = GL_FRAMEBUFFER_UNSUPPORTED_EXT;
      }
#endif
   }
}

/**
 * Try to do a glBlitFramebuffer using glCopyTexSubImage2D
 * We can do this when the dst renderbuffer is actually a texture and
 * there is no scaling, mirroring or scissoring.
 *
 * \return new buffer mask indicating the buffers left to blit using the
 *         normal path.
 */
static GLbitfield
intel_blit_framebuffer_copy_tex_sub_image(struct gl_context *ctx,
                                          GLint srcX0, GLint srcY0,
                                          GLint srcX1, GLint srcY1,
                                          GLint dstX0, GLint dstY0,
                                          GLint dstX1, GLint dstY1,
                                          GLbitfield mask, GLenum filter)
{
   if (mask & GL_COLOR_BUFFER_BIT) {
      const struct gl_framebuffer *drawFb = ctx->DrawBuffer;
      const struct gl_framebuffer *readFb = ctx->ReadBuffer;
      const struct gl_renderbuffer_attachment *drawAtt =
         &drawFb->Attachment[drawFb->_ColorDrawBufferIndexes[0]];

      /* If the source and destination are the same size with no
         mirroring, the rectangles are within the size of the
         texture and there is no scissor then we can use
         glCopyTexSubimage2D to implement the blit. This will end
         up as a fast hardware blit on some drivers */
      if (drawAtt && drawAtt->Texture &&
          srcX0 - srcX1 == dstX0 - dstX1 &&
          srcY0 - srcY1 == dstY0 - dstY1 &&
          srcX1 >= srcX0 &&
          srcY1 >= srcY0 &&
          srcX0 >= 0 && srcX1 <= readFb->Width &&
          srcY0 >= 0 && srcY1 <= readFb->Height &&
          dstX0 >= 0 && dstX1 <= drawFb->Width &&
          dstY0 >= 0 && dstY1 <= drawFb->Height &&
          !ctx->Scissor.Enabled) {
         const struct gl_texture_object *texObj = drawAtt->Texture;
         const GLuint dstLevel = drawAtt->TextureLevel;
         const GLenum target = texObj->Target;

         struct gl_texture_image *texImage =
            _mesa_select_tex_image(ctx, texObj, target, dstLevel);

         if (intel_copy_texsubimage(intel_context(ctx),
                                    intel_texture_image(texImage),
                                    dstX0, dstY0,
                                    srcX0, srcY0,
                                    srcX1 - srcX0, /* width */
                                    srcY1 - srcY0))
            mask &= ~GL_COLOR_BUFFER_BIT;
      }
   }

   return mask;
}

static void
intel_blit_framebuffer(struct gl_context *ctx,
                       GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1,
                       GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1,
                       GLbitfield mask, GLenum filter)
{
   /* Try faster, glCopyTexSubImage2D approach first which uses the BLT. */
   mask = intel_blit_framebuffer_copy_tex_sub_image(ctx,
                                                    srcX0, srcY0, srcX1, srcY1,
                                                    dstX0, dstY0, dstX1, dstY1,
                                                    mask, filter);
   if (mask == 0x0)
      return;

   _mesa_meta_BlitFramebuffer(ctx,
                              srcX0, srcY0, srcX1, srcY1,
                              dstX0, dstY0, dstX1, dstY1,
                              mask, filter);
}

void
intel_renderbuffer_set_needs_hiz_resolve(struct intel_renderbuffer *irb)
{
   if (irb->mt) {
      intel_miptree_slice_set_needs_hiz_resolve(irb->mt,
                                                irb->mt_level,
                                                irb->mt_layer);
   } else if (irb->wrapped_depth) {
      intel_renderbuffer_set_needs_hiz_resolve(
	    intel_renderbuffer(irb->wrapped_depth));
   } else {
      return;
   }
}

void
intel_renderbuffer_set_needs_depth_resolve(struct intel_renderbuffer *irb)
{
   if (irb->mt) {
      intel_miptree_slice_set_needs_depth_resolve(irb->mt,
                                                  irb->mt_level,
                                                  irb->mt_layer);
   } else if (irb->wrapped_depth) {
      intel_renderbuffer_set_needs_depth_resolve(
	    intel_renderbuffer(irb->wrapped_depth));
   } else {
      return;
   }
}

bool
intel_renderbuffer_resolve_hiz(struct intel_context *intel,
			       struct intel_renderbuffer *irb)
{
   if (irb->mt)
      return intel_miptree_slice_resolve_hiz(intel,
                                             irb->mt,
                                             irb->mt_level,
                                             irb->mt_layer);
   if (irb->wrapped_depth)
      return intel_renderbuffer_resolve_hiz(intel,
					    intel_renderbuffer(irb->wrapped_depth));

   return false;
}

bool
intel_renderbuffer_resolve_depth(struct intel_context *intel,
				 struct intel_renderbuffer *irb)
{
   if (irb->mt)
      return intel_miptree_slice_resolve_depth(intel,
                                               irb->mt,
                                               irb->mt_level,
                                               irb->mt_layer);

   if (irb->wrapped_depth)
      return intel_renderbuffer_resolve_depth(intel,
                                              intel_renderbuffer(irb->wrapped_depth));

   return false;
}

/**
 * Do one-time context initializations related to GL_EXT_framebuffer_object.
 * Hook in device driver functions.
 */
void
intel_fbo_init(struct intel_context *intel)
{
   intel->ctx.Driver.NewFramebuffer = intel_new_framebuffer;
   intel->ctx.Driver.NewRenderbuffer = intel_new_renderbuffer;
   intel->ctx.Driver.MapRenderbuffer = intel_map_renderbuffer;
   intel->ctx.Driver.UnmapRenderbuffer = intel_unmap_renderbuffer;
   intel->ctx.Driver.BindFramebuffer = intel_bind_framebuffer;
   intel->ctx.Driver.FramebufferRenderbuffer = intel_framebuffer_renderbuffer;
   intel->ctx.Driver.RenderTexture = intel_render_texture;
   intel->ctx.Driver.FinishRenderTexture = intel_finish_render_texture;
   intel->ctx.Driver.ResizeBuffers = intel_resize_buffers;
   intel->ctx.Driver.ValidateFramebuffer = intel_validate_framebuffer;
   intel->ctx.Driver.BlitFramebuffer = intel_blit_framebuffer;

#if FEATURE_OES_EGL_image
   intel->ctx.Driver.EGLImageTargetRenderbufferStorage =
      intel_image_target_renderbuffer_storage;
#endif   
}
