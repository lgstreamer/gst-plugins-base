/* GStreamer
 *
 * Copyright (C) 2010, Thiago Santos <thiago.sousa.santos@collabora.co.uk>
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
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>

#ifdef HAVE_VALGRIND
#include <valgrind/valgrind.h>
#else
#define RUNNING_ON_VALGRIND FALSE
#endif

#define SAMPLE_CAPS "application/x-gst-check-test"

static GstPad *mysinkpad;

static GstStaticPadTemplate sinktemplate = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);

static GstElement *
setup_appsrc (void)
{
  GstElement *appsrc;

  GST_DEBUG ("setup_appsrc");
  appsrc = gst_check_setup_element ("appsrc");
  mysinkpad = gst_check_setup_sink_pad (appsrc, &sinktemplate);

  gst_pad_set_active (mysinkpad, TRUE);

  return appsrc;
}

static void
cleanup_appsrc (GstElement * appsrc)
{
  GST_DEBUG ("cleanup_appsrc");

  gst_check_drop_buffers ();
  gst_check_teardown_sink_pad (appsrc);
  gst_check_teardown_element (appsrc);
}

/*
 * Pushes 4 buffers into appsrc and checks the caps on them on the output.
 *
 * Appsrc is configured with caps=SAMPLE_CAPS, so the buffers should have the
 * same caps that they were pushed with.
 *
 * The 4 buffers have NULL, SAMPLE_CAPS, NULL, SAMPLE_CAPS caps,
 * respectively.
 */
GST_START_TEST (test_appsrc_non_null_caps)
{
  GstElement *src;
  GstBuffer *buffer;
  GstCaps *caps, *ccaps;

  src = setup_appsrc ();

  caps = gst_caps_from_string (SAMPLE_CAPS);
  g_object_set (src, "caps", caps, NULL);

  ASSERT_SET_STATE (src, GST_STATE_PLAYING, GST_STATE_CHANGE_SUCCESS);

  buffer = gst_buffer_new_and_alloc (4);
  fail_unless (gst_app_src_push_buffer (GST_APP_SRC (src),
          buffer) == GST_FLOW_OK);

  buffer = gst_buffer_new_and_alloc (4);
  fail_unless (gst_app_src_push_buffer (GST_APP_SRC (src),
          buffer) == GST_FLOW_OK);

  buffer = gst_buffer_new_and_alloc (4);
  fail_unless (gst_app_src_push_buffer (GST_APP_SRC (src),
          buffer) == GST_FLOW_OK);

  buffer = gst_buffer_new_and_alloc (4);
  fail_unless (gst_app_src_push_buffer (GST_APP_SRC (src),
          buffer) == GST_FLOW_OK);

  fail_unless (gst_app_src_end_of_stream (GST_APP_SRC (src)) == GST_FLOW_OK);

  /* Give some time to the appsrc loop to push the buffers */
  g_usleep (G_USEC_PER_SEC * 3);

  /* Check the output caps */
  fail_unless (g_list_length (buffers) == 4);

  ccaps = gst_pad_get_current_caps (mysinkpad);
  fail_unless (gst_caps_is_equal (ccaps, caps));
  gst_caps_unref (ccaps);

  ASSERT_SET_STATE (src, GST_STATE_NULL, GST_STATE_CHANGE_SUCCESS);
  gst_caps_unref (caps);
  cleanup_appsrc (src);
}

GST_END_TEST;

static GstAppSinkCallbacks app_callbacks;

typedef struct
{
  GstElement *source;
  GstElement *sink;
} ProgramData;

static GstFlowReturn
on_new_sample_from_source (GstAppSink * elt, gpointer user_data)
{
  ProgramData *data = (ProgramData *) user_data;
  GstSample *sample;
  GstBuffer *buffer;
  GstElement *source;

  sample = gst_app_sink_pull_sample (GST_APP_SINK (elt));
  buffer = gst_sample_get_buffer (sample);
  source = gst_bin_get_by_name (GST_BIN (data->sink), "testsource");
  gst_app_src_push_buffer (GST_APP_SRC (source), gst_buffer_ref (buffer));
  gst_sample_unref (sample);
  g_object_unref (source);
  return GST_FLOW_OK;
}

/*
 * appsink => appsrc pipelines executed 100 times:
 * - appsink pipeline has sync=false
 * - appsrc pipeline has sync=true
 * - appsrc has block=true
 * after 1 second an error message is posted on appsink pipeline bus
 * when the error is received the appsrc pipeline is set to NULL
 * and then the appsink pipeline is
 * set to NULL too, this must not deadlock
 */

GST_START_TEST (test_appsrc_block_deadlock)
{
  GstElement *testsink;
  ProgramData *data;

  GST_INFO ("iteration %d", __i__);

  data = g_new0 (ProgramData, 1);

  data->source =
      gst_parse_launch ("videotestsrc ! video/x-raw,width=16,height=16 ! "
      "appsink sync=false name=testsink", NULL);

  fail_unless (data->source != NULL);

  app_callbacks.new_sample = on_new_sample_from_source;
  testsink = gst_bin_get_by_name (GST_BIN (data->source), "testsink");
  gst_app_sink_set_callbacks (GST_APP_SINK_CAST (testsink), &app_callbacks,
      data, NULL);

  gst_object_unref (testsink);

  data->sink =
      gst_parse_launch
      ("appsrc name=testsource block=1 max-bytes=1000 is-live=true ! "
      "fakesink sync=true", NULL);

  fail_unless (data->sink != NULL);

  ASSERT_SET_STATE (data->sink, GST_STATE_PLAYING, GST_STATE_CHANGE_ASYNC);
  ASSERT_SET_STATE (data->source, GST_STATE_PLAYING, GST_STATE_CHANGE_ASYNC);

  /* wait for preroll */
  gst_element_get_state (data->source, NULL, NULL, GST_CLOCK_TIME_NONE);
  gst_element_get_state (data->sink, NULL, NULL, GST_CLOCK_TIME_NONE);

  g_usleep (50 * (G_USEC_PER_SEC / 1000));

  ASSERT_SET_STATE (data->sink, GST_STATE_NULL, GST_STATE_CHANGE_SUCCESS);
  ASSERT_SET_STATE (data->source, GST_STATE_NULL, GST_STATE_CHANGE_SUCCESS);

  gst_object_unref (data->source);
  gst_object_unref (data->sink);
  g_free (data);
}

GST_END_TEST;

typedef struct
{
  GstCaps *caps1;
  GstCaps *caps2;
  GstCaps *expected_caps;
} Helper;

static void
caps_notify_cb (GObject * obj, GObject * child, GParamSpec * pspec, Helper * h)
{
  GstCaps *caps = NULL;

  g_object_get (child, "caps", &caps, NULL);
  if (caps) {
    GST_LOG_OBJECT (child, "expected caps: %" GST_PTR_FORMAT, h->expected_caps);
    GST_LOG_OBJECT (child, "caps set to  : %" GST_PTR_FORMAT, caps);
    fail_unless (gst_caps_is_equal (caps, h->expected_caps));
    gst_caps_unref (caps);
  }
}

static void
handoff_cb (GstElement * sink, GstBuffer * buf, GstPad * pad, Helper * h)
{
  /* have our buffer, now the caps should change */
  h->expected_caps = h->caps2;
  GST_INFO ("got buffer, expect caps %" GST_PTR_FORMAT " next", h->caps2);
}

/* Make sure that if set_caps() is called twice before the source is started,
 * the caps are just replaced and not put into the internal queue */
GST_START_TEST (test_appsrc_set_caps_twice)
{
  GstElement *pipe, *src, *sink;
  GstMessage *msg;
  GstCaps *caps;
  Helper h;

  h.caps1 = gst_caps_new_simple ("foo/bar", "bleh", G_TYPE_INT, 2, NULL);
  h.caps2 = gst_caps_new_simple ("bar/foo", "xyz", G_TYPE_INT, 3, NULL);

  pipe = gst_pipeline_new ("pipeline");
  src = gst_element_factory_make ("appsrc", NULL);
  sink = gst_element_factory_make ("fakesink", NULL);
  gst_bin_add_many (GST_BIN (pipe), src, sink, NULL);
  gst_element_link (src, sink);

  g_signal_connect (pipe, "deep-notify::caps", G_CALLBACK (caps_notify_cb), &h);

  g_object_set (sink, "signal-handoffs", TRUE, NULL);
  g_signal_connect (sink, "handoff", G_CALLBACK (handoff_cb), &h);

  /* case 1: set caps to caps1, then set again to caps2, all this before
   * appsrc is started and before any buffers are in the queue yet. We don't
   * want to see any trace of caps1 during negotiation in this case. */
  gst_app_src_set_caps (GST_APP_SRC (src), h.caps1);
  caps = gst_app_src_get_caps (GST_APP_SRC (src));
  fail_unless (gst_caps_is_equal (caps, h.caps1));
  gst_caps_unref (caps);

  gst_app_src_set_caps (GST_APP_SRC (src), h.caps2);
  caps = gst_app_src_get_caps (GST_APP_SRC (src));
  fail_unless (gst_caps_is_equal (caps, h.caps2));
  gst_caps_unref (caps);

  gst_app_src_end_of_stream (GST_APP_SRC (src));

  h.expected_caps = h.caps2;

  gst_element_set_state (pipe, GST_STATE_PLAYING);

  msg =
      gst_bus_timed_pop_filtered (GST_ELEMENT_BUS (pipe), -1, GST_MESSAGE_EOS);
  gst_message_unref (msg);

  gst_element_set_state (pipe, GST_STATE_NULL);
  gst_object_unref (pipe);

  GST_INFO ("Case #2");

  /* case 2: set caps to caps1, then push a buffer and set to caps2, again
   * before appsrc is started. In this case appsrc should negotiate to caps1
   * first, and then caps2 after pushing the first buffer. */

  /* We're creating a new pipeline/appsrc here because appsrc's behaviour
   * change slightly after setting it to NULL/READY and then re-using it */
  pipe = gst_pipeline_new ("pipeline");
  src = gst_element_factory_make ("appsrc", NULL);
  sink = gst_element_factory_make ("fakesink", NULL);
  gst_bin_add_many (GST_BIN (pipe), src, sink, NULL);
  gst_element_link (src, sink);

  g_signal_connect (pipe, "deep-notify::caps", G_CALLBACK (caps_notify_cb), &h);

  g_object_set (sink, "signal-handoffs", TRUE, NULL);
  g_signal_connect (sink, "handoff", G_CALLBACK (handoff_cb), &h);

  gst_app_src_set_caps (GST_APP_SRC (src), h.caps1);
  caps = gst_app_src_get_caps (GST_APP_SRC (src));
  fail_unless (gst_caps_is_equal (caps, h.caps1));
  gst_caps_unref (caps);

  /* first caps1, then buffer, then later caps2 */
  h.expected_caps = h.caps1;

  gst_element_set_state (pipe, GST_STATE_PLAYING);

  gst_app_src_push_buffer (GST_APP_SRC (src), gst_buffer_new ());

  gst_app_src_set_caps (GST_APP_SRC (src), h.caps2);
  caps = gst_app_src_get_caps (GST_APP_SRC (src));
  fail_unless (gst_caps_is_equal (caps, h.caps2));
  gst_caps_unref (caps);

  gst_app_src_end_of_stream (GST_APP_SRC (src));

  msg =
      gst_bus_timed_pop_filtered (GST_ELEMENT_BUS (pipe), -1, GST_MESSAGE_EOS);
  gst_message_unref (msg);

  gst_element_set_state (pipe, GST_STATE_NULL);
  gst_object_unref (pipe);

  gst_caps_unref (h.caps2);
  gst_caps_unref (h.caps1);
}

GST_END_TEST;

static gboolean
seek_cb (GstAppSrc * src, guint64 offset, gpointer data)
{
  /* Return fake true */
  return TRUE;
}

static void
caps_cb (GObject * obj, GObject * child, GParamSpec * pspec,
    GstCaps ** received_caps)
{
  GstCaps *caps = NULL;

  /* Collect the caps */
  g_object_get (child, "caps", &caps, NULL);
  if (caps) {
    GST_LOG_OBJECT (child, "caps set to  : %" GST_PTR_FORMAT, caps);
    gst_caps_replace (received_caps, caps);
    gst_caps_unref (caps);
  }
}

GST_START_TEST (test_appsrc_caps_in_push_modes)
{
  GstElement *pipe, *src, *sink;
  GstMessage *msg;
  GstCaps *caps, *caps1, *received_caps;
  gint i;
  GstMessageType msg_types;
  GstAppSrcCallbacks cb = { 0 };
  GstAppStreamType modes[] = { GST_APP_STREAM_TYPE_STREAM,
    GST_APP_STREAM_TYPE_SEEKABLE,
    GST_APP_STREAM_TYPE_RANDOM_ACCESS
  };

  for (i = 0; i < sizeof (modes) / sizeof (modes[0]); i++) {
    GST_INFO ("checking mode %d", modes[i]);
    caps1 = gst_caps_new_simple ("foo/bar", "bleh", G_TYPE_INT, 2, NULL);
    received_caps = NULL;

    pipe = gst_pipeline_new ("pipeline");
    src = gst_element_factory_make ("appsrc", NULL);
    sink = gst_element_factory_make ("fakesink", NULL);
    gst_bin_add_many (GST_BIN (pipe), src, sink, NULL);
    gst_element_link (src, sink);

    g_object_set (G_OBJECT (src), "stream-type", modes[i], NULL);
    if (modes[i] != GST_APP_STREAM_TYPE_STREAM) {
      cb.seek_data = seek_cb;
      gst_app_src_set_callbacks (GST_APP_SRC (src), &cb, NULL, NULL);
    }
    g_signal_connect (pipe, "deep-notify::caps", G_CALLBACK (caps_cb),
        &received_caps);

    gst_app_src_set_caps (GST_APP_SRC (src), caps1);
    caps = gst_app_src_get_caps (GST_APP_SRC (src));
    fail_unless (gst_caps_is_equal (caps, caps1));
    gst_caps_unref (caps);

    gst_element_set_state (pipe, GST_STATE_PLAYING);

    if (modes[i] != GST_APP_STREAM_TYPE_RANDOM_ACCESS) {
      gst_app_src_end_of_stream (GST_APP_SRC (src));
      msg_types = GST_MESSAGE_EOS;
    } else {
      gst_app_src_push_buffer (GST_APP_SRC (src), gst_buffer_new ());
      msg_types = GST_MESSAGE_ASYNC_DONE;
    }

    msg = gst_bus_timed_pop_filtered (GST_ELEMENT_BUS (pipe), -1, msg_types);
    gst_message_unref (msg);
    /* The collected caps should match with one that was pushed */
    fail_unless (received_caps && gst_caps_is_equal (received_caps, caps1));

    gst_element_set_state (pipe, GST_STATE_NULL);
    gst_object_unref (pipe);
    gst_caps_unref (caps1);
    if (received_caps)
      gst_caps_unref (received_caps);
  }
}

GST_END_TEST;

/* This test simulates a pipeline blocked pushing caps using a blocking pad
 * probe. This state is seen if the application push buffers and later change
 * the caps on one stream before the other stream have prerolled. In this
 * state, GStreamer 1.12 and previous would deadlock inside GstBaseSrc as
 * it was holding the live lock while calling create(). AppSrc serialize the
 * caps event into it's queue and then push it downstream when create() is
 * called. */

static GstPadProbeReturn
caps_event_probe_cb (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  GMainLoop *loop = user_data;

  if (GST_EVENT_TYPE (info->data) == GST_EVENT_CAPS) {
    g_main_loop_quit (loop);
    return GST_PAD_PROBE_OK;
  }

  return GST_PAD_PROBE_PASS;
}

GST_START_TEST (test_appsrc_blocked_on_caps)
{
  GstElement *pipeline = NULL, *app = NULL;
  GstPad *pad = NULL;
  GstCaps *caps = NULL;
  GError *error = NULL;
  GMainLoop *loop;

  loop = g_main_loop_new (NULL, FALSE);

  pipeline = gst_parse_launch ("appsrc is-live=1 name=app ! fakesink", &error);
  g_assert_no_error (error);

  app = gst_bin_get_by_name (GST_BIN (pipeline), "app");
  pad = gst_element_get_static_pad (app, "src");

  gst_pad_add_probe (pad,
      GST_PAD_PROBE_TYPE_BLOCK | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
      caps_event_probe_cb, loop, NULL);
  gst_object_unref (app);
  gst_object_unref (pad);

  gst_element_set_state (pipeline, GST_STATE_PLAYING);

  caps = gst_caps_from_string ("application/x-test");
  gst_app_src_set_caps (GST_APP_SRC (app), caps);
  gst_caps_unref (caps);

  g_main_loop_run (loop);

#if 0
  /* This would work around the issue by deblocking the source on older
   * version of GStreamer */
  gst_element_send_event (app, gst_event_new_flush_start ());
#endif

  /* As appsrc change the caps GstBaseSrc::create() virtual function, the live
   * lock use to remains held and prevented the state change from happening. */
  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_object_unref (pipeline);
  g_main_loop_unref (loop);
}

GST_END_TEST;

static guint expect_offset;
static gboolean chainlist_called;
static gboolean done;

static gboolean
event_func (GstPad * pad, GstObject * parent, GstEvent * event)
{
  GST_LOG ("event %" GST_PTR_FORMAT, event);
  if (GST_EVENT_TYPE (event) == GST_EVENT_EOS) {
    g_mutex_lock (&check_mutex);
    done = TRUE;
    g_cond_signal (&check_cond);
    g_mutex_unlock (&check_mutex);
  }
  gst_event_unref (event);
  return TRUE;
}

static GstFlowReturn
chain_____func (GstPad * pad, GstObject * parent, GstBuffer * buf)
{
  GST_LOG ("  buffer # %3u", (guint) GST_BUFFER_OFFSET (buf));

  fail_unless_equals_int (GST_BUFFER_OFFSET (buf), expect_offset);
  ++expect_offset;
  gst_buffer_unref (buf);

  return GST_FLOW_OK;
}

static GstFlowReturn
chainlist_func (GstPad * pad, GstObject * parent, GstBufferList * list)
{
  guint i, len;

  len = gst_buffer_list_length (list);

  GST_DEBUG ("buffer list with %u buffers", len);
  for (i = 0; i < len; ++i) {
    GstBuffer *buf = gst_buffer_list_get (list, i);
    GST_LOG ("  buffer # %3u", (guint) GST_BUFFER_OFFSET (buf));

    fail_unless_equals_int (GST_BUFFER_OFFSET (buf), expect_offset);
    ++expect_offset;
  }
  chainlist_called = TRUE;
  gst_buffer_list_unref (list);
  return GST_FLOW_OK;
}

GST_START_TEST (test_appsrc_push_buffer_list)
{
  GstElement *src;
  guint i;

  src = gst_element_factory_make ("appsrc", "appsrc");

  mysinkpad = gst_check_setup_sink_pad (src, &sinktemplate);
  gst_pad_set_chain_function (mysinkpad, chain_____func);
  gst_pad_set_chain_list_function (mysinkpad, chainlist_func);
  gst_pad_set_event_function (mysinkpad, event_func);
  gst_pad_set_active (mysinkpad, TRUE);

  expect_offset = 0;
  chainlist_called = FALSE;
  done = FALSE;

  gst_element_set_state (src, GST_STATE_PLAYING);

#define NUM_BUFFERS 100

  for (i = 0; i < NUM_BUFFERS; ++i) {
    GstFlowReturn flow;
    GstBuffer *buf;

    buf = gst_buffer_new ();
    GST_BUFFER_OFFSET (buf) = i;

    if (i == 0 || g_random_boolean ()) {
      GstBufferList *buflist = gst_buffer_list_new ();

      gst_buffer_list_add (buflist, buf);

      buf = gst_buffer_new ();
      GST_BUFFER_OFFSET (buf) = ++i;
      gst_buffer_list_add (buflist, buf);
      if (g_random_boolean ()) {
        flow = gst_app_src_push_buffer_list (GST_APP_SRC (src), buflist);
      } else {
        g_signal_emit_by_name (src, "push-buffer-list", buflist, &flow);
        gst_buffer_list_unref (buflist);
      }
    } else {
      flow = gst_app_src_push_buffer (GST_APP_SRC (src), buf);
    }
    fail_unless_equals_int (flow, GST_FLOW_OK);
  }

  gst_app_src_end_of_stream (GST_APP_SRC (src));

  g_mutex_lock (&check_mutex);
  while (!done)
    g_cond_wait (&check_cond, &check_mutex);
  g_mutex_unlock (&check_mutex);

  gst_element_set_state (src, GST_STATE_NULL);

  /* make sure the buffer list was pushed out as list! */
  fail_unless (chainlist_called);

  /* can be NUM_BUFFERS or NUM_BUFFERS + 1 depending on whether last item
   * was buffer list or not */
  fail_unless (expect_offset >= NUM_BUFFERS);

  gst_check_teardown_sink_pad (src);

  gst_object_unref (src);
}

GST_END_TEST;

static GstPadProbeReturn
appsrc_pad_probe (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  GList **expected = (GList **) (user_data);
  GList *next;
  GstEvent *exp;
  GstBuffer *exp_buf;

  if (GST_IS_EVENT (GST_PAD_PROBE_INFO_DATA (info))) {
    GstEvent *ev = GST_PAD_PROBE_INFO_EVENT (info);
    GST_DEBUG ("Got event %s", GST_EVENT_TYPE_NAME (ev));
    switch (GST_EVENT_TYPE (ev)) {
      case GST_EVENT_SEGMENT:
      {
        fail_if (*expected == NULL,
            "appsrc pushed a SEGMENT event but we didn't expect any");
        next = (*expected)->next;
        fail_unless (GST_IS_EVENT ((*expected)->data),
            "appsrc pushed a SEGMENT event but we expected any others");
        exp = GST_EVENT ((*expected)->data);
        fail_unless (GST_EVENT_TYPE (ev) == GST_EVENT_TYPE (exp),
            "Got event of type %s but expected event was %s",
            GST_EVENT_TYPE_NAME (ev), GST_EVENT_TYPE_NAME (exp));

        {
          const GstSegment *recvseg, *expectseg;

          /* Compare segment values */
          gst_event_parse_segment (ev, &recvseg);
          gst_event_parse_segment (exp, &expectseg);

          fail_unless_equals_int (recvseg->format, expectseg->format);
          fail_unless_equals_uint64 (recvseg->offset, expectseg->offset);
          fail_unless_equals_uint64 (recvseg->start, expectseg->start);
          fail_unless_equals_uint64 (recvseg->stop, expectseg->stop);
          fail_unless_equals_uint64 (recvseg->time, expectseg->time);
        }

        gst_event_unref (exp);
        g_list_free1 (*expected);
        *expected = next;
      }
        break;
      case GST_EVENT_EOS:
        fail_if (*expected == NULL,
            "appsrc pushed a EOS event but we didn't expect any");
        next = (*expected)->next;
        fail_unless (GST_IS_EVENT ((*expected)->data),
            "appsrc pushed a EOS event but we expected any others");
        exp = GST_EVENT ((*expected)->data);
        fail_unless (GST_EVENT_TYPE (ev) == GST_EVENT_TYPE (exp),
            "Got event of type %s but expected event was %s",
            GST_EVENT_TYPE_NAME (ev), GST_EVENT_TYPE_NAME (exp));

        gst_event_unref (exp);
        g_list_free1 (*expected);
        *expected = next;
        break;
      case GST_EVENT_CAPS:
      {
        GstCaps *caps;
        fail_if (*expected == NULL,
            "appsrc pushed a CAPS event but we didn't expect any");
        next = (*expected)->next;
        fail_unless (GST_IS_EVENT ((*expected)->data),
            "appsrc pushed a CAPS event but we expected any others");
        exp = GST_EVENT ((*expected)->data);
        fail_unless (GST_EVENT_TYPE (ev) == GST_EVENT_TYPE (exp),
            "Got event of type %s but expected event was %s",
            GST_EVENT_TYPE_NAME (ev), GST_EVENT_TYPE_NAME (exp));
        gst_event_parse_caps (ev, &caps);
        GST_DEBUG ("caps set to  : %" GST_PTR_FORMAT, caps);

        gst_event_unref (exp);
        g_list_free1 (*expected);
        *expected = next;
        break;
      }

      default:
        break;
    }
  } else if (GST_IS_BUFFER (GST_PAD_PROBE_INFO_DATA (info))) {
    GstBuffer *recvbuf = GST_PAD_PROBE_INFO_BUFFER (info);
    GST_DEBUG ("Got buffer");
    fail_if (*expected == NULL,
        "appsrc pushed a buffer but we didn't expect any");
    next = (*expected)->next;
    fail_unless (GST_IS_BUFFER ((*expected)->data),
        "appsrc pushed a buffer but we expected that it's not a event");

    exp_buf = GST_BUFFER ((*expected)->data);
    fail_unless_equals_uint64 (GST_BUFFER_PTS (recvbuf),
        GST_BUFFER_PTS (exp_buf));
    fail_unless_equals_uint64 (GST_BUFFER_DTS (recvbuf),
        GST_BUFFER_DTS (exp_buf));
    fail_unless_equals_uint64 (GST_BUFFER_DURATION (recvbuf),
        GST_BUFFER_DURATION (exp_buf));

    g_list_free1 (*expected);
    *expected = next;
  }

  return GST_PAD_PROBE_OK;
}

/* Assuming application driven streaming with multiple period.
 * application provides custom segment per each period */
GST_START_TEST (test_appsrc_period_with_custom_segment)
{
  GstElement *pipe, *src, *sink;
  GstMessage *msg;
  gint i, j, period, tc;
  GstAppSrcCallbacks cb = { 0 };
  GstAppStreamType modes[] = { GST_APP_STREAM_TYPE_STREAM,
    GST_APP_STREAM_TYPE_SEEKABLE
  };
  GstFormat formats_tc[] = { GST_FORMAT_TIME, GST_FORMAT_BYTES };
  GstSegment *segment;
  GstSample *sample;
  GstBuffer *buffer;
  GstClockTime period_duration = 5 * GST_SECOND;
  GstEvent *event;
  gulong probe_id;
  GstPad *pad;
  GList *expected = NULL;

  for (tc = 0; tc < 2; tc++) {
    /* Case 0: two period with FORMAT_BYTES
     * Case 1: two period with FORMAT_TIME */

    GST_INFO ("Test Case #%d", tc);

    for (i = 0; i < G_N_ELEMENTS (modes); i++) {
      /* mode 0: stream-type == GST_APP_STREAM_TYPE_STREAM
       * mode 1: stream-type == GST_APP_STREAM_TYPE_SEEKABLE */

      GST_INFO ("checking mode %d", modes[i]);

      pipe = gst_pipeline_new ("pipeline");
      src = gst_element_factory_make ("appsrc", NULL);
      sink = gst_element_factory_make ("fakesink", NULL);
      gst_bin_add_many (GST_BIN (pipe), src, sink, NULL);
      fail_unless (gst_element_link (src, sink) == TRUE);
      pad = gst_element_get_static_pad (sink, "sink");

      probe_id = gst_pad_add_probe (pad,
          GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
          (GstPadProbeCallback) appsrc_pad_probe, &expected, NULL);

      g_object_set (G_OBJECT (src), "stream-type", modes[i], "format",
          formats_tc[tc], NULL);

      if (modes[i] != GST_APP_STREAM_TYPE_STREAM) {
        cb.seek_data = seek_cb;
        gst_app_src_set_callbacks (GST_APP_SRC (src), &cb, NULL, NULL);
      }

      ASSERT_SET_STATE (pipe, GST_STATE_PLAYING, GST_STATE_CHANGE_ASYNC);

      /* 2 periods exits */
      for (period = 0; period < 2; period++) {
        /* Total presentation timeline is form 0 sec to 10 sec
         * - Each period's first PTS is 1 sec and last PTS is 5 sec
         * - First period has presentation timeline with 0 ~ 5
         * - Last period has presentation timeline with 5 ~ 10
         */

        /* PREPARE SEGMENT */
        segment = gst_segment_new ();
        gst_segment_init (segment, formats_tc[tc]);
        segment->start = segment->position = GST_SECOND;
        segment->time = period * period_duration;

        /* PREPARE BUFFER */
        buffer = gst_buffer_new_and_alloc (4);
        GST_BUFFER_DTS (buffer) = GST_BUFFER_PTS (buffer) = GST_SECOND;
        GST_BUFFER_DURATION (buffer) = GST_SECOND;

        /* PREPARE SAMPLE */
        sample = gst_sample_new (buffer, NULL, segment, NULL);

        expected = g_list_append (expected, gst_event_new_segment (segment));
        expected = g_list_append (expected, buffer);

        /* 1st sample includes buffer and segment */
        fail_unless (gst_app_src_push_sample (GST_APP_SRC (src), sample)
            == GST_FLOW_OK);

        /* CLEAN UP */
        gst_segment_free (segment);
        gst_buffer_unref (buffer);
        gst_sample_unref (sample);

        /* Push the left buffers in the current period */
        for (j = 2; j <= 5; j++) {
          buffer = gst_buffer_new_and_alloc (4);
          GST_BUFFER_DTS (buffer) = GST_BUFFER_PTS (buffer) = j * GST_SECOND;
          GST_BUFFER_DURATION (buffer) = GST_SECOND;
          expected = g_list_append (expected, buffer);
          fail_unless (gst_app_src_push_buffer (GST_APP_SRC (src), buffer)
              == GST_FLOW_OK);
        }
      }

      if (modes[i] != GST_APP_STREAM_TYPE_STREAM) {
        /* Client request seek to 7 sec position (which belongs to 2nd period)
         * Application must provides corresponding buffer (of 2nd period) with
         * new custom segment */

        GstClockTime requested_pos = 7 * GST_SECOND;
        /* In this test case, we are checking the serialized order of
         * events and buffers, so, give some time to the appsrc loop to
         * push all to sink */
        g_usleep (G_USEC_PER_SEC * 1);

        GST_DEBUG ("Seek to %" GST_TIME_FORMAT, GST_TIME_ARGS (requested_pos));
        event = gst_event_new_seek (1.0, formats_tc[tc], GST_SEEK_FLAG_FLUSH,
            GST_SEEK_TYPE_SET, requested_pos, GST_SEEK_TYPE_NONE, -1);
        fail_unless (gst_element_send_event (pipe, event) == TRUE);

        /* PREPARE SEGMENT */
        segment = gst_segment_new ();
        gst_segment_init (segment, formats_tc[tc]);
        segment->start = segment->position = 3 * GST_SECOND;
        segment->time = requested_pos;

        /* PREPARE BUFFER */
        buffer = gst_buffer_new_and_alloc (4);
        GST_BUFFER_DTS (buffer) = GST_BUFFER_PTS (buffer) = 3 * GST_SECOND;

        /* PREPARE SAMPLE */
        sample = gst_sample_new (buffer, NULL, segment, NULL);

        expected = g_list_append (expected, gst_event_new_segment (segment));
        expected = g_list_append (expected, buffer);

        /* 1st sample includes buffer and segment */
        fail_unless (gst_app_src_push_sample (GST_APP_SRC (src), sample)
            == GST_FLOW_OK);

        /* CLEAN UP */
        gst_segment_free (segment);
        gst_buffer_unref (buffer);
        gst_sample_unref (sample);

        /* Push the left buffers in the current period */
        for (j = 4; j <= 5; j++) {
          buffer = gst_buffer_new_and_alloc (4);
          GST_BUFFER_DTS (buffer) = GST_BUFFER_PTS (buffer) = j * GST_SECOND;
          GST_BUFFER_DURATION (buffer) = GST_SECOND;
          expected = g_list_append (expected, buffer);
          fail_unless (gst_app_src_push_buffer (GST_APP_SRC (src), buffer)
              == GST_FLOW_OK);
        }
      }

      expected = g_list_append (expected, gst_event_new_eos ());
      fail_unless (gst_app_src_end_of_stream (GST_APP_SRC (src)) ==
          GST_FLOW_OK);

      msg =
          gst_bus_timed_pop_filtered (GST_ELEMENT_BUS (pipe), -1,
          GST_MESSAGE_EOS);
      fail_unless (msg);
      gst_message_unref (msg);

      gst_pad_remove_probe (pad, probe_id);
      gst_object_unref (pad);

      ASSERT_SET_STATE (pipe, GST_STATE_NULL, GST_STATE_CHANGE_SUCCESS);
      gst_object_unref (pipe);
      fail_if (expected != NULL);
    }
  }
}

GST_END_TEST;


GST_START_TEST (test_appsrc_custom_segment_twice)
{
  GstElement *pipe, *src, *sink;
  GstMessage *msg;
  gint i, tc;
  GstAppSrcCallbacks cb = { 0 };
  GstAppStreamType modes[] = { GST_APP_STREAM_TYPE_STREAM,
    GST_APP_STREAM_TYPE_SEEKABLE
  };
  GstSample *sample;
  gulong probe_id;
  GstPad *pad;
  GList *expected = NULL;
  GstSegment *segment;
  GstBuffer *buffer;

  for (tc = 0; tc < 4; tc++) {
    /* Case 0: Push segment1 without buffer,
     * then push segment1 with buffer again.
     * Expected behaviour is that pushing segment only once to downstream */

    /* Case 1: Push segment1 with buffer,
     * then push segment1 with buffer again.
     * Expected behaviour is that pushing segment only once to downstream */

    /* Case 2: Push segment1 without buffer,
     * then push segment2 with buffer.
     * Expected behaviour is that pushing only segment2 with buffer
     * to downstream */

    /* Case 3: Push segment1 with buffer,
     * then push segment2 with buffer.
     * Expected behaviour is that pushing segment1 with buffer,
     * and then segment2 with buffer */

    GST_INFO ("Test Case #%d", tc);

    for (i = 0; i < G_N_ELEMENTS (modes); i++) {
      GST_INFO ("checking mode %d", modes[i]);

      pipe = gst_pipeline_new ("pipeline");
      src = gst_element_factory_make ("appsrc", NULL);
      sink = gst_element_factory_make ("fakesink", NULL);
      gst_bin_add_many (GST_BIN (pipe), src, sink, NULL);
      fail_unless (gst_element_link (src, sink));

      pad = gst_element_get_static_pad (sink, "sink");

      probe_id = gst_pad_add_probe (pad,
          GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
          (GstPadProbeCallback) appsrc_pad_probe, &expected, NULL);

      g_object_set (G_OBJECT (src), "stream-type", modes[i], "format",
          GST_FORMAT_TIME, NULL);

      if (modes[i] != GST_APP_STREAM_TYPE_STREAM) {
        cb.seek_data = seek_cb;
        gst_app_src_set_callbacks (GST_APP_SRC (src), &cb, NULL, NULL);
      }

      ASSERT_SET_STATE (pipe, GST_STATE_PLAYING, GST_STATE_CHANGE_ASYNC);

      GST_DEBUG ("Prepare/Push the first sample");
      /* PREPARE SEGMENT */
      segment = gst_segment_new ();
      gst_segment_init (segment, GST_FORMAT_TIME);
      segment->start = segment->position = segment->time = GST_SECOND;

      /* PREPARE BUFFER */
      buffer = gst_buffer_new_and_alloc (4);
      GST_BUFFER_DTS (buffer) = GST_BUFFER_PTS (buffer) = GST_SECOND;
      GST_BUFFER_DURATION (buffer) = GST_SECOND;

      /* PREPARE FIRST SAMPLE */
      if (tc == 0) {            /* Test Case 0: Push a sample without buffer */
        sample = gst_sample_new (NULL, NULL, segment, NULL);
        expected = g_list_append (expected, gst_event_new_segment (segment));
      } else if (tc == 2) {
        /* Test Case 2: Push a sample without buffer.
         * We don't expect this segment will be used,
         * because the updated next sample will be actually used */
        sample = gst_sample_new (NULL, NULL, segment, NULL);
      } else {
        sample = gst_sample_new (buffer, NULL, segment, NULL);
        expected = g_list_append (expected, gst_event_new_segment (segment));
        expected = g_list_append (expected, buffer);
      }

      /* PUSH THE FIRST SAMPLE */
      fail_unless (gst_app_src_push_sample (GST_APP_SRC (src), sample)
          == GST_FLOW_OK);

      /* CLEAN UP */
      gst_segment_free (segment);
      gst_buffer_unref (buffer);
      gst_sample_unref (sample);

      GST_DEBUG ("Prepare/Push the last sample");
      /* PREPARE SEGMENT */
      segment = gst_segment_new ();
      gst_segment_init (segment, GST_FORMAT_TIME);
      segment->start = segment->position = segment->time =
          (tc == 0 || tc == 1) ? 1 * GST_SECOND : 2 * GST_SECOND;

      /* PREPARE BUFFER */
      buffer = gst_buffer_new_and_alloc (4);
      GST_BUFFER_DTS (buffer) = GST_BUFFER_PTS (buffer) = 2 * GST_SECOND;
      GST_BUFFER_DURATION (buffer) = GST_SECOND;

      /* PREPARE THE LAST SAMPLE */
      if (tc == 0 || tc == 1) {
        /* Test Case 0 or 1: Push a sample with duplicated segment */
        sample = gst_sample_new (buffer, NULL, segment, NULL);
        expected = g_list_append (expected, buffer);
      } else {
        sample = gst_sample_new (buffer, NULL, segment, NULL);
        expected = g_list_append (expected, gst_event_new_segment (segment));
        expected = g_list_append (expected, buffer);
      }
      fail_unless (gst_app_src_push_sample (GST_APP_SRC (src), sample)
          == GST_FLOW_OK);

      /* CLEAN UP */
      gst_segment_free (segment);
      gst_buffer_unref (buffer);
      gst_sample_unref (sample);

      expected = g_list_append (expected, gst_event_new_eos ());
      fail_unless (gst_app_src_end_of_stream (GST_APP_SRC (src)) ==
          GST_FLOW_OK);

      msg =
          gst_bus_timed_pop_filtered (GST_ELEMENT_BUS (pipe), -1,
          GST_MESSAGE_EOS);
      fail_unless (msg);
      gst_message_unref (msg);

      gst_pad_remove_probe (pad, probe_id);
      gst_object_unref (pad);

      ASSERT_SET_STATE (pipe, GST_STATE_NULL, GST_STATE_CHANGE_SUCCESS);
      gst_object_unref (pipe);
      fail_if (expected != NULL);
    }
  }
}

GST_END_TEST;


GST_START_TEST (test_appsrc_invalid_custom_segment)
{
  GstElement *pipe, *src, *sink;
  GstMessage *msg;
  gint i, tc;
  GstMessageType msg_types;
  GstAppSrcCallbacks cb = { 0 };
  GstAppStreamType modes[] = { GST_APP_STREAM_TYPE_STREAM,
    GST_APP_STREAM_TYPE_SEEKABLE,
    GST_APP_STREAM_TYPE_RANDOM_ACCESS
  };
  GstSegment *segment;
  GstSample *sample;
  GstCaps *caps;
  gulong probe_id;
  GstPad *pad;
  GList *expected = NULL;
  GstBuffer *buffer;
  GstFormat seg_format, appsrc_format;

  /* format of custom segment in a GstSample should be matched to that of appsrc.
   * if not, all the members of the sample will be refused with return of
   * GST_FLOW_ERROR. Note that, if user didn't set segment into GstSample,
   * default segment format will be GST_FORMAT_UNDEFINED. appsrc will not
   * accept it, but just ignore it without error.
   */
  for (tc = 0; tc < 2; tc++) {
    /* in each test case, two samples will be pushed with following conditions.
     * depending on condition, the first sample can be invalid,
     * but it will be followed by valid sample.
     * appsrc output should have valid one or nothing, depending on what we pushed
     *
     * - Case 0: appsrc format = BYTES, segment format = TIME
     * - Case 1: appsrc format = TIME, segment format = BYTES
     */

    switch (tc) {
      case 0:
        seg_format = GST_FORMAT_TIME;
        appsrc_format = GST_FORMAT_BYTES;
        break;
      case 1:
        seg_format = GST_FORMAT_BYTES;
        appsrc_format = GST_FORMAT_TIME;
        break;
      default:
        break;
    }
    GST_INFO ("Case #%d", tc);

    for (i = 0; i < G_N_ELEMENTS (modes); i++) {
      GST_INFO ("checking mode %d", modes[i]);

      pipe = gst_pipeline_new ("pipeline");
      src = gst_element_factory_make ("appsrc", NULL);
      sink = gst_element_factory_make ("fakesink", NULL);
      gst_bin_add_many (GST_BIN (pipe), src, sink, NULL);
      fail_unless (gst_element_link (src, sink));

      pad = gst_element_get_static_pad (sink, "sink");

      probe_id = gst_pad_add_probe (pad,
          GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
          (GstPadProbeCallback) appsrc_pad_probe, &expected, NULL);

      g_object_set (G_OBJECT (src), "stream-type", modes[i], "format",
          appsrc_format, NULL);

      if (modes[i] != GST_APP_STREAM_TYPE_STREAM) {
        cb.seek_data = seek_cb;
        gst_app_src_set_callbacks (GST_APP_SRC (src), &cb, NULL, NULL);
      }

      ASSERT_SET_STATE (pipe, GST_STATE_PLAYING, GST_STATE_CHANGE_ASYNC);

      GST_DEBUG ("Prepare/Push the first sample");
      /* PREPARE CAPS */
      caps = gst_caps_new_simple ("foo/bar", "order", G_TYPE_INT, 0, NULL);

      /* PREPARE SEGMENT */
      segment = gst_segment_new ();
      gst_segment_init (segment, seg_format);
      segment->start = segment->position = segment->time = GST_SECOND;

      /* PREPARE BUFFER */
      buffer = gst_buffer_new_and_alloc (4);
      GST_BUFFER_DTS (buffer) = GST_BUFFER_PTS (buffer) = GST_SECOND;
      GST_BUFFER_DURATION (buffer) = GST_SECOND;

      /* PREPARE FIRST SAMPLE */
      sample = gst_sample_new (buffer, caps, segment, NULL);

      /* if a sample includes invalid segment, appsrc will refuse the sample
       * (i.e., buffer and segment) with return of GST_FLOW_ERROR */

      fail_unless (gst_app_src_push_sample (GST_APP_SRC (src), sample)
          == GST_FLOW_ERROR);

      /* CLEAN UP */
      gst_caps_unref (caps);
      gst_segment_free (segment);
      gst_buffer_unref (buffer);
      gst_sample_unref (sample);

      GST_DEBUG ("Prepare/Push valid sample again");
      /* PREPARE CAPS */
      caps = gst_caps_new_simple ("bar/foo", "order", G_TYPE_INT, 1, NULL);

      /* PREPARE SEGMENT */
      segment = gst_segment_new ();
      gst_segment_init (segment, appsrc_format);
      segment->start = segment->position = segment->time = 2 * GST_SECOND;

      /* PREPARE BUFFER */
      buffer = gst_buffer_new_and_alloc (4);
      GST_BUFFER_DTS (buffer) = GST_BUFFER_PTS (buffer) = 2 * GST_SECOND;
      GST_BUFFER_DURATION (buffer) = GST_SECOND;

      /* PREPARE LAST SAMPLE */
      sample = gst_sample_new (buffer, caps, segment, NULL);
      expected = g_list_append (expected, gst_event_new_caps (caps));
      expected = g_list_append (expected, gst_event_new_segment (segment));
      expected = g_list_append (expected, buffer);

      fail_unless (gst_app_src_push_sample (GST_APP_SRC (src), sample)
          == GST_FLOW_OK);

      /* CLEAN UP */
      gst_caps_unref (caps);
      gst_segment_free (segment);
      gst_buffer_unref (buffer);
      gst_sample_unref (sample);

      if (modes[i] != GST_APP_STREAM_TYPE_RANDOM_ACCESS) {
        expected = g_list_append (expected, gst_event_new_eos ());
        fail_unless (gst_app_src_end_of_stream (GST_APP_SRC (src)) ==
            GST_FLOW_OK);
        msg_types = GST_MESSAGE_EOS;
      } else {
        msg_types = GST_MESSAGE_ASYNC_DONE;
      }

      msg = gst_bus_timed_pop_filtered (GST_ELEMENT_BUS (pipe), -1, msg_types);
      fail_unless (msg);
      gst_message_unref (msg);

      gst_pad_remove_probe (pad, probe_id);
      gst_object_unref (pad);

      ASSERT_SET_STATE (pipe, GST_STATE_NULL, GST_STATE_CHANGE_SUCCESS);
      gst_object_unref (pipe);
      fail_if (expected != NULL);
    }
  }
}

GST_END_TEST;

static Suite *
appsrc_suite (void)
{
  Suite *s = suite_create ("appsrc");
  TCase *tc_chain = tcase_create ("general");

  tcase_add_test (tc_chain, test_appsrc_non_null_caps);
  tcase_add_test (tc_chain, test_appsrc_set_caps_twice);
  tcase_add_test (tc_chain, test_appsrc_caps_in_push_modes);
  tcase_add_test (tc_chain, test_appsrc_blocked_on_caps);
  tcase_add_test (tc_chain, test_appsrc_push_buffer_list);
  tcase_add_test (tc_chain, test_appsrc_period_with_custom_segment);
  tcase_add_test (tc_chain, test_appsrc_custom_segment_twice);
  tcase_add_test (tc_chain, test_appsrc_invalid_custom_segment);

  if (RUNNING_ON_VALGRIND)
    tcase_add_loop_test (tc_chain, test_appsrc_block_deadlock, 0, 5);
  else
    tcase_add_loop_test (tc_chain, test_appsrc_block_deadlock, 0, 100);

  suite_add_tcase (s, tc_chain);

  return s;
}

GST_CHECK_MAIN (appsrc);
