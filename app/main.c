/*  vim: set sts=2 sw=2 et :
 *
 *  Copyright (C) 2015 Centricular Ltd
 *  Author(s): Nirbheek Chauhan <nirbheek@centricular.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "lib.h"
#include "utils.h"

#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)

#include <glib-unix.h>

static GMainLoop *loop = NULL;

static gboolean
kill_remote_peer (OneVideoRemotePeer * remote)
{
  one_video_remote_peer_remove (remote);
  g_main_loop_quit (loop);

  return G_SOURCE_REMOVE;
}

static gboolean
on_local_peer_stop (OneVideoLocalPeer * local)
{
  if (local->state & ONE_VIDEO_LOCAL_STATE_STOPPED) {
      g_print ("Local peer stopped, exiting...\n");
      goto quit;
  }

  if (local->state & ONE_VIDEO_LOCAL_STATE_FAILED &&
      local->state & ONE_VIDEO_LOCAL_STATE_NEGOTIATOR) {
      g_print ("Local negotiator peer failed, exiting...\n");
      one_video_local_peer_stop (local);
      goto quit;
  }

  return G_SOURCE_CONTINUE;
quit:
  g_main_loop_quit (loop);

  return G_SOURCE_REMOVE;
}

static gboolean
on_app_exit (OneVideoLocalPeer * local)
{
  if (local->state & ONE_VIDEO_LOCAL_STATE_NEGOTIATING)
    one_video_local_peer_negotiate_stop (local);
  one_video_local_peer_stop (local);
  g_main_loop_quit (loop);

  return G_SOURCE_REMOVE;
}

static void
on_negotiate_done (GObject * source_object, GAsyncResult * res,
    gpointer user_data)
{
  gboolean ret;
  OneVideoLocalPeer *local;
  GError *error = NULL;

  local = g_task_get_task_data (G_TASK (res));
  ret = one_video_local_peer_negotiate_finish (local, res, &error);
  if (ret) {
    g_print ("All remotes have replied.\n");
    one_video_local_peer_start (local);
    return;
  } else {
    if (error != NULL)
      g_printerr ("Error while negotiating: %s\n", error->message);
  }
}

static void
dial_remotes (OneVideoLocalPeer * local, gchar ** remotes)
{
  guint index;

  for (index = 0; index < g_strv_length (remotes); index++) {
    OneVideoRemotePeer *remote;

    remote = one_video_remote_peer_new (local, remotes[index]);
    one_video_local_peer_add_remote (local, remote);

    g_print ("Created and added remote peer %s\n", remote->addr_s);
  }

  one_video_local_peer_negotiate_async (local, NULL, on_negotiate_done, NULL);
}

static gboolean
device_is_in_use (GstDevice * device)
{
  gchar *name;
  GstElement *check, *src, *sink;
  GstStateChangeReturn state_ret;
  GstState state;
  gboolean ret = FALSE;

  return FALSE;

  check = gst_pipeline_new ("test-v4l2");
  src = gst_device_create_element (device, "test-src");
  sink = gst_element_factory_make ("fakesink", NULL);
  gst_bin_add_many (GST_BIN (check), src, sink, NULL);
  g_assert (gst_element_link (src, sink));
  gst_element_set_state (check, GST_STATE_PLAYING);

  /* Wait for upto 10 seconds in case the state change is ASYNC */
  state_ret = gst_element_get_state (check, &state, NULL, 5*GST_SECOND);
  if (state_ret == GST_STATE_CHANGE_FAILURE) {
    ret = TRUE;
    name = gst_device_get_display_name (device);
    g_printerr ("Unable to use device %s, failing\n", name);
    g_free (name);
  } else if (state_ret == GST_STATE_CHANGE_ASYNC) {
    ret = TRUE;
    name = gst_device_get_display_name (device);
    g_printerr ("Took too long checking device %s, failing\n", name);
    g_free (name);
  } else {
    g_assert (state_ret == GST_STATE_CHANGE_SUCCESS);
    g_assert (state == GST_STATE_PLAYING);
  }

  gst_element_set_state (check, GST_STATE_NULL);
  gst_object_unref (check);
  return ret;
}

static GstDevice *
get_device_choice (GList * devices)
{
  gchar *name;
  GList *device;
  GString *line;
  GIOChannel *channel;
  GError *error = NULL;
  guint num = 0;

  if (devices == NULL) {
    g_printerr ("No video sources detected, using the test video source\n");
    return NULL;
  }

  channel = g_io_channel_unix_new (STDIN_FILENO);
  g_print ("Choose a webcam to use for sending video by entering the "
      "number next to the name:\n");
  g_print ("Test video source [%u]\n", num);
  num += 1;
  for (device = devices; device; device = device->next, num++) {
    /* TODO: Add API to GstDevice for this */
    if (device_is_in_use (device->data))
      continue;
    name = gst_device_get_display_name (device->data);
    g_print ("%s [%u]\n", name, num);
    g_free (name);
  }
  g_print ("> ");
  device = NULL;

again:
  line = g_string_new ("");
  switch (g_io_channel_read_line_string (channel, line, NULL, &error)) {
    guint index;
    case G_IO_STATUS_NORMAL:
      index = g_ascii_digit_value (line->str[0]);
      if (index < 0 || index >= num) {
        g_printerr ("Invalid selection %c, try again\n> ", line->str[0]);
        g_string_free (line, TRUE);
        goto again;
      }
      if (index == 0) {
        /* device is NULL and the test device will be selected */
        g_print ("Selected test video source, continuing...\n");
        break;
      }
      g_assert ((device = g_list_nth (devices, index - 1)));
      name = gst_device_get_display_name (device->data);
      g_print ("Selected device %s, continuing...\n", name);
      g_free (name);
      break;
    case G_IO_STATUS_ERROR:
      g_printerr ("ERROR reading line: %s\n", error->message);
      break;
    case G_IO_STATUS_EOF:
      g_printerr ("Nothing entered? (EOF)\n");
      goto again;
    case G_IO_STATUS_AGAIN:
      g_printerr ("EAGAIN\n");
      goto again;
    default:
      g_assert_not_reached ();
  }

  g_string_free (line, TRUE);
  g_io_channel_unref (channel);

  return device ? device->data : NULL;
}

int
main (int   argc,
      char *argv[])
{
  OneVideoLocalPeer *local;
  GOptionContext *optctx;
  GInetAddress *inet_addr = NULL;
  GSocketAddress *listen_addr;
  GError *error = NULL;
  GList *devices;

  guint exit_after = 0;
  gboolean auto_exit = FALSE;
  guint iface_port = ONE_VIDEO_DEFAULT_COMM_PORT;
  gchar *iface_name = NULL;
  gchar **remotes = NULL;
  GOptionEntry entries[] = {
    {"exit-after", 0, 0, G_OPTION_ARG_INT, &exit_after, "Exit cleanly after N"
          " seconds (default: never exit)", "SECONDS"},
    {"auto-exit", 0, 0, G_OPTION_ARG_NONE, &auto_exit, "Automatically exit when"
          " the call is ended in passive mode (default: no)", NULL},
    {"interface", 'i', 0, G_OPTION_ARG_STRING, &iface_name, "Network interface"
          " to listen on (default: any)", "NAME"},
    {"port", 'p', 0, G_OPTION_ARG_INT, &iface_port, "TCP port to listen on"
          " for incoming connections (default: " STR(ONE_VIDEO_DEFAULT_COMM_PORT)
          ")", "PORT"},
    {"peer", 'c', 0, G_OPTION_ARG_STRING_ARRAY, &remotes, "Peers with an"
          " optional port to connect to. Specify multiple times to connect to"
          " several peers. Without this option, passive mode is used in which"
          " we wait for incoming connections.", "PEER:PORT"},
    {NULL}
  };

  gst_init (&argc, &argv);
  optctx = g_option_context_new (" - Peer-to-Peer low-latency high-bandwidth "
      "VoIP application");
  g_option_context_add_main_entries (optctx, entries, NULL);
  g_option_context_add_group (optctx, gst_init_get_option_group ());
  if (!g_option_context_parse (optctx, &argc, &argv, &error)) {
    g_printerr ("Error parsing options: %s\n", error->message);
    return -1;
  }
  g_option_context_free (optctx);

  loop = g_main_loop_new (NULL, FALSE);

  if (iface_name != NULL) {
    inet_addr = one_video_get_inet_addr_for_iface (iface_name);
  } else {
    inet_addr = g_inet_address_new_loopback (G_SOCKET_FAMILY_IPV4);
    /* FIXME: Listen on the default route instead */
    g_printerr ("Interface not specified, listening on localhost\n");
  }

  listen_addr = g_inet_socket_address_new (inet_addr, iface_port);
  g_object_unref (inet_addr);

  local = one_video_local_peer_new (G_INET_SOCKET_ADDRESS (listen_addr));
  g_object_unref (listen_addr);

  devices = one_video_local_peer_get_video_devices (local);
  one_video_local_peer_set_video_device (local, get_device_choice (devices));
  g_list_free_full (devices, g_object_unref);

  if (remotes == NULL) {
    g_print ("No remotes specified; listening for incoming connections\n");
    /* Stop the mainloop and exit when the call ends */
  } else {
    g_print ("Dialling remotes...\n");
    dial_remotes (local, remotes);
    g_print ("Waiting for remotes to reply...\n");
  }

  /* If in passive mode, auto exit only when requested */
  if (remotes != NULL || auto_exit)
    g_idle_add ((GSourceFunc) on_local_peer_stop, local);
  g_unix_signal_add (SIGINT, (GSourceFunc) on_app_exit, local);
  if (exit_after > 0)
    g_timeout_add_seconds (exit_after, (GSourceFunc) on_app_exit, local);

  g_main_loop_run (loop);

  g_clear_pointer (&local, one_video_local_peer_free);
  g_clear_pointer (&loop, g_main_loop_unref);
  g_strfreev (remotes);
  g_free (iface_name);

  return 0;
}
