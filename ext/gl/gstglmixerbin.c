/*
 *
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>

#include "gstglmixerbin.h"

#define GST_CAT_DEFAULT gst_gl_mixer_bin_debug
GST_DEBUG_CATEGORY (gst_gl_mixer_bin_debug);

#define DEFAULT_LATENCY        0

struct input_chain
{
  GstGLMixerBin *self;
  GstGhostPad *ghost_pad;
  GstElement *upload;
  GstElement *in_convert;
  GstPad *mixer_pad;
};

static void
_free_input_chain (struct input_chain *chain)
{
  if (!chain)
    return;

  chain->ghost_pad = NULL;

  if (chain->upload) {
    gst_element_set_state (chain->upload, GST_STATE_NULL);
    gst_bin_remove (GST_BIN (chain->self), chain->upload);
    chain->upload = NULL;
  }

  if (chain->in_convert) {
    gst_element_set_state (chain->in_convert, GST_STATE_NULL);
    gst_bin_remove (GST_BIN (chain->self), chain->in_convert);
    chain->in_convert = NULL;
  }

  if (chain->mixer_pad) {
    gst_element_release_request_pad (chain->self->mixer, chain->mixer_pad);
    gst_object_unref (chain->mixer_pad);
    chain->mixer_pad = NULL;
  }

  g_free (chain);
}

struct _GstGLMixerBinPrivate
{
  gboolean running;

  GList *input_chains;
};

enum
{
  PROP_0,
  PROP_MIXER,
  PROP_LATENCY,
};

enum
{
  SIGNAL_0,
  SIGNAL_CREATE_ELEMENT,
  LAST_SIGNAL
};

static void gst_gl_mixer_bin_child_proxy_init (gpointer g_iface,
    gpointer iface_data);

#define GST_GL_MIXER_BIN_GET_PRIVATE(o)					\
  (G_TYPE_INSTANCE_GET_PRIVATE((o), GST_TYPE_GL_MIXER_BIN, GstGLMixerBinPrivate))
G_DEFINE_TYPE_WITH_CODE (GstGLMixerBin, gst_gl_mixer_bin, GST_TYPE_BIN,
    G_IMPLEMENT_INTERFACE (GST_TYPE_CHILD_PROXY,
        gst_gl_mixer_bin_child_proxy_init));

static guint gst_gl_mixer_bin_signals[LAST_SIGNAL] = { 0 };

static GstStaticPadTemplate src_factory = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-raw(ANY)")
    );

static void gst_gl_mixer_bin_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_gl_mixer_bin_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);
static void gst_gl_mixer_bin_dispose (GObject * object);

static GstPad *gst_gl_mixer_bin_request_new_pad (GstElement * element,
    GstPadTemplate * templ, const gchar * req_name, const GstCaps * caps);
static void gst_gl_mixer_bin_release_pad (GstElement * element, GstPad * pad);
static GstStateChangeReturn gst_gl_mixer_bin_change_state (GstElement *
    element, GstStateChange transition);

static void
gst_gl_mixer_bin_class_init (GstGLMixerBinClass * klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstCaps *upload_caps;

  g_type_class_add_private (klass, sizeof (GstGLMixerBinPrivate));

  GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, "glmixerbin", 0,
      "opengl mixer bin");

  element_class->request_new_pad = gst_gl_mixer_bin_request_new_pad;
  element_class->release_pad = gst_gl_mixer_bin_release_pad;
  element_class->change_state = gst_gl_mixer_bin_change_state;

  gobject_class->get_property = gst_gl_mixer_bin_get_property;
  gobject_class->set_property = gst_gl_mixer_bin_set_property;
  gobject_class->dispose = GST_DEBUG_FUNCPTR (gst_gl_mixer_bin_dispose);

  g_object_class_install_property (gobject_class, PROP_MIXER,
      g_param_spec_object ("mixer",
          "GL mixer element",
          "The GL mixer chain to use",
          GST_TYPE_ELEMENT,
          GST_PARAM_MUTABLE_READY | G_PARAM_READWRITE |
          G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_LATENCY,
      g_param_spec_int64 ("latency", "Buffer latency",
          "Additional latency in live mode to allow upstream "
          "to take longer to produce buffers for the current "
          "position", 0,
          (G_MAXLONG == G_MAXINT64) ? G_MAXINT64 : (G_MAXLONG * GST_SECOND - 1),
          DEFAULT_LATENCY, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * GstMixerBin::create-element:
   * @object: the #GstGLMixerBin
   *
   * Will be emitted when we need the processing element/s that this bin will use
   *
   * Returns: a new #GstElement
   */
  gst_gl_mixer_bin_signals[SIGNAL_CREATE_ELEMENT] =
      g_signal_new ("create-element", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, g_cclosure_marshal_generic,
      GST_TYPE_ELEMENT, 0);

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&src_factory));

  upload_caps = gst_gl_upload_get_input_template_caps ();
  gst_element_class_add_pad_template (element_class,
      gst_pad_template_new ("sink_%u", GST_PAD_SINK, GST_PAD_REQUEST,
          upload_caps));
  gst_caps_unref (upload_caps);
}

static void
gst_gl_mixer_bin_init (GstGLMixerBin * self)
{
  gboolean res = TRUE;
  GstPad *pad;

  self->priv = GST_GL_MIXER_BIN_GET_PRIVATE (self);

  self->out_convert = gst_element_factory_make ("glcolorconvert", NULL);
  self->download = gst_element_factory_make ("gldownload", NULL);
  res &= gst_bin_add (GST_BIN (self), self->out_convert);
  res &= gst_bin_add (GST_BIN (self), self->download);

  res &=
      gst_element_link_pads (self->out_convert, "src", self->download, "sink");

  pad = gst_element_get_static_pad (self->download, "src");
  if (!pad) {
    res = FALSE;
  } else {
    GST_DEBUG_OBJECT (self, "setting target src pad %" GST_PTR_FORMAT, pad);
    self->srcpad = gst_ghost_pad_new ("src", pad);
    gst_element_add_pad (GST_ELEMENT_CAST (self), self->srcpad);
    gst_object_unref (pad);
  }

  if (!res)
    GST_ERROR_OBJECT (self, "failed to create output chain");
}

static void
gst_gl_mixer_bin_dispose (GObject * object)
{
  GstGLMixerBin *self = GST_GL_MIXER_BIN (object);
  GList *l = self->priv->input_chains;

  while (l) {
    struct input_chain *chain = l->data;

    if (self->mixer && chain->mixer_pad) {
      gst_element_release_request_pad (GST_ELEMENT (self->mixer),
          chain->mixer_pad);
      gst_object_unref (chain->mixer_pad);
      chain->mixer_pad = NULL;
    }

    l = l->next;
  }

  g_list_free_full (self->priv->input_chains, (GDestroyNotify) g_free);

  G_OBJECT_CLASS (gst_gl_mixer_bin_parent_class)->dispose (object);
}

static gboolean
_create_input_chain (GstGLMixerBin * self, struct input_chain *chain,
    GstPad * mixer_pad)
{
  GstGLMixerBinClass *klass = GST_GL_MIXER_BIN_GET_CLASS (self);
  GstPad *pad;
  gboolean res = TRUE;
  gchar *name;

  chain->self = self;
  chain->mixer_pad = mixer_pad;

  chain->upload = gst_element_factory_make ("glupload", NULL);
  chain->in_convert = gst_element_factory_make ("glcolorconvert", NULL);

  res &= gst_bin_add (GST_BIN (self), chain->in_convert);
  res &= gst_bin_add (GST_BIN (self), chain->upload);

  pad = gst_element_get_static_pad (chain->in_convert, "src");
  if (gst_pad_link (pad, mixer_pad) != GST_PAD_LINK_OK) {
    gst_object_unref (pad);
    return FALSE;
  }
  gst_object_unref (pad);
  res &=
      gst_element_link_pads (chain->upload, "src", chain->in_convert, "sink");

  pad = gst_element_get_static_pad (chain->upload, "sink");
  if (!pad) {
    return FALSE;
  } else {
    GST_DEBUG_OBJECT (self, "setting target sink pad %" GST_PTR_FORMAT, pad);
    name = gst_object_get_name (GST_OBJECT (mixer_pad));
    if (klass->create_input_pad) {
      chain->ghost_pad = klass->create_input_pad (self, chain->mixer_pad);
      gst_object_set_name (GST_OBJECT (chain->ghost_pad), name);
      gst_ghost_pad_set_target (chain->ghost_pad, pad);
    } else {
      chain->ghost_pad =
          GST_GHOST_PAD (gst_ghost_pad_new (GST_PAD_NAME (chain->mixer_pad),
              pad));
    }
    g_free (name);

    GST_OBJECT_LOCK (self);
    if (self->priv->running)
      gst_pad_set_active (GST_PAD (chain->ghost_pad), TRUE);
    GST_OBJECT_UNLOCK (self);

    gst_element_add_pad (GST_ELEMENT_CAST (self), GST_PAD (chain->ghost_pad));
    gst_object_unref (pad);
  }

  gst_element_sync_state_with_parent (chain->upload);
  gst_element_sync_state_with_parent (chain->in_convert);

  return TRUE;
}

static GstPadTemplate *
_find_element_pad_template (GstElement * element,
    GstPadDirection direction, GstPadPresence presence)
{
  GstElementClass *klass = GST_ELEMENT_GET_CLASS (element);
  GList *templ_list = gst_element_class_get_pad_template_list (klass);
  GstPadTemplate *templ;

  /* find suitable template */
  while (templ_list) {
    templ = (GstPadTemplate *) templ_list->data;

    if (GST_PAD_TEMPLATE_DIRECTION (templ) != direction
        || GST_PAD_TEMPLATE_PRESENCE (templ) != presence) {
      templ_list = templ_list->next;
      templ = NULL;
      continue;
    }

    break;
  }

  return templ;
}

static gboolean
_connect_mixer_element (GstGLMixerBin * self)
{
  gboolean res = TRUE;

  g_return_val_if_fail (self->priv->input_chains == NULL, FALSE);

  gst_object_set_name (GST_OBJECT (self->mixer), "mixer");
  res &= gst_bin_add (GST_BIN (self), self->mixer);

  res &= gst_element_link_pads (self->mixer, "src", self->out_convert, "sink");

  if (!res)
    GST_ERROR_OBJECT (self, "Failed to link mixer element into the pipeline");

  gst_element_sync_state_with_parent (self->mixer);

  return res;
}

void
gst_gl_mixer_bin_finish_init_with_element (GstGLMixerBin * self,
    GstElement * element)
{
  g_return_if_fail (GST_IS_ELEMENT (element));

  self->mixer = element;

  if (!_connect_mixer_element (self)) {
    gst_object_unref (self->mixer);
    self->mixer = NULL;
  }
}

void
gst_gl_mixer_bin_finish_init (GstGLMixerBin * self)
{
  GstGLMixerBinClass *klass = GST_GL_MIXER_BIN_GET_CLASS (self);
  GstElement *element = NULL;

  if (klass->create_element)
    element = klass->create_element ();

  if (element)
    gst_gl_mixer_bin_finish_init_with_element (self, element);
}

static void
gst_gl_mixer_bin_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec)
{
  GstGLMixerBin *self = GST_GL_MIXER_BIN (object);

  switch (prop_id) {
    case PROP_MIXER:
      g_value_set_object (value, self->mixer);
      break;
    default:
      if (self->mixer)
        g_object_get_property (G_OBJECT (self->mixer), pspec->name, value);
      break;
  }
}

static void
gst_gl_mixer_bin_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec)
{
  GstGLMixerBin *self = GST_GL_MIXER_BIN (object);

  switch (prop_id) {
    case PROP_MIXER:
    {
      GstElement *mixer = g_value_get_object (value);
      /* FIXME: deal with replacing a mixer */
      g_return_if_fail (!self->mixer || (self->mixer == mixer));
      self->mixer = mixer;
      if (mixer) {
        gst_object_ref_sink (mixer);
        _connect_mixer_element (self);
      }
      break;
    }
    default:
      if (self->mixer)
        g_object_set_property (G_OBJECT (self->mixer), pspec->name, value);
      break;
  }
}

static GstPad *
gst_gl_mixer_bin_request_new_pad (GstElement * element, GstPadTemplate * templ,
    const gchar * req_name, const GstCaps * caps)
{
  GstGLMixerBin *self = GST_GL_MIXER_BIN (element);
  GstPadTemplate *mixer_templ;
  struct input_chain *chain;
  GstPad *mixer_pad;

  chain = g_new0 (struct input_chain, 1);

  mixer_templ = _find_element_pad_template (self->mixer,
      GST_PAD_TEMPLATE_DIRECTION (templ), GST_PAD_TEMPLATE_PRESENCE (templ));
  g_return_val_if_fail (mixer_templ, NULL);

  mixer_pad =
      gst_element_request_pad (self->mixer, mixer_templ, req_name, NULL);
  g_return_val_if_fail (mixer_pad, NULL);

  if (!_create_input_chain (self, chain, mixer_pad)) {
    gst_element_release_request_pad (self->mixer, mixer_pad);
    _free_input_chain (chain);
    return NULL;
  }

  GST_OBJECT_LOCK (element);
  self->priv->input_chains = g_list_prepend (self->priv->input_chains, chain);
  GST_OBJECT_UNLOCK (element);

  gst_child_proxy_child_added (GST_CHILD_PROXY (self),
      G_OBJECT (chain->ghost_pad), GST_OBJECT_NAME (chain->ghost_pad));

  return GST_PAD (chain->ghost_pad);
}

static void
gst_gl_mixer_bin_release_pad (GstElement * element, GstPad * pad)
{
  GstGLMixerBin *self = GST_GL_MIXER_BIN (element);
  GList *l = self->priv->input_chains;
  gboolean released = FALSE;

  GST_OBJECT_LOCK (element);
  while (l) {
    struct input_chain *chain = l->data;
    if (GST_PAD (chain->ghost_pad) == pad) {
      self->priv->input_chains =
          g_list_remove_link (self->priv->input_chains, l);
      GST_OBJECT_UNLOCK (element);

      _free_input_chain (chain);
      gst_element_remove_pad (element, pad);
      released = TRUE;
      break;
    }
    l = l->next;
  }
  if (!released)
    GST_OBJECT_UNLOCK (element);
}

static GstStateChangeReturn
gst_gl_mixer_bin_change_state (GstElement * element, GstStateChange transition)
{
  GstGLMixerBin *self = GST_GL_MIXER_BIN (element);
  GstGLMixerBinClass *klass = GST_GL_MIXER_BIN_GET_CLASS (self);
  GstStateChangeReturn ret;

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
      GST_OBJECT_LOCK (element);
      if (!self->mixer) {
        if (klass->create_element)
          self->mixer = klass->create_element ();

        if (!self->mixer)
          g_signal_emit (element,
              gst_gl_mixer_bin_signals[SIGNAL_CREATE_ELEMENT], 0, &self->mixer);

        if (!self->mixer) {
          GST_ERROR_OBJECT (element, "Failed to retrieve element");
          GST_OBJECT_UNLOCK (element);
          return GST_STATE_CHANGE_FAILURE;
        }
        GST_OBJECT_UNLOCK (element);
        if (!_connect_mixer_element (self))
          return GST_STATE_CHANGE_FAILURE;

        GST_OBJECT_LOCK (element);
      }
      self->priv->running = TRUE;
      GST_OBJECT_UNLOCK (element);
      break;
    default:
      break;
  }

  ret =
      GST_ELEMENT_CLASS (gst_gl_mixer_bin_parent_class)->change_state (element,
      transition);
  if (ret == GST_STATE_CHANGE_FAILURE)
    return ret;

  switch (transition) {
    case GST_STATE_CHANGE_READY_TO_NULL:
      GST_OBJECT_LOCK (self);
      self->priv->running = FALSE;
      GST_OBJECT_UNLOCK (self);
    default:
      break;
  }

  return ret;
}

static GObject *
gst_gl_mixer_bin_child_proxy_get_child_by_index (GstChildProxy * child_proxy,
    guint index)
{
  GstGLMixerBin *mixer = GST_GL_MIXER_BIN (child_proxy);
  GstBin *bin = GST_BIN_CAST (child_proxy);
  GObject *res = NULL;

  GST_OBJECT_LOCK (bin);
  /* XXX: not exactly thread safe with ordering */
  if (index < bin->numchildren) {
    if ((res = g_list_nth_data (bin->children, index)))
      gst_object_ref (res);
  } else {
    struct input_chain *chain;
    if ((chain =
            g_list_nth_data (mixer->priv->input_chains,
                index - bin->numchildren))) {
      res = gst_object_ref (chain->ghost_pad);
    }
  }
  GST_OBJECT_UNLOCK (bin);

  return res;
}

static guint
gst_gl_mixer_bin_child_proxy_get_children_count (GstChildProxy * child_proxy)
{
  GstGLMixerBin *mixer = GST_GL_MIXER_BIN (child_proxy);
  GstBin *bin = GST_BIN_CAST (child_proxy);
  guint num;

  GST_OBJECT_LOCK (bin);
  num = bin->numchildren + g_list_length (mixer->priv->input_chains);
  GST_OBJECT_UNLOCK (bin);

  return num;
}

static void
gst_gl_mixer_bin_child_proxy_init (gpointer g_iface, gpointer iface_data)
{
  GstChildProxyInterface *iface = g_iface;

  iface->get_children_count = gst_gl_mixer_bin_child_proxy_get_children_count;
  iface->get_child_by_index = gst_gl_mixer_bin_child_proxy_get_child_by_index;
}
