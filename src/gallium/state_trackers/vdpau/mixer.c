/**************************************************************************
 *
 * Copyright 2010 Thomas Balling Sørensen.
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

#include <vdpau/vdpau.h>

#include "util/u_memory.h"
#include "util/u_debug.h"

#include "vl/vl_csc.h"

#include "vdpau_private.h"

/**
 * Create a VdpVideoMixer.
 */
VdpStatus
vlVdpVideoMixerCreate(VdpDevice device,
                      uint32_t feature_count,
                      VdpVideoMixerFeature const *features,
                      uint32_t parameter_count,
                      VdpVideoMixerParameter const *parameters,
                      void const *const *parameter_values,
                      VdpVideoMixer *mixer)
{
   vlVdpVideoMixer *vmixer = NULL;
   VdpStatus ret;
   float csc[16];

   VDPAU_MSG(VDPAU_TRACE, "[VDPAU] Creating VideoMixer\n");

   vlVdpDevice *dev = vlGetDataHTAB(device);
   if (!dev)
      return VDP_STATUS_INVALID_HANDLE;

   vmixer = CALLOC(1, sizeof(vlVdpVideoMixer));
   if (!vmixer)
      return VDP_STATUS_RESOURCES;

   vmixer->device = dev;
   vl_compositor_init(&vmixer->compositor, dev->context->pipe);

   vl_csc_get_matrix
   (
      debug_get_bool_option("G3DVL_NO_CSC", FALSE) ?
      VL_CSC_COLOR_STANDARD_IDENTITY : VL_CSC_COLOR_STANDARD_BT_601,
      NULL, true, csc
   );
   vl_compositor_set_csc_matrix(&vmixer->compositor, csc);

   /*
    * TODO: Handle features and parameters
    */

   *mixer = vlAddDataHTAB(vmixer);
   if (*mixer == 0) {
      ret = VDP_STATUS_ERROR;
      goto no_handle;
   }

   return VDP_STATUS_OK;

no_handle:
   return ret;
}

/**
 * Destroy a VdpVideoMixer.
 */
VdpStatus
vlVdpVideoMixerDestroy(VdpVideoMixer mixer)
{
   vlVdpVideoMixer *vmixer;

   VDPAU_MSG(VDPAU_TRACE, "[VDPAU] Destroying VideoMixer\n");

   vmixer = vlGetDataHTAB(mixer);
   if (!vmixer)
      return VDP_STATUS_INVALID_HANDLE;

   vl_compositor_cleanup(&vmixer->compositor);

   FREE(vmixer);

   return VDP_STATUS_OK;
}

/**
 * Enable or disable features.
 */
VdpStatus
vlVdpVideoMixerSetFeatureEnables(VdpVideoMixer mixer,
                                 uint32_t feature_count,
                                 VdpVideoMixerFeature const *features,
                                 VdpBool const *feature_enables)
{
   VDPAU_MSG(VDPAU_TRACE, "[VDPAU] Setting VideoMixer features\n");

   if (!(features && feature_enables))
      return VDP_STATUS_INVALID_POINTER;

   vlVdpVideoMixer *vmixer = vlGetDataHTAB(mixer);
   if (!vmixer)
      return VDP_STATUS_INVALID_HANDLE;

   /*
    * TODO: Set features
    */

   return VDP_STATUS_OK;
}

/**
 * Perform a video post-processing and compositing operation.
 */
VdpStatus vlVdpVideoMixerRender(VdpVideoMixer mixer,
                                VdpOutputSurface background_surface,
                                VdpRect const *background_source_rect,
                                VdpVideoMixerPictureStructure current_picture_structure,
                                uint32_t video_surface_past_count,
                                VdpVideoSurface const *video_surface_past,
                                VdpVideoSurface video_surface_current,
                                uint32_t video_surface_future_count,
                                VdpVideoSurface const *video_surface_future,
                                VdpRect const *video_source_rect,
                                VdpOutputSurface destination_surface,
                                VdpRect const *destination_rect,
                                VdpRect const *destination_video_rect,
                                uint32_t layer_count,
                                VdpLayer const *layers)
{
   struct pipe_video_rect src_rect;

   vlVdpVideoMixer *vmixer;
   vlVdpSurface *surf;
   vlVdpOutputSurface *dst;

   vmixer = vlGetDataHTAB(mixer);
   if (!vmixer)
      return VDP_STATUS_INVALID_HANDLE;

   surf = vlGetDataHTAB(video_surface_current);
   if (!surf)
      return VDP_STATUS_INVALID_HANDLE;

   dst = vlGetDataHTAB(destination_surface);
   if (!dst)
      return VDP_STATUS_INVALID_HANDLE;

   vl_compositor_clear_layers(&vmixer->compositor);
   vl_compositor_set_buffer_layer(&vmixer->compositor, 0, surf->video_buffer,
                                  RectToPipe(video_source_rect, &src_rect), NULL);
   vl_compositor_render(&vmixer->compositor, dst->surface, NULL, NULL, false);

   return VDP_STATUS_OK;
}

/**
 * Set attribute values.
 */
VdpStatus
vlVdpVideoMixerSetAttributeValues(VdpVideoMixer mixer,
                                  uint32_t attribute_count,
                                  VdpVideoMixerAttribute const *attributes,
                                  void const *const *attribute_values)
{
   if (!(attributes && attribute_values))
      return VDP_STATUS_INVALID_POINTER;

   vlVdpVideoMixer *vmixer = vlGetDataHTAB(mixer);
   if (!vmixer)
      return VDP_STATUS_INVALID_HANDLE;

   /*
    * TODO: Implement the function
    */

   return VDP_STATUS_OK;
}

/**
 * Retrieve whether features were requested at creation time.
 */
VdpStatus
vlVdpVideoMixerGetFeatureSupport(VdpVideoMixer mixer,
                                 uint32_t feature_count,
                                 VdpVideoMixerFeature const *features,
                                 VdpBool *feature_supports)
{
   return VDP_STATUS_NO_IMPLEMENTATION;
}

/**
 * Retrieve whether features are enabled.
 */
VdpStatus
vlVdpVideoMixerGetFeatureEnables(VdpVideoMixer mixer,
                                 uint32_t feature_count,
                                 VdpVideoMixerFeature const *features,
                                 VdpBool *feature_enables)
{
   return VDP_STATUS_NO_IMPLEMENTATION;
}

/**
 * Retrieve parameter values given at creation time.
 */
VdpStatus
vlVdpVideoMixerGetParameterValues(VdpVideoMixer mixer,
                                  uint32_t parameter_count,
                                  VdpVideoMixerParameter const *parameters,
                                  void *const *parameter_values)
{
   return VDP_STATUS_NO_IMPLEMENTATION;
}

/**
 * Retrieve current attribute values.
 */
VdpStatus
vlVdpVideoMixerGetAttributeValues(VdpVideoMixer mixer,
                                  uint32_t attribute_count,
                                  VdpVideoMixerAttribute const *attributes,
                                  void *const *attribute_values)
{
   return VDP_STATUS_NO_IMPLEMENTATION;
}

/**
 * Generate a color space conversion matrix.
 */
VdpStatus
vlVdpGenerateCSCMatrix(VdpProcamp *procamp,
                       VdpColorStandard standard,
                       VdpCSCMatrix *csc_matrix)
{
   float matrix[16];
   enum VL_CSC_COLOR_STANDARD vl_std;
   struct vl_procamp camp;

   if (!(csc_matrix && procamp))
      return VDP_STATUS_INVALID_POINTER;

   if (procamp->struct_version > VDP_PROCAMP_VERSION)
      return VDP_STATUS_INVALID_STRUCT_VERSION;

   switch (standard) {
      case VDP_COLOR_STANDARD_ITUR_BT_601: vl_std = VL_CSC_COLOR_STANDARD_BT_601; break;
      case VDP_COLOR_STANDARD_ITUR_BT_709: vl_std = VL_CSC_COLOR_STANDARD_BT_709; break;
      case VDP_COLOR_STANDARD_SMPTE_240M:  vl_std = VL_CSC_COLOR_STANDARD_SMPTE_240M; break;
      default: return VDP_STATUS_INVALID_COLOR_STANDARD;
   }
   camp.brightness = procamp->brightness;
   camp.contrast = procamp->contrast;
   camp.saturation = procamp->saturation;
   camp.hue = procamp->hue;
   vl_csc_get_matrix(vl_std, &camp, 1, matrix);
   memcpy(csc_matrix, matrix, sizeof(float)*12);
   return VDP_STATUS_OK;
}
