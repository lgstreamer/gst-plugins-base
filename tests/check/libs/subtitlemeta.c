/* GStreamer
 *
 * unit test for subtitle meta
 *
 * Copyright (C) 2018 LG Electronics, Inc.
 *           Jinuk Jeon <jinuk.jeon@lge.com>
 *           DongYun Seo <dongyun.seo@lge.com>
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

#include <gst/check/gstcheck.h>
#include <gst/subtitle/subtitle.h>
#include <gst/app/gstappsink.h>

static GstPad *mysrcpad;

static GstStaticPadTemplate srctemplate = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("application/x-gst-check")
    );


static GstBusSyncReply
_on_bus_message (GstBus * bus, GstMessage * message, gpointer * data)
{
  GST_DEBUG ("_on_bus_message called");
  switch (GST_MESSAGE_TYPE (message)) {
    case GST_MESSAGE_APPLICATION:
    {
      const GstStructure *strc = gst_message_get_structure (message);
      guint64 period_start;
      gint64 offset;
      guint subtitle_index;

      gst_structure_get_uint64 (strc, "period-start", &period_start);
      gst_structure_get_int64 (strc, "offset", &offset);
      gst_structure_get_uint (strc, "subtitle-index", &subtitle_index);
      GST_DEBUG ("start:%u , offset:%d, index:%u", period_start,
          offset, subtitle_index);

      fail_if (period_start != 60000);
      fail_if (offset != 70000);
      fail_if (subtitle_index != 1);
      break;
    }
    default:
      break;
  }

  return GST_BUS_PASS;
}

void
_local_main_loop_thread (gpointer loop)
{
  GMainLoop **main_loop = (GMainLoop **) loop;
  *main_loop = g_main_loop_new (NULL, FALSE);
  g_main_loop_run (*main_loop);

  return NULL;
}

static GstFlowReturn
callback_function_sample_fallback (GstAppSink * appsink, gpointer p_counter)
{
  GstSample *sample;
  GstBuffer *buf;
  GstSubtitleDataMeta *meta;
  GstStructure *strc;

  GST_DEBUG ("callback_function_sample_fallback called");
  sample = gst_app_sink_pull_sample (appsink);
  buf = gst_sample_get_buffer (sample);
  fail_unless (GST_IS_BUFFER (buf));

  meta = gst_buffer_get_subtitle_data_meta (buf);
  fail_if (meta == NULL);
  GST_DEBUG ("start:%llu , offset:%lld, index:%u", meta->period_start,
      meta->offset, meta->subtitle_index);
  fail_if (meta->period_start != 60000);
  fail_if (meta->offset != 70000);
  fail_if (meta->subtitle_index != 1);

  strc = gst_structure_new ("subtitle_meta",
      "period-start", G_TYPE_UINT64, meta->period_start,
      "offset", G_TYPE_INT64, meta->offset,
      "subtitle-index", G_TYPE_UINT, meta->subtitle_index, NULL);

  fail_if (!gst_element_post_message (GST_ELEMENT_CAST (appsink),
          gst_message_new_application (GST_OBJECT_CAST (appsink), strc)));

  return GST_FLOW_OK;
}

GST_START_TEST (test_subtitlemeta_data)
{
  GstBuffer *buffer;
  GstSubtitleDataMeta *meta;
  GstMapInfo info;

  buffer = gst_buffer_new_and_alloc (4);
  fail_if (buffer == NULL);

  fail_unless (gst_buffer_map (buffer, &info, GST_MAP_WRITE));
  fail_if (info.data == NULL);
  memset (info.data, 0, 4);
  gst_buffer_unmap (buffer, &info);

  meta = gst_buffer_add_subtitle_data_meta (buffer, 60000, 70000, 1);
  fail_if (meta == NULL);

  meta = gst_buffer_get_subtitle_data_meta (buffer);
  fail_if (meta == NULL);
  fail_if (meta->period_start != 60000);
  fail_if (meta->offset != 70000);
  fail_if (meta->subtitle_index != 1);

  gst_buffer_unref (buffer);
}

GST_END_TEST;

GST_START_TEST (test_subtitlemeta_notify)
{
  GstElement *appsink;
  GstBuffer *buffer;
  GstSubtitleDataMeta *meta;
  GstMapInfo info;
  GstElement *pipeline;
  GstBus *bus;
  GMainLoop *mainloop;
  GThread *loop_thread;
  GstCaps *caps;

  pipeline = gst_pipeline_new ("pipeline");
  appsink = gst_element_factory_make ("appsink", NULL);

  gst_bin_add_many ((GstBin *) pipeline, appsink, NULL);
  mysrcpad = gst_check_setup_src_pad (appsink, &srctemplate);
  gst_pad_set_active (mysrcpad, TRUE);

  caps = gst_caps_new_empty_simple ("application/x-gst-check");
  gst_check_setup_events (mysrcpad, appsink, caps, GST_FORMAT_TIME);
  gst_caps_unref (caps);

  g_signal_connect (appsink, "new-sample",
      G_CALLBACK (callback_function_sample_fallback), NULL);

  g_object_set (appsink, "emit-signals", TRUE, NULL);

  bus = gst_pipeline_get_bus (GST_PIPELINE (pipeline));
  gst_bus_set_sync_handler (bus, (GstBusSyncHandler) _on_bus_message, NULL,
      NULL);

  gst_element_set_state (pipeline, GST_STATE_PLAYING);

  loop_thread = g_thread_new ("BusMessageThread", _local_main_loop_thread,
      &mainloop);
  fail_if (loop_thread == NULL);

  buffer = gst_buffer_new_and_alloc (4);
  fail_if (buffer == NULL);

  fail_unless (gst_buffer_map (buffer, &info, GST_MAP_WRITE));
  fail_if (info.data == NULL);
  memset (info.data, 0, 4);
  gst_buffer_unmap (buffer, &info);

  meta = gst_buffer_add_subtitle_data_meta (buffer, 60000, 70000, 1);
  fail_if (meta == NULL);

  GST_DEBUG ("push buffer with subtitle meta");
  fail_unless (gst_pad_push (mysrcpad, buffer) == GST_FLOW_OK);

  GST_DEBUG ("cleaning up appsink");
  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_object_unref (pipeline);
  g_main_loop_quit (mainloop);
  g_main_loop_unref (mainloop);
}

GST_END_TEST;

static Suite *
subtitlemeta_suite (void)
{
  Suite *s = suite_create ("GstSubtitleMeta");
  TCase *tc_chain = tcase_create ("general");

  suite_add_tcase (s, tc_chain);
  tcase_add_test (tc_chain, test_subtitlemeta_data);
  tcase_add_test (tc_chain, test_subtitlemeta_notify);

  return s;
}

GST_CHECK_MAIN (subtitlemeta);
