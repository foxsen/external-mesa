/*
 * Copyright (C) 2009 Maciej Cencora <m.cencora@gmail.com>
 *
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE COPYRIGHT OWNER(S) AND/OR ITS SUPPLIERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include "radeon_common.h"
#include "radeon_texture.h"

#include "main/enums.h"
#include "main/image.h"
#include "main/teximage.h"
#include "main/texstate.h"
#include "drivers/common/meta.h"

#include "radeon_mipmap_tree.h"

static GLboolean
do_copy_texsubimage(struct gl_context *ctx,
                    GLenum target, GLint level,
                    struct radeon_tex_obj *tobj,
                    radeon_texture_image *timg,
                    GLint dstx, GLint dsty,
                    GLint x, GLint y,
                    GLsizei width, GLsizei height)
{
    radeonContextPtr radeon = RADEON_CONTEXT(ctx);
    struct radeon_renderbuffer *rrb;
    unsigned src_bpp;
    unsigned dst_bpp;
    gl_format src_mesaformat;
    gl_format dst_mesaformat;
    unsigned flip_y;

    if (!radeon->vtbl.blit) {
        return GL_FALSE;
    }

    if (_mesa_get_format_bits(timg->base.Base.TexFormat, GL_DEPTH_BITS) > 0) {
        if (ctx->ReadBuffer->_DepthBuffer && ctx->ReadBuffer->_DepthBuffer->Wrapped) {
            rrb = radeon_renderbuffer(ctx->ReadBuffer->_DepthBuffer->Wrapped);
        } else {
            rrb = radeon_renderbuffer(ctx->ReadBuffer->_DepthBuffer);
        }
        flip_y = ctx->ReadBuffer->Attachment[BUFFER_DEPTH].Type == GL_NONE;
    } else {
        rrb = radeon_renderbuffer(ctx->ReadBuffer->_ColorReadBuffer);
        flip_y = ctx->ReadBuffer->Attachment[BUFFER_COLOR0].Type == GL_NONE;
    }

    // This is software renderbuffer, fallback to swrast
    if (!rrb) {
        return GL_FALSE;
    }

    if (!timg->mt) {
        radeon_validate_texture_miptree(ctx, &tobj->base);
    }

    assert(rrb->bo);
    assert(timg->mt);
    assert(timg->mt->bo);
    assert(timg->base.Base.Width >= dstx + width);
    assert(timg->base.Base.Height >= dsty + height);

    intptr_t src_offset = rrb->draw_offset;
    intptr_t dst_offset = radeon_miptree_image_offset(timg->mt, _mesa_tex_target_to_face(target), level);

    if (0) {
        fprintf(stderr, "%s: copying to face %d, level %d\n",
                __FUNCTION__, _mesa_tex_target_to_face(target), level);
        fprintf(stderr, "to: x %d, y %d, offset %d\n", dstx, dsty, (uint32_t) dst_offset);
        fprintf(stderr, "from (%dx%d) width %d, height %d, offset %d, pitch %d\n",
                x, y, rrb->base.Width, rrb->base.Height, (uint32_t) src_offset, rrb->pitch/rrb->cpp);
        fprintf(stderr, "src size %d, dst size %d\n", rrb->bo->size, timg->mt->bo->size);

    }

    src_mesaformat = rrb->base.Format;
    dst_mesaformat = timg->base.Base.TexFormat;
    src_bpp = _mesa_get_format_bytes(src_mesaformat);
    dst_bpp = _mesa_get_format_bytes(dst_mesaformat);
    if (!radeon->vtbl.check_blit(dst_mesaformat)) {
	    /* depth formats tend to be special */
	    if (_mesa_get_format_bits(dst_mesaformat, GL_DEPTH_BITS) > 0)
		    return GL_FALSE;

	    if (src_bpp != dst_bpp)
		    return GL_FALSE;

	    switch (dst_bpp) {
	    case 2:
		    src_mesaformat = MESA_FORMAT_RGB565;
		    dst_mesaformat = MESA_FORMAT_RGB565;
		    break;
	    case 4:
		    src_mesaformat = MESA_FORMAT_ARGB8888;
		    dst_mesaformat = MESA_FORMAT_ARGB8888;
		    break;
	    case 1:
		    src_mesaformat = MESA_FORMAT_A8;
		    dst_mesaformat = MESA_FORMAT_A8;
		    break;
	    default:
		    return GL_FALSE;
	    }
    }

    /* blit from src buffer to texture */
    return radeon->vtbl.blit(ctx, rrb->bo, src_offset, src_mesaformat, rrb->pitch/rrb->cpp,
                             rrb->base.Width, rrb->base.Height, x, y,
                             timg->mt->bo, dst_offset, dst_mesaformat,
                             timg->mt->levels[level].rowstride / dst_bpp,
                             timg->base.Base.Width, timg->base.Base.Height,
                             dstx, dsty, width, height, flip_y);
}

void
radeonCopyTexSubImage2D(struct gl_context *ctx, GLenum target, GLint level,
                        GLint xoffset, GLint yoffset,
                        GLint x, GLint y,
                        GLsizei width, GLsizei height)
{
    struct gl_texture_unit *texUnit = _mesa_get_current_tex_unit(ctx);
    struct gl_texture_object *texObj = _mesa_select_tex_object(ctx, texUnit, target);
    struct gl_texture_image *texImage = _mesa_select_tex_image(ctx, texObj, target, level);

    radeonContextPtr radeon = RADEON_CONTEXT(ctx);
    radeon_prepare_render(radeon);

    if (!do_copy_texsubimage(ctx, target, level,
                             radeon_tex_obj(texObj), (radeon_texture_image *)texImage,
                             xoffset, yoffset, x, y, width, height)) {

        radeon_print(RADEON_FALLBACKS, RADEON_NORMAL,
                     "Falling back to sw for glCopyTexSubImage2D\n");

        _mesa_meta_CopyTexSubImage2D(ctx, target, level,
                                     xoffset, yoffset, x, y, width, height);
    }
}