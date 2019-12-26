/* GStreamer
 *
 * Copyright (C) <2018> HoonHee Lee <hoonhee.lee@lge.com>
 *                      DongYun Seo <dongyun.seo@lge.com>
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

static void priv_play_bin3_init (GstPlayBin3 * playbin);
static void priv_play_bin3_finalize (GstPlayBin3 * playbin);
static void priv_play_bin3_handle_message (GstBin * bin, GstMessage * msg,
    gboolean * steal);
static gboolean priv_play_bin3_send_event (GstPlayBin3 * playbin,
    GstEvent * event, gboolean * steal);
static void priv_play_bin3_deep_element_added (GstBin * bin, GstBin * sub_bin,
    GstElement * child);
static void priv_play_bin3_deep_element_removed (GstBin * bin,
    GstBin * sub_bin, GstElement * child);
static void priv_init_group (GstPlayBin3 * playbin, GstSourceGroup * group);
static void priv_free_group (GstPlayBin3 * playbin, GstSourceGroup * group);
static void priv_query_smart_properties (GstPlayBin3 * playbin,
    GstSourceGroup * group);

static void
priv_play_bin3_init (GstPlayBin3 * playbin)
{
  GST_LOG_OBJECT (playbin, "Initialized");
}

static void
priv_play_bin3_finalize (GstPlayBin3 * playbin)
{
  GST_LOG_OBJECT (playbin, "Releasing ...");

  if (playbin->mq)
    gst_object_unref (playbin->mq);
  if (playbin->decodebin)
    gst_object_unref (playbin->decodebin);

  GST_LOG_OBJECT (playbin, "Released");
}

#ifndef GST_DISABLE_GST_DEBUG
#define tv_debug_groups(playbin) G_STMT_START {	\
    guint i;					\
    						\
    for (i = 0; i < 2; i++) {				\
      GstSourceGroup *group = &playbin->groups[i];	\
      							\
      GST_DEBUG ("GstSourceGroup #%d (%s)", i, (group == playbin->curr_group) ? "current" : (group == playbin->next_group) ? "next" : "unused"); \
      GST_DEBUG ("  valid:%d , active:%d , playing:%d", group->valid, group->active, group->playing); \
      GST_DEBUG ("  uri:%s", group->uri);				\
      GST_DEBUG ("  timeout-id:%d", (guint) group->timeout_id);	\
      GST_DEBUG ("  gap-seqnum:%d", (guint) group->gap_seqnum);	\
    }								\
  } G_STMT_END
#else
#define tv_debug_groups(p) {}
#endif

static void
priv_query_smart_properties (GstPlayBin3 * playbin, GstSourceGroup * group)
{
  GstSmartPropertiesReturn ret;
  ret =
      gst_element_get_smart_properties (GST_ELEMENT_CAST (playbin->decodebin),
      "use-fallback-preroll", &group->use_fallback_preroll,
      "adaptive-mode", &group->adaptive_mode,
      "dvr-playback", &group->dvr_playback, NULL);

  GST_DEBUG_OBJECT (playbin,
      "response of custom query: [%d], use-fallback-preroll = [%d] adaptive-mode = [%d], dvr-playback = [%d]",
      ret, group->use_fallback_preroll, group->adaptive_mode,
      group->dvr_playback);
  if (group->dvr_playback) {
    GST_DEBUG_OBJECT (playbin, "digital video recoding playback mode");
    gst_play_sink_set_dvr_playback (playbin->playsink, TRUE);
  }
}

static gboolean
check_backend_needs_gap (gpointer data)
{
  GstPlayBin3 *playbin = (GstPlayBin3 *) data;
  GstSourceGroup *target_group;
  GList *tmp;
  guint32 curgap_seqnum = 0;

  g_return_val_if_fail (playbin, FALSE);

  GST_PLAY_BIN3_LOCK (playbin);

  target_group = get_group (playbin);
  if (!target_group) {
    GST_LOG_OBJECT (playbin, "No groups");
    GST_PLAY_BIN3_UNLOCK (playbin);
    return FALSE;
  }

  GST_SOURCE_GROUP_LOCK (target_group);

  if (target_group != playbin->curr_group || !target_group->playing) {
    GST_LOG_OBJECT (playbin, "No activated current group");
    tv_debug_groups (playbin);
    target_group->timeout_id = 0;
    GST_SOURCE_GROUP_UNLOCK (target_group);
    GST_PLAY_BIN3_UNLOCK (playbin);
    return FALSE;
  }

  curgap_seqnum = target_group->gap_seqnum;
  if (curgap_seqnum == 0) {
    GST_DEBUG_OBJECT (playbin, "Completed for last async state changing");
    target_group->timeout_id = 0;
    GST_SOURCE_GROUP_UNLOCK (target_group);
    GST_PLAY_BIN3_UNLOCK (playbin);
    return FALSE;
  }

  GST_DEBUG_OBJECT (playbin, "seqnum: %u", (guint) curgap_seqnum);

  for (tmp = target_group->source_pads; tmp; tmp = tmp->next) {
    SourcePad *res = (SourcePad *) tmp->data;
    GstElement *target_sink = NULL;
    GstPad *peer = NULL;
    GstEvent *seg_ev = NULL;
    GstStateChangeReturn last_state_result = GST_STATE_CHANGE_SUCCESS;
    gboolean unref = FALSE;

    if (res->stream_type != GST_STREAM_TYPE_AUDIO
        && res->stream_type != GST_STREAM_TYPE_VIDEO) {
      GST_LOG_OBJECT (playbin, "Stream type is neither AUDIO nor VIDEO");
      continue;
    }

    if (res->stream_type == GST_STREAM_TYPE_AUDIO) {
      if (target_group->audio_sink) {
        target_sink = target_group->audio_sink;
      } else {
        GST_PLAY_BIN3_UNLOCK (playbin);
        target_sink =
            gst_play_sink_get_sink (playbin->playsink,
            GST_PLAY_SINK_TYPE_AUDIO);
        unref = TRUE;
        GST_PLAY_BIN3_LOCK (playbin);
      }
    } else {
      if (target_group->video_sink) {
        target_sink = target_group->video_sink;
      } else {
        GST_PLAY_BIN3_UNLOCK (playbin);
        target_sink =
            gst_play_sink_get_sink (playbin->playsink,
            GST_PLAY_SINK_TYPE_VIDEO);
        unref = TRUE;
        GST_PLAY_BIN3_LOCK (playbin);
      }
    }

    if (!target_sink) {
      GST_LOG_OBJECT (playbin, "No corresponding sink for %s",
          gst_stream_type_get_name (res->stream_type));
      continue;
    }

    if (res->push_gap) {
      GST_LOG_OBJECT (res->pad, "GAP is traveling on target sink (%s)",
          GST_OBJECT_NAME (target_sink));
      if (unref)
        gst_object_unref (target_sink);
      continue;
    }

    if (res->is_eos) {
      GST_LOG_OBJECT (res->pad, "EOS is already marked in %s",
          GST_OBJECT_NAME (target_sink));
      if (unref)
        gst_object_unref (target_sink);
      continue;
    }

    peer = gst_pad_get_peer (res->pad);

    if (!peer) {
      GST_LOG_OBJECT (playbin, "No corresponding peerpad for %s",
          gst_stream_type_get_name (res->stream_type));
      if (unref)
        gst_object_unref (target_sink);
      continue;
    }

    last_state_result = GST_STATE_RETURN (target_sink);
    GST_FIXME_OBJECT (peer, "%s, last returned state(%s)",
        GST_OBJECT_NAME (target_sink),
        gst_element_state_change_return_get_name (last_state_result));
    if (last_state_result != GST_STATE_CHANGE_ASYNC) {
      GST_LOG_OBJECT (peer, "%s not in ASYNC STATUS",
          GST_OBJECT_NAME (target_sink));
      gst_object_unref (peer);
      if (unref)
        gst_object_unref (target_sink);
      continue;
    }

    if (unref)
      gst_object_unref (target_sink);

    GST_FIXME_OBJECT (peer,
        "Current status (active: %d, linked: %d, flushing: %d, eos: %d)",
        GST_PAD_IS_ACTIVE (peer), GST_PAD_IS_LINKED (peer),
        GST_PAD_IS_FLUSHING (peer), GST_PAD_IS_EOS (peer));

    seg_ev = gst_pad_get_sticky_event (peer, GST_EVENT_SEGMENT, 0);
    if (!seg_ev) {
      GST_WARNING_OBJECT (peer,
          "No segment before GAP, It causes jitter in basesink without segment.");
      gst_object_unref (peer);
      GST_SOURCE_GROUP_UNLOCK (target_group);
      GST_PLAY_BIN3_UNLOCK (playbin);
      return TRUE;
    } else {
      guint max_size_buffers = 0;
      guint max_size_bytes = 0;
      guint64 max_size_time = 0;
      guint playsink_type;
      const GstSegment *curseg;
      gst_event_parse_segment (seg_ev, &curseg);
      GST_LOG_OBJECT (peer, "Current segment: %" GST_SEGMENT_FORMAT, curseg);

      if (!res->mark_seg) {
        res->mark_seg = TRUE;
        gst_event_unref (seg_ev);
        gst_object_unref (peer);
        GST_FIXME_OBJECT (peer, "We hope preroll buffer is received soon");
        GST_SOURCE_GROUP_UNLOCK (target_group);
        GST_PLAY_BIN3_UNLOCK (playbin);
        return TRUE;
      }

      /* FIXME: GAP means there is no buffer during specified time. But, the purpopse of
       * this function here is to complete asynchronous state changes in exception cases
       * caused by system delayed or invalid stream or other issues. In order to avoid
       * deadlock problem during taking pad stream lock in gap event and chainfunc in a
       * different threads, we increase the max-size-buffers to video/audio queue before
       * pushing gap event. After that recover the old max-size-buffers. */
      playsink_type =
          res->stream_type ==
          GST_STREAM_TYPE_VIDEO ? GST_PLAY_SINK_TYPE_VIDEO :
          GST_PLAY_SINK_TYPE_AUDIO;
      max_size_buffers =
          gst_play_sink_get_queue_max_size_buffers (playbin->playsink,
          playsink_type, &max_size_bytes, &max_size_time);

      if (max_size_buffers) {
        GST_DEBUG_OBJECT (peer,
            "Forced to increase %s queue max-size-buffers to %d",
            playsink_type == GST_PLAY_SINK_TYPE_VIDEO ? "video" : "audio",
            max_size_buffers + 1);
        gst_play_sink_set_queue_max_size_buffers (playbin->playsink,
            playsink_type, max_size_buffers + 1, 0, 0, TRUE);
      }

      res->push_gap = TRUE;
      GST_SOURCE_GROUP_UNLOCK (target_group);
      GST_PLAY_BIN3_UNLOCK (playbin);
      GST_SYS_FIXME_OBJECT (peer, "Sending GAP !!");
      /* FIXME: Do we need exactly start and duration for gap ?? */
      if (!gst_pad_send_event (peer, gst_event_new_gap (0,
                  GST_CLOCK_TIME_NONE))) {
        GST_PLAY_BIN3_LOCK (playbin);
        GST_SOURCE_GROUP_LOCK (target_group);
        GST_SYS_WARNING_OBJECT (peer, "Failed Gap event. Try again.");

        if (max_size_buffers)
          gst_play_sink_set_queue_max_size_buffers (playbin->playsink,
              playsink_type, max_size_buffers, max_size_bytes, max_size_time,
              FALSE);

        res->push_gap = FALSE;
        res->mark_seg = FALSE;
        gst_event_unref (seg_ev);
        gst_object_unref (peer);
        GST_SOURCE_GROUP_UNLOCK (target_group);
        GST_PLAY_BIN3_UNLOCK (playbin);
        return TRUE;
      }

      GST_PLAY_BIN3_LOCK (playbin);
      GST_SOURCE_GROUP_LOCK (target_group);
      GST_SYS_FIXME_OBJECT (peer, "Sent GAP !!");

      if (max_size_buffers) {
        GST_DEBUG_OBJECT (peer, "Recover %s queue max-size-buffers to %d",
            playsink_type == GST_STREAM_TYPE_VIDEO ? "video" : "audio",
            max_size_buffers);
        gst_play_sink_set_queue_max_size_buffers (playbin->playsink,
            playsink_type, max_size_buffers, max_size_bytes, max_size_time,
            FALSE);
      }
      gst_event_unref (seg_ev);
    }
    gst_object_unref (peer);
  }
  target_group->gap_seqnum = 0;
  target_group->timeout_id = 0;

  GST_SOURCE_GROUP_UNLOCK (target_group);
  GST_PLAY_BIN3_UNLOCK (playbin);

  return FALSE;
}

/* Call with GST_PLAY_BIN3_LOCK taken */
static void
add_timer_to_finish_async_status (GstPlayBin3 * playbin, GstMessage * msg,
    GstElement * asink, GstElement * vsink)
{
  GstSourceGroup *target_group;
  GstElement *g_asink, *g_vsink;
  GstStateChangeReturn ret_asink = GST_STATE_CHANGE_SUCCESS, ret_vsink =
      GST_STATE_CHANGE_SUCCESS;
  guint32 curgap_seqnum = 0;

  GST_LOG_OBJECT (playbin, "Checking ASYNC");

  target_group = get_group (playbin);
  if (!target_group) {
    GST_FIXME_OBJECT (playbin, "No groups");
    return;
  }

  GST_SOURCE_GROUP_LOCK (target_group);
  if (target_group != playbin->curr_group || !target_group->playing) {
    GST_LOG_OBJECT (playbin, "No activated current group");
    tv_debug_groups (playbin);
    GST_SOURCE_GROUP_UNLOCK (target_group);
    return;
  }
  GST_SOURCE_GROUP_UNLOCK (target_group);

  g_asink = asink;
  g_vsink = vsink;

  if (g_asink)
    ret_asink = GST_STATE_RETURN (g_asink);

  if (g_vsink)
    ret_vsink = GST_STATE_RETURN (g_vsink);

  if (GST_STATE_RETURN (playbin->playsink) == GST_STATE_CHANGE_ASYNC
      && (ret_asink == GST_STATE_CHANGE_ASYNC
          || ret_vsink == GST_STATE_CHANGE_ASYNC)) {
    guint32 cur_gap_seqnum = 0;

    GST_DEBUG_OBJECT (playbin, "From message(%s:%u)",
        GST_MESSAGE_TYPE_NAME (msg), (guint) GST_MESSAGE_SEQNUM (msg));
    GST_DEBUG_OBJECT (playbin, "  audio-sink(%s), last returned state(%s)",
        (g_asink ? GST_OBJECT_NAME (g_asink) : "<NONE>"),
        (g_asink ? gst_element_state_change_return_get_name (ret_asink) :
            "<NONE>"));
    GST_DEBUG_OBJECT (playbin, "  video-sink(%s), last returned state(%s)",
        (g_vsink ? GST_OBJECT_NAME (g_vsink) : "<NONE>"),
        (g_vsink ? gst_element_state_change_return_get_name (ret_vsink) :
            "<NONE>"));

    GST_SOURCE_GROUP_LOCK (target_group);
    cur_gap_seqnum = target_group->gap_seqnum;
    if (cur_gap_seqnum > 0 && gst_message_get_seqnum (msg) != cur_gap_seqnum) {
      GST_LOG_OBJECT (playbin,
          "  New Asynchronous message in the meantime (timeout-id: %d)",
          target_group->timeout_id);
    } else {
      target_group->gap_seqnum = gst_message_get_seqnum (msg);
      target_group->timeout_id =
          g_timeout_add (250, &check_backend_needs_gap, playbin);
      GST_FIXME_OBJECT (playbin,
          "  Timer(id:%d) will start after 250ms (seqmum:%u)",
          (guint) target_group->timeout_id, (guint) target_group->gap_seqnum);
    }
    GST_SOURCE_GROUP_UNLOCK (target_group);
  }
}

static void
get_current_sinks (GstPlayBin3 * playbin, GstElement ** asink,
    GstElement ** vsink)
{
  *asink = gst_play_sink_get_sink (playbin->playsink, GST_PLAY_SINK_TYPE_AUDIO);
  *vsink = gst_play_sink_get_sink (playbin->playsink, GST_PLAY_SINK_TYPE_VIDEO);
}

static gboolean
message_owner_is_sink (GstPlayBin3 * playbin, GstElement * element)
{
  gboolean is_sink = FALSE;
  GstElement *g_asink, *g_vsink;
  gboolean unref_audio = FALSE;
  gboolean unref_video = FALSE;

  get_current_sinks (playbin, &g_asink, &g_vsink);

  if (element == g_asink || element == g_vsink) {
    GST_LOG_OBJECT (playbin, "Found Sink(%s) element",
        GST_OBJECT_NAME (element));
    is_sink = TRUE;
  }

  if (g_asink)
    gst_object_unref (g_asink);
  if (g_vsink)
    gst_object_unref (g_vsink);

  return is_sink;
}

static void
priv_play_bin3_handle_message (GstBin * bin, GstMessage * msg, gboolean * steal)
{
  GstPlayBin3 *playbin = GST_PLAY_BIN3 (bin);

  switch (GST_MESSAGE_TYPE (msg)) {
    case GST_MESSAGE_STATE_CHANGED:{
      GstState state;
      GstElement *g_asink = NULL, *g_vsink = NULL;
      GstSourceGroup *target_group = NULL;
      gboolean unref_audio = FALSE;
      gboolean unref_video = FALSE;

      if (GST_MESSAGE_SRC (msg) != playbin->mq
          && !message_owner_is_sink (playbin, GST_MESSAGE_SRC (msg)))
        break;

      GST_LOG_OBJECT (playbin, "Got state change %" GST_PTR_FORMAT, msg);

      GST_PLAY_BIN3_LOCK (playbin);
      *steal = TRUE;

      /* FIXME: Fire a signal to bumping max size of multiqueue */
      if (GST_MESSAGE_SRC (msg) == playbin->mq) {
        GstState state;
        gst_message_parse_state_changed (msg, NULL, &state, NULL);

        if (state == GST_STATE_PAUSED) {
          GST_FIXME_OBJECT (playbin,
              "  [STATE CHANGE] Activate preroll state to %s",
              GST_OBJECT_NAME (playbin->mq));
          g_signal_emit_by_name (playbin->mq, "preroll-state", TRUE, NULL);
        } else if (state == GST_STATE_PLAYING) {
          GST_FIXME_OBJECT (playbin,
              "  [STATE CHANGE] Deactivate preroll state to %s",
              GST_OBJECT_NAME (playbin->mq));
          g_signal_emit_by_name (playbin->mq, "preroll-state", FALSE, NULL);
        }
        GST_PLAY_BIN3_UNLOCK (playbin);
        break;
      }

      target_group = get_group (playbin);

      if (!target_group) {
        GST_LOG_OBJECT (playbin, "No current group");
        GST_PLAY_BIN3_UNLOCK (playbin);
        break;
      }

      if (!target_group->use_fallback_preroll) {
        GST_PLAY_BIN3_UNLOCK (playbin);
        break;
      }

      if (!target_group->playing) {
        GST_LOG_OBJECT (playbin, "Not in playing");
        GST_PLAY_BIN3_UNLOCK (playbin);
        break;
      }

      get_current_sinks (playbin, &g_asink, &g_vsink);

      GST_SOURCE_GROUP_LOCK (target_group);
      if (GST_STATE_RETURN (playbin->playsink) != GST_STATE_CHANGE_ASYNC) {
        GST_LOG_OBJECT (playbin, "playsink is not in async");
        goto unref;
      }

      GST_SOURCE_GROUP_UNLOCK (target_group);
      add_timer_to_finish_async_status (playbin, msg, g_asink, g_vsink);
      GST_SOURCE_GROUP_LOCK (target_group);

    unref:
      if (g_asink)
        gst_object_unref (g_asink);
      if (g_vsink)
        gst_object_unref (g_vsink);
      GST_SOURCE_GROUP_UNLOCK (target_group);
      GST_PLAY_BIN3_UNLOCK (playbin);
      break;
    }
    case GST_MESSAGE_ASYNC_START:{
      GstSourceGroup *target_group;

      if (GST_MESSAGE_SRC (msg) != playbin->playsink)
        break;

      GST_DEBUG_OBJECT (playbin, "Got ASYNC-START (%s)",
          GST_OBJECT_NAME (GST_MESSAGE_SRC (msg)));

      GST_PLAY_BIN3_LOCK (playbin);
      *steal = TRUE;

      /* FIXME: Fire a signal to bumping max size of multiqueue */
      if (playbin->mq) {
        GST_FIXME_OBJECT (playbin,
            "  [ASYNC START] Activate preroll state to %s",
            GST_OBJECT_NAME (playbin->mq));
        g_signal_emit_by_name (playbin->mq, "preroll-state", TRUE, NULL);
      }

      target_group = get_group (playbin);
      if (!target_group) {
        GST_LOG_OBJECT (playbin, "No current group");
        GST_PLAY_BIN3_UNLOCK (playbin);
        break;
      }

      if (!target_group->use_fallback_preroll) {
        GST_PLAY_BIN3_UNLOCK (playbin);
        break;
      }

      GST_SOURCE_GROUP_LOCK (target_group);
      if (GST_STATE_RETURN (playbin->playsink) == GST_STATE_CHANGE_ASYNC) {
        GstElement *g_asink, *g_vsink;
        gboolean unref_audio = FALSE;
        gboolean unref_video = FALSE;

        GST_SOURCE_GROUP_UNLOCK (target_group);
        get_current_sinks (playbin, &g_asink, &g_vsink);
        add_timer_to_finish_async_status (playbin, msg, g_asink, g_vsink);
        GST_SOURCE_GROUP_LOCK (target_group);
        if (g_asink)
          gst_object_unref (g_asink);
        if (g_vsink)
          gst_object_unref (g_vsink);
      }
      GST_SOURCE_GROUP_UNLOCK (target_group);
      GST_PLAY_BIN3_UNLOCK (playbin);
      break;
    }
    case GST_MESSAGE_ASYNC_DONE:{
      GstSourceGroup *target_group = NULL;
      GList *tmp = NULL;

      if (GST_MESSAGE_SRC (msg) != playbin->playsink)
        break;

      GST_PLAY_BIN3_LOCK (playbin);
      *steal = TRUE;

      target_group = get_group (playbin);
      if (!target_group) {
        GST_LOG_OBJECT (playbin, "No current group");
        GST_PLAY_BIN3_UNLOCK (playbin);
        break;
      }

      if (playbin->mq) {
        GST_FIXME_OBJECT (playbin,
            "  [ASYNC DONE] Deactivate preroll state to %s",
            GST_OBJECT_NAME (playbin->mq));
        g_signal_emit_by_name (playbin->mq, "preroll-state", FALSE, NULL);
      }

      GST_DEBUG_OBJECT (playbin, "Got ASYNC-DONE (%s)",
          GST_OBJECT_NAME (GST_MESSAGE_SRC (msg)));

      GST_SOURCE_GROUP_LOCK (target_group);
      target_group->gap_seqnum = 0;
      if (target_group->timeout_id) {
        GST_DEBUG_OBJECT (playbin, "Remove timer: %d",
            target_group->timeout_id);
        g_source_remove (target_group->timeout_id);
        target_group->timeout_id = 0;
      }
      for (tmp = target_group->source_pads; tmp; tmp = tmp->next) {
        SourcePad *res = (SourcePad *) tmp->data;
        res->push_gap = FALSE;
        res->mark_seg = FALSE;
      }
      GST_SOURCE_GROUP_UNLOCK (target_group);
      GST_PLAY_BIN3_UNLOCK (playbin);
      break;
    }
    default:
      break;
  }
}

static gboolean
priv_play_bin3_send_event (GstPlayBin3 * playbin, GstEvent * event,
    gboolean * steal)
{
  gboolean res = TRUE;
  GstSourceGroup *target_group = NULL;
  target_group = get_group (playbin);

  if (!target_group->adaptive_mode && playbin->adaptivedemux
      && GST_EVENT_TYPE (event) == GST_EVENT_SEEK) {
    GST_SYS_LOG_OBJECT (playbin,
        "Directly send seek event to adaptivedemux: %" GST_PTR_FORMAT, event);
    gst_element_send_event (playbin->adaptivedemux, event);
    *steal = TRUE;
  }

  return res;
}

static void
priv_play_bin3_deep_element_added (GstBin * bin, GstBin * sub_bin,
    GstElement * child)
{
  GstPlayBin3 *playbin = GST_PLAY_BIN3 (bin);
  gchar *element_name = gst_element_get_name (child);
  gchar *subbin_name = gst_element_get_name (GST_ELEMENT_CAST (sub_bin));
  GstElementClass *cklass = GST_ELEMENT_GET_CLASS (child);
  const gchar *cklass_meta =
      gst_element_class_get_metadata (cklass, GST_ELEMENT_METADATA_KLASS);

  if (g_str_has_prefix (element_name, "decodebin3")) {
    if (G_UNLIKELY (playbin->decodebin))
      gst_object_unref (playbin->decodebin);
    GST_DEBUG_OBJECT (playbin, "Ref Decodebin3");
    playbin->decodebin = GST_ELEMENT_CAST (gst_object_ref (child));
  } else if (g_str_has_prefix (element_name, "multiqueue") &&
      g_str_has_prefix (subbin_name, "decodebin3")) {
    /* Only concern multiqueue in decodebin3, and let's ingnore the other multiqueue
     * (e.g., in textbin) */
    if (G_UNLIKELY (playbin->mq))
      gst_object_unref (playbin->mq);
    GST_DEBUG_OBJECT (playbin, "Ref MultiQueue");
    playbin->mq = GST_ELEMENT_CAST (gst_object_ref (child));
  }

  if (cklass_meta && g_strrstr (cklass_meta, "Demuxer") &&
      g_strrstr (cklass_meta, "Adaptive")) {
    GST_DEBUG_OBJECT (child, "Find adaptivedemux element");

    if (G_UNLIKELY (playbin->adaptivedemux))
      gst_object_unref (playbin->adaptivedemux);
    playbin->adaptivedemux = gst_object_ref (GST_OBJECT (child));
  }

  g_free (element_name);
  g_free (subbin_name);
}

static void
priv_play_bin3_deep_element_removed (GstBin * bin, GstBin * sub_bin,
    GstElement * child)
{
  GstPlayBin3 *playbin = GST_PLAY_BIN3 (bin);
  gchar *element_name = gst_element_get_name (child);
  gchar *subbin_name = gst_element_get_name (GST_ELEMENT_CAST (sub_bin));
  GstElementClass *cklass = GST_ELEMENT_GET_CLASS (child);
  const gchar *cklass_meta =
      gst_element_class_get_metadata (cklass, GST_ELEMENT_METADATA_KLASS);

  if (g_str_has_prefix (element_name, "decodebin3")) {
    if (playbin->decodebin && playbin->decodebin == child) {
      GST_DEBUG_OBJECT (playbin, "Unref Decodebin3");
      gst_object_unref (playbin->decodebin);
      playbin->decodebin = NULL;
    }
  } else if (g_str_has_prefix (element_name, "multiqueue")) {
    if (playbin->mq && playbin->mq == child) {
      gst_object_unref (playbin->mq);
      GST_DEBUG_OBJECT (playbin, "Unref MultiQueue");
      playbin->mq = NULL;
    }
  }

  if (cklass_meta && g_strrstr (cklass_meta, "Demuxer") &&
      g_strrstr (cklass_meta, "Adaptive")) {
    if (playbin->adaptivedemux == child) {
      gst_object_unref (playbin->adaptivedemux);
      playbin->adaptivedemux = NULL;
    }
  }

  g_free (element_name);
  g_free (subbin_name);
}

static void
priv_init_group (GstPlayBin3 * playbin, GstSourceGroup * group)
{
  GST_LOG_OBJECT (playbin, "Initializing group %p", group);
  group->gap_seqnum = 0;
  group->timeout_id = 0;
  group->use_fallback_preroll = FALSE;
  GST_LOG_OBJECT (playbin, "Initialized group %p", group);
}

static void
priv_free_group (GstPlayBin3 * playbin, GstSourceGroup * group)
{
  GST_LOG_OBJECT (playbin, "Releasing group %p", group);
  group->gap_seqnum = 0;
  if (group->timeout_id) {
    GST_DEBUG_OBJECT (playbin, "Removing timer: %d", group->timeout_id);
    g_source_remove (group->timeout_id);
    group->timeout_id = 0;
  }
  group->use_fallback_preroll = FALSE;
  GST_LOG_OBJECT (playbin, "Released group %p", group);
}
