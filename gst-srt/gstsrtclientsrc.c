/* GStreamer SRT plugin based on libsrt
 * Copyright (C) 2017, Collabora Ltd.
 *   Author:Justin Kim <justin.kim@collabora.com>
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
 * SECTION:element-srtclientsrc
 * @title: srtclientsrc
 *
 * srtclientsrc is a network source that reads <ulink url="http://www.srtalliance.org/">SRT</ulink>
 * packets from the network. Although SRT is a protocol based on UDP, srtclientsrc works like
 * a client socket of connection-oriented protocol.
 *
 * <refsect2>
 * <title>Examples</title>
 * |[
 * gst-launch-1.0 -v srtclientsrc uri="srt://127.0.0.1:7001" ! fakesink
 * ]| This pipeline shows how to connect SRT server by setting #GstSRTClientSrc:uri property.
 *
 * |[
 * gst-launch-1.0 -v srtclientsrc uri="srt://192.168.1.10:7001" rendez-vous ! fakesink
 * ]| This pipeline shows how to connect SRT server by setting #GstSRTClientSrc:uri property and using the rendez-vous mode.
 * </refsect2>
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstsrtclientsrc.h"
#include <srt.h>
#include <gio/gio.h>

#include "gstsrt.h"
#include "gstsrtmeta.h"

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE ("src",
  GST_PAD_SRC,
  GST_PAD_ALWAYS,
  GST_STATIC_CAPS_ANY);

#define GST_CAT_DEFAULT gst_debug_srt_client_src
GST_DEBUG_CATEGORY (GST_CAT_DEFAULT);

struct _GstSRTClientSrcPrivate
{
  SRTSOCKET sock;
  gint poll_id;
  gint poll_timeout;
  gint last_msg_num;

  gboolean rendezvous;
  gchar *bind_address;
  guint16 bind_port;
};

#define GST_SRT_CLIENT_SRC_GET_PRIVATE(obj)  \
       (G_TYPE_INSTANCE_GET_PRIVATE ((obj), GST_TYPE_SRT_CLIENT_SRC, GstSRTClientSrcPrivate))

#define SRT_DEFAULT_POLL_TIMEOUT - 1
enum
{
  PROP_POLL_TIMEOUT = 1,
  PROP_BIND_ADDRESS,
  PROP_BIND_PORT,
  PROP_RENDEZ_VOUS,
#if GST_VERSION_MINOR >= 14
  PROP_STATS,
#endif

  /*< private > */
  PROP_LAST
};

static GParamSpec *properties[PROP_LAST];

#define gst_srt_client_src_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE (GstSRTClientSrc, gst_srt_client_src,
  GST_TYPE_SRT_BASE_SRC, G_ADD_PRIVATE (GstSRTClientSrc)
  GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, "srtclientsrc", 0,
    "SRT Client Source"));

void SRTLogHandler (void* opaque, int level, const char* file, int line, const char* area, const char* message);

static void
gst_srt_client_src_get_property (GObject * object,
  guint prop_id, GValue * value, GParamSpec * pspec)
{
  GstSRTClientSrc *self = GST_SRT_CLIENT_SRC (object);
  GstSRTClientSrcPrivate *priv = GST_SRT_CLIENT_SRC_GET_PRIVATE (self);

  switch (prop_id) {
  case PROP_POLL_TIMEOUT:
    g_value_set_int (value, priv->poll_timeout);
    break;
  case PROP_BIND_PORT:
    g_value_set_int (value, priv->rendezvous);
    break;
  case PROP_BIND_ADDRESS:
    g_value_set_string (value, priv->bind_address);
    break;
  case PROP_RENDEZ_VOUS:
    g_value_set_boolean (value, priv->bind_port);
    break;
#if GST_VERSION_MINOR >= 14
  case PROP_STATS:
    g_value_take_boxed (value, gst_srt_base_src_get_stats (priv->sock));
    break;
#endif
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    break;
  }
}

static void
gst_srt_client_src_set_property (GObject * object,
  guint prop_id, const GValue * value, GParamSpec * pspec)
{
  GstSRTBaseSrc *self = GST_SRT_BASE_SRC (object);
  GstSRTClientSrcPrivate *priv = GST_SRT_CLIENT_SRC_GET_PRIVATE (self);

  switch (prop_id) {
  case PROP_POLL_TIMEOUT:
    priv->poll_timeout = g_value_get_int (value);
    break;
  case PROP_BIND_ADDRESS:
    g_free (priv->bind_address);
    priv->bind_address = g_value_dup_string (value);
    break;
  case PROP_BIND_PORT:
    priv->bind_port = g_value_get_int (value);
    break;
  case PROP_RENDEZ_VOUS:
    priv->rendezvous = g_value_get_boolean (value);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    break;
  }
}

static void
gst_srt_client_src_finalize (GObject * object)
{
  GstSRTClientSrc *self = GST_SRT_CLIENT_SRC (object);
  GstSRTClientSrcPrivate *priv = GST_SRT_CLIENT_SRC_GET_PRIVATE (self);

  if (priv->poll_id != SRT_ERROR) {
    srt_epoll_release (priv->poll_id);
    priv->poll_id = SRT_ERROR;
  }

  if (priv->sock != SRT_INVALID_SOCK) {
    srt_close (priv->sock);
    priv->sock = SRT_INVALID_SOCK;
  }

  g_free (priv->bind_address);
  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static GstFlowReturn
gst_srt_client_src_fill (GstPushSrc * src, GstBuffer * outbuf)
{
  GstSRTClientSrc *self = GST_SRT_CLIENT_SRC (src);
  GstSRTClientSrcPrivate *priv = GST_SRT_CLIENT_SRC_GET_PRIVATE (self);
  GstFlowReturn ret = GST_FLOW_OK;
  GstMapInfo info;
  /*int numSockets = 1;*/
  /*SRTSOCKET readySocket = 0;*/
  gint recv_len;
  SRT_MSGCTRL ctrl;

  /*
  if (srt_epoll_wait (priv->poll_id,
    &readySocket, &numSockets, 0, 0,
    priv->poll_timeout,
    0, 0, 0, 0) == -1) {

    / * Assuming that timeout error is normal * /
    if (srt_getlasterror (NULL) != SRT_ETIMEOUT) {
      GST_ELEMENT_ERROR (src, RESOURCE, READ,
        (NULL), ("srt_epoll_wait error: %s", srt_getlasterror_str ()));
      ret = GST_FLOW_ERROR;
    }
    else {
      GST_DEBUG_OBJECT (self, "SRT timeout");
    }
    srt_clearlasterror ();
    goto out;
  }
  */

  if (!gst_buffer_map (outbuf, &info, GST_MAP_WRITE)) {
    GST_ELEMENT_ERROR (src, RESOURCE, READ,
      ("Could not map the output stream"), (NULL));
    ret = GST_FLOW_ERROR;
    goto out;
  }

  GST_LOG_OBJECT(self, "Will recv");
  recv_len = srt_recvmsg2 (priv->sock, (char*)info.data,
      (int)gst_buffer_get_size (outbuf), &ctrl);
  GST_LOG_OBJECT(self, "recieved");

  gst_buffer_unmap (outbuf, &info);

  if (recv_len == SRT_ERROR) {
    GST_ELEMENT_ERROR (self, RESOURCE, READ,
      (NULL), ("srt_recvmsg error: %s", srt_getlasterror_str ()));
    ret = GST_FLOW_ERROR;
    goto out;
  } else if (recv_len == 0) {
    GST_DEBUG_OBJECT (self, "SRT EOS");
    ret = GST_FLOW_EOS;
    goto out;
  }
  else if (recv_len != 1316) {
      GST_WARNING ("Weird received size of %d", recv_len);
  }


  // Check if we've dropped some packets
  if (priv->last_msg_num != 0
      && (ctrl.msgno - priv->last_msg_num) > 1) {
      GST_WARNING_OBJECT (self, "Dropped %d. %d->%d", (ctrl.msgno - priv->last_msg_num - 1), priv->last_msg_num, ctrl.msgno);
  }
  priv->last_msg_num = ctrl.msgno;


  GST_BUFFER_PTS (outbuf) =
    gst_clock_get_time (GST_ELEMENT_CLOCK (src)) -
    GST_ELEMENT_CAST (src)->base_time;

  gst_buffer_resize (outbuf, 0, recv_len);

  GST_LOG_OBJECT (src,
    "filled buffer from _get of size %" G_GSIZE_FORMAT ", ts %"
    GST_TIME_FORMAT ", dur %" GST_TIME_FORMAT
    ", offset %" G_GINT64_FORMAT ", offset_end %" G_GINT64_FORMAT,
    gst_buffer_get_size (outbuf),
    GST_TIME_ARGS (GST_BUFFER_TIMESTAMP (outbuf)),
    GST_TIME_ARGS (GST_BUFFER_DURATION (outbuf)),
    GST_BUFFER_OFFSET (outbuf), GST_BUFFER_OFFSET_END (outbuf));

  // Add the src time that we received from srt as a GstMeta
  // this is so that downstream SRT elements can read that 
  // gstmeta and use it to keep the same time
  GstSrtMeta* meta = GST_SRT_META_ADD (outbuf);
  meta->src_time = ctrl.srctime;

out:
  return ret;
}

static gboolean
gst_srt_client_src_start (GstBaseSrc * src)
{
  GstSRTClientSrc *self = GST_SRT_CLIENT_SRC (src);
  GstSRTClientSrcPrivate *priv = GST_SRT_CLIENT_SRC_GET_PRIVATE (self);
  GstSRTBaseSrc *base = GST_SRT_BASE_SRC (src);
  GstUri *uri = gst_uri_ref (base->uri);
  GSocketAddress *socket_address = NULL;

  GST_INFO_OBJECT (self, "Will start SRT client src");

  // Enable for SRT logging, which is very noisy but informative
  // Also know that this requires SRT compiled with logging and
  // ideally, heavy logging
#if 0
  char NAME[] = "SRTLIB";
  srt_setloglevel (7);
  //srt_setloglevel (LOG_NOTICE);
  srt_setlogflags (0
      | SRT_LOGF_DISABLE_TIME
      | SRT_LOGF_DISABLE_SEVERITY
      | SRT_LOGF_DISABLE_THREADNAME
      | SRT_LOGF_DISABLE_EOL
  );
  srt_setloghandler (NAME, SRTLogHandler);
#endif

  priv->sock = gst_srt_client_connect_full (GST_ELEMENT (src), FALSE,
    gst_uri_get_host (uri), gst_uri_get_port (uri), priv->rendezvous,
    priv->bind_address, priv->bind_port, base->latency,
    &socket_address, &priv->poll_id, base->passphrase, base->key_length);
  GST_INFO_OBJECT (self, "SRT client src connected");

  priv->last_msg_num = 0;
  g_clear_object (&socket_address);
  g_clear_pointer (&uri, gst_uri_unref);

  return (priv->sock != SRT_INVALID_SOCK);
}

static gboolean
gst_srt_client_src_stop (GstBaseSrc * src)
{
  /* This is now handled in unlock
  GstSRTClientSrc *self = GST_SRT_CLIENT_SRC(src);
  GstSRTClientSrcPrivate *priv = GST_SRT_CLIENT_SRC_GET_PRIVATE(self);

  srt_epoll_remove_usock(priv->poll_id, priv->sock);
  srt_epoll_release(priv->poll_id);

  GST_DEBUG_OBJECT(self, "closing SRT connection");
  srt_close(priv->sock);
  */

  return TRUE;
}

static gboolean
gst_srt_client_src_unlock (GstBaseSrc * src)
{
  GstSRTClientSrc *self = GST_SRT_CLIENT_SRC (src);
  GstSRTClientSrcPrivate *priv = GST_SRT_CLIENT_SRC_GET_PRIVATE (self);
  GST_INFO_OBJECT (self, "unlocking client SRT connection");
  if (priv->poll_id != SRT_ERROR) {
    if (priv->sock != SRT_INVALID_SOCK)
      srt_epoll_remove_usock (priv->poll_id, priv->sock);
    srt_epoll_release (priv->poll_id);
  }
  priv->poll_id = SRT_ERROR;

  GST_INFO_OBJECT (self, "closing SRT connection");
  if (priv->sock != SRT_INVALID_SOCK)
    srt_close (priv->sock);
  priv->sock = SRT_INVALID_SOCK;
  return TRUE;
}

static void
gst_srt_client_src_class_init (GstSRTClientSrcClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *gstelement_class = GST_ELEMENT_CLASS (klass);
  GstBaseSrcClass *gstbasesrc_class = GST_BASE_SRC_CLASS (klass);
  GstPushSrcClass *gstpushsrc_class = GST_PUSH_SRC_CLASS (klass);

  gobject_class->set_property = gst_srt_client_src_set_property;
  gobject_class->get_property = gst_srt_client_src_get_property;
  gobject_class->finalize = gst_srt_client_src_finalize;

  /**
    * GstSRTClientSrc:poll-timeout:
    *
    * The timeout(ms) value when polling SRT socket.
    */
  properties[PROP_POLL_TIMEOUT] =
    g_param_spec_int ("poll-timeout", "Poll timeout",
      "Return poll wait after timeout miliseconds (-1 = infinite)", -1,
      G_MAXINT32, SRT_DEFAULT_POLL_TIMEOUT,
      G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY | G_PARAM_STATIC_STRINGS);

  properties[PROP_BIND_ADDRESS] =
    g_param_spec_string ("bind-address", "Bind Address",
      "Address to bind socket to (required for rendez-vous mode) ", NULL,
      G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY | G_PARAM_STATIC_STRINGS);

  properties[PROP_BIND_PORT] =
    g_param_spec_int ("bind-port", "Bind Port",
      "Port to bind socket to (Ignored in rendez-vous mode)", 0,
      G_MAXUINT16, 0,
      G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY | G_PARAM_STATIC_STRINGS);

  properties[PROP_RENDEZ_VOUS] =
    g_param_spec_boolean ("rendez-vous", "Rendez Vous",
      "Work in Rendez-Vous mode instead of client/caller mode", FALSE,
      G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY | G_PARAM_STATIC_STRINGS);

#if GST_VERSION_MINOR >= 14
  properties[PROP_STATS] = g_param_spec_boxed ("stats", "Statistics",
    "SRT Statistics", GST_TYPE_STRUCTURE,
    G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
#endif

  g_object_class_install_properties (gobject_class, PROP_LAST, properties);

  gst_element_class_add_static_pad_template (gstelement_class, &src_template);
  gst_element_class_set_metadata (gstelement_class,
    "SRT client source", "Source/Network",
    "Receive data over the network via SRT",
    "Justin Kim <justin.kim@collabora.com>");

  gstbasesrc_class->start = GST_DEBUG_FUNCPTR (gst_srt_client_src_start);
  gstbasesrc_class->stop = GST_DEBUG_FUNCPTR (gst_srt_client_src_stop);
  gstbasesrc_class->unlock = GST_DEBUG_FUNCPTR (gst_srt_client_src_unlock);
  gstpushsrc_class->fill = GST_DEBUG_FUNCPTR (gst_srt_client_src_fill);
}

static void
gst_srt_client_src_init (GstSRTClientSrc * self)
{
  GstSRTClientSrcPrivate *priv = GST_SRT_CLIENT_SRC_GET_PRIVATE (self);

  priv->sock = SRT_INVALID_SOCK;
  priv->poll_id = SRT_ERROR;
  priv->poll_timeout = SRT_DEFAULT_POLL_TIMEOUT;
  priv->rendezvous = FALSE;
  priv->bind_address = NULL;
  priv->bind_port = 0;
}
