
/**************************************************************************

Copyright 1998-1999 Precision Insight, Inc., Cedar Park, Texas.
All Rights Reserved.

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sub license, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice (including the
next paragraph) shall be included in all copies or substantial portions
of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
IN NO EVENT SHALL PRECISION INSIGHT AND/OR ITS SUPPLIERS BE LIABLE FOR
ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

**************************************************************************/

/*
 * Authors:
 *   Kevin E. Martin <kevin@precisioninsight.com>
 *
 * When we're building the XMesa driver for use in the X server (as the
 * indirect render) we include this file when building the xm_*.c files.
 * We need to define some types and macros differently when building
 * in the Xserver vs. stand-alone Mesa.
 */

#ifndef _XMESA_XF86_H_
#define _XMESA_XF86_H_

#include "GL/glxtokens.h"
#include "scrnintstr.h"
#include "pixmapstr.h"
#include "gcstruct.h"
#include "servermd.h"


typedef struct _XMesaImageRec {
    int width, height;
    char *data;
    int bytes_per_line; /* Padded to 32 bits */
    int bits_per_pixel;
} XMesaImage;

typedef ScreenRec   XMesaDisplay;
typedef PixmapPtr   XMesaPixmap;
typedef ColormapPtr XMesaColormap;
typedef DrawablePtr XMesaDrawable;
typedef WindowPtr   XMesaWindow;
typedef GCPtr       XMesaGC;
typedef VisualPtr   XMesaVisualInfo;
typedef DDXPointRec XMesaPoint;
typedef xColorItem  XMesaColor;

#define XMesaSetGeneric(__d,__gc,__val,__mask) \
do { \
    CARD32 __v[1]; \
    (void) __d; \
    __v[0] = __val; \
    dixChangeGC(NullClient, __gc, __mask, __v, NULL); \
} while (0)

#define XMesaSetGenericPtr(__d,__gc,__pval,__mask) \
do { \
    ChangeGCVal __v[1]; \
    (void) __d; \
    __v[0].ptr = __pval; \
    dixChangeGC(NullClient, __gc, __mask, NULL, __v); \
} while (0)

#define XMesaSetForeground(d,gc,v) XMesaSetGeneric(d,gc,v,GCForeground)
#define XMesaSetBackground(d,gc,v) XMesaSetGeneric(d,gc,v,GCBackground)
#define XMesaSetPlaneMask(d,gc,v)  XMesaSetGeneric(d,gc,v,GCPlaneMask)
#define XMesaSetFunction(d,gc,v)   XMesaSetGeneric(d,gc,v,GCFunction)
#define XMesaSetFillStyle(d,gc,v)  XMesaSetGeneric(d,gc,v,GCFillStyle)

#define XMesaSetTile(d,gc,v)       XMesaSetGenericPtr(d,gc,v,GCTile)

#define XMesaDrawPoint(__d,__b,__gc,__x,__y) \
do { \
    XMesaPoint __p[1]; \
    (void) __d; \
    __p[0].x = __x; \
    __p[0].y = __y; \
    ValidateGC(__b, __gc); \
    (*gc->ops->PolyPoint)(__b, __gc, CoordModeOrigin, 1, __p); \
} while (0)

#define XMesaDrawPoints(__d,__b,__gc,__p,__n,__m) \
do { \
    (void) __d; \
    ValidateGC(__b, __gc); \
    (*gc->ops->PolyPoint)(__b, __gc, __m, __n, __p); \
} while (0)

#define XMesaDrawLine(__d, __b, __gc, __x0, __y0, __x1, __y1) \
do { \
    XMesaPoint __p[2]; \
    (void) __d; \
    __p[0].x = __x0; \
    __p[0].y = __y0; \
    __p[1].x = __x1; \
    __p[1].y = __y1; \
    ValidateGC(__b, __gc); \
    (*gc->ops->PolyLines)(__b, __gc, CoordModeOrigin, 2, __p); \
} while (0)

#define XMesaFillRectangle(__d,__b,__gc,__x,__y,__w,__h) \
do { \
    xRectangle __r[1]; \
    (void) __d; \
    ValidateGC((DrawablePtr)__b, __gc); \
    __r[0].x = __x; \
    __r[0].y = __y; \
    __r[0].width = __w; \
    __r[0].height = __h; \
    (*__gc->ops->PolyFillRect)((DrawablePtr)__b, __gc, 1, __r); \
} while (0)

static _X_INLINE XMesaImage *XMesaGetImage(XMesaDisplay *dpy, PixmapPtr p, int x,
					int y, unsigned int width,
					unsigned int height,
					unsigned long plane_mask, int format)
{
    XMesaImage *img = Xcalloc(sizeof(*img));

    img->width = p->drawable.width;
    img->height = p->drawable.height;
    img->bits_per_pixel = p->drawable.bitsPerPixel;
    img->bytes_per_line = PixmapBytePad(width, p->drawable.depth);
    img->data = malloc(height * img->bytes_per_line);

    /* Assumes: Images are always in ZPixmap format */
    (*p->drawable.pScreen->GetImage)(&p->drawable, x, y, width, height,
				     plane_mask, ZPixmap, img->data);

    return img;
}

#define XMesaPutImage(__d,__b,__gc,__i,__sx,__sy,__x,__y,__w,__h) \
do { \
    /* Assumes: Images are always in ZPixmap format */ \
    (void) __d; \
    ASSERT(!__sx && !__sy); /* The SubImage case */     \
    ValidateGC(__b, __gc); \
    (*__gc->ops->PutImage)(__b, __gc, ((XMesaDrawable)(__b))->depth, \
			   __x, __y, __w, __h, 0, ZPixmap, \
			   ((XMesaImage *)(__i))->data); \
} while (0)

#define XMesaCopyArea(__d,__sb,__db,__gc,__sx,__sy,__w,__h,__x,__y) \
do { \
    (void) __d; \
    ValidateGC(__db, __gc); \
    (*__gc->ops->CopyArea)((DrawablePtr)__sb, __db, __gc, \
			   __sx, __sy, __w, __h, __x, __y); \
} while (0)


/* CreatePixmap returns a PixmapPtr; so, it cannot be inside braces */
#ifdef CREATE_PIXMAP_USAGE_SCRATCH
#define XMesaCreatePixmap(__d,__b,__w,__h,__depth) \
  (*__d->CreatePixmap)(__d, __w, __h, __depth, 0)
#else
#define XMesaCreatePixmap(__d,__b,__w,__h,__depth) \
    (*__d->CreatePixmap)(__d, __w, __h, __depth)
#endif

#define XMesaFreePixmap(__d,__b) \
    (*__d->DestroyPixmap)(__b)

#define XMesaFreeGC(__d,__gc) \
do { \
    (void) __d; \
    FreeScratchGC(__gc); \
} while (0)

#define GET_COLORMAP_SIZE(__v)  __v->ColormapEntries
#define GET_REDMASK(__v)        __v->mesa_visual.redMask
#define GET_GREENMASK(__v)      __v->mesa_visual.greenMask
#define GET_BLUEMASK(__v)       __v->mesa_visual.blueMask
#define GET_VISUAL_DEPTH(__v)   __v->nplanes
#define GET_BLACK_PIXEL(__v)    __v->display->blackPixel
#define CHECK_BYTE_ORDER(__v)   GL_TRUE

#endif
