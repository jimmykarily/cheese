/*
 * Copyright © 2009 Filippo Argiolas <filippo.argiolas@gmail.com>
 * Copyright © 2007,2008 Jaap Haitsma <jaap@haitsma.org>
 * Copyright © 2007-2009 daniel g. siegel <dgsiegel@gnome.org>
 * Copyright © 2008 Ryan Zeigler <zeiglerr@gmail.com>
 *
 * Licensed under the GNU General Public License Version 2
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifdef HAVE_CONFIG_H
  #include <cheese-config.h>
#endif

#include <glib.h>
#include <glib/gi18n.h>
#include <gio/gio.h>

#include "cheese-camera-device.h"

/**
 * SECTION:cheese-camera-device
 * @short_description: Object to represent a video capture device
 * @stability: Unstable
 * @include: cheese/cheese-camera-device.h
 *
 * #CheeseCameraDevice provides an abstraction of a video capture device.
 */

static void     cheese_camera_device_initable_iface_init (GInitableIface *iface);
static gboolean cheese_camera_device_initable_init (GInitable    *initable,
                                                    GCancellable *cancellable,
                                                    GError      **error);

G_DEFINE_TYPE_WITH_CODE (CheeseCameraDevice, cheese_camera_device, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE,
                                                cheese_camera_device_initable_iface_init))

#define CHEESE_CAMERA_DEVICE_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), CHEESE_TYPE_CAMERA_DEVICE, \
                                                                          CheeseCameraDevicePrivate))

#define CHEESE_CAMERA_DEVICE_ERROR cheese_camera_device_error_quark ()

/*
 * CheeseCameraDeviceError:
 * @CHEESE_CAMERA_DEVICE_ERROR_UNKNOWN: unknown error
 * @CHEESE_CAMERA_DEVICE_ERROR_NOT_SUPPORTED: cancellation of device
 * initialisation was requested, but is not supported
 * @CHEESE_CAMERA_DEVICE_ERROR_UNSUPPORTED_CAPS: unsupported GStreamer
 * capabilities
 * @CHEESE_CAMERA_DEVICE_ERROR_FAILED_INITIALIZATION: the device failed to
 * initialize for capability probing
 *
 * Errors that can occur during device initialization.
 */
enum CheeseCameraDeviceError
{
  CHEESE_CAMERA_DEVICE_ERROR_UNKNOWN,
  CHEESE_CAMERA_DEVICE_ERROR_NOT_SUPPORTED,
  CHEESE_CAMERA_DEVICE_ERROR_UNSUPPORTED_CAPS,
  CHEESE_CAMERA_DEVICE_ERROR_FAILED_INITIALIZATION
};

GST_DEBUG_CATEGORY (cheese_camera_device_cat);
#define GST_CAT_DEFAULT cheese_camera_device_cat

static gchar *supported_formats[] = {
  "video/x-raw-rgb",
  "video/x-raw-yuv",
  NULL
};

/* FIXME: make this configurable */
/*
 * CHEESE_MAXIMUM_RATE:
 *
 * The maximum framerate, in frames per second.
 */
static const guint CHEESE_MAXIMUM_RATE = 30;

enum
{
  PROP_0,
  PROP_NAME,
  PROP_DEVICE_NODE,
  PROP_UUID,
  PROP_V4LAPI_VERSION,
  PROP_LAST
};

static GParamSpec *properties[PROP_LAST];

struct _CheeseCameraDevicePrivate
{
  gchar *device_node;
  gchar *uuid;
  const gchar *src;
  gchar *name;
  guint  v4lapi_version;
  GstCaps *caps;
  GList   *formats;

  GError *construct_error;
};

GQuark cheese_camera_device_error_quark (void);

GQuark
cheese_camera_device_error_quark (void)
{
  return g_quark_from_static_string ("cheese-camera-device-error-quark");
}

/* CheeseVideoFormat */

static CheeseVideoFormat *
cheese_video_format_copy (const CheeseVideoFormat *format)
{
  return g_slice_dup (CheeseVideoFormat, format);
}

static void
cheese_video_format_free (CheeseVideoFormat *format)
{
  if (G_LIKELY (format != NULL))
    g_slice_free (CheeseVideoFormat, format);
}

GType
cheese_video_format_get_type (void)
{
  static GType our_type = 0;

  if (G_UNLIKELY (our_type == 0))
    our_type =
      g_boxed_type_register_static ("CheeseVideoFormat",
                                    (GBoxedCopyFunc) cheese_video_format_copy,
                                    (GBoxedFreeFunc) cheese_video_format_free);
  return our_type;
}

/* the rest */

static gint
compare_formats (gconstpointer a, gconstpointer b)
{
  const CheeseVideoFormat *c = a;
  const CheeseVideoFormat *d = b;

  /* descending sort for rectangle area */
  return (d->width * d->height - c->width * c->height);
}

/*
 * cheese_camera_device_filter_caps:
 * @device: the #CheeseCameraDevice
 * @caps: the #GstCaps that the device supports
 * @formats: an array of strings of video formats, in the form axb, where a and
 * b are in units of pixels
 *
 * Filter the supplied @caps with %CHEESE_MAXIMUM_RATE to only allow @formats
 * which can reach the desired framerate.
 *
 * Returns: the filtered #GstCaps
 */
static GstCaps *
cheese_camera_device_filter_caps (CheeseCameraDevice *device, const GstCaps *caps, GStrv formats)
{
  GstCaps *filter;
  GstCaps *allowed;
  guint    i;

  filter = gst_caps_new_simple (formats[0],
                                "framerate", GST_TYPE_FRACTION_RANGE,
                                0, 1, CHEESE_MAXIMUM_RATE, 1,
                                NULL);

  for (i = 1; i < g_strv_length (formats); i++)
  {
    gst_caps_append (filter,
                     gst_caps_new_simple (formats[i],
                                          "framerate", GST_TYPE_FRACTION_RANGE,
                                          0, 1, CHEESE_MAXIMUM_RATE, 1,
                                          NULL));
  }

  allowed = gst_caps_intersect (caps, filter);

  GST_DEBUG ("Supported caps %" GST_PTR_FORMAT, caps);
  GST_DEBUG ("Filter caps %" GST_PTR_FORMAT, filter);
  GST_DEBUG ("Filtered caps %" GST_PTR_FORMAT, allowed);

  gst_caps_unref (filter);

  return allowed;
}

/*
 * cheese_camera_device_add_format:
 * @device: a #CheeseCameraDevice
 * @format: the #CheeseVideoFormat to add
 *
 * Add the supplied @format to the list of formats supported by the @device.
 */
static void
cheese_camera_device_add_format (CheeseCameraDevice *device, CheeseVideoFormat *format)
{
  CheeseCameraDevicePrivate *priv =  device->priv;
  GList *l;

  for (l = priv->formats; l != NULL; l = l->next)
  {
    CheeseVideoFormat *item = l->data;
    if ((item != NULL) &&
        (item->width == format->width) &&
        (item->height == format->height)) return;
  }

  GST_INFO ("%dx%d", format->width, format->height);

  priv->formats = g_list_append (priv->formats, format);
}

/*
 * free_format_list:
 * @device: a #CheeseCameraDevice
 *
 * Free the list of video formats for the @device.
 */
static void
free_format_list (CheeseCameraDevice *device)
{
  CheeseCameraDevicePrivate *priv = device->priv;

  g_list_free_full (priv->formats, g_free);
  priv->formats = NULL;
}

/*
 * cheese_camera_device_update_format_table:
 * @device: a #CheeseCameraDevice
 *
 * Clear the current list of video formats supported by the @device and create
 * it anew.
 */
static void
cheese_camera_device_update_format_table (CheeseCameraDevice *device)
{
  CheeseCameraDevicePrivate *priv = device->priv;

  guint i;
  guint num_structures;

  free_format_list (device);

  num_structures = gst_caps_get_size (priv->caps);
  for (i = 0; i < num_structures; i++)
  {
    GstStructure *structure;
    const GValue *width, *height;
    structure = gst_caps_get_structure (priv->caps, i);

    width  = gst_structure_get_value (structure, "width");
    height = gst_structure_get_value (structure, "height");

    if (G_VALUE_HOLDS_INT (width))
    {
      CheeseVideoFormat *format = g_new0 (CheeseVideoFormat, 1);

      gst_structure_get_int (structure, "width", &(format->width));
      gst_structure_get_int (structure, "height", &(format->height));
      cheese_camera_device_add_format (device, format);
    }
    else if (GST_VALUE_HOLDS_INT_RANGE (width))
    {
      gint min_width, max_width, min_height, max_height;
      gint cur_width, cur_height;

      min_width  = gst_value_get_int_range_min (width);
      max_width  = gst_value_get_int_range_max (width);
      min_height = gst_value_get_int_range_min (height);
      max_height = gst_value_get_int_range_max (height);

      cur_width  = min_width;
      cur_height = min_height;

      /* Gstreamer will sometimes give us a range with min_xxx == max_xxx,
       * we use <= here (and not below) to make this work */
      while (cur_width <= max_width && cur_height <= max_height)
      {
        CheeseVideoFormat *format = g_new0 (CheeseVideoFormat, 1);

        format->width  = cur_width;
        format->height = cur_height;

        cheese_camera_device_add_format (device, format);

        cur_width  *= 2;
        cur_height *= 2;
      }

      cur_width  = max_width;
      cur_height = max_height;
      while (cur_width > min_width && cur_height > min_height)
      {
        CheeseVideoFormat *format = g_new0 (CheeseVideoFormat, 1);

        format->width  = cur_width;
        format->height = cur_height;

        cheese_camera_device_add_format (device, format);

        cur_width  /= 2;
        cur_height /= 2;
      }
    }
    else
    {
      g_critical ("GValue type %s, cannot be handled for resolution width", G_VALUE_TYPE_NAME (width));
    }
  }
}

/*
 * cheese_camera_device_get_caps:
 * @device: a #CheeseCameraDevice
 *
 * Probe the #GstCaps that the @device supports.
 */
static void
cheese_camera_device_get_caps (CheeseCameraDevice *device)
{
  CheeseCameraDevicePrivate *priv = device->priv;

  gchar               *pipeline_desc;
  GstElement          *pipeline;
  GstStateChangeReturn ret;
  GstMessage          *msg;
  GstBus              *bus;
  GError              *err = NULL;

  pipeline_desc = g_strdup_printf ("%s name=source device=%s ! fakesink",
                                   priv->src, priv->device_node);
  pipeline = gst_parse_launch (pipeline_desc, &err);
  if ((pipeline != NULL) && (err == NULL))
  {
    /* Start the pipeline and wait for max. 10 seconds for it to start up */
    gst_element_set_state (pipeline, GST_STATE_READY);
    ret = gst_element_get_state (pipeline, NULL, NULL, 10 * GST_SECOND);

    /* Check if any error messages were posted on the bus */
    bus = gst_element_get_bus (pipeline);
    msg = gst_bus_pop_filtered (bus, GST_MESSAGE_ERROR);
    gst_object_unref (bus);

    if ((msg == NULL) && (ret == GST_STATE_CHANGE_SUCCESS))
    {
      GstElement *src;
      GstPad     *pad;
      GstCaps    *caps;

      src = gst_bin_get_by_name (GST_BIN (pipeline), "source");

      GST_LOG ("Device: %s (%s)\n", priv->name, priv->device_node);
      pad        = gst_element_get_pad (src, "src");
      caps       = gst_pad_get_caps (pad);
      priv->caps = cheese_camera_device_filter_caps (device, caps, supported_formats);
      if (!gst_caps_is_empty (priv->caps))
        cheese_camera_device_update_format_table (device);
      else
      {
        g_set_error_literal (&priv->construct_error,
                             CHEESE_CAMERA_DEVICE_ERROR,
                             CHEESE_CAMERA_DEVICE_ERROR_UNSUPPORTED_CAPS,
                             _("Device capabilities not supported"));
      }

      gst_object_unref (pad);
      gst_caps_unref (caps);
      gst_object_unref (src);
    }
    else
    {
      if (msg)
      {
        gchar *dbg_info = NULL;
        gst_message_parse_error (msg, &err, &dbg_info);
        GST_WARNING ("Failed to start the capability probing pipeline");
        GST_WARNING ("Error from element %s: %s, %s",
                     GST_OBJECT_NAME (msg->src),
                     err->message,
                     (dbg_info) ? dbg_info : "no extra debug detail");
        g_error_free (err);
        err = NULL;

        /* construct_error is meant to be displayed in the UI
         * (although it currently isn't displayed in cheese),
         * err->message from gstreamer is too technical for this
         * purpose, the idea is warn the user about an error and point
         * him to the logs for more info */
        g_set_error (&priv->construct_error,
                     CHEESE_CAMERA_DEVICE_ERROR,
                     CHEESE_CAMERA_DEVICE_ERROR_FAILED_INITIALIZATION,
                     _("Failed to initialize device %s for capability probing"),
                     priv->device_node);
      }
    }
    gst_element_set_state (pipeline, GST_STATE_NULL);
    gst_object_unref (pipeline);
  }

  if (err)
    g_error_free (err);

  g_free (pipeline_desc);
}

static void
cheese_camera_device_constructed (GObject *object)
{
  CheeseCameraDevice        *device = CHEESE_CAMERA_DEVICE (object);
  CheeseCameraDevicePrivate *priv   = device->priv;

  priv->src = (priv->v4lapi_version == 2) ? "v4l2src" : "v4lsrc";

  cheese_camera_device_get_caps (device);

  if (G_OBJECT_CLASS (cheese_camera_device_parent_class)->constructed)
    G_OBJECT_CLASS (cheese_camera_device_parent_class)->constructed (object);
}

static void
cheese_camera_device_get_property (GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
  CheeseCameraDevice        *device = CHEESE_CAMERA_DEVICE (object);
  CheeseCameraDevicePrivate *priv   = device->priv;

  switch (prop_id)
  {
    case PROP_NAME:
      g_value_set_string (value, priv->name);
      break;
    case PROP_DEVICE_NODE:
      g_value_set_string (value, priv->device_node);
      break;
    case PROP_UUID:
      g_value_set_string (value, priv->uuid);
      break;
    case PROP_V4LAPI_VERSION:
      g_value_set_uint (value, priv->v4lapi_version);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
cheese_camera_device_set_property (GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
  CheeseCameraDevice        *device = CHEESE_CAMERA_DEVICE (object);
  CheeseCameraDevicePrivate *priv   = device->priv;

  switch (prop_id)
  {
    case PROP_NAME:
      if (priv->name)
        g_free (priv->name);
      priv->name = g_value_dup_string (value);
      break;
    case PROP_UUID:
      if (priv->uuid)
        g_free (priv->uuid);
      priv->uuid = g_value_dup_string (value);
      break;
    case PROP_DEVICE_NODE:
      if (priv->device_node)
        g_free (priv->device_node);
      priv->device_node = g_value_dup_string (value);
      break;
    case PROP_V4LAPI_VERSION:
      priv->v4lapi_version = g_value_get_uint (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
cheese_camera_device_finalize (GObject *object)
{
  CheeseCameraDevice        *device = CHEESE_CAMERA_DEVICE (object);
  CheeseCameraDevicePrivate *priv   = device->priv;

  g_free (priv->device_node);
  g_free (priv->uuid);
  g_free (priv->name);

  gst_caps_unref (priv->caps);
  free_format_list (device);

  G_OBJECT_CLASS (cheese_camera_device_parent_class)->finalize (object);
}

static void
cheese_camera_device_class_init (CheeseCameraDeviceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  if (cheese_camera_device_cat == NULL)
    GST_DEBUG_CATEGORY_INIT (cheese_camera_device_cat,
                             "cheese-camera-device",
                             0, "Cheese Camera Device");

  object_class->finalize     = cheese_camera_device_finalize;
  object_class->get_property = cheese_camera_device_get_property;
  object_class->set_property = cheese_camera_device_set_property;
  object_class->constructed  = cheese_camera_device_constructed;

  /**
   * CheeseCameraDevice:name:
   *
   * Human-readable name of the video capture device, for display to the user.
   */
  properties[PROP_NAME] = g_param_spec_string ("name",
                                               "Name of the device",
                                               "Human-readable name of the video capture device",
                                               NULL,
                                               G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY);

  /**
   * CheeseCameraDevice:device-node:
   *
   * Path to the device node of the video capture device.
   */
  properties[PROP_DEVICE_NODE] = g_param_spec_string ("device-node",
                                                      "Device node",
                                                      "Path to the device node of the video capture device",
                                                      NULL,
                                                      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY);

  /**
   * CheeseCameraDevice:uuid:
   *
   * UUID of the video capture device.
   */
  properties[PROP_UUID] = g_param_spec_string ("uuid",
                                               "Device UUID",
                                               "UUID of the video capture device",
                                               NULL,
                                               G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY);

  /**
   * CheeseCameraDevice:v4l-api-version:
   *
   * Version of the Video4Linux API that the device supports. Currently, either
   * 1 or 2 are supported.
   */
  properties[PROP_V4LAPI_VERSION] = g_param_spec_uint ("v4l-api-version",
                                                       "Video4Linux API version",
                                                       "Version of the Video4Linux API that the device supports",
                                                       1, 2, 2,
                                                       G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY);

  g_object_class_install_properties (object_class, PROP_LAST, properties);

  g_type_class_add_private (klass, sizeof (CheeseCameraDevicePrivate));
}

static void
cheese_camera_device_initable_iface_init (GInitableIface *iface)
{
  iface->init = cheese_camera_device_initable_init;
}

static void
cheese_camera_device_init (CheeseCameraDevice *device)
{
  CheeseCameraDevicePrivate *priv = device->priv = CHEESE_CAMERA_DEVICE_GET_PRIVATE (device);

  priv->device_node = NULL;
  priv->uuid = NULL;
  priv->src = NULL;
  priv->name = g_strdup (_("Unknown device"));
  priv->caps = gst_caps_new_empty ();

  priv->formats = NULL;

  priv->construct_error = NULL;
}

static gboolean
cheese_camera_device_initable_init (GInitable    *initable,
                                    GCancellable *cancellable,
                                    GError      **error)
{
  CheeseCameraDevice        *device = CHEESE_CAMERA_DEVICE (initable);
  CheeseCameraDevicePrivate *priv   = device->priv;

  g_return_val_if_fail (CHEESE_IS_CAMERA_DEVICE (initable), FALSE);

  if (cancellable != NULL)
  {
    g_set_error_literal (error,
                         CHEESE_CAMERA_DEVICE_ERROR,
                         CHEESE_CAMERA_DEVICE_ERROR_NOT_SUPPORTED,
                         _("Cancellable initialization not supported"));
    return FALSE;
  }

  if (priv->construct_error)
  {
    if (error)
      *error = g_error_copy (priv->construct_error);
    return FALSE;
  }

  return TRUE;
}

/* public methods */

/**
 * cheese_camera_device_new:
 * @uuid: UUID of the device, as supplied by udev
 * @device_node: (type filename): path to the device node of the video capture
 * device
 * @name: human-readable name of the device, as supplied by udev
 * @v4l_api_version: version of the Video4Linux API that the device uses. Currently
 * either 1 or 2
 * @error: a location to store errors
 *
 * Tries to create a new #CheeseCameraDevice with the supplied parameters. If
 * construction fails, %NULL is returned, and @error is set.
 *
 * Returns: a new #CheeseCameraDevice, or %NULL
 */
CheeseCameraDevice *
cheese_camera_device_new (const gchar *uuid,
                          const gchar *device_node,
                          const gchar *name,
                          guint        v4l_api_version,
                          GError     **error)
{
  return CHEESE_CAMERA_DEVICE (g_initable_new (CHEESE_TYPE_CAMERA_DEVICE,
                                               NULL, error,
                                               "uuid", uuid,
                                               "device-node", device_node,
                                               "name", name,
                                               "v4l-api-version", v4l_api_version,
                                               NULL));
}

/**
 * cheese_camera_device_get_format_list:
 * @device: a #CheeseCameraDevice
 *
 * Get the sorted list of #CheeseVideoFormat that the @device supports.
 *
 * Returns: (element-type Cheese.VideoFormat) (transfer container): list of
 * #CheeseVideoFormat
 */
GList *
cheese_camera_device_get_format_list (CheeseCameraDevice *device)
{
  g_return_val_if_fail (CHEESE_IS_CAMERA_DEVICE (device), NULL);

  return g_list_sort (g_list_copy (device->priv->formats), compare_formats);
}

/**
 * cheese_camera_device_get_name:
 * @device: a #CheeseCameraDevice
 *
 * Get a human-readable name for the device, as reported by udev, which is
 * suitable for display to a user.
 *
 * Returns: (transfer none): the human-readable name of the video capture device
 */
const gchar *
cheese_camera_device_get_name (CheeseCameraDevice *device)
{
  g_return_val_if_fail (CHEESE_IS_CAMERA_DEVICE (device), NULL);

  return device->priv->name;
}

/**
 * cheese_camera_device_get_uuid:
 * @device: a #CheeseCameraDevice
 *
 * Get the UUID of the @device, as reported by udev.
 *
 * Returns: (transfer none): the UUID of the video capture device
 */
const gchar *
cheese_camera_device_get_uuid (CheeseCameraDevice *device)
{
  g_return_val_if_fail (CHEESE_IS_CAMERA_DEVICE (device), NULL);

  return device->priv->uuid;
}

/**
 * cheese_camera_device_get_src:
 * @device: a #CheeseCameraDevice
 *
 * Get the name of the source GStreamer element for the @device. Currently,
 * this will be either v4lsrc or v4l2src, depending on the version of the
 * Video4Linux API that the device supports.
 *
 * Returns: (transfer none): the name of the source GStreamer element
 */
const gchar *
cheese_camera_device_get_src (CheeseCameraDevice *device)
{
  g_return_val_if_fail (CHEESE_IS_CAMERA_DEVICE (device), NULL);

  return device->priv->src;
}

/**
 * cheese_camera_device_get_device_node:
 * @device: a #CheeseCameraDevice
 *
 * Get the path to the device node associated with the @device.
 *
 * Returns: (transfer none): the path to the device node of the video capture
 * device
 */
const gchar *
cheese_camera_device_get_device_node (CheeseCameraDevice *device)
{
  g_return_val_if_fail (CHEESE_IS_CAMERA_DEVICE (device), NULL);

  return device->priv->device_node;
}

/**
 * cheese_camera_device_get_best_format:
 * @device: a #CheeseCameraDevice
 *
 * Get the #CheeseVideoFormat with the highest rsolution for this @device.
 *
 * Returns: (transfer full): the highest-resolution supported
 * #CheeseVideoFormat
 */

CheeseVideoFormat *
cheese_camera_device_get_best_format (CheeseCameraDevice *device)
{
  g_return_val_if_fail (CHEESE_IS_CAMERA_DEVICE (device), NULL);

  CheeseVideoFormat *format = g_boxed_copy (CHEESE_TYPE_VIDEO_FORMAT,
                                            cheese_camera_device_get_format_list (device)->data);

  GST_INFO ("%dx%d", format->width, format->height);
  return format;
}

/**
 * cheese_camera_device_get_caps_for_format:
 * @device: a #CheeseCameraDevice
 * @format: a #CheeseVideoFormat
 *
 * Get the #GstCaps for the given @format on the @device.
 *
 * Returns: (transfer full): the #GstCaps for the given @format
 */
GstCaps *
cheese_camera_device_get_caps_for_format (CheeseCameraDevice *device,
                                          CheeseVideoFormat  *format)
{
  GstCaps *desired_caps;
  GstCaps *subset_caps;
  guint    i, length;

  g_return_val_if_fail (CHEESE_IS_CAMERA_DEVICE (device), NULL);

  GST_INFO ("Getting caps for %dx%d", format->width, format->height);

  desired_caps = gst_caps_new_simple (supported_formats[0],
                                      "width", G_TYPE_INT,
                                      format->width,
                                      "height", G_TYPE_INT,
                                      format->height,
                                      NULL);

  length = g_strv_length (supported_formats);
  for (i = 1; i < length; i++)
  {
    gst_caps_append (desired_caps,
                     gst_caps_new_simple (supported_formats[i],
                                          "width", G_TYPE_INT,
                                          format->width,
                                          "height", G_TYPE_INT,
                                          format->height,
                                          NULL));
  }

  subset_caps = gst_caps_intersect (desired_caps, device->priv->caps);
  gst_caps_unref (desired_caps);

  GST_INFO ("Got %" GST_PTR_FORMAT, subset_caps);

  return subset_caps;
}
