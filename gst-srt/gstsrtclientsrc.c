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
   * </refsect2>
   *
   */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstsrtclientsrc.h"
#include <srt.h>
#include <gio/gio.h>

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE("src",
	GST_PAD_SRC,
	GST_PAD_ALWAYS,
	GST_STATIC_CAPS_ANY);

#define GST_CAT_DEFAULT gst_debug_srt_client_src
GST_DEBUG_CATEGORY(GST_CAT_DEFAULT);

struct _GstSRTClientSrcPrivate
{
	SRTSOCKET sock;
	gint poll_id;
	gint poll_timeout;
	gint latency;
};

#define GST_SRT_CLIENT_SRC_GET_PRIVATE(obj)  \
       (G_TYPE_INSTANCE_GET_PRIVATE ((obj), GST_TYPE_SRT_CLIENT_SRC, GstSRTClientSrcPrivate))

#define SRT_DEFAULT_POLL_TIMEOUT - 1
#define SRT_DEFAULT_LATENCY 125
enum
{
	PROP_POLL_TIMEOUT = 1,
	PROP_LATENCY,

	/*< private > */
	PROP_LAST
};

static GParamSpec *properties[PROP_LAST];

#define gst_srt_client_src_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE(GstSRTClientSrc, gst_srt_client_src,
	GST_TYPE_SRT_BASE_SRC, G_ADD_PRIVATE(GstSRTClientSrc)
	GST_DEBUG_CATEGORY_INIT(GST_CAT_DEFAULT, "srtclientsrc", 0,
		"SRT Client Source"));

static void
gst_srt_client_src_get_property(GObject * object,
	guint prop_id, GValue * value, GParamSpec * pspec)
{
	GstSRTClientSrc *self = GST_SRT_CLIENT_SRC(object);
	GstSRTClientSrcPrivate *priv = GST_SRT_CLIENT_SRC_GET_PRIVATE(self);

	switch (prop_id) {
	case PROP_POLL_TIMEOUT:
		g_value_set_int(value, priv->poll_timeout);
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
gst_srt_client_src_set_property(GObject * object,
	guint prop_id, const GValue * value, GParamSpec * pspec)
{
	GstSRTBaseSrc *self = GST_SRT_BASE_SRC(object);
	GstSRTClientSrcPrivate *priv = GST_SRT_CLIENT_SRC_GET_PRIVATE(self);

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

static void
gst_srt_client_src_finalize(GObject * object)
{
	GstSRTClientSrc *self = GST_SRT_CLIENT_SRC(object);
	GstSRTClientSrcPrivate *priv = GST_SRT_CLIENT_SRC_GET_PRIVATE(self);

	if (priv->poll_id != SRT_ERROR) {
		srt_epoll_release(priv->poll_id);
		priv->poll_id = SRT_ERROR;

	}

	if (priv->sock != SRT_ERROR) {
		srt_close(priv->sock);
		priv->sock = SRT_ERROR;

	}

	G_OBJECT_CLASS(parent_class)->finalize(object);
}

static GstFlowReturn
gst_srt_client_src_fill(GstPushSrc * src, GstBuffer * outbuf)
{
	GstSRTClientSrc *self = GST_SRT_CLIENT_SRC(src);
	GstSRTClientSrcPrivate *priv = GST_SRT_CLIENT_SRC_GET_PRIVATE(self);
	GstFlowReturn ret = GST_FLOW_OK;
	GstMapInfo info;
	SRTSOCKET ready[2];
	gint recv_len;

	//GST_DEBUG("Will fill");
	if (srt_epoll_wait(priv->poll_id, 0, 0, ready, &(int) {
		2}, priv->poll_timeout, 0, 0, 0, 0) == -1) {

		/* Assuming that timeout error is normal */
		if (srt_getlasterror(NULL) != SRT_ETIMEOUT) {
			GST_DEBUG_OBJECT(self, "%s", srt_getlasterror_str());
			ret = GST_FLOW_ERROR;

		}
		else {
			GST_DEBUG("SRT timeout");
		}
		srt_clearlasterror();
		goto out;
	}
	//GST_DEBUG("Filled");


	if (!gst_buffer_map(outbuf, &info, GST_MAP_WRITE)) {
		GST_ELEMENT_ERROR(src, RESOURCE, WRITE,
			("Could not map the output stream"), (NULL));
		ret = GST_FLOW_ERROR;
		goto out;

	}

	//GST_DEBUG("Will recv");
	recv_len = srt_recvmsg(priv->sock, (char *)info.data,
		gst_buffer_get_size(outbuf));
	//GST_DEBUG("recieved");

	gst_buffer_unmap(outbuf, &info);

	if (recv_len == SRT_ERROR) {
		GST_WARNING_OBJECT(self, "%s", srt_getlasterror_str());
		ret = GST_FLOW_ERROR;
		goto out;

	}
	else if (recv_len == 0) {
		GST_DEBUG("SRT EOS");
		ret = GST_FLOW_EOS;
		goto out;

	}

	GST_BUFFER_PTS(outbuf) =
		gst_clock_get_time(GST_ELEMENT_CLOCK(src)) -
		GST_ELEMENT_CAST(src)->base_time;

	gst_buffer_resize(outbuf, 0, recv_len);

	GST_LOG_OBJECT(src,
		"filled buffer from _get of size %" G_GSIZE_FORMAT ", ts %"
		GST_TIME_FORMAT ", dur %" GST_TIME_FORMAT
		", offset %" G_GINT64_FORMAT ", offset_end %" G_GINT64_FORMAT,
		gst_buffer_get_size(outbuf),
		GST_TIME_ARGS(GST_BUFFER_TIMESTAMP(outbuf)),
		GST_TIME_ARGS(GST_BUFFER_DURATION(outbuf)),
		GST_BUFFER_OFFSET(outbuf), GST_BUFFER_OFFSET_END(outbuf));

out:
	return ret;
}

static gboolean
gst_srt_client_src_start(GstBaseSrc * src)
{
	GST_DEBUG("Will start SRT client src");
	GstSRTClientSrc *self = GST_SRT_CLIENT_SRC(src);
	GstSRTClientSrcPrivate *priv = GST_SRT_CLIENT_SRC_GET_PRIVATE(self);
	GstUri *uri = gst_uri_ref(GST_SRT_BASE_SRC(self)->uri);
	GSocketAddress *socket_address = NULL;
	GError *error = NULL;
	struct sockaddr sa;
	size_t sa_len;

	socket_address =
		g_inet_socket_address_new_from_string(gst_uri_get_host(uri),
			gst_uri_get_port(uri));

	if (socket_address == NULL) {
		GST_WARNING_OBJECT(self,
			"failed to extract host or port from the given URI");
		goto failed;

	}

	sa_len = g_socket_address_get_native_size(socket_address);
	if (!g_socket_address_to_native(socket_address, &sa, sa_len, &error)) {
		GST_WARNING_OBJECT(self, "cannot resolve address (reason: %s)",
			error->message);
		goto failed;

	}

	priv->sock = srt_socket(sa.sa_family, SOCK_DGRAM, 0);
	if (priv->sock == SRT_ERROR) {
		GST_WARNING_OBJECT(self, "failed to create SRT socket (reason: %s)",
			srt_getlasterror_str());
		goto failed;

	}

	/* Make SRT server socket non-blocking */
	srt_setsockopt(priv->sock, 0, SRTO_SNDSYN, &(int) {
		0}, sizeof(int));
	
	/* Make sure TSBPD mode is enable (SRT mode) */
	srt_setsockopt(priv->sock, 0, SRTO_TSBPDMODE, &(int) {
		1}, sizeof(int));

	/* This is a source, we're always a receiver */
	srt_setsockopt(priv->sock, 0, SRTO_SENDER, &(int) {
		0}, sizeof(int));

	srt_setsockopt(priv->sock, 0, SRTO_TSBPDDELAY, &priv->latency, sizeof(int));

	priv->poll_id = srt_epoll_create();
	if (priv->poll_id == -1) {
		GST_WARNING_OBJECT(self,
			"failed to create poll id for SRT socket (reason: %s)",
			srt_getlasterror_str());
		goto failed;

	}

	srt_epoll_add_usock(priv->poll_id, priv->sock, &(int) {
		SRT_EPOLL_OUT});

	if (srt_connect(priv->sock, &sa, sa_len) == SRT_ERROR) {
		GST_WARNING_OBJECT(self, "failed to connect to host (reason: %s)",
			srt_getlasterror_str());
		goto failed;

	}

	g_clear_pointer(&uri, gst_uri_unref);
	g_clear_object(&socket_address);

	GST_DEBUG("SRT client src started");
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
	g_clear_object(&socket_address);

	return FALSE;
}

static gboolean
gst_srt_client_src_stop(GstBaseSrc * src)
{
	GstSRTClientSrc *self = GST_SRT_CLIENT_SRC(src);
	GstSRTClientSrcPrivate *priv = GST_SRT_CLIENT_SRC_GET_PRIVATE(self);

	/* This is now handled in unlock
	srt_epoll_remove_usock(priv->poll_id, priv->sock);
	srt_epoll_release(priv->poll_id);

	GST_DEBUG_OBJECT(self, "closing SRT connection");
	srt_close(priv->sock);
	*/

	return TRUE;
}

static gboolean
gst_srt_client_src_unlock(GstBaseSrc * src)
{
	GstSRTClientSrc *self = GST_SRT_CLIENT_SRC(src);
	GstSRTClientSrcPrivate *priv = GST_SRT_CLIENT_SRC_GET_PRIVATE(self);
	GST_DEBUG_OBJECT(self, "unlocking client SRT connection");
	srt_epoll_remove_usock(priv->poll_id, priv->sock);
	srt_epoll_release(priv->poll_id);
	srt_close(priv->sock);
}

static void
gst_srt_client_src_class_init(GstSRTClientSrcClass * klass)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
	GstElementClass *gstelement_class = GST_ELEMENT_CLASS(klass);
	GstBaseSrcClass *gstbasesrc_class = GST_BASE_SRC_CLASS(klass);
	GstPushSrcClass *gstpushsrc_class = GST_PUSH_SRC_CLASS(klass);

	gobject_class->set_property = gst_srt_client_src_set_property;
	gobject_class->get_property = gst_srt_client_src_get_property;
	gobject_class->finalize = gst_srt_client_src_finalize;

	/**
		* GstSRTClientSrc:poll-timeout:
		*
		* The timeout(ms) value when polling SRT socket.
		*/
	properties[PROP_POLL_TIMEOUT] =
		g_param_spec_int("poll-timeout", "Poll timeout",
			"Return poll wait after timeout miliseconds (-1 = infinite)", -1,
			G_MAXINT32, SRT_DEFAULT_POLL_TIMEOUT,
			G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY | G_PARAM_STATIC_STRINGS);

	properties[PROP_LATENCY] =
		g_param_spec_int("latency", "latency",
			"Minimum latency(milliseconds)", 0,
			G_MAXINT32, SRT_DEFAULT_LATENCY,
			G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	g_object_class_install_properties(gobject_class, PROP_LAST,	properties);

	gst_element_class_add_static_pad_template(gstelement_class, &src_template);
	gst_element_class_set_metadata(gstelement_class,
		"SRT client source", "Source/Network",
		"Receive data over the network via SRT",
		"Justin Kim <justin.kim@collabora.com>");

	gstbasesrc_class->start = GST_DEBUG_FUNCPTR(gst_srt_client_src_start);
	gstbasesrc_class->stop = GST_DEBUG_FUNCPTR(gst_srt_client_src_stop);
	gstbasesrc_class->unlock = GST_DEBUG_FUNCPTR(gst_srt_client_src_unlock);
	gstpushsrc_class->fill = GST_DEBUG_FUNCPTR(gst_srt_client_src_fill);
}

static void
gst_srt_client_src_init(GstSRTClientSrc * self)
{
	GstSRTClientSrcPrivate *priv = GST_SRT_CLIENT_SRC_GET_PRIVATE(self);

	priv->sock = SRT_ERROR;
	priv->poll_id = SRT_ERROR;
	priv->poll_timeout = SRT_DEFAULT_POLL_TIMEOUT;
	priv->latency = SRT_DEFAULT_LATENCY;
}
