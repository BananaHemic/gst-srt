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
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstsrtclientsrc.h"
#include "gstsrtserversrc.h"
#include "gstsrtclientsink.h"
#include "gstsrtserversink.h"

#include <srt.h>

static gboolean
plugin_init(GstPlugin * plugin)
{
	if (!gst_element_register(plugin, "srtclientsrc", GST_RANK_PRIMARY,
		GST_TYPE_SRT_CLIENT_SRC))
		return FALSE;

	if (!gst_element_register(plugin, "srtserversrc", GST_RANK_SECONDARY,
		GST_TYPE_SRT_SERVER_SRC))
		return FALSE;

	if (!gst_element_register(plugin, "srtclientsink", GST_RANK_PRIMARY,
		GST_TYPE_SRT_CLIENT_SINK))
		return FALSE;

	if (!gst_element_register(plugin, "srtserversink", GST_RANK_PRIMARY,
		GST_TYPE_SRT_SERVER_SINK))
		return FALSE;

	return TRUE;
}

#ifndef PACKAGE
#define PACKAGE "myfirstplugin"
#endif

GST_PLUGIN_DEFINE(
	GST_VERSION_MAJOR,
	GST_VERSION_MINOR,
	srt,
	"transfer data via SRT",
	plugin_init,
	"0.0.1",
	"GPL",
	"custom",
	"me")