# Mesa 3-D graphics library
#
# Copyright (C) 2010-2011 Chia-I Wu <olvaffe@gmail.com>
# Copyright (C) 2010-2011 LunarG Inc.
#
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included
# in all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
# DEALINGS IN THE SOFTWARE.

LOCAL_PATH := $(call my-dir)

# from Makefile
C_SOURCES = \
	lp_bld_alpha.c \
	lp_bld_blend_aos.c \
	lp_bld_blend_logicop.c \
	lp_bld_blend_soa.c \
	lp_bld_depth.c \
	lp_bld_interp.c \
	lp_clear.c \
	lp_context.c \
	lp_draw_arrays.c \
	lp_fence.c \
	lp_flush.c \
	lp_jit.c \
	lp_memory.c \
	lp_perf.c \
	lp_query.c \
	lp_rast.c \
	lp_rast_debug.c \
	lp_rast_tri.c \
	lp_scene.c \
	lp_scene_queue.c \
	lp_screen.c \
	lp_setup.c \
	lp_setup_line.c \
	lp_setup_point.c \
	lp_setup_tri.c \
	lp_setup_vbuf.c \
	lp_state_blend.c \
	lp_state_clip.c \
	lp_state_derived.c \
	lp_state_fs.c \
	lp_state_setup.c \
	lp_state_gs.c \
	lp_state_rasterizer.c \
	lp_state_sampler.c \
        lp_state_so.c \
	lp_state_surface.c \
	lp_state_vertex.c \
	lp_state_vs.c \
	lp_surface.c \
	lp_tex_sample.c \
	lp_texture.c \
	lp_tile_image.c \
	lp_tile_soa.c

include $(CLEAR_VARS)

LOCAL_SRC_FILES := \
	$(filter-out lp_tile_soa.c, $(C_SOURCES))

LOCAL_MODULE := libmesa_pipe_llvmpipe

# generate lp_tile_soa.c
LOCAL_MODULE_CLASS := STATIC_LIBRARIES
intermediates := $(call local-intermediates-dir)
LOCAL_GENERATED_SOURCES := $(intermediates)/lp_tile_soa.c

$(intermediates)/lp_tile_soa.c: PRIVATE_CUSTOM_TOOL = python $^ > $@
$(intermediates)/lp_tile_soa.c: $(LOCAL_PATH)/lp_tile_soa.py $(GALLIUM_TOP)/auxiliary/util/u_format.csv
	$(transform-generated-source)

include $(MESA_LLVM_MK)

include $(GALLIUM_COMMON_MK)
include $(BUILD_STATIC_LIBRARY)
