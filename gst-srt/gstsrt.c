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

#include "gstsrt.h"

#include "gstsrtclientsrc.h"
#include "gstsrtserversrc.h"
#include "gstsrtclientsink.h"
#include "gstsrtserversink.h"

#include <srt.h>

#define GST_CAT_DEFAULT gst_debug_srt
GST_DEBUG_CATEGORY (GST_CAT_DEFAULT);

SRTSOCKET
gst_srt_client_connect_full (GstElement * elem, gboolean is_sender,
  const gchar * host, guint16 port, gboolean rendezvous,
  const gchar * bind_address, guint16 bind_port, int latency,
  GSocketAddress ** socket_address, gint * poll_id, gchar * passphrase,
  int key_length)
{
  SRTSOCKET sock = SRT_INVALID_SOCK;
  GError *error = NULL;
  gpointer sa;
  size_t sa_len;

  //TODO change open_read to open_write based on is sender
  if (host == NULL) {
    GST_ELEMENT_ERROR (elem, RESOURCE, OPEN_READ, ("Invalid host"),
      ("Unspecified NULL host"));
    goto failed;
  }

  *socket_address = g_inet_socket_address_new_from_string (host, port);

  if (*socket_address == NULL) {
    GST_ELEMENT_ERROR (elem, RESOURCE, OPEN_READ, ("Invalid host"),
      ("Failed to parse host"));
    goto failed;
  }

  *poll_id = srt_epoll_create ();
  GST_INFO_OBJECT (elem, "SRT Epoll Created %i", *poll_id);
  if (*poll_id == -1) {
    GST_ELEMENT_ERROR (elem, LIBRARY, INIT, (NULL),
      ("failed to create poll id for SRT socket (reason: %s)",
        srt_getlasterror_str ()));
    goto failed;
  }

  sa_len = g_socket_address_get_native_size (*socket_address);
  sa = g_alloca (sa_len);
  if (!g_socket_address_to_native (*socket_address, sa, sa_len, &error)) {
    GST_ELEMENT_ERROR (elem, RESOURCE, OPEN_READ, ("Invalid address"),
      ("cannot resolve address (reason: %s)", error->message));
    goto failed;
  }

  GSocketFamily address_family = g_socket_address_get_family (*socket_address);
  if (address_family == AF_INET)
      GST_LOG_OBJECT (elem, "Using IPv4");
  else if (address_family == AF_INET6)
      GST_WARNING_OBJECT (elem, "Using IPv6 with SRT, this is not fully supported");
  else if (address_family == AF_UNIX)
      GST_WARNING_OBJECT (elem, "Using Unix socket with SRT, this is not fully supported");
  else
      GST_WARNING_OBJECT (elem, "Unknown address family");

  // The SOCK_DGRAM and 0 are legacy from UDT and are not used
  sock = srt_socket (address_family, SOCK_DGRAM, 0);
  GST_INFO_OBJECT (elem, "SRT Socket made");
  if (sock == SRT_ERROR) {
    GST_ELEMENT_ERROR (elem, LIBRARY, INIT, (NULL),
      ("failed to create SRT socket (reason: %s)", srt_getlasterror_str ()));
    goto failed;
  }

  int on = 1;
  int off = 0;
  /* Make sure TSBPD mode is enable (SRT mode) */
  srt_setsockopt (sock, 0, SRTO_TSBPDMODE, &on, sizeof (int));

  /* srt recommends disabling linger */
  srt_setsockopt (sock, 0, SRTO_LINGER, &off, sizeof (int));

  /* If this is a sink, we're a sender, otherwise we're a receiver */
  int is_sender_int = (int)is_sender;
  srt_setsockopt (sock, 0, SRTO_SENDER, &is_sender_int, sizeof (int));

  // If we're a sender, latency is the minimum latency for the receiver
  // If we're a receiver, it's our latency
  if(is_sender)
      srt_setsockopt (sock, 0, SRTO_PEERLATENCY, &latency, sizeof (int));
  else
      srt_setsockopt (sock, 0, SRTO_RCVLATENCY, &latency, sizeof (int));

  GST_INFO_OBJECT (elem, "Using as latency: %i", latency);

  int rendezvousInt = (int)rendezvous;
  srt_setsockopt (sock, 0, SRTO_RENDEZVOUS, &rendezvousInt, sizeof (int));

  if (passphrase != NULL && passphrase[0] != '\0') {
    GST_INFO_OBJECT (elem, "Using passphrase");
    srt_setsockopt (sock, 0, SRTO_PASSPHRASE, passphrase, (int)strlen (passphrase));
    srt_setsockopt (sock, 0, SRTO_PBKEYLEN, &key_length, sizeof (int));
  }

  if (bind_address || bind_port || rendezvous) {
    gpointer bsa;
    size_t bsa_len;
    GSocketAddress *b_socket_address = NULL;
    GST_INFO_OBJECT (elem, "Setting up for rendezvous");

    if (bind_address == NULL)
      bind_address = "0.0.0.0";

    if (rendezvous)
      bind_port = port;

    b_socket_address = g_inet_socket_address_new_from_string (bind_address,
      bind_port);

    if (b_socket_address == NULL) {
      GST_ELEMENT_ERROR (elem, RESOURCE, OPEN_READ, ("Invalid bind address"),
        ("Failed to parse bind address: %s:%d", bind_address, bind_port));
      goto failed;
    }

    bsa_len = g_socket_address_get_native_size (b_socket_address);
    bsa = g_alloca (bsa_len);
    if (!g_socket_address_to_native (b_socket_address, bsa, bsa_len, &error)) {
      GST_ELEMENT_ERROR (elem, RESOURCE, OPEN_READ, ("Invalid bind address"),
        ("Can't parse bind address to sockaddr: %s", error->message));
      g_clear_object (&b_socket_address);
      goto failed;
    }
    g_clear_object (&b_socket_address);

    if (srt_bind (sock, bsa, (int)bsa_len) == SRT_ERROR) {
      GST_ELEMENT_ERROR (elem, RESOURCE, OPEN_READ,
        ("Can't bind to address"),
        ("Can't bind to %s:%d (reason: %s)", bind_address, bind_port,
          srt_getlasterror_str ()));
      goto failed;
    }
  }

  int connectRet = srt_connect (sock, sa, (int)sa_len);
  if (connectRet == SRT_ERROR) {
    GST_ELEMENT_ERROR (elem, RESOURCE, OPEN_READ, ("Connection error"),
      ("failed to connect to host (reason: %s)", srt_getlasterror_str ()));
    goto failed;
  }
  GST_INFO_OBJECT (elem, "SRT connect returned %i", connectRet);

  SRT_SOCKSTATUS status = srt_getsockstate (sock);
  if (status != SRTS_CONNECTED) {
      GST_ERROR_OBJECT (elem, "Socket not connected! err: %s", srt_getlasterror_str ());
      goto failed;
  }

  int events = is_sender ? SRT_EPOLL_IN | SRT_EPOLL_OUT | SRT_EPOLL_ERR
    : SRT_EPOLL_IN | SRT_EPOLL_ERR;
  int addUsockRet = srt_epoll_add_usock (*poll_id, sock, &events);
  GST_INFO_OBJECT (elem, "SRT Epoll Has Usock Added. Returned: %i", addUsockRet );

  return sock;

failed:
  if (*poll_id != SRT_ERROR) {
    srt_epoll_release (*poll_id);
    *poll_id = SRT_ERROR;
  }

  if (sock != SRT_INVALID_SOCK) {
    srt_close (sock);
    sock = SRT_INVALID_SOCK;
  }

  g_clear_error (&error);
  g_clear_object (socket_address);

  return SRT_INVALID_SOCK;
}

SRTSOCKET
gst_srt_client_connect (GstElement * elem, int sender,
  const gchar * host, guint16 port, int rendez_vous,
  const gchar * bind_address, guint16 bind_port, int latency,
  GSocketAddress ** socket_address, gint * poll_id)
{
  return gst_srt_client_connect_full (elem, sender, host, port,
    rendez_vous, bind_address, bind_port, latency, socket_address, poll_id,
    NULL, 0);
}

static gboolean
plugin_init (GstPlugin * plugin)
{
  GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, "srt", 0,
    "SRT");

  if (!gst_element_register (plugin, "srtclientsrc", GST_RANK_PRIMARY,
    GST_TYPE_SRT_CLIENT_SRC))
    return FALSE;

  if (!gst_element_register (plugin, "srtserversrc", GST_RANK_SECONDARY,
    GST_TYPE_SRT_SERVER_SRC))
    return FALSE;

  if (!gst_element_register (plugin, "srtclientsink", GST_RANK_PRIMARY,
    GST_TYPE_SRT_CLIENT_SINK))
    return FALSE;

  if (!gst_element_register (plugin, "srtserversink", GST_RANK_PRIMARY,
    GST_TYPE_SRT_SERVER_SINK))
    return FALSE;

  return TRUE;
}

#ifndef PACKAGE
#define PACKAGE "myfirstplugin"
#endif

GST_PLUGIN_DEFINE (
  GST_VERSION_MAJOR,
  GST_VERSION_MINOR,
  srt,
  "transfer data via SRT",
  plugin_init,
  "0.0.1",
  "GPL",
  "custom",
  "me")