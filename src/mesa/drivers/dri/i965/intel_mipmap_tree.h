/*
 * Copyright 2006 VMware, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL VMWARE AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/** @file intel_mipmap_tree.h
 *
 * This file defines the structure that wraps a BO and describes how the
 * mipmap levels and slices of a texture are laid out.
 *
 * The hardware has a fixed layout of a texture depending on parameters such
 * as the target/type (2D, 3D, CUBE), width, height, pitch, and number of
 * mipmap levels.  The individual level/layer slices are each 2D rectangles of
 * pixels at some x/y offset from the start of the brw_bo.
 *
 * Original OpenGL allowed texture miplevels to be specified in arbitrary
 * order, and a texture may change size over time.  Thus, each
 * intel_texture_image has a reference to a miptree that contains the pixel
 * data sized appropriately for it, which will later be referenced by/copied
 * to the intel_texture_object at draw time (intel_finalize_mipmap_tree()) so
 * that there's a single miptree for the complete texture.
 */

#ifndef INTEL_MIPMAP_TREE_H
#define INTEL_MIPMAP_TREE_H

#include <assert.h>

#include "main/mtypes.h"
#include "isl/isl.h"
#include "blorp/blorp.h"
#include "brw_bufmgr.h"
#include <GL/internal/dri_interface.h>

#ifdef __cplusplus
extern "C" {
#endif

struct brw_context;
struct intel_renderbuffer;

struct intel_texture_image;

/**
 * This bit extends the set of GL_MAP_*_BIT enums.
 *
 * When calling intel_miptree_map() on an ETC-transcoded-to-RGB miptree or a
 * depthstencil-split-to-separate-stencil miptree, we'll normally make a
 * temporary and recreate the kind of data requested by Mesa core, since we're
 * satisfying some glGetTexImage() request or something.
 *
 * However, occasionally you want to actually map the miptree's current data
 * without transcoding back.  This flag to intel_miptree_map() gets you that.
 */
#define BRW_MAP_DIRECT_BIT	0x80000000

struct intel_miptree_map {
   /** Bitfield of GL_MAP_*_BIT and BRW_MAP_*_BIT. */
   GLbitfield mode;
   /** Region of interest for the map. */
   int x, y, w, h;
   /** Possibly malloced temporary buffer for the mapping. */
   void *buffer;
   /** Possible pointer to a temporary linear miptree for the mapping. */
   struct intel_mipmap_tree *linear_mt;
   /** Pointer to the start of (map_x, map_y) returned by the mapping. */
   void *ptr;
   /** Stride of the mapping. */
   int stride;
};

/**
 * Describes the location of each texture image within a miptree.
 */
struct intel_mipmap_level
{
   /** Offset to this miptree level, used in computing x_offset. */
   GLuint level_x;
   /** Offset to this miptree level, used in computing y_offset. */
   GLuint level_y;

   /**
    * \brief Number of 2D slices in this miplevel.
    *
    * The exact semantics of depth varies according to the texture target:
    *    - For GL_TEXTURE_CUBE_MAP, depth is 6.
    *    - For GL_TEXTURE_2D_ARRAY, depth is the number of array slices. It is
    *      identical for all miplevels in the texture.
    *    - For GL_TEXTURE_3D, it is the texture's depth at this miplevel. Its
    *      value, like width and height, varies with miplevel.
    *    - For other texture types, depth is 1.
    *    - Additionally, for UMS and CMS miptrees, depth is multiplied by
    *      sample count.
    */
   GLuint depth;

   /**
    * \brief Is HiZ enabled for this level?
    *
    * If \c mt->level[l].has_hiz is set, then (1) \c mt->hiz_mt has been
    * allocated and (2) the HiZ memory for the slices in this level reside at
    * \c mt->hiz_mt->level[l].
    */
   bool has_hiz;

   /**
    * \brief List of 2D images in this mipmap level.
    *
    * This may be a list of cube faces, array slices in 2D array texture, or
    * layers in a 3D texture. The list's length is \c depth.
    */
   struct intel_mipmap_slice {
      /**
       * \name Offset to slice
       * \{
       *
       * Hardware formats are so diverse that that there is no unified way to
       * compute the slice offsets, so we store them in this table.
       *
       * The (x, y) offset to slice \c s at level \c l relative the miptrees
       * base address is
       * \code
       *     x = mt->level[l].slice[s].x_offset
       *     y = mt->level[l].slice[s].y_offset
       *
       * On some hardware generations, we program these offsets into
       * RENDER_SURFACE_STATE.XOffset and RENDER_SURFACE_STATE.YOffset.
       */
      GLuint x_offset;
      GLuint y_offset;
      /** \} */

      /**
       * Mapping information. Persistent for the duration of
       * intel_miptree_map/unmap on this slice.
       */
      struct intel_miptree_map *map;
   } *slice;
};

/**
 * Enum for keeping track of the different MSAA layouts supported by Gen7.
 */
enum intel_msaa_layout
{
   /**
    * Ordinary surface with no MSAA.
    */
   INTEL_MSAA_LAYOUT_NONE,

   /**
    * Interleaved Multisample Surface.  The additional samples are
    * accommodated by scaling up the width and the height of the surface so
    * that all the samples corresponding to a pixel are located at nearby
    * memory locations.
    *
    * @see PRM section "Interleaved Multisampled Surfaces"
    */
   INTEL_MSAA_LAYOUT_IMS,

   /**
    * Uncompressed Multisample Surface.  The surface is stored as a 2D array,
    * with array slice n containing all pixel data for sample n.
    *
    * @see PRM section "Uncompressed Multisampled Surfaces"
    */
   INTEL_MSAA_LAYOUT_UMS,

   /**
    * Compressed Multisample Surface.  The surface is stored as in
    * INTEL_MSAA_LAYOUT_UMS, but there is an additional buffer called the MCS
    * (Multisample Control Surface) buffer.  Each pixel in the MCS buffer
    * indicates the mapping from sample number to array slice.  This allows
    * the common case (where all samples constituting a pixel have the same
    * color value) to be stored efficiently by just using a single array
    * slice.
    *
    * @see PRM section "Compressed Multisampled Surfaces"
    */
   INTEL_MSAA_LAYOUT_CMS,
};

enum miptree_array_layout {
   /* Each array slice contains all miplevels packed together.
    *
    * Gen hardware usually wants multilevel miptrees configured this way.
    *
    * A 2D Array texture with 2 slices and multiple LODs using
    * ALL_LOD_IN_EACH_SLICE would look somewhat like this:
    *
    *   +----------+
    *   |          |
    *   |          |
    *   +----------+
    *   +---+ +-+
    *   |   | +-+
    *   +---+ *
    *   +----------+
    *   |          |
    *   |          |
    *   +----------+
    *   +---+ +-+
    *   |   | +-+
    *   +---+ *
    */
   ALL_LOD_IN_EACH_SLICE,

   /* Each LOD contains all slices of that LOD packed together.
    *
    * In some situations, Gen7+ hardware can use the array_spacing_lod0
    * feature to save space when the surface only contains LOD 0.
    *
    * Gen6 uses this for separate stencil and hiz since gen6 does not support
    * multiple LODs for separate stencil and hiz.
    *
    * A 2D Array texture with 2 slices and multiple LODs using
    * ALL_SLICES_AT_EACH_LOD would look somewhat like this:
    *
    *   +----------+
    *   |          |
    *   |          |
    *   +----------+
    *   |          |
    *   |          |
    *   +----------+
    *   +---+ +-+
    *   |   | +-+
    *   +---+ +-+
    *   |   | :
    *   +---+
    */
   ALL_SLICES_AT_EACH_LOD,

   /* On Sandy Bridge, HiZ and stencil buffers work the same as on Ivy Bridge
    * except that they don't technically support mipmapping.  That does not,
    * however, stop us from doing it.  As far as Sandy Bridge hardware is
    * concerned, HiZ and stencil always operates on a single miplevel 2D
    * (possibly array) image.  The dimensions of that image are NOT minified.
    *
    * In order to implement HiZ and stencil on Sandy Bridge, we create one
    * full-sized 2D (possibly array) image for every LOD with every image
    * aligned to a page boundary.  In order to save memory, we pretend that
    * the width of each miplevel is minified and we place LOD1 and above below
    * LOD0 but horizontally adjacent to each other.  When considered as
    * full-sized images, LOD1 and above technically overlap.  However, since
    * we only write to part of that image, the hardware will never notice the
    * overlap.
    *
    * This layout looks something like this:
    *
    *   +---------+
    *   |         |
    *   |         |
    *   +---------+
    *   |         |
    *   |         |
    *   +---------+
    *
    *   +----+ +-+ .
    *   |    | +-+
    *   +----+
    *
    *   +----+ +-+ .
    *   |    | +-+
    *   +----+
    */
   GEN6_HIZ_STENCIL,
};

/**
 * Miptree aux buffer. These buffers are associated with a miptree, but the
 * format is managed by the hardware.
 *
 * For Gen7+, we always give the hardware the start of the buffer, and let it
 * handle all accesses to the buffer. Therefore we don't need the full miptree
 * layout structure for this buffer.
 */
struct intel_miptree_aux_buffer
{
   struct isl_surf surf;

   /**
    * Buffer object containing the pixel data.
    *
    * @see RENDER_SURFACE_STATE.AuxiliarySurfaceBaseAddress
    * @see 3DSTATE_HIER_DEPTH_BUFFER.AuxiliarySurfaceBaseAddress
    */
   struct brw_bo *bo;

   /**
    * Offset into bo where the surface starts.
    *
    * @see intel_mipmap_aux_buffer::bo
    *
    * @see RENDER_SURFACE_STATE.AuxiliarySurfaceBaseAddress
    * @see 3DSTATE_DEPTH_BUFFER.SurfaceBaseAddress
    * @see 3DSTATE_HIER_DEPTH_BUFFER.SurfaceBaseAddress
    * @see 3DSTATE_STENCIL_BUFFER.SurfaceBaseAddress
    */
   uint32_t offset;

   /*
    * Size of the MCS surface.
    *
    * This is needed when doing any gtt mapped operations on the buffer (which
    * will be Y-tiled). It is possible that it will not be the same as bo->size
    * when the drm allocator rounds up the requested size.
    */
   size_t size;

   /**
    * Pitch in bytes.
    *
    * @see RENDER_SURFACE_STATE.AuxiliarySurfacePitch
    * @see 3DSTATE_HIER_DEPTH_BUFFER.SurfacePitch
    */
   uint32_t pitch;

   /**
    * The distance in rows between array slices.
    *
    * @see RENDER_SURFACE_STATE.AuxiliarySurfaceQPitch
    * @see 3DSTATE_HIER_DEPTH_BUFFER.SurfaceQPitch
    */
   uint32_t qpitch;
};

struct intel_mipmap_tree
{
   struct isl_surf surf;

   /**
    * Buffer object containing the surface.
    *
    * @see intel_mipmap_tree::offset
    * @see RENDER_SURFACE_STATE.SurfaceBaseAddress
    * @see RENDER_SURFACE_STATE.AuxiliarySurfaceBaseAddress
    * @see 3DSTATE_DEPTH_BUFFER.SurfaceBaseAddress
    * @see 3DSTATE_HIER_DEPTH_BUFFER.SurfaceBaseAddress
    * @see 3DSTATE_STENCIL_BUFFER.SurfaceBaseAddress
    */
   struct brw_bo *bo;

   /**
    * Pitch in bytes.
    *
    * @see RENDER_SURFACE_STATE.SurfacePitch
    * @see RENDER_SURFACE_STATE.AuxiliarySurfacePitch
    * @see 3DSTATE_DEPTH_BUFFER.SurfacePitch
    * @see 3DSTATE_HIER_DEPTH_BUFFER.SurfacePitch
    * @see 3DSTATE_STENCIL_BUFFER.SurfacePitch
    */
   uint32_t pitch;

   /**
    * One of the I915_TILING_* flags.
    *
    * @see RENDER_SURFACE_STATE.TileMode
    * @see 3DSTATE_DEPTH_BUFFER.TileMode
    */
   uint32_t tiling;

   /**
    * @brief One of GL_TEXTURE_2D, GL_TEXTURE_2D_ARRAY, etc.
    *
    * @see RENDER_SURFACE_STATE.SurfaceType
    * @see RENDER_SURFACE_STATE.SurfaceArray
    * @see 3DSTATE_DEPTH_BUFFER.SurfaceType
    */
   GLenum target;

   /**
    * Generally, this is just the same as the gl_texture_image->TexFormat or
    * gl_renderbuffer->Format.
    *
    * However, for textures and renderbuffers with packed depth/stencil formats
    * on hardware where we want or need to use separate stencil, there will be
    * two miptrees for storing the data.  If the depthstencil texture or rb is
    * MESA_FORMAT_Z32_FLOAT_S8X24_UINT, then mt->format will be
    * MESA_FORMAT_Z_FLOAT32, otherwise for MESA_FORMAT_Z24_UNORM_S8_UINT objects it will be
    * MESA_FORMAT_Z24_UNORM_X8_UINT.
    *
    * For ETC1/ETC2 textures, this is one of the uncompressed mesa texture
    * formats if the hardware lacks support for ETC1/ETC2. See @ref etc_format.
    *
    * @see RENDER_SURFACE_STATE.SurfaceFormat
    * @see 3DSTATE_DEPTH_BUFFER.SurfaceFormat
    */
   mesa_format format;

   /**
    * This variable stores the value of ETC compressed texture format
    *
    * @see RENDER_SURFACE_STATE.SurfaceFormat
    */
   mesa_format etc_format;

   /**
    * @name Surface Alignment
    * @{
    *
    * This defines the alignment of the upperleft pixel of each "slice" in the
    * surface. The alignment is in pixel coordinates relative to the surface's
    * most upperleft pixel, which is the pixel at (x=0, y=0, layer=0,
    * level=0).
    *
    * The hardware docs do not use the term "slice".  We use "slice" to mean
    * the pixels at a given miplevel and layer. For 2D surfaces, the layer is
    * the array slice; for 3D surfaces, the layer is the z offset.
    *
    * In the surface layout equations found in the hardware docs, the
    * horizontal and vertical surface alignments often appear as variables 'i'
    * and 'j'.
    */

   /** @see RENDER_SURFACE_STATE.SurfaceHorizontalAlignment */
   uint32_t halign;

   /** @see RENDER_SURFACE_STATE.SurfaceVerticalAlignment */
   uint32_t valign;
   /** @} */

   GLuint first_level;
   GLuint last_level;

   /**
    * Level zero image dimensions.  These dimensions correspond to the
    * physical layout of data in memory.  Accordingly, they account for the
    * extra width, height, and or depth that must be allocated in order to
    * accommodate multisample formats, and they account for the extra factor
    * of 6 in depth that must be allocated in order to accommodate cubemap
    * textures.
    */
   GLuint physical_width0, physical_height0, physical_depth0;

   /** Bytes per pixel (or bytes per block if compressed) */
   GLuint cpp;

   /**
    * @see RENDER_SURFACE_STATE.NumberOfMultisamples
    * @see 3DSTATE_MULTISAMPLE.NumberOfMultisamples
    */
   GLuint num_samples;

   bool compressed;

   /**
    * @name Level zero image dimensions
    * @{
    *
    * These dimensions correspond to the
    * logical width, height, and depth of the texture as seen by client code.
    * Accordingly, they do not account for the extra width, height, and/or
    * depth that must be allocated in order to accommodate multisample
    * formats, nor do they account for the extra factor of 6 in depth that
    * must be allocated in order to accommodate cubemap textures.
    */

   /**
    * @see RENDER_SURFACE_STATE.Width
    * @see 3DSTATE_DEPTH_BUFFER.Width
    */
   uint32_t logical_width0;

   /**
    * @see RENDER_SURFACE_STATE.Height
    * @see 3DSTATE_DEPTH_BUFFER.Height
    */
   uint32_t logical_height0;

   /**
    * @see RENDER_SURFACE_STATE.Depth
    * @see 3DSTATE_DEPTH_BUFFER.Depth
    */
   uint32_t logical_depth0;
   /** @} */

   /**
    * Indicates if we use the standard miptree layout (ALL_LOD_IN_EACH_SLICE),
    * or if we tightly pack array slices at each LOD (ALL_SLICES_AT_EACH_LOD).
    */
   enum miptree_array_layout array_layout;

   /**
    * The distance in between array slices.
    *
    * The value is the one that is sent in the surface state. The actual
    * meaning depends on certain criteria. Usually it is simply the number of
    * uncompressed rows between each slice. However on Gen9+ for compressed
    * surfaces it is the number of blocks. For 1D array surfaces that have the
    * mipmap tree stored horizontally it is the number of pixels between each
    * slice.
    *
    * @see RENDER_SURFACE_STATE.SurfaceQPitch
    * @see 3DSTATE_DEPTH_BUFFER.SurfaceQPitch
    * @see 3DSTATE_HIER_DEPTH_BUFFER.SurfaceQPitch
    * @see 3DSTATE_STENCIL_BUFFER.SurfaceQPitch
    */
   uint32_t qpitch;

   /**
    * MSAA layout used by this buffer.
    *
    * @see RENDER_SURFACE_STATE.MultisampledSurfaceStorageFormat
    */
   enum intel_msaa_layout msaa_layout;

   /* Derived from the above:
    */
   GLuint total_width;
   GLuint total_height;

   /* Includes image offset tables: */
   struct intel_mipmap_level level[MAX_TEXTURE_LEVELS];

   /**
    * Offset into bo where the surface starts.
    *
    * @see intel_mipmap_tree::bo
    *
    * @see RENDER_SURFACE_STATE.AuxiliarySurfaceBaseAddress
    * @see 3DSTATE_DEPTH_BUFFER.SurfaceBaseAddress
    * @see 3DSTATE_HIER_DEPTH_BUFFER.SurfaceBaseAddress
    * @see 3DSTATE_STENCIL_BUFFER.SurfaceBaseAddress
    */
   uint32_t offset;

   /**
    * \brief HiZ aux buffer
    *
    * To allocate the hiz buffer, use intel_miptree_alloc_hiz().
    *
    * To determine if hiz is enabled, do not check this pointer. Instead, use
    * intel_miptree_level_has_hiz().
    */
   struct intel_miptree_aux_buffer *hiz_buf;

   /**
    * \brief The type of auxiliary compression used by this miptree.
    *
    * This describes the type of auxiliary compression that is intended to be
    * used by this miptree.  An aux usage of ISL_AUX_USAGE_NONE means that
    * auxiliary compression is permanently disabled.  An aux usage other than
    * ISL_AUX_USAGE_NONE does not imply that the auxiliary buffer has actually
    * been allocated nor does it imply that auxiliary compression will always
    * be enabled for this surface.  For instance, with CCS_D, we may allocate
    * the CCS on-the-fly and it may not be used for texturing if the miptree
    * is fully resolved.
    */
   enum isl_aux_usage aux_usage;

   /**
    * \brief Whether or not this miptree supports fast clears.
    */
   bool supports_fast_clear;

   /**
    * \brief Maps miptree slices to their current aux state
    *
    * This two-dimensional array is indexed as [level][layer] and stores an
    * aux state for each slice.
    */
   enum isl_aux_state **aux_state;

   /**
    * \brief Stencil miptree for depthstencil textures.
    *
    * This miptree is used for depthstencil textures and renderbuffers that
    * require separate stencil.  It always has the true copy of the stencil
    * bits, regardless of mt->format.
    *
    * \see 3DSTATE_STENCIL_BUFFER
    * \see intel_miptree_map_depthstencil()
    * \see intel_miptree_unmap_depthstencil()
    */
   struct intel_mipmap_tree *stencil_mt;

   /**
    * \brief Stencil texturing miptree for sampling from a stencil texture
    *
    * Some hardware doesn't support sampling from the stencil texture as
    * required by the GL_ARB_stencil_texturing extenion. To workaround this we
    * blit the texture into a new texture that can be sampled.
    *
    * \see intel_update_r8stencil()
    */
   struct intel_mipmap_tree *r8stencil_mt;
   bool r8stencil_needs_update;

   /**
    * \brief MCS auxiliary buffer.
    *
    * This buffer contains the "multisample control surface", which stores
    * the necessary information to implement compressed MSAA
    * (INTEL_MSAA_FORMAT_CMS) and "fast color clear" behaviour on Gen7+.
    *
    * NULL if no MCS buffer is in use for this surface.
    */
   struct intel_miptree_aux_buffer *mcs_buf;

   /**
    * Planes 1 and 2 in case this is a planar surface.
    */
   struct intel_mipmap_tree *plane[2];

   /**
    * Fast clear color for this surface.  For depth surfaces, the clear value
    * is stored as a float32 in the red component.
    */
   union isl_color_value fast_clear_color;

   /**
    * Tells if the underlying buffer is to be also consumed by entities other
    * than the driver. This allows logic to turn off features such as lossless
    * compression which is not currently understood by client applications.
    */
   bool is_scanout;

   /* These are also refcounted:
    */
   GLuint refcount;
};

bool
intel_miptree_alloc_ccs(struct brw_context *brw,
                        struct intel_mipmap_tree *mt);

enum {
   MIPTREE_LAYOUT_ACCELERATED_UPLOAD       = 1 << 0,
   MIPTREE_LAYOUT_GEN6_HIZ_STENCIL         = 1 << 1,
   MIPTREE_LAYOUT_FOR_BO                   = 1 << 2,
   MIPTREE_LAYOUT_DISABLE_AUX              = 1 << 3,
   MIPTREE_LAYOUT_FORCE_HALIGN16           = 1 << 4,

   MIPTREE_LAYOUT_TILING_Y                 = 1 << 5,
   MIPTREE_LAYOUT_TILING_NONE              = 1 << 6,
   MIPTREE_LAYOUT_TILING_ANY               = MIPTREE_LAYOUT_TILING_Y |
                                             MIPTREE_LAYOUT_TILING_NONE,

   MIPTREE_LAYOUT_FOR_SCANOUT              = 1 << 7,
};

struct intel_mipmap_tree *intel_miptree_create(struct brw_context *brw,
                                               GLenum target,
					       mesa_format format,
                                               GLuint first_level,
                                               GLuint last_level,
                                               GLuint width0,
                                               GLuint height0,
                                               GLuint depth0,
                                               GLuint num_samples,
                                               uint32_t flags);

struct intel_mipmap_tree *
intel_miptree_create_for_bo(struct brw_context *brw,
                            struct brw_bo *bo,
                            mesa_format format,
                            uint32_t offset,
                            uint32_t width,
                            uint32_t height,
                            uint32_t depth,
                            int pitch,
                            uint32_t layout_flags);

struct intel_mipmap_tree *
intel_miptree_create_for_dri_image(struct brw_context *brw,
                                   __DRIimage *image,
                                   GLenum target,
                                   enum isl_colorspace colorspace,
                                   bool is_winsys_image);

bool
intel_update_winsys_renderbuffer_miptree(struct brw_context *intel,
                                         struct intel_renderbuffer *irb,
                                         struct intel_mipmap_tree *singlesample_mt,
                                         uint32_t width, uint32_t height,
                                         uint32_t pitch);

/**
 * Create a miptree appropriate as the storage for a non-texture renderbuffer.
 * The miptree has the following properties:
 *     - The target is GL_TEXTURE_2D.
 *     - There are no levels other than the base level 0.
 *     - Depth is 1.
 */
struct intel_mipmap_tree*
intel_miptree_create_for_renderbuffer(struct brw_context *brw,
                                      mesa_format format,
                                      uint32_t width,
                                      uint32_t height,
                                      uint32_t num_samples);

mesa_format
intel_depth_format_for_depthstencil_format(mesa_format format);

mesa_format
intel_lower_compressed_format(struct brw_context *brw, mesa_format format);

/** \brief Assert that the level and layer are valid for the miptree. */
void
intel_miptree_check_level_layer(const struct intel_mipmap_tree *mt,
                                uint32_t level,
                                uint32_t layer);

void intel_miptree_reference(struct intel_mipmap_tree **dst,
                             struct intel_mipmap_tree *src);

void intel_miptree_release(struct intel_mipmap_tree **mt);

/* Check if an image fits an existing mipmap tree layout
 */
bool intel_miptree_match_image(struct intel_mipmap_tree *mt,
                                    struct gl_texture_image *image);

void
intel_miptree_get_image_offset(const struct intel_mipmap_tree *mt,
			       GLuint level, GLuint slice,
			       GLuint *x, GLuint *y);

enum isl_surf_dim
get_isl_surf_dim(GLenum target);

enum isl_dim_layout
get_isl_dim_layout(const struct gen_device_info *devinfo, uint32_t tiling,
                   GLenum target, enum miptree_array_layout array_layout);

enum isl_tiling
intel_miptree_get_isl_tiling(const struct intel_mipmap_tree *mt);

void
intel_miptree_get_isl_surf(struct brw_context *brw,
                           const struct intel_mipmap_tree *mt,
                           struct isl_surf *surf);

enum isl_aux_usage
intel_miptree_get_aux_isl_usage(const struct brw_context *brw,
                                const struct intel_mipmap_tree *mt);

void
intel_get_image_dims(struct gl_texture_image *image,
                     int *width, int *height, int *depth);

void
intel_get_tile_masks(uint32_t tiling, uint32_t cpp,
                     uint32_t *mask_x, uint32_t *mask_y);

void
intel_get_tile_dims(uint32_t tiling, uint32_t cpp,
                    uint32_t *tile_w, uint32_t *tile_h);

uint32_t
intel_miptree_get_tile_offsets(const struct intel_mipmap_tree *mt,
                               GLuint level, GLuint slice,
                               uint32_t *tile_x,
                               uint32_t *tile_y);
uint32_t
intel_miptree_get_aligned_offset(const struct intel_mipmap_tree *mt,
                                 uint32_t x, uint32_t y);

void intel_miptree_set_level_info(struct intel_mipmap_tree *mt,
                                  GLuint level,
                                  GLuint x, GLuint y, GLuint d);

void intel_miptree_set_image_offset(struct intel_mipmap_tree *mt,
                                    GLuint level,
                                    GLuint img, GLuint x, GLuint y);

void
intel_miptree_copy_slice(struct brw_context *brw,
                         struct intel_mipmap_tree *src_mt,
                         unsigned src_level, unsigned src_layer,
                         struct intel_mipmap_tree *dst_mt,
                         unsigned dst_level, unsigned dst_layer);

void
intel_miptree_copy_teximage(struct brw_context *brw,
                            struct intel_texture_image *intelImage,
                            struct intel_mipmap_tree *dst_mt, bool invalidate);

/**
 * \name Miptree HiZ functions
 * \{
 *
 * It is safe to call the "slice_set_need_resolve" and "slice_resolve"
 * functions on a miptree without HiZ. In that case, each function is a no-op.
 */

/**
 * \brief Allocate the miptree's embedded HiZ miptree.
 * \see intel_mipmap_tree:hiz_mt
 * \return false if allocation failed
 */
bool
intel_miptree_alloc_hiz(struct brw_context *brw,
			struct intel_mipmap_tree *mt);

bool
intel_miptree_level_has_hiz(const struct intel_mipmap_tree *mt, uint32_t level);

/**\}*/

bool
intel_miptree_has_color_unresolved(const struct intel_mipmap_tree *mt,
                                   unsigned start_level, unsigned num_levels,
                                   unsigned start_layer, unsigned num_layers);


#define INTEL_REMAINING_LAYERS UINT32_MAX
#define INTEL_REMAINING_LEVELS UINT32_MAX

/** Prepare a miptree for access
 *
 * This function should be called prior to any access to miptree in order to
 * perform any needed resolves.
 *
 * \param[in]  start_level    The first mip level to be accessed
 *
 * \param[in]  num_levels     The number of miplevels to be accessed or
 *                            INTEL_REMAINING_LEVELS to indicate every level
 *                            above start_level will be accessed
 *
 * \param[in]  start_layer    The first array slice or 3D layer to be accessed
 *
 * \param[in]  num_layers     The number of array slices or 3D layers be
 *                            accessed or INTEL_REMAINING_LAYERS to indicate
 *                            every layer above start_layer will be accessed
 *
 * \param[in]  aux_supported  Whether or not the access will support the
 *                            miptree's auxiliary compression format;  this
 *                            must be false for uncompressed miptrees
 *
 * \param[in]  fast_clear_supported Whether or not the access will support
 *                                  fast clears in the miptree's auxiliary
 *                                  compression format
 */
void
intel_miptree_prepare_access(struct brw_context *brw,
                             struct intel_mipmap_tree *mt,
                             uint32_t start_level, uint32_t num_levels,
                             uint32_t start_layer, uint32_t num_layers,
                             bool aux_supported, bool fast_clear_supported);

/** Complete a write operation
 *
 * This function should be called after any operation writes to a miptree.
 * This will update the miptree's compression state so that future resolves
 * happen correctly.  Technically, this function can be called before the
 * write occurs but the caller must ensure that they don't interlace
 * intel_miptree_prepare_access and intel_miptree_finish_write calls to
 * overlapping layer/level ranges.
 *
 * \param[in]  level             The mip level that was written
 *
 * \param[in]  start_layer       The first array slice or 3D layer written
 *
 * \param[in]  num_layers        The number of array slices or 3D layers
 *                               written or INTEL_REMAINING_LAYERS to indicate
 *                               every layer above start_layer was written
 *
 * \param[in]  written_with_aux  Whether or not the write was done with
 *                               auxiliary compression enabled
 */
void
intel_miptree_finish_write(struct brw_context *brw,
                           struct intel_mipmap_tree *mt, uint32_t level,
                           uint32_t start_layer, uint32_t num_layers,
                           bool written_with_aux);

/** Get the auxiliary compression state of a miptree slice */
enum isl_aux_state
intel_miptree_get_aux_state(const struct intel_mipmap_tree *mt,
                            uint32_t level, uint32_t layer);

/** Set the auxiliary compression state of a miptree slice range
 *
 * This function directly sets the auxiliary compression state of a slice
 * range of a miptree.  It only modifies data structures and does not do any
 * resolves.  This should only be called by code which directly performs
 * compression operations such as fast clears and resolves.  Most code should
 * use intel_miptree_prepare_access or intel_miptree_finish_write.
 */
void
intel_miptree_set_aux_state(struct brw_context *brw,
                            struct intel_mipmap_tree *mt, uint32_t level,
                            uint32_t start_layer, uint32_t num_layers,
                            enum isl_aux_state aux_state);

/**
 * Prepare a miptree for raw access
 *
 * This helper prepares the miptree for access that knows nothing about any
 * sort of compression whatsoever.  This is useful when mapping the surface or
 * using it with the blitter.
 */
static inline void
intel_miptree_access_raw(struct brw_context *brw,
                         struct intel_mipmap_tree *mt,
                         uint32_t level, uint32_t layer,
                         bool write)
{
   intel_miptree_prepare_access(brw, mt, level, 1, layer, 1, false, false);
   if (write)
      intel_miptree_finish_write(brw, mt, level, layer, 1, false);
}

void
intel_miptree_prepare_texture(struct brw_context *brw,
                              struct intel_mipmap_tree *mt,
                              mesa_format view_format,
                              bool *aux_supported_out);
void
intel_miptree_prepare_image(struct brw_context *brw,
                            struct intel_mipmap_tree *mt);
void
intel_miptree_prepare_fb_fetch(struct brw_context *brw,
                               struct intel_mipmap_tree *mt, uint32_t level,
                               uint32_t start_layer, uint32_t num_layers);
void
intel_miptree_prepare_render(struct brw_context *brw,
                             struct intel_mipmap_tree *mt, uint32_t level,
                             uint32_t start_layer, uint32_t layer_count,
                             bool srgb_enabled);
void
intel_miptree_finish_render(struct brw_context *brw,
                            struct intel_mipmap_tree *mt, uint32_t level,
                            uint32_t start_layer, uint32_t layer_count);
void
intel_miptree_prepare_depth(struct brw_context *brw,
                            struct intel_mipmap_tree *mt, uint32_t level,
                            uint32_t start_layer, uint32_t layer_count);
void
intel_miptree_finish_depth(struct brw_context *brw,
                           struct intel_mipmap_tree *mt, uint32_t level,
                           uint32_t start_layer, uint32_t layer_count,
                           bool depth_written);

void
intel_miptree_make_shareable(struct brw_context *brw,
                             struct intel_mipmap_tree *mt);

void
intel_miptree_updownsample(struct brw_context *brw,
                           struct intel_mipmap_tree *src,
                           struct intel_mipmap_tree *dst);

void
intel_update_r8stencil(struct brw_context *brw,
                       struct intel_mipmap_tree *mt);

/**
 * Horizontal distance from one slice to the next in the two-dimensional
 * miptree layout.
 */
unsigned
brw_miptree_get_horizontal_slice_pitch(const struct brw_context *brw,
                                       const struct intel_mipmap_tree *mt,
                                       unsigned level);

bool
brw_miptree_layout(struct brw_context *brw,
                   struct intel_mipmap_tree *mt,
                   uint32_t layout_flags);

void
intel_miptree_map(struct brw_context *brw,
		  struct intel_mipmap_tree *mt,
		  unsigned int level,
		  unsigned int slice,
		  unsigned int x,
		  unsigned int y,
		  unsigned int w,
		  unsigned int h,
		  GLbitfield mode,
		  void **out_ptr,
		  ptrdiff_t *out_stride);

void
intel_miptree_unmap(struct brw_context *brw,
		    struct intel_mipmap_tree *mt,
		    unsigned int level,
		    unsigned int slice);

bool
intel_miptree_sample_with_hiz(struct brw_context *brw,
                              struct intel_mipmap_tree *mt);

#ifdef __cplusplus
}
#endif

#endif
