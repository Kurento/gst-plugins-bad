/*
 * glshader gstreamer plugin
 * Copyrithg (C) 2008 Filippo Argiolas <filippo.argiolas@gmail.com>
 * Copyright (C) 2009 Luc Deschenaux <luc.deschenaux@freesurf.ch>
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

/**
 * SECTION:element-glshader
 *
 * OpenGL fragment shader filter
 *
 * <refsect2>
 * <title>Examples</title>
 * |[
 * gst-launch-1.0 videotestsrc ! glupload ! glshader fragment="\"`cat myshader.frag`\"" ! glimagesink
 * ]|
 * FBO (Frame Buffer Object) and GLSL (OpenGL Shading Language) are required. A #version header is required
 * </refsect2>
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>

#include "gstglfiltershader.h"
#if HAVE_GRAPHENE
#include <graphene-gobject.h>
#endif

enum
{
  PROP_0,
  PROP_SHADER,
  PROP_VERTEX,
  PROP_FRAGMENT,
  PROP_UNIFORMS,
  PROP_UPDATE_SHADER,
  PROP_LAST,
};

enum
{
  SIGNAL_0,
  SIGNAL_CREATE_SHADER,
  SIGNAL_LAST,
};

static guint gst_gl_shader_signals[SIGNAL_LAST] = { 0 };

#define GST_CAT_DEFAULT gst_gl_filtershader_debug
GST_DEBUG_CATEGORY_STATIC (GST_CAT_DEFAULT);

#define DEBUG_INIT \
  GST_DEBUG_CATEGORY_INIT (gst_gl_filtershader_debug, "glshader", 0, "glshader element");
#define gst_gl_filtershader_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE (GstGLFilterShader, gst_gl_filtershader,
    GST_TYPE_GL_FILTER, DEBUG_INIT);

static void gst_gl_filtershader_finalize (GObject * object);
static void gst_gl_filtershader_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_gl_filtershader_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);
static gboolean gst_gl_filtershader_gl_start (GstGLBaseFilter * base);
static void gst_gl_filtershader_gl_stop (GstGLBaseFilter * base);
static gboolean gst_gl_filtershader_filter (GstGLFilter * filter,
    GstBuffer * inbuf, GstBuffer * outbuf);
static gboolean gst_gl_filtershader_filter_texture (GstGLFilter * filter,
    guint in_tex, guint out_tex);
static void gst_gl_filtershader_hcallback (gint width, gint height,
    guint texture, gpointer stuff);

static void
gst_gl_filtershader_class_init (GstGLFilterShaderClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *element_class;

  gobject_class = (GObjectClass *) klass;
  element_class = GST_ELEMENT_CLASS (klass);

  gobject_class->finalize = gst_gl_filtershader_finalize;
  gobject_class->set_property = gst_gl_filtershader_set_property;
  gobject_class->get_property = gst_gl_filtershader_get_property;

  g_object_class_install_property (gobject_class, PROP_SHADER,
      g_param_spec_object ("shader", "Shader object",
          "GstGLShader to use", GST_GL_TYPE_SHADER,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_VERTEX,
      g_param_spec_string ("vertex", "Vertex Source",
          "GLSL vertex source", NULL,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_FRAGMENT,
      g_param_spec_string ("fragment", "Fragment Source",
          "GLSL fragment source", NULL,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  /* FIXME: add other stages */

  g_object_class_install_property (gobject_class, PROP_UNIFORMS,
      g_param_spec_boxed ("uniforms", "GLSL Uniforms",
          "GLSL Uniforms", GST_TYPE_STRUCTURE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_UPDATE_SHADER,
      g_param_spec_boolean ("update-shader", "Update Shader",
          "Emit the \'create-shader\' signal for the next frame",
          FALSE, G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS));

  /*
   * GstGLFilterShader::create-shader:
   * @object: the #GstGLFilterShader
   *
   * Ask's the application for a shader to render with as a result of
   * inititialization or setting the 'update-shader' property.
   *
   * Returns: a new #GstGLShader for use in the rendering pipeline
   */
  gst_gl_shader_signals[SIGNAL_CREATE_SHADER] =
      g_signal_new ("create-shader", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, g_cclosure_marshal_generic,
      GST_GL_TYPE_SHADER, 0);

  gst_element_class_set_metadata (element_class,
      "OpenGL fragment shader filter", "Filter/Effect",
      "Perform operations with a GLSL shader", "<matthew@centricular.com>");

  GST_GL_FILTER_CLASS (klass)->filter = gst_gl_filtershader_filter;
  GST_GL_FILTER_CLASS (klass)->filter_texture =
      gst_gl_filtershader_filter_texture;

  GST_GL_BASE_FILTER_CLASS (klass)->gl_start = gst_gl_filtershader_gl_start;
  GST_GL_BASE_FILTER_CLASS (klass)->gl_stop = gst_gl_filtershader_gl_stop;
  GST_GL_BASE_FILTER_CLASS (klass)->supported_gl_api =
      GST_GL_API_OPENGL | GST_GL_API_GLES2 | GST_GL_API_OPENGL3;
}

static void
gst_gl_filtershader_init (GstGLFilterShader * filtershader)
{
  filtershader->new_source = TRUE;
}

static void
gst_gl_filtershader_finalize (GObject * object)
{
  GstGLFilterShader *filtershader = GST_GL_FILTERSHADER (object);

  g_free (filtershader->vertex);
  filtershader->vertex = NULL;

  g_free (filtershader->fragment);
  filtershader->fragment = NULL;

  if (filtershader->uniforms)
    gst_structure_free (filtershader->uniforms);
  filtershader->uniforms = NULL;

  G_OBJECT_CLASS (gst_gl_filtershader_parent_class)->finalize (object);
}

static void
gst_gl_filtershader_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstGLFilterShader *filtershader = GST_GL_FILTERSHADER (object);

  switch (prop_id) {
    case PROP_SHADER:
      GST_OBJECT_LOCK (filtershader);
      gst_object_replace ((GstObject **) & filtershader->shader,
          g_value_dup_object (value));
      filtershader->new_source = FALSE;
      GST_OBJECT_UNLOCK (filtershader);
      break;
    case PROP_VERTEX:
      GST_OBJECT_LOCK (filtershader);
      g_free (filtershader->vertex);
      filtershader->vertex = g_value_dup_string (value);
      filtershader->new_source = TRUE;
      GST_OBJECT_UNLOCK (filtershader);
      break;
    case PROP_FRAGMENT:
      GST_OBJECT_LOCK (filtershader);
      g_free (filtershader->fragment);
      filtershader->fragment = g_value_dup_string (value);
      filtershader->new_source = TRUE;
      GST_OBJECT_UNLOCK (filtershader);
      break;
    case PROP_UNIFORMS:
      GST_OBJECT_LOCK (filtershader);
      if (filtershader->uniforms)
        gst_structure_free (filtershader->uniforms);
      filtershader->uniforms = g_value_dup_boxed (value);
      filtershader->new_uniforms = TRUE;
      GST_OBJECT_UNLOCK (filtershader);
      break;
    case PROP_UPDATE_SHADER:
      GST_OBJECT_LOCK (filtershader);
      filtershader->update_shader = g_value_get_boolean (value);
      GST_OBJECT_UNLOCK (filtershader);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_gl_filtershader_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstGLFilterShader *filtershader = GST_GL_FILTERSHADER (object);

  switch (prop_id) {
    case PROP_SHADER:
      GST_OBJECT_LOCK (filtershader);
      g_value_set_object (value, filtershader->shader);
      GST_OBJECT_UNLOCK (filtershader);
      break;
    case PROP_VERTEX:
      GST_OBJECT_LOCK (filtershader);
      g_value_set_string (value, filtershader->vertex);
      GST_OBJECT_UNLOCK (filtershader);
      break;
    case PROP_FRAGMENT:
      GST_OBJECT_LOCK (filtershader);
      g_value_set_string (value, filtershader->fragment);
      GST_OBJECT_UNLOCK (filtershader);
      break;
    case PROP_UNIFORMS:
      GST_OBJECT_LOCK (filtershader);
      g_value_set_boxed (value, filtershader->uniforms);
      GST_OBJECT_UNLOCK (filtershader);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_gl_filtershader_gl_stop (GstGLBaseFilter * base)
{
  GstGLFilterShader *filtershader = GST_GL_FILTERSHADER (base);

  if (filtershader->shader)
    gst_object_unref (filtershader->shader);
  filtershader->shader = NULL;

  GST_GL_BASE_FILTER_CLASS (parent_class)->gl_stop (base);
}

static gboolean
gst_gl_filtershader_gl_start (GstGLBaseFilter * base)
{
  return GST_GL_BASE_FILTER_CLASS (parent_class)->gl_start (base);
}

static inline gboolean
_gst_clock_time_to_double (GstClockTime time, gdouble * result)
{
  if (!GST_CLOCK_TIME_IS_VALID (time))
    return FALSE;

  *result = (gdouble) time / GST_SECOND;

  return TRUE;
}

static inline gboolean
_gint64_time_val_to_double (gint64 time, gdouble * result)
{
  if (time == -1)
    return FALSE;

  *result = (gdouble) time / GST_USECOND;

  return TRUE;
}

static gboolean
gst_gl_filtershader_filter (GstGLFilter * filter, GstBuffer * inbuf,
    GstBuffer * outbuf)
{
  GstGLFilterShader *filtershader = GST_GL_FILTERSHADER (filter);

  if (!_gst_clock_time_to_double (GST_BUFFER_PTS (inbuf), &filtershader->time)) {
    if (!_gst_clock_time_to_double (GST_BUFFER_DTS (inbuf),
            &filtershader->time))
      _gint64_time_val_to_double (g_get_monotonic_time (), &filtershader->time);
  }

  return gst_gl_filter_filter_texture (filter, inbuf, outbuf);
}

static gboolean
gst_gl_filtershader_filter_texture (GstGLFilter * filter, guint in_tex,
    guint out_tex)
{
  GstGLFilterShader *filtershader = GST_GL_FILTERSHADER (filter);

  gst_gl_filter_render_to_target (filter, TRUE, in_tex, out_tex,
      gst_gl_filtershader_hcallback, filtershader);

  if (!filtershader->shader)
    return FALSE;

  return TRUE;
}

static gboolean
_set_uniform (GQuark field_id, const GValue * value, gpointer user_data)
{
  GstGLShader *shader = user_data;
  const gchar *field_name = g_quark_to_string (field_id);

  if (G_TYPE_CHECK_VALUE_TYPE ((value), G_TYPE_INT)) {
    gst_gl_shader_set_uniform_1i (shader, field_name, g_value_get_int (value));
  } else if (G_TYPE_CHECK_VALUE_TYPE ((value), G_TYPE_FLOAT)) {
    gst_gl_shader_set_uniform_1f (shader, field_name,
        g_value_get_float (value));
#if HAVE_GRAPHENE
  } else if (G_TYPE_CHECK_VALUE_TYPE ((value), GRAPHENE_TYPE_VEC2)) {
    graphene_vec2_t *vec2 = g_value_get_boxed (value);
    float x = graphene_vec2_get_x (vec2);
    float y = graphene_vec2_get_y (vec2);
    gst_gl_shader_set_uniform_2f (shader, field_name, x, y);
  } else if (G_TYPE_CHECK_VALUE_TYPE ((value), GRAPHENE_TYPE_VEC3)) {
    graphene_vec3_t *vec3 = g_value_get_boxed (value);
    float x = graphene_vec3_get_x (vec3);
    float y = graphene_vec3_get_y (vec3);
    float z = graphene_vec3_get_z (vec3);
    gst_gl_shader_set_uniform_3f (shader, field_name, x, y, z);
  } else if (G_TYPE_CHECK_VALUE_TYPE ((value), GRAPHENE_TYPE_VEC4)) {
    graphene_vec4_t *vec4 = g_value_get_boxed (value);
    float x = graphene_vec4_get_x (vec4);
    float y = graphene_vec4_get_y (vec4);
    float z = graphene_vec4_get_z (vec4);
    float w = graphene_vec4_get_w (vec4);
    gst_gl_shader_set_uniform_4f (shader, field_name, x, y, z, w);
  } else if (G_TYPE_CHECK_VALUE_TYPE ((value), GRAPHENE_TYPE_MATRIX)) {
    graphene_matrix_t *matrix = g_value_get_boxed (value);
    float matrix_f[16];
    graphene_matrix_to_float (matrix, matrix_f);
    gst_gl_shader_set_uniform_matrix_4fv (shader, field_name, 1, FALSE,
        matrix_f);
#endif
  } else {
    /* FIXME: Add support for unsigned ints, non 4x4 matrices, etc */
    GST_FIXME ("Don't know how to set the \'%s\' paramater.  Unknown type",
        field_name);
    return TRUE;
  }

  return TRUE;
}

static void
_update_uniforms (GstGLFilterShader * filtershader)
{
  if (filtershader->new_uniforms && filtershader->uniforms) {
    gst_gl_shader_use (filtershader->shader);

    gst_structure_foreach (filtershader->uniforms,
        (GstStructureForeachFunc) _set_uniform, filtershader->shader);
    filtershader->new_uniforms = FALSE;
  }
}

static GstGLShader *
_maybe_recompile_shader (GstGLFilterShader * filtershader)
{
  GstGLContext *context = GST_GL_BASE_FILTER (filtershader)->context;
  GstGLShader *shader;
  GError *error = NULL;

  GST_OBJECT_LOCK (filtershader);

  if (!filtershader->shader || filtershader->update_shader) {
    filtershader->update_shader = FALSE;
    GST_OBJECT_UNLOCK (filtershader);
    g_signal_emit (filtershader, gst_gl_shader_signals[SIGNAL_CREATE_SHADER], 0,
        &shader);
    GST_OBJECT_LOCK (filtershader);

    if (shader) {
      if (filtershader->shader)
        gst_object_unref (filtershader->shader);
      filtershader->new_source = FALSE;
      filtershader->shader = gst_object_ref (shader);
      filtershader->new_uniforms = TRUE;
      _update_uniforms (filtershader);
      GST_OBJECT_UNLOCK (filtershader);
      return shader;
    }
  }

  if (filtershader->shader) {
    shader = gst_object_ref (filtershader->shader);
    _update_uniforms (filtershader);
    GST_OBJECT_UNLOCK (filtershader);
    return shader;
  }

  if (filtershader->new_source) {
    GstGLSLStage *stage;

    shader = gst_gl_shader_new (context);

    if (filtershader->vertex) {
      if (!(stage = gst_glsl_stage_new_with_string (context, GL_VERTEX_SHADER,
                  GST_GLSL_VERSION_NONE, GST_GLSL_PROFILE_NONE,
                  filtershader->vertex))) {
        g_set_error (&error, GST_GLSL_ERROR, GST_GLSL_ERROR_COMPILE,
            "Failed to create shader vertex stage");
        goto print_error;
      }
    } else {
      stage = gst_glsl_stage_new_default_vertex (context);
    }

    if (!gst_gl_shader_compile_attach_stage (shader, stage, &error)) {
      gst_object_unref (stage);
      goto print_error;
    }

    if (filtershader->fragment) {
      if (!(stage = gst_glsl_stage_new_with_string (context, GL_FRAGMENT_SHADER,
                  GST_GLSL_VERSION_NONE, GST_GLSL_PROFILE_NONE,
                  filtershader->fragment))) {
        g_set_error (&error, GST_GLSL_ERROR, GST_GLSL_ERROR_COMPILE,
            "Failed to create shader fragment stage");
        goto print_error;
      }
    } else {
      stage = gst_glsl_stage_new_default_fragment (context);
    }

    if (!gst_gl_shader_compile_attach_stage (shader, stage, &error)) {
      gst_object_unref (stage);
      goto print_error;
    }

    if (!gst_gl_shader_link (shader, &error)) {
      goto print_error;
    }
    if (filtershader->shader)
      gst_object_unref (filtershader->shader);
    filtershader->shader = gst_object_ref (shader);
    filtershader->new_source = FALSE;
    filtershader->new_uniforms = TRUE;
    _update_uniforms (filtershader);

    GST_OBJECT_UNLOCK (filtershader);
    return shader;
  } else if (filtershader->shader) {
    _update_uniforms (filtershader);
    shader = gst_object_ref (filtershader->shader);
    GST_OBJECT_UNLOCK (filtershader);
    return shader;
  }

  return NULL;

print_error:
  if (shader) {
    gst_object_unref (shader);
    shader = NULL;
  }

  GST_OBJECT_UNLOCK (filtershader);
  GST_ELEMENT_ERROR (filtershader, RESOURCE, NOT_FOUND,
      ("%s", error->message), (NULL));
  return NULL;
}

static void
gst_gl_filtershader_hcallback (gint width, gint height, guint texture,
    gpointer stuff)
{
  GstGLFilter *filter = GST_GL_FILTER (stuff);
  GstGLFilterShader *filtershader = GST_GL_FILTERSHADER (filter);
  GstGLFuncs *gl = GST_GL_BASE_FILTER (filter)->context->gl_vtable;
  GstGLShader *shader;

  if (!(shader = _maybe_recompile_shader (filtershader)))
    return;

  gl->ClearColor (0.0, 0.0, 0.0, 1.0);
  gl->Clear (GL_COLOR_BUFFER_BIT);

  gst_gl_shader_use (shader);

  /* FIXME: propertise these */
  gst_gl_shader_set_uniform_1i (shader, "tex", 0);
  gst_gl_shader_set_uniform_1f (shader, "width", width);
  gst_gl_shader_set_uniform_1f (shader, "height", height);
  gst_gl_shader_set_uniform_1f (shader, "time", filtershader->time);

  /* FIXME: propertise these */
  filter->draw_attr_position_loc =
      gst_gl_shader_get_attribute_location (shader, "a_position");
  filter->draw_attr_texture_loc =
      gst_gl_shader_get_attribute_location (shader, "a_texcoord");

  gl->ActiveTexture (GL_TEXTURE0);
  gl->BindTexture (GL_TEXTURE_2D, texture);

  gst_gl_filter_draw_texture (filter, texture, width, height);

  gst_object_unref (shader);
}
