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

#include "gstsrtclientsink.h"
#include <srt.h>
#include <gio/gio.h>

#define SRT_URI_SCHEME "srt"
#define SRT_DEFAULT_PORT 7001
#define SRT_DEFAULT_HOST "127.0.0.1"
#define SRT_DEFAULT_URI SRT_URI_SCHEME"://"SRT_DEFAULT_HOST":"G_STRINGIFY(SRT_DEFAULT_PORT)
#define SRT_DEFAULT_POLL_TIMEOUT - 1
#define SRT_DEFAULT_LATENCY 125

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE("sink",
	GST_PAD_SINK,
	GST_PAD_ALWAYS,
	GST_STATIC_CAPS_ANY);

#define GST_CAT_DEFAULT gst_debug_srt_client_sink
GST_DEBUG_CATEGORY(GST_CAT_DEFAULT);

struct _GstSRTClientSinkPrivate
{
	SRTSOCKET sock;
	GSocketAddress *sockaddr;
	gint poll_id;
	gint poll_timeout;
	gint latency;
};

#define GST_SRT_CLIENT_SINK_GET_PRIVATE(obj)  \
       (G_TYPE_INSTANCE_GET_PRIVATE ((obj), GST_TYPE_SRT_CLIENT_SINK, GstSRTClientSinkPrivate))

enum
{
	PROP_POLL_TIMEOUT = 1,
	PROP_STATS,
	PROP_LATENCY,
	/*< private > */
	PROP_LAST
};

static GParamSpec *properties[PROP_LAST];

#define gst_srt_client_sink_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE(GstSRTClientSink, gst_srt_client_sink,
	GST_TYPE_SRT_BASE_SINK, G_ADD_PRIVATE(GstSRTClientSink)
	GST_DEBUG_CATEGORY_INIT(GST_CAT_DEFAULT, "srtclientsink", 0,
		"SRT Client Sink"));

static void
gst_srt_client_sink_get_property(GObject * object,
	guint prop_id, GValue * value, GParamSpec * pspec)
{
	GstSRTClientSink *self = GST_SRT_CLIENT_SINK(object);
	GstSRTClientSinkPrivate *priv = GST_SRT_CLIENT_SINK_GET_PRIVATE(self);

	switch (prop_id) {
	case PROP_POLL_TIMEOUT:
		g_value_set_int(value, priv->poll_timeout);
		break;
	case PROP_STATS:
		g_value_take_boxed(value, gst_srt_base_sink_get_stats(priv->sockaddr,
			priv->sock));
		break;
	case PROP_LATENCY:
		g_value_set_int(value, priv->latency);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;

	}
}

static void
gst_srt_client_sink_set_property(GObject * object,
	guint prop_id, const GValue * value, GParamSpec * pspec)
{
	GstSRTClientSink *self = GST_SRT_CLIENT_SINK(object);
	GstSRTClientSinkPrivate *priv = GST_SRT_CLIENT_SINK_GET_PRIVATE(self);

	switch (prop_id) {
	case PROP_POLL_TIMEOUT:
		priv->poll_timeout = g_value_get_int(value);
		break;
	case PROP_LATENCY:
		priv->latency = g_value_get_int(value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;
	}

}

static gboolean
gst_srt_client_sink_start(GstBaseSink * sink)
{
	GST_DEBUG("Will start SRT client sink");
	GstSRTClientSink *self = GST_SRT_CLIENT_SINK(sink);
	GstSRTClientSinkPrivate *priv = GST_SRT_CLIENT_SINK_GET_PRIVATE(self);
	GstUri *uri = gst_uri_ref(GST_SRT_BASE_SINK(self)->uri);
	GError *error = NULL;
	struct sockaddr sa;
	size_t sa_len;

	priv->sockaddr =
		g_inet_socket_address_new_from_string(gst_uri_get_host(uri),
			gst_uri_get_port(uri));

	if (priv->sockaddr == NULL) {
		GST_ELEMENT_ERROR(self, RESOURCE, OPEN_WRITE, NULL,
			("failed to extract host or port from the given URI"));
		goto failed;

	}

	sa_len = g_socket_address_get_native_size(priv->sockaddr);
	if (!g_socket_address_to_native(priv->sockaddr, &sa, sa_len, &error)) {
		GST_ELEMENT_ERROR(self, RESOURCE, OPEN_WRITE, NULL,
			("cannot resolve address (reason: %s)", error->message));
		goto failed;

	}

	priv->sock = srt_socket(sa.sa_family, SOCK_DGRAM, 0);
	if (priv->sock == SRT_INVALID_SOCK) {
		GST_ELEMENT_ERROR(self, RESOURCE, OPEN_WRITE, NULL,
			("failed to create SRT socket (reason: %s)", srt_getlasterror_str()));
		goto failed;

	}

	/* Make SRT non-blocking */
	srt_setsockopt(priv->sock, 0, SRTO_SNDSYN, &(int) {
		0}, sizeof(int));

	/* Make sure TSBPD mode is enable (SRT mode) */
	srt_setsockopt(priv->sock, 0, SRTO_TSBPDMODE, &(int) {
		1}, sizeof(int));
	
	/* This is a sink, we're always a sender */
	srt_setsockopt(priv->sock, 0, SRTO_SENDER, &(int) {
		1}, sizeof(int));
	//TODO what is this.

	srt_setsockopt(priv->sock, 0, SRTO_TSBPDDELAY, &priv->latency, sizeof(int));

	priv->poll_id = srt_epoll_create();
	if (priv->poll_id == -1) {
		GST_ELEMENT_ERROR(self, RESOURCE, OPEN_WRITE, NULL,
			("failed to create poll id for SRT socket (reason: %s)",
				srt_getlasterror_str()));
		goto failed;

	}
	srt_epoll_add_usock(priv->poll_id, priv->sock, &(int) {
		SRT_EPOLL_OUT});

	if (srt_connect(priv->sock, &sa, sa_len) == SRT_ERROR) {
		GST_ELEMENT_ERROR(self, RESOURCE, OPEN_WRITE, NULL,
			("failed to connect to host (reason: %s)", srt_getlasterror_str()));
		goto failed;

	}

	g_clear_pointer(&uri, gst_uri_unref);

	GST_DEBUG("SRT client sink started");
	return TRUE;

failed:
	if (priv->poll_id != SRT_ERROR) {
		srt_epoll_release(priv->poll_id);
		priv->poll_id = SRT_ERROR;

	}

	if (priv->sock != SRT_INVALID_SOCK) {
		srt_close(priv->sock);
		priv->sock = SRT_INVALID_SOCK;
	}

	g_clear_error(&error);
	g_clear_pointer(&uri, gst_uri_unref);
	g_clear_object(&priv->sockaddr);

	return FALSE;
}

static gboolean
gst_srt_client_sink_send_buffer(GstSRTBaseSink * sink,
	const GstMapInfo * mapinfo)
{
	GstSRTClientSink *self = GST_SRT_CLIENT_SINK(sink);
	GstSRTClientSinkPrivate *priv = GST_SRT_CLIENT_SINK_GET_PRIVATE(self);

	if (srt_sendmsg2(priv->sock, (char *)mapinfo->data, mapinfo->size,
		0) == SRT_ERROR) {
		GST_ELEMENT_ERROR(self, RESOURCE, WRITE, NULL,
			("%s", srt_getlasterror_str()));
		return FALSE;

	}

	return TRUE;
}

static gboolean
gst_srt_client_sink_stop(GstBaseSink * sink)
{
	GstSRTClientSink *self = GST_SRT_CLIENT_SINK(sink);
	GstSRTClientSinkPrivate *priv = GST_SRT_CLIENT_SINK_GET_PRIVATE(self);

	GST_DEBUG_OBJECT(self, "closing SRT connection");

	if (priv->poll_id != SRT_ERROR) {
		srt_epoll_remove_usock(priv->poll_id, priv->sock);
		srt_epoll_release(priv->poll_id);
		priv->poll_id = SRT_ERROR;

	}

	if (priv->sock != SRT_INVALID_SOCK) {
		srt_close(priv->sock);
		priv->sock = SRT_INVALID_SOCK;

	}
	g_clear_object(&priv->sockaddr);
	return TRUE;
}

static void
gst_srt_client_sink_class_init(GstSRTClientSinkClass * klass)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
	GstElementClass *gstelement_class = GST_ELEMENT_CLASS(klass);
	GstBaseSinkClass *gstbasesink_class = GST_BASE_SINK_CLASS(klass);
	GstSRTBaseSinkClass *gstsrtbasesink_class = GST_SRT_BASE_SINK_CLASS(klass);

	gobject_class->set_property = gst_srt_client_sink_set_property;
	gobject_class->get_property = gst_srt_client_sink_get_property;

	properties[PROP_POLL_TIMEOUT] =
		g_param_spec_int("poll-timeout", "Poll Timeout",
			"Return poll wait after timeout miliseconds (-1 = infinite)", -1,
			G_MAXINT32, SRT_DEFAULT_POLL_TIMEOUT,
			G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[PROP_STATS] = g_param_spec_boxed("stats", "Statistics",
		"SRT Statistics", GST_TYPE_STRUCTURE,
		G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

	properties[PROP_LATENCY] =
		g_param_spec_int("latency", "latency",
		"Minimum latency(milliseconds)", 0,
		G_MAXINT32, SRT_DEFAULT_LATENCY,
		G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	g_object_class_install_properties(gobject_class, PROP_LAST,	properties);

	gst_element_class_add_static_pad_template(gstelement_class, &sink_template);
	gst_element_class_set_metadata(gstelement_class,
		"SRT client sink", "Sink/Network",
		"Send data over the network via SRT",
		"Justin Kim <justin.kim@collabora.com>");

	gstbasesink_class->start = GST_DEBUG_FUNCPTR(gst_srt_client_sink_start);
	gstbasesink_class->stop = GST_DEBUG_FUNCPTR(gst_srt_client_sink_stop);

	gstsrtbasesink_class->send_buffer =
		GST_DEBUG_FUNCPTR(gst_srt_client_sink_send_buffer);
	GST_DEBUG("SRT client init");
}

static void
gst_srt_client_sink_init(GstSRTClientSink * self)
{
	GstSRTClientSinkPrivate *priv = GST_SRT_CLIENT_SINK_GET_PRIVATE(self);
	priv->poll_timeout = SRT_DEFAULT_POLL_TIMEOUT;
	priv->latency = SRT_DEFAULT_LATENCY;
}