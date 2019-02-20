/* GStreamer SRT plugin based on libsrt
 * Copyright (C) 2017, Collabora Ltd.
 *   Authors:
 *       Justin Kim <justin.kim@collabora.com>
 *       Olivier Crête <olivier.crete@collabora.com>
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
   * SECTION:element-srtserversink
   * @title: srtserversink
   *
   * srtserversink is a network sink that sends <ulink url="http://www.srtalliance.org/">SRT</ulink>
   * packets to the network. Although SRT is an UDP-based protocol, srtserversink works like
   * a server socket of connection-oriented protocol.
   *
   * <refsect2>
   * <title>Examples</title>
   * |[
   * gst-launch-1.0 -v audiotestsrc ! srtserversink
   * ]| This pipeline shows how to serve SRT packets through the default port.
   * </refsect2>
   *
   */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstsrtserversink.h"
#include "gstsrt.h"
#include <srt.h>
#include <gio/gio.h>

#define SRT_DEFAULT_POLL_TIMEOUT - 1
// Recommended size of the send buffer, in bytes
#define SRT_SEND_BUFFER_SIZE 1024 * 1024
// How many times a send fails in a row before we disconnect a client
#define MAX_SEND_FAILS 10

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE ("sink",
  GST_PAD_SINK,
  GST_PAD_ALWAYS,
  GST_STATIC_CAPS_ANY);

#define GST_CAT_DEFAULT gst_debug_srt_server_sink
GST_DEBUG_CATEGORY (GST_CAT_DEFAULT);

struct _GstSRTServerSinkPrivate
{
  gboolean cancelled;

  SRTSOCKET sock;
  gint poll_id;
  gint poll_timeout;

  GThread *thread;

  GList *clients;
  GAsyncQueue *pending_clients;
};

#define GST_SRT_SERVER_SINK_GET_PRIVATE(obj)  \
       (G_TYPE_INSTANCE_GET_PRIVATE ((obj), GST_TYPE_SRT_SERVER_SINK, GstSRTServerSinkPrivate))

enum
{
  PROP_POLL_TIMEOUT = 1,
#if GST_VERSION_MINOR >= 14
  PROP_STATS,
#endif
  /*< private > */
  PROP_LAST
};

static GParamSpec *properties[PROP_LAST];

enum
{
  SIG_CLIENT_ADDED,
  SIG_CLIENT_REMOVED,

  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

#define gst_srt_server_sink_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE (GstSRTServerSink, gst_srt_server_sink,
  GST_TYPE_SRT_BASE_SINK, G_ADD_PRIVATE (GstSRTServerSink)
  GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, "srtserversink", 0,
    "SRT Server Sink"));

typedef struct
{
  int sock;
  GSocketAddress *sockaddr;
  int num_send_fails;
} SRTClient;

static SRTClient *
srt_client_new (void)
{
  SRTClient *client = g_new0 (SRTClient, 1);
  client->sock = SRT_INVALID_SOCK;
  client->num_send_fails = 0;
  GST_DEBUG ("New SRT client");
  return client;
}

static void
srt_client_free (SRTClient * client)
{
  g_return_if_fail (client != NULL);
  g_clear_object (&client->sockaddr);

  if (client->sock != SRT_INVALID_SOCK) {
    srt_close (client->sock);
  }

  g_free (client);
}

static void
srt_emit_client_removed (SRTClient * client, gpointer user_data)
{
  GstSRTServerSink *self = GST_SRT_SERVER_SINK (user_data);
  g_return_if_fail (client != NULL && GST_IS_SRT_SERVER_SINK (self));

  g_signal_emit (self, signals[SIG_CLIENT_REMOVED], 0, client->sock,
    client->sockaddr);
}

static void
gst_srt_server_sink_get_property (GObject * object,
  guint prop_id, GValue * value, GParamSpec * pspec)
{
  GstSRTServerSink *self = GST_SRT_SERVER_SINK (object);
  GstSRTServerSinkPrivate *priv = GST_SRT_SERVER_SINK_GET_PRIVATE (self);

  switch (prop_id) {
  case PROP_POLL_TIMEOUT:
    g_value_set_int (value, priv->poll_timeout);
    break;
#if GST_VERSION_MINOR >= 14
  case PROP_STATS:
  {
    GList *item;

    GST_OBJECT_LOCK (self);
    for (item = priv->clients; item; item = item->next) {
      SRTClient *client = item->data;
      GValue tmp = G_VALUE_INIT;

      g_value_init (&tmp, GST_TYPE_STRUCTURE);
      g_value_take_boxed (&tmp, gst_srt_base_sink_get_stats (client->sockaddr,
        client->sock));
      gst_value_array_append_and_take_value (value, &tmp);
    }
    GST_OBJECT_UNLOCK (self);
    break;
  }
#endif
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    break;
  }
}

static void
gst_srt_server_sink_set_property (GObject * object,
  guint prop_id, const GValue * value, GParamSpec * pspec)
{
  GstSRTServerSink *self = GST_SRT_SERVER_SINK (object);
  GstSRTServerSinkPrivate *priv = GST_SRT_SERVER_SINK_GET_PRIVATE (self);

  switch (prop_id) {
  case PROP_POLL_TIMEOUT:
    priv->poll_timeout = g_value_get_int (value);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    break;
  }
}

static gpointer
thread_func (gpointer data)
{
  GstSRTServerSink *self = GST_SRT_SERVER_SINK (data);
  GstSRTServerSinkPrivate *priv = GST_SRT_SERVER_SINK_GET_PRIVATE (self);

  while(!priv->cancelled){
    SRTClient *client;
    int numReadySockets = 1;
    SRTSOCKET readySocket;
    struct sockaddr sa;
    int sa_len;

    // Wait until we can write
    if (srt_epoll_wait (priv->poll_id,
                0, 0, &readySocket, &numReadySockets,
                priv->poll_timeout,
                0, 0, 0, 0) == -1) {
      int srt_errno = srt_getlasterror (NULL);

      if (srt_errno != SRT_ETIMEOUT) {
        GST_ELEMENT_ERROR (self, RESOURCE, FAILED,
          ("SRT error: %s", srt_getlasterror_str ()), (NULL));
        continue;
      }

      /* Mimicking cancellable */
      if (srt_errno == SRT_ETIMEOUT && priv->cancelled) {
        GST_DEBUG_OBJECT (self, "Cancelled waiting for client");
        continue;
      }
    }

    client = srt_client_new ();
    client->sock = srt_accept (priv->sock, &sa, &sa_len);

    if (client->sock == SRT_INVALID_SOCK) {
      GST_WARNING_OBJECT (self, "detected invalid SRT client socket (reason: %s)",
        srt_getlasterror_str ());
      srt_clearlasterror ();
      srt_client_free (client);
      continue;
    }

    client->sockaddr = g_socket_address_new_from_native (&sa, sa_len);

    GST_INFO_OBJECT(self, "Added client");
    g_signal_emit (self, signals[SIG_CLIENT_ADDED], 0, client->sock,
      client->sockaddr);

    g_async_queue_push(priv->pending_clients, client);
    GST_DEBUG_OBJECT (self, "client added");
  }
  GST_INFO_OBJECT(self, "New client polling thread safe exit");
  return NULL;
}

static gboolean
gst_srt_server_sink_start (GstBaseSink * sink)
{
  GstSRTServerSink *self = GST_SRT_SERVER_SINK (sink);
  GstSRTServerSinkPrivate *priv = GST_SRT_SERVER_SINK_GET_PRIVATE (self);
  GstSRTBaseSink *base = GST_SRT_BASE_SINK (sink);
  GstUri *uri = gst_uri_ref (base->uri);
  GSocketAddress *socket_address = NULL;
  GError *error = NULL;
  gboolean ret = TRUE;
  struct sockaddr sa;
  size_t sa_len;
  const gchar *host;
  int lat = base->latency;

  if (gst_uri_get_port (uri) == GST_URI_NO_PORT) {
    GST_ELEMENT_ERROR (sink, RESOURCE, OPEN_WRITE, NULL, (("Invalid port")));
    return FALSE;
  }

  host = gst_uri_get_host (uri);
  if (host == NULL) {
    GInetAddress *any = g_inet_address_new_any (G_SOCKET_FAMILY_IPV4);
    socket_address = g_inet_socket_address_new (any, gst_uri_get_port (uri));
    g_object_unref (any);
  }
  else {
    socket_address =
      g_inet_socket_address_new_from_string (host, gst_uri_get_port (uri));
  }

  if (socket_address == NULL) {
    GST_WARNING_OBJECT (self,
      "failed to extract host or port from the given URI");
    goto failed;
  }

  sa_len = g_socket_address_get_native_size (socket_address);
  if (!g_socket_address_to_native (socket_address, &sa, sa_len, &error)) {
    GST_WARNING_OBJECT (self, "cannot resolve address (reason: %s)",
      error->message);
    goto failed;
  }

  priv->sock = srt_socket (sa.sa_family, SOCK_DGRAM, 0);
  if (priv->sock == SRT_INVALID_SOCK) {
    GST_WARNING_OBJECT (self, "failed to create SRT socket (reason: %s)",
      srt_getlasterror_str ());
    goto failed;
  }

  int on = 1;
  int off = 0;
  int64_t zero = 0;
  /* Make SRT non blocking */
  srt_setsockopt (priv->sock, 0, SRTO_SNDSYN, &off, sizeof (int));

  /* Use the larger recommended send buffer */
  int send_buff_bytes = SRT_SEND_BUFFER_SIZE;
  srt_setsockopt(priv->sock, 0, SRTO_UDP_SNDBUF, &send_buff_bytes, sizeof (int));

  /* Make sure TSBPD mode is enable (SRT mode) */
  srt_setsockopt (priv->sock, 0, SRTO_TSBPDMODE, &on, sizeof (int));

  /* srt recommends disabling linger */
  srt_setsockopt (priv->sock, 0, SRTO_LINGER, &off, sizeof (int));

  /* srt recommends having a max BW of 0, so relative */
  srt_setsockflag (priv->sock, SRTO_MAXBW, &zero, sizeof (int64_t));

  /* This is a sink, we're always a sender */
  srt_setsockopt (priv->sock, 0, SRTO_SENDER, &on, sizeof (int));

  /* Set the minimum latency we'll allow the receiver to use*/
  srt_setsockopt (priv->sock, 0, SRTO_PEERLATENCY, &lat, sizeof (int));
  /*srt_setsockopt (priv->sock, 0, SRTO_TSBPDDELAY, &lat, sizeof (int));*/

  priv->poll_id = srt_epoll_create ();
  if (priv->poll_id == -1) {
    GST_WARNING_OBJECT (self,
      "failed to create poll id for SRT socket (reason: %s)",
      srt_getlasterror_str ());
    goto failed;
  }
  int events = SRT_EPOLL_IN | SRT_EPOLL_OUT | SRT_EPOLL_ERR;
  srt_epoll_add_usock (priv->poll_id, priv->sock, &events);

  if (srt_bind (priv->sock, &sa, (int)sa_len) == SRT_ERROR) {
    GST_WARNING_OBJECT (self, "failed to bind SRT server socket (reason: %s)",
      srt_getlasterror_str ());
    goto failed;
  }

  if (srt_listen (priv->sock, 5) == SRT_ERROR) {
    GST_WARNING_OBJECT (self, "failed to listen SRT socket (reason: %s)",
      srt_getlasterror_str ());
    goto failed;
  }

  priv->thread = g_thread_try_new ("srtserversink", thread_func, self, &error);
  if (error != NULL) {
    GST_WARNING_OBJECT (self, "failed to create thread (reason: %s)",
      error->message);
    goto failed;
  }

  g_clear_pointer (&uri, gst_uri_unref);
  g_clear_object (&socket_address);

  return ret;

failed:
  priv->cancelled = TRUE;
  if (priv->poll_id != SRT_ERROR) {
    srt_epoll_release (priv->poll_id);
    priv->poll_id = SRT_ERROR;
  }

  if (priv->sock != SRT_INVALID_SOCK) {
    srt_close (priv->sock);
    priv->sock = SRT_INVALID_SOCK;
  }

  if (priv->thread) {
    g_thread_join (priv->thread);
    g_clear_pointer (&priv->thread, g_thread_unref);
  }

  g_clear_error (&error);
  g_clear_pointer (&uri, gst_uri_unref);
  g_clear_object (&socket_address);

  return FALSE;
}

static gboolean inline
send_buffer_internal (GstSRTBaseSink * sink,
  const GstMapInfo * mapinfo, gpointer user_data)
{
  SRTClient *client = user_data;

  if (srt_sendmsg2 (client->sock, (char *)mapinfo->data, (int)mapinfo->size,
    0) == SRT_ERROR) {
    GST_WARNING_OBJECT (sink, "Removing client Code:%d Reason: %s",
      srt_getlasterror (NULL), srt_getlasterror_str ());
    return FALSE;
  }

  return TRUE;
}

static gboolean inline
can_client_recv (SRTSOCKET socket) {
    int num_bytes_unacknowledged;
    int num_bytes_len = sizeof (num_bytes_unacknowledged);
    const int send_buffer_size = SRT_SEND_BUFFER_SIZE;
    const int default_msg_size = 1316; // 1316 is for mpegts
    // Get how many bytes are not yet acknowledged
    srt_getsockflag (socket, SRTO_SNDDATA, &num_bytes_unacknowledged, &num_bytes_len);

    return num_bytes_unacknowledged + default_msg_size < send_buffer_size;
}

static gboolean inline
gst_srt_server_sink_send_buffer (GstSRTBaseSink * sink,
  const GstMapInfo * mapinfo)
{
  GstSRTServerSink *self = GST_SRT_SERVER_SINK (sink);
  GstSRTServerSinkPrivate *priv = GST_SRT_SERVER_SINK_GET_PRIVATE (self);
  GST_OBJECT_LOCK (sink);
  GList *clients = priv->clients;

  while (clients != NULL) {
     SRTClient *client = clients->data;
     clients = clients->next;

     if (!can_client_recv (client->sock)) {
       client->num_send_fails++;
       if (client->num_send_fails >= MAX_SEND_FAILS) {
          GST_WARNING_OBJECT (sink, "Removing client as a result of too many send fails");
          priv->clients = g_list_remove (priv->clients, client);
          g_signal_emit (self, signals[SIG_CLIENT_REMOVED], 0, client->sock,
          client->sockaddr);
          srt_client_free (client);
          continue;
       }
     }

    if (!send_buffer_internal (sink, mapinfo, client)){
      priv->clients = g_list_remove (priv->clients, client);
      g_signal_emit (self, signals[SIG_CLIENT_REMOVED], 0, client->sock,
        client->sockaddr);
      srt_client_free (client);
    }
  }

  // Process new clients
  SRTClient *client = (SRTClient*)g_async_queue_try_pop(priv->pending_clients);

  while(client != NULL){
    if (!gst_srt_base_sink_send_headers (sink, send_buffer_internal, client))
      goto err;
    GST_INFO_OBJECT(self, "Sent client headers");

    if (!send_buffer_internal (sink, mapinfo, client))
      goto err;
    /* GList recommends prepending to lists for performance */
    priv->clients = g_list_prepend(priv->clients, client);
    client = (SRTClient*)g_async_queue_try_pop(priv->pending_clients);
    continue;

    err:
      g_signal_emit (self, signals[SIG_CLIENT_REMOVED], 0, client->sock,
        client->sockaddr);
      srt_client_free (client);
      client = (SRTClient*)g_async_queue_try_pop(priv->pending_clients);
  }
  GST_OBJECT_UNLOCK (sink);

  return TRUE;
}

static gboolean
gst_srt_server_sink_stop (GstBaseSink * sink)
{
  GstSRTServerSink *self = GST_SRT_SERVER_SINK (sink);
  GstSRTServerSinkPrivate *priv = GST_SRT_SERVER_SINK_GET_PRIVATE (self);

  priv->cancelled = TRUE;

  GST_DEBUG_OBJECT (self, "closing SRT connection");
  srt_epoll_remove_usock (priv->poll_id, priv->sock);
  srt_epoll_release (priv->poll_id);
  srt_close (priv->sock);

  if (priv->thread) {
    g_thread_join (priv->thread);
    g_clear_pointer (&priv->thread, g_thread_unref);
  }

  GST_OBJECT_LOCK (sink);
  GST_DEBUG_OBJECT (self, "closing client sockets");
  g_list_foreach (priv->clients, (GFunc)srt_emit_client_removed, self);
  g_list_free_full (priv->clients, (GDestroyNotify)srt_client_free);
  /* async queue doesn't have a foreach, so we have to manually iterate
   * through it to remove all pending clients */
  SRTClient *client = g_async_queue_try_pop(priv->pending_clients);
  while (client != NULL){
    srt_emit_client_removed(client, self);
    srt_client_free(client);
    client = g_async_queue_try_pop(priv->pending_clients);
  }
  g_async_queue_unref(priv->pending_clients);
  GST_OBJECT_UNLOCK (sink);

  return GST_BASE_SINK_CLASS (parent_class)->stop (sink);
}

static gboolean
gst_srt_server_sink_unlock (GstBaseSink * sink)
{
  GstSRTServerSink *self = GST_SRT_SERVER_SINK (sink);
  GstSRTServerSinkPrivate *priv = GST_SRT_SERVER_SINK_GET_PRIVATE (self);

  priv->cancelled = TRUE;

  return TRUE;
}

static gboolean
gst_srt_server_sink_unlock_stop (GstBaseSink * sink)
{
  GstSRTServerSink *self = GST_SRT_SERVER_SINK (sink);
  GstSRTServerSinkPrivate *priv = GST_SRT_SERVER_SINK_GET_PRIVATE (self);

  priv->cancelled = FALSE;

  return TRUE;
}

static void
gst_srt_server_sink_class_init (GstSRTServerSinkClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *gstelement_class = GST_ELEMENT_CLASS (klass);
  GstBaseSinkClass *gstbasesink_class = GST_BASE_SINK_CLASS (klass);
  GstSRTBaseSinkClass *gstsrtbasesink_class = GST_SRT_BASE_SINK_CLASS (klass);

  gobject_class->set_property = gst_srt_server_sink_set_property;
  gobject_class->get_property = gst_srt_server_sink_get_property;

  properties[PROP_POLL_TIMEOUT] =
    g_param_spec_int ("poll-timeout", "Poll Timeout",
      "Return poll wait after timeout miliseconds (-1 = infinite)", -1,
      G_MAXINT32, SRT_DEFAULT_POLL_TIMEOUT,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

#if GST_VERSION_MINOR >= 14
  properties[PROP_STATS] = gst_param_spec_array ("stats", "Statistics",
    "Array of GstStructures containing SRT statistics",
    g_param_spec_boxed ("stats", "Statistics",
      "Statistics for one client", GST_TYPE_STRUCTURE,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS),
    G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
#endif

  g_object_class_install_properties (gobject_class, PROP_LAST, properties);

  /**
    * GstSRTServerSink::client-added:
    * @gstsrtserversink: the srtserversink element that emitted this signal
    * @sock: the client socket descriptor that was added to srtserversink
    * @addr: the pointer of "struct sockaddr" that describes the @sock
    * @addr_len: the length of @addr
    *
    * The given socket descriptor was added to srtserversink.
    */
  signals[SIG_CLIENT_ADDED] =
    g_signal_new ("client-added", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET (GstSRTServerSinkClass, client_added),
      NULL, NULL, g_cclosure_marshal_generic, G_TYPE_NONE,
      2, G_TYPE_INT, G_TYPE_SOCKET_ADDRESS);

  /**
    * GstSRTServerSink::client-removed:
    * @gstsrtserversink: the srtserversink element that emitted this signal
    * @sock: the client socket descriptor that was added to srtserversink
    * @addr: the pointer of "struct sockaddr" that describes the @sock
    * @addr_len: the length of @addr
    *
    * The given socket descriptor was removed from srtserversink.
    */
  signals[SIG_CLIENT_REMOVED] =
    g_signal_new ("client-removed", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET (GstSRTServerSinkClass,
        client_removed), NULL, NULL, g_cclosure_marshal_generic, G_TYPE_NONE,
      2, G_TYPE_INT, G_TYPE_SOCKET_ADDRESS);

  gst_element_class_add_static_pad_template (gstelement_class, &sink_template);
  gst_element_class_set_metadata (gstelement_class,
    "SRT server sink", "Sink/Network",
    "Send data over the network via SRT",
    "Justin Kim <justin.kim@collabora.com>");

  gstbasesink_class->start = GST_DEBUG_FUNCPTR (gst_srt_server_sink_start);
  gstbasesink_class->stop = GST_DEBUG_FUNCPTR (gst_srt_server_sink_stop);
  gstbasesink_class->unlock = GST_DEBUG_FUNCPTR (gst_srt_server_sink_unlock);
  gstbasesink_class->unlock_stop =
    GST_DEBUG_FUNCPTR (gst_srt_server_sink_unlock_stop);

  gstsrtbasesink_class->send_buffer =
    GST_DEBUG_FUNCPTR (gst_srt_server_sink_send_buffer);
}

static void
gst_srt_server_sink_init (GstSRTServerSink * self)
{
  GstSRTServerSinkPrivate *priv = GST_SRT_SERVER_SINK_GET_PRIVATE (self);
  priv->poll_timeout = SRT_DEFAULT_POLL_TIMEOUT;
  priv->pending_clients = g_async_queue_new();
}
