/* GStreamer
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

#ifndef __GST_SRT_CLIENT_SINK_H__
#define __GST_SRT_CLIENT_SINK_H__

#include "gstsrtbasesink.h"

#ifndef _WIN32
#include <sys/socket.h>
#else
#include <WinSock2.h>
#endif

G_BEGIN_DECLS

#define GST_TYPE_SRT_CLIENT_SINK              (gst_srt_client_sink_get_type ())
#define GST_IS_SRT_CLIENT_SINK(obj)           (G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TYPE_SRT_CLIENT_SINK))
#define GST_IS_SRT_CLIENT_SINK_CLASS(klass)   (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_SRT_CLIENT_SINK))
#define GST_SRT_CLIENT_SINK_GET_CLASS(obj)    (G_TYPE_INSTANCE_GET_CLASS((obj), GST_TYPE_SRT_CLIENT_SINK, GstSRTClientSinkClass))
#define GST_SRT_CLIENT_SINK(obj)              (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_SRT_CLIENT_SINK, GstSRTClientSink))
#define GST_SRT_CLIENT_SINK_CLASS(klass)      (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_SRT_CLIENT_SINK, GstSRTClientSinkClass))
#define GST_SRT_CLIENT_SINK_CAST(obj)         ((GstSRTClientSink*)(obj))
#define GST_SRT_CLIENT_SINK_CLASS_CAST(klass) ((GstSRTClientSinkClass*)(klass))

typedef struct _GstSRTClientSink GstSRTClientSink;
typedef struct _GstSRTClientSinkClass GstSRTClientSinkClass;
typedef struct _GstSRTClientSinkPrivate GstSRTClientSinkPrivate;

struct _GstSRTClientSink {
  GstSRTBaseSink parent;

  /*< private >*/
  gpointer _gst_reserved[GST_PADDING];

};

struct _GstSRTClientSinkClass {
  GstSRTBaseSinkClass parent_class;

  gpointer _gst_reserved[GST_PADDING_LARGE];

};

GST_EXPORT
GType gst_srt_client_sink_get_type (void);

G_END_DECLS

#endif /* __GST_SRT_CLIENT_SINK_H__ */