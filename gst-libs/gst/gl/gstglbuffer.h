/*
 * GStreamer
 * Copyright (C) 2015 Matthew Waters <matthew@centricular.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifndef _GST_GL_BUFFER_H_
#define _GST_GL_BUFFER_H_

#include <gst/gst.h>
#include <gst/gstallocator.h>
#include <gst/gstmemory.h>

#include <gst/gl/gstglbasememory.h>

G_BEGIN_DECLS

#define GST_TYPE_GL_BUFFER_ALLOCATOR (gst_gl_buffer_allocator_get_type())
GType gst_gl_buffer_allocator_get_type(void);

#define GST_IS_GL_BUFFER_ALLOCATOR(obj)              (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GST_TYPE_GL_ALLOCATOR))
#define GST_IS_GL_BUFFER_ALLOCATOR_CLASS(klass)      (G_TYPE_CHECK_CLASS_TYPE ((klass), GST_TYPE_GL_BUFFER_ALLOCATOR))
#define GST_GL_BUFFER_ALLOCATOR_GET_CLASS(obj)       (G_TYPE_INSTANCE_GET_CLASS ((obj), GST_TYPE_GL_BUFFER_ALLOCATOR, GstGLBufferAllocatorClass))
#define GST_GL_BUFFER_ALLOCATOR(obj)                 (G_TYPE_CHECK_INSTANCE_CAST ((obj), GST_TYPE_GL_BUFFER_ALLOCATOR, GstGLBufferAllocator))
#define GST_GL_BUFFER_ALLOCATOR_CLASS(klass)         (G_TYPE_CHECK_CLASS_CAST ((klass), GST_TYPE_GL_BUFFER_ALLOCATOR, GstGLBufferAllocatorClass))
#define GST_GL_BUFFER_ALLOCATOR_CAST(obj)            ((GstGLBufferAllocator *)(obj))

/**
 * GstGLBuffer:
 * @mem: the parent object
 * @context: the #GstGLContext to use for GL operations
 * @id: the buffer id for this memory
 * @target: the OpenGL target of this texture for binding purposes
 * @usage_hints: the OpenGL usage hints this buffer was created with
 *
 * Represents information about a GL buffer
 */
struct _GstGLBuffer
{
  GstGLBaseMemory       mem;

  guint                 id;
  guint                 target;         /* XXX: put this in the allocator? */
  guint                 usage_hints;     /* XXX: put this in the allocator? */
};

typedef struct _GstGLBufferAllocationParams GstGLBufferAllocationParams;

#define GST_GL_ALLOCATION_PARAMS_ALLOC_FLAG_BUFFER (1 << 4)

/**
 * GstGLBufferAllocationParams:
 * @parent: parent object
 * @gl_target: the OpenGL target to bind the buffer to
 * @gl_usage: the OpenGL usage hint to create the buffer with
 */
struct _GstGLBufferAllocationParams
{
  GstGLAllocationParams     parent;

  guint                     gl_target;
  guint                     gl_usage;
};

GstGLBufferAllocationParams *   gst_gl_buffer_allocation_params_new     (GstGLContext * context,
                                                                         gsize alloc_size,
                                                                         GstAllocationParams * alloc_params,
                                                                         guint gl_target,
                                                                         guint gl_usage);

/**
 * GstGLBufferAllocator
 *
 * Opaque #GstGLAllocator struct
 */
struct _GstGLBufferAllocator
{
  GstGLBaseMemoryAllocator parent;
};

/**
 * GstGLBufferAllocatorClass:
 *
 * The #GstGLBufferAllocatorClass only contains private data
 */
struct _GstGLBufferAllocatorClass
{
  GstGLBaseMemoryAllocatorClass parent_class;
};

#define GST_CAPS_FEATURE_MEMORY_GL_BUFFER "memory:GLBuffer"

/**
 * GST_GL_BUFFER_ALLOCATOR_NAME:
 *
 * The name of the GL buffer allocator
 */
#define GST_GL_BUFFER_ALLOCATOR_NAME   "GLBuffer"

void          gst_gl_buffer_init_once (void);
gboolean      gst_is_gl_buffer        (GstMemory * mem);

GstGLBuffer * gst_gl_buffer_alloc     (GstGLContext * context,
                                       guint gl_target,
                                       guint gl_usage,
                                       GstAllocationParams * params,
                                       gsize maxsize);

G_END_DECLS

#endif /* _GST_GL_BUFFER_H_ */
