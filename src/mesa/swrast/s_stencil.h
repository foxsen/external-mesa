/*
 * Mesa 3-D graphics library
 * Version:  6.3
 *
 * Copyright (C) 1999-2005  Brian Paul   All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * BRIAN PAUL BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN
 * AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */


#ifndef S_STENCIL_H
#define S_STENCIL_H


#include "main/mtypes.h"
#include "s_span.h"



extern GLboolean
_swrast_stencil_and_ztest_span(struct gl_context *ctx, SWspan *span);


extern void
_swrast_read_stencil_span(struct gl_context *ctx, struct gl_renderbuffer *rb,
                          GLint n, GLint x, GLint y, GLubyte stencil[]);


extern void
_swrast_write_stencil_span( struct gl_context *ctx, GLint n, GLint x, GLint y,
                          const GLubyte stencil[] );


extern void
_swrast_clear_stencil_buffer( struct gl_context *ctx, struct gl_renderbuffer *rb );


#endif
