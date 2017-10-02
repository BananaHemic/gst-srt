/* GStreamer SRT plugin based on libsrt
 * Copyright (C) 2017, Collabora Ltd.
 *   Authors:
 *       Justin Kim <justin.kim@collabora.com>
 *       Olivier Cr�te <olivier.crete@collabora.com>
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

#include "gstsrtserversink.h"
#include <srt.h>

 //#include <netinet/in.h>

#define SRT_URI_SCHEME "srt"
#define SRT_DEFAULT_PORT 7001
#define SRT_DEFAULT_HOST "127.0.0.1"
#define SRT_DEFAULT_URI SRT_URI_SCHEME"://"SRT_DEFAULT_HOST":"G_STRINGIFY(SRT_DEFAULT_PORT)
#define SRT_DEFAULT_POLL_TIMEOUT - 1

#define GST_CAT_DEFAULT gst_debug_srt_base_sink
GST_DEBUG_CATEGORY(GST_CAT_DEFAULT);

enum
{
	PROP_URI = 1,
	/*< private > */
	PROP_LAST
};

static GParamSpec *properties[PROP_LAST];

static void gst_srt_base_sink_uri_handler_init(gpointer g_iface,
	gpointer iface_data);
static gchar *gst_srt_base_sink_uri_get_uri(GstURIHandler * handler);
static gboolean gst_srt_base_sink_uri_set_uri(GstURIHandler * handler,
	const gchar * uri, GError ** error);

#define gst_srt_base_sink_parent_class parent_class
G_DEFINE_ABSTRACT_TYPE_WITH_CODE(GstSRTBaseSink, gst_srt_base_sink,
	GST_TYPE_BASE_SINK,
	G_IMPLEMENT_INTERFACE(GST_TYPE_URI_HANDLER,
		gst_srt_base_sink_uri_handler_init)
	GST_DEBUG_CATEGORY_INIT(GST_CAT_DEFAULT, "srtbasesink", 0,
		"SRT Base Sink"));

static void
gst_srt_base_sink_get_property(GObject * object,
	guint prop_id, GValue * value, GParamSpec * pspec)
{
	GstSRTBaseSink *self = GST_SRT_BASE_SINK(object);

	switch (prop_id) {
	case PROP_URI:
		if (self->uri != NULL) {
			gchar *uri_str = gst_srt_base_sink_uri_get_uri(GST_URI_HANDLER(self));
			g_value_take_string(value, uri_str);

		}
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;

	}
}

static void
gst_srt_base_sink_set_property(GObject * object,
	guint prop_id, const GValue * value, GParamSpec * pspec)
{
	GstSRTBaseSink *self = GST_SRT_BASE_SINK(object);

	switch (prop_id) {
	case PROP_URI:
		gst_srt_base_sink_uri_set_uri(GST_URI_HANDLER(self),
			g_value_get_string(value), NULL);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;

	}
}

static void
gst_srt_base_sink_finalize(GObject * object)
{
	GstSRTBaseSink *self = GST_SRT_BASE_SINK(object);

	g_clear_pointer(&self->uri, gst_uri_unref);

	G_OBJECT_CLASS(parent_class)->finalize(object);
}

static GstFlowReturn
gst_srt_base_sink_render(GstBaseSink * sink, GstBuffer * buffer)
{
	GstSRTBaseSink *self = GST_SRT_BASE_SINK(sink);
	GstMapInfo info;
	GstSRTBaseSinkClass *bclass = GST_SRT_BASE_SINK_GET_CLASS(sink);
	GstFlowReturn ret = GST_FLOW_OK;

	GST_TRACE_OBJECT(self, "sending buffer %p, offset %"
		G_GINT64_FORMAT ", offset_end %" G_GINT64_FORMAT
		", timestamp %" GST_TIME_FORMAT ", duration %" GST_TIME_FORMAT
		", size %" G_GSIZE_FORMAT,
		buffer, GST_BUFFER_OFFSET(buffer),
		GST_BUFFER_OFFSET_END(buffer),
		GST_TIME_ARGS(GST_BUFFER_TIMESTAMP(buffer)),
		GST_TIME_ARGS(GST_BUFFER_DURATION(buffer)),
		gst_buffer_get_size(buffer));

	if (!gst_buffer_map(buffer, &info, GST_MAP_READ)) {
		GST_ELEMENT_ERROR(self, RESOURCE, READ,
			("Could not map the input stream"), (NULL));
		return GST_FLOW_ERROR;

	}

	if (!bclass->send_buffer(self, &info))
		ret = GST_FLOW_ERROR;

	gst_buffer_unmap(buffer, &info);

	return ret;
}

static void
gst_srt_base_sink_class_init(GstSRTBaseSinkClass * klass)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
	GstBaseSinkClass *gstbasesink_class = GST_BASE_SINK_CLASS(klass);

	gobject_class->set_property = gst_srt_base_sink_set_property;
	gobject_class->get_property = gst_srt_base_sink_get_property;
	gobject_class->finalize = gst_srt_base_sink_finalize;

	/**
		* GstSRTBaseSink:uri:
		*
		* The URI used by SRT Connection.
		*/
	properties[PROP_URI] = g_param_spec_string("uri", "URI",
		"URI in the form of srt://address:port", SRT_DEFAULT_URI,
		G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	g_object_class_install_properties(gobject_class, PROP_LAST, properties);

	gstbasesink_class->render = GST_DEBUG_FUNCPTR(gst_srt_base_sink_render);
}

static void
gst_srt_base_sink_init(GstSRTBaseSink * self)
{
	self->uri = gst_uri_from_string(SRT_DEFAULT_URI);
	self->queued_buffers = NULL;
}

static GstURIType
gst_srt_base_sink_uri_get_type(GType type)
{
	return GST_URI_SINK;
}

static const gchar *const *
gst_srt_base_sink_uri_get_protocols(GType type)
{
	static const gchar *protocols[] = { SRT_URI_SCHEME, NULL };

	return protocols;
}

static gchar *
gst_srt_base_sink_uri_get_uri(GstURIHandler * handler)
{
	gchar *uri_str;
	GstSRTBaseSink *self = GST_SRT_BASE_SINK(handler);

	GST_OBJECT_LOCK(self);
	uri_str = gst_uri_to_string(self->uri);
	GST_OBJECT_UNLOCK(self);

	return uri_str;
}

static gboolean
gst_srt_base_sink_uri_set_uri(GstURIHandler * handler,
	const gchar * uri, GError ** error)
{
	GstSRTBaseSink *self = GST_SRT_BASE_SINK(handler);
	gboolean ret = TRUE;
	GstUri *parsed_uri = gst_uri_from_string(uri);

	GST_TRACE_OBJECT(self, "Requested URI=%s", uri);

	if (g_strcmp0(gst_uri_get_scheme(parsed_uri), SRT_URI_SCHEME) != 0) {
		g_set_error(error, GST_URI_ERROR, GST_URI_ERROR_BAD_URI,
			"Invalid SRT URI scheme");
		ret = FALSE;
		goto out;

	}

	GST_OBJECT_LOCK(self);

	g_clear_pointer(&self->uri, gst_uri_unref);
	self->uri = gst_uri_ref(parsed_uri);

	GST_OBJECT_UNLOCK(self);

out:
	g_clear_pointer(&parsed_uri, gst_uri_unref);
	return ret;
}

static void
gst_srt_base_sink_uri_handler_init(gpointer g_iface, gpointer iface_data)
{
	GstURIHandlerInterface *iface = (GstURIHandlerInterface *)g_iface;

	iface->get_type = gst_srt_base_sink_uri_get_type;
	iface->get_protocols = gst_srt_base_sink_uri_get_protocols;
	iface->get_uri = gst_srt_base_sink_uri_get_uri;
	iface->set_uri = gst_srt_base_sink_uri_set_uri;
}

GstStructure *
gst_srt_base_sink_get_stats(GSocketAddress * sockaddr, SRTSOCKET sock)
{
	SRT_TRACEBSTATS stats;
	int ret;
	GValue v = G_VALUE_INIT;
	GstStructure *s;

	if (sock == SRT_INVALID_SOCK || sockaddr == NULL)
		return gst_structure_new_empty("application/x-srt-statistics");

	s = gst_structure_new("application/x-srt-statistics",
		"sockaddr", G_TYPE_SOCKET_ADDRESS, sockaddr, NULL);

	ret = srt_bstats(sock, &stats, 0);
	if (ret >= 0) {
		gst_structure_set(s,
			/* number of sent data packets, including retransmissions */
			"packets-sent", G_TYPE_INT64, stats.pktSent,
			/* number of lost packets (sender side) */
			"packets-sent-lost", G_TYPE_INT, stats.pktSndLoss,
			/* number of retransmitted packets */
			"packets-retransmitted", G_TYPE_INT, stats.pktRetrans,
			/* number of received ACK packets */
			"packet-ack-received", G_TYPE_INT, stats.pktRecvACK,
			/* number of received NAK packets */
			"packet-nack-received", G_TYPE_INT, stats.pktRecvNAK,
			/* time duration when UDT is sending data (idle time exclusive) */
			"send-duration-us", G_TYPE_INT64, stats.usSndDuration,
			/* number of sent data bytes, including retransmissions */
			"bytes-sent", G_TYPE_UINT64, stats.byteSent,
			/* number of retransmitted bytes */
			"bytes-retransmitted", G_TYPE_UINT64, stats.byteRetrans,
			/* number of too-late-to-send dropped bytes */
			"bytes-sent-dropped", G_TYPE_UINT64, stats.byteSndDrop,
			/* number of too-late-to-send dropped packets */
			"packets-sent-dropped", G_TYPE_INT, stats.pktSndDrop,
			/* sending rate in Mb/s */
			"send-rate-mbps", G_TYPE_DOUBLE, stats.msRTT,
			/* estimated bandwidth, in Mb/s */
			"bandwidth-mbps", G_TYPE_DOUBLE, stats.mbpsBandwidth,
			/* busy sending time (i.e., idle time exclusive) */
			"send-duration-us", G_TYPE_UINT64, stats.usSndDuration,
			"rtt-ms", G_TYPE_DOUBLE, stats.msRTT, NULL);

	}

	g_value_init(&v, G_TYPE_STRING);
	g_value_take_string(&v,
		g_socket_connectable_to_string(G_SOCKET_CONNECTABLE(sockaddr)));
	gst_structure_take_value(s, "sockaddr-str", &v);

	return s;
}