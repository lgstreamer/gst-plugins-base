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

#include <string.h>
#include <gst/audio/audio.h>

static gboolean is_demuxer_element (GstElement * element);
static void demuxer_no_more_pads_cb (GstElement * element,
    DecodebinInput * input);
static void parsebin_deep_element_added (GstBin * bin, GstBin * sub_bin,
    GstElement * child, DecodebinInput * input);
static gboolean caps_in_slots (GList * list, const gchar * sid);
static GstStreamType guess_stream_type_from_caps (GstCaps * caps);
static gboolean is_request_resource_done (GstDecodebin3 * dbin);
static void change_actual_decoder (GstDecodebin3 * dbin,
    DecodebinOutputStream * output);
static GList *get_expected_active_selection (GstDecodebin3 * dbin,
    GstStreamCollection * collection);
static GstMessage *update_active_collection (MultiQueueSlot * slot,
    gboolean update_selection);
static gboolean pad_in_list (GList * list, GstPad * pad);
static gint sort_streams (GstStream * sa, GstStream * sb);
static GstStreamCollection *get_active_collection (GstDecodebin3 * dbin);
static gboolean link_pending_pad_to_slot (DecodebinInput * input,
    MultiQueueSlot * slot);
static GstCaps *negotiate_default_caps (GstDecodebin3 * dbin,
    GstCaps * input_caps, GstPad * srcpad, GstStreamType stream_type);
static const gchar *collection_has_stream (GstStreamCollection * collection,
    gchar * sid);
static gboolean ongoing_change_upstream (GstDecodebin3 * dbin,
    MultiQueueSlot * slot);

/* Virtual Functions */
static void priv_decodebin3_init (GstDecodebin3 * dbin);
static void priv_decodebin3_dispose (GstDecodebin3 * dbin);
static void priv_parsebin_init (GstDecodebin3 * dbin, DecodebinInput * input);
static GstStreamCollection *priv_get_sorted_collection (GstDecodebin3 * dbin,
    GstStreamCollection * collection);
static void priv_append_exposed_pad (GstDecodebin3 * dbin, GstPad * pad);
static void priv_query_smart_properties (GstDecodebin3 * dbin,
    DecodebinInput * input);
static void priv_set_monitor_pts_continuity (GstDecodebin3 * dbin,
    DecodebinInput * input);
static void priv_create_new_input (GstDecodebin3 * dbin,
    DecodebinInput * input);
static void priv_update_requested_selection (GstDecodebin3 * dbin,
    GstStreamCollection * collection);
static void priv_decodebin3_handle_message (GstDecodebin3 * dbin,
    GstMessage * message, gboolean * steal, gboolean * do_post);
static GstMessage *priv_is_selection_done (GstDecodebin3 * dbin);
static GstPadProbeReturn priv_multiqueue_src_probe (GstPad * pad,
    GstPadProbeInfo * info, MultiQueueSlot * slot);
static void priv_create_new_slot (GstDecodebin3 * dbin, MultiQueueSlot * slot);
static void priv_free_multiqueue_slot (GstDecodebin3 * dbin,
    MultiQueueSlot * slot);
static gboolean priv_handle_stream_switch (GstDecodebin3 * dbin,
    GList * select_streams, guint32 seqnum);
static GstPadProbeReturn priv_ghost_pad_event_probe (GstDecodebin3 * dbin,
    GstPad * pad, GstEvent * event, DecodebinOutputStream * output,
    gboolean * steal);
static void priv_release_resource_related (GstDecodebin3 * dbin);
static void priv_remove_exposed_pad (GstDecodebin3 * dbin, GstPad * pad);
static gboolean priv_check_slot_for_eos (GstDecodebin3 * dbin,
    MultiQueueSlot * slot);
static void priv_remove_input_stream (GstDecodebin3 * dbin, GstPad * pad,
    DecodebinInputStream * input);
static void priv_reconfigure_output_stream (DecodebinOutputStream * output,
    MultiQueueSlot * slot, gboolean reassign);

static gboolean
is_demuxer_element (GstElement * element)
{
  GstElementFactory *srcfactory;
  GstElementClass *elemclass;
  GList *walk;
  const gchar *klass;
  gint potential_src_pads = 0;

  srcfactory = gst_element_get_factory (element);
  klass =
      gst_element_factory_get_metadata (srcfactory, GST_ELEMENT_METADATA_KLASS);

  /* Can't be a demuxer unless it has Demux in the klass name */
  if (!strstr (klass, "Demux"))
    return FALSE;

  /* Walk the src pad templates and count how many the element
   * might produce */
  elemclass = GST_ELEMENT_GET_CLASS (element);

  walk = gst_element_class_get_pad_template_list (elemclass);
  while (walk != NULL) {
    GstPadTemplate *templ;

    templ = (GstPadTemplate *) walk->data;
    if (GST_PAD_TEMPLATE_DIRECTION (templ) == GST_PAD_SRC) {
      switch (GST_PAD_TEMPLATE_PRESENCE (templ)) {
        case GST_PAD_ALWAYS:
        case GST_PAD_SOMETIMES:
          if (strstr (GST_PAD_TEMPLATE_NAME_TEMPLATE (templ), "%"))
            potential_src_pads += 2;    /* Might make multiple pads */
          else
            potential_src_pads += 1;
          break;
        case GST_PAD_REQUEST:
          potential_src_pads += 2;
          break;
      }
    }
    walk = g_list_next (walk);
  }

  if (potential_src_pads < 2)
    return FALSE;

  return TRUE;
}

static void
demuxer_no_more_pads_cb (GstElement * element, DecodebinInput * input)
{
  GstDecodebin3 *dbin;

  GST_DEBUG_OBJECT (element, "got no more pads");

  dbin = input->dbin;
  input->demuxer_no_more_pads = TRUE;

  SELECTION_LOCK (dbin);
  if (!dbin->request_resource && is_request_resource_done (dbin)) {
    /* Send request resource message */
    GST_SYS_DEBUG_OBJECT (dbin, "posting request-resource message");
    dbin->request_resource = TRUE;
    SELECTION_UNLOCK (dbin);
    if (!gst_element_post_message (GST_ELEMENT_CAST (dbin),
            gst_message_new_application (GST_OBJECT_CAST (dbin),
                gst_structure_new_empty ("request-resource"))))
      GST_ERROR_OBJECT (dbin, "ERROR: Send request resource message");
  } else
    SELECTION_UNLOCK (dbin);
}

static void
parsebin_deep_element_added (GstBin * bin, GstBin * sub_bin, GstElement * child,
    DecodebinInput * input)
{
  gchar *elem_name = NULL;
  elem_name = gst_element_get_name (child);

  if (is_demuxer_element (child)) {
    GST_DEBUG ("Demuxer is created: %" GST_PTR_FORMAT, child);
    input->has_demuxer = TRUE;
    g_signal_connect (child, "no-more-pads",
        G_CALLBACK (demuxer_no_more_pads_cb), input);
  } else if (g_str_has_prefix (elem_name, "splitappsink")) {
    GST_DEBUG ("Splitter is created: %" GST_PTR_FORMAT, child);
  }
  g_free (elem_name);
}

static void
priv_decodebin3_init (GstDecodebin3 * dbin)
{
  dbin->resource_info = NULL;
  dbin->request_resource = FALSE;
  dbin->monitor_pts_continuity = FALSE;
  dbin->use_fallback_preroll = FALSE;
  dbin->adaptive_mode = FALSE;
  dbin->dvr_playback = FALSE;
  GST_DEBUG_OBJECT (dbin, "Done");
}

static void
priv_decodebin3_dispose (GstDecodebin3 * dbin)
{
  g_clear_object (&dbin->active_collection);

  if (dbin->resource_info) {
    gst_structure_free (dbin->resource_info);
    dbin->resource_info = NULL;
  }

  if (dbin->exposed_pads) {
    g_list_free_full (dbin->exposed_pads, gst_object_unref);
    dbin->exposed_pads = NULL;
  }

  dbin->use_fallback_preroll = FALSE;

  GST_DEBUG_OBJECT (dbin, "Done");
}

static void
priv_parsebin_init (GstDecodebin3 * dbin, DecodebinInput * input)
{
  g_signal_connect (input->parsebin, "deep-element-added",
      G_CALLBACK (parsebin_deep_element_added), input);
}

static gboolean
caps_in_slots (GList * list, const gchar * sid)
{
  GList *tmp;

  for (tmp = list; tmp; tmp = tmp->next) {
    MultiQueueSlot *slot = (MultiQueueSlot *) tmp->data;
    GstStream *slot_stream = slot->active_stream;

    if (slot_stream) {
      const gchar *slot_sid = gst_stream_get_stream_id (slot_stream);
      if (!g_strcmp0 (sid, slot_sid)) {
        GstCaps *slot_caps = gst_stream_get_caps (slot_stream);
        if (slot_caps) {
          gst_caps_unref (slot_caps);
          return TRUE;
        }
      }
    }
  }

  return FALSE;
}

static GstStreamType
guess_stream_type_from_caps (GstCaps * caps)
{
  GstStructure *s;
  const gchar *name;

  if (gst_caps_get_size (caps) < 1)
    return GST_STREAM_TYPE_UNKNOWN;

  s = gst_caps_get_structure (caps, 0);
  name = gst_structure_get_name (s);

  if (g_str_has_prefix (name, "video/") || g_str_has_prefix (name, "image/"))
    return GST_STREAM_TYPE_VIDEO;
  if (g_str_has_prefix (name, "audio/"))
    return GST_STREAM_TYPE_AUDIO;
  if (g_str_has_prefix (name, "text/") ||
      g_str_has_prefix (name, "subpicture/") ||
      g_str_has_prefix (name, "application/ttml+xml"))
    return GST_STREAM_TYPE_TEXT;

  return GST_STREAM_TYPE_UNKNOWN;
}

/* Must be called with LOCK taken */
static gboolean
is_request_resource_done (GstDecodebin3 * dbin)
{
  GstStreamCollection *collection, *active_collection;
  GstStreamType exposed_stream_types = 0;
  GstStreamType activated_stream_types = 0;
  GList *tmp;
  GList *sub_inputs;
  GList *mq_slots;
  guint nb_streams = 0;
  guint i = 0;
  gboolean no_more_pads_done = TRUE;
  DecodebinInput *main_input = NULL;

  /* Check collection */
  if (!dbin->collection) {
    GST_DEBUG_OBJECT (dbin, "No collection");
    return FALSE;
  }

  /* FIXME : Should we check output_streams ?? */
  if (!dbin->output_streams) {
    GST_DEBUG_OBJECT (dbin, "No output stream");
    return FALSE;
  }

  collection = dbin->collection;
  mq_slots = dbin->slots;
  nb_streams = gst_stream_collection_get_size (collection);
  main_input = dbin->main_input;
  sub_inputs = dbin->other_inputs;

  /* Check demuxer is done */
  for (tmp = sub_inputs; tmp; tmp = tmp->next) {
    DecodebinInput *sub_input = (DecodebinInput *) tmp->data;
    if (sub_input->has_demuxer) {
      no_more_pads_done &= sub_input->demuxer_no_more_pads;
    }
  }

  if (main_input->has_demuxer) {
    GST_DEBUG_OBJECT (dbin,
        "main_input: has_demuxer [%d], demuxer_no_more_pads [%d]",
        main_input->has_demuxer, main_input->demuxer_no_more_pads);
    no_more_pads_done &= main_input->demuxer_no_more_pads;
  }

  if (!no_more_pads_done) {
    GST_DEBUG_OBJECT (dbin, "no-more-pads signal is not emmitted yet");
    return FALSE;
  }

  /* Check All stream-start events are probed in MQ's srcpad */
  active_collection = get_active_collection (dbin);

  if (!active_collection) {
    GST_DEBUG_OBJECT (dbin, "active collection is not created");
    return FALSE;
  }

  if (gst_stream_collection_get_size (collection) !=
      gst_stream_collection_get_size (active_collection)) {
    GST_DEBUG_OBJECT (dbin, "active collection is not fully ready");
    return FALSE;
  }

  /* Check All A/V streams are activated on MQ's srcpad */
  for (i = 0; i < nb_streams; i++) {
    GstStream *stream = gst_stream_collection_get_stream (active_collection, i);
    const gchar *sid = gst_stream_get_stream_id (stream);
    GstStreamType stream_type = gst_stream_get_stream_type (stream);

    if (!(stream_type & GST_STREAM_TYPE_VIDEO
            || stream_type & GST_STREAM_TYPE_AUDIO))
      continue;

    if (!caps_in_slots (mq_slots, sid)) {
      GST_DEBUG_OBJECT (dbin, "sid(%s) is not activated on MQ slot", sid);
      return FALSE;
    } else
      GST_DEBUG_OBJECT (dbin, "sid(%s) is activated on MQ slot", sid);
  }

  /* Check puppet is ready in decproxy */
  for (tmp = dbin->output_streams; tmp; tmp = tmp->next) {
    DecodebinOutputStream *output = (DecodebinOutputStream *) tmp->data;
    MultiQueueSlot *slot = output->slot;
    activated_stream_types |= output->type;
    if (output->decoder) {
      gchar *decname = NULL;
      decname = gst_element_get_name (output->decoder);
      GST_DEBUG_OBJECT (dbin, "output->decoder %p:%p", output, output->decoder);
      if (g_str_has_prefix (decname, "decproxy") && !output->puppet_done) {
        g_free (decname);
        GST_DEBUG_OBJECT (dbin, "Puppet is not ready %p:%p", output,
            output->decoder);
        return FALSE;
      }
      g_free (decname);
    } else {
      GstCaps *caps = (GstCaps *) gst_stream_get_caps (slot->active_stream);
      if (!gst_caps_can_intersect (caps, dbin->caps)) {
        GST_DEBUG_OBJECT (dbin, "stream needs decoder, type %s",
            gst_stream_type_get_name (output->type));
        gst_caps_unref (caps);
        return FALSE;
      }
      gst_caps_unref (caps);
      if (output->type == GST_STREAM_TYPE_TEXT && !output->linked) {
        GST_DEBUG_OBJECT (dbin, "text output is not linked yet");
        return FALSE;
      }
    }
  }

  if (activated_stream_types == GST_STREAM_TYPE_TEXT) {
    GST_DEBUG_OBJECT (dbin,
        "Text streams are only activated. We don't have video and audio streams");
    return FALSE;
  }

  for (tmp = dbin->exposed_pads; tmp; tmp = tmp->next) {
    GstPad *epad = (GstPad *) tmp->data;
    GstCaps *ecaps = NULL;
    GstStreamType stream_type;

    ecaps = gst_pad_get_current_caps (epad);
    if (ecaps == NULL)
      ecaps = gst_pad_query_caps (epad, NULL);

    if (!ecaps)
      continue;

    stream_type = guess_stream_type_from_caps (ecaps);
    if (stream_type != GST_STREAM_TYPE_UNKNOWN) {
      exposed_stream_types |= stream_type;
      GST_DEBUG_OBJECT (dbin, "Adding to exposed_stream_type: %" GST_PTR_FORMAT,
          ecaps);
    }

    gst_caps_unref (ecaps);
  }

  GST_DEBUG_OBJECT (dbin,
      "exposed_stream_types [0x%x] activated_stream_types [0x%x]",
      exposed_stream_types, activated_stream_types);
  if ((exposed_stream_types & ~activated_stream_types &
          (GST_STREAM_TYPE_VIDEO | GST_STREAM_TYPE_AUDIO)) != 0) {
    return FALSE;
  }

  GST_DEBUG_OBJECT (dbin, "Ready to request resource");

  return TRUE;
}

static void
change_actual_decoder (GstDecodebin3 * dbin, DecodebinOutputStream * output)
{
  GstStructure *s;
  GstEvent *event;

  GST_OBJECT_LOCK (dbin);
  s = gst_structure_copy (dbin->resource_info);
  gst_structure_set (s, "active", G_TYPE_BOOLEAN, TRUE, NULL);
  GST_DEBUG_OBJECT (output->decoder,
      "sending acquired-resource evnet : %" GST_PTR_FORMAT, s);
  event = gst_event_new_custom (GST_EVENT_CUSTOM_UPSTREAM, s);
  GST_OBJECT_UNLOCK (dbin);
  gst_element_send_event (output->decoder, event);
}

static void
priv_reconfigure_output_stream (DecodebinOutputStream * output,
    MultiQueueSlot * slot, gboolean reassign)
{
  GstDecodebin3 *dbin = output->dbin;
  GstCaps *new_caps = (GstCaps *) gst_stream_get_caps (slot->active_stream);
  gboolean needs_decoder;
  gboolean can_reassign = FALSE;
  gboolean needs_actual_decoder = FALSE;

  needs_decoder = gst_caps_can_intersect (new_caps, dbin->caps) != TRUE;

  GST_DEBUG_OBJECT (dbin,
      "Reconfiguring output %p to slot: %s:%s(%p), needs_decoder: %s", output,
      GST_DEBUG_PAD_NAME (slot->src_pad), slot,
      needs_decoder ? "TRUE" : "FALSE");

  /* FIXME : Maybe make the output un-hook itself automatically ? */
  if (output->slot != NULL && output->slot != slot) {
    GST_WARNING_OBJECT (dbin,
        "Output still linked to another slot: %s:%s(%p)",
        GST_DEBUG_PAD_NAME (output->slot->src_pad), output->slot);
    gst_caps_unref (new_caps);
    return;
  }

  /* FIXME: We can understand that current decproxy can be reused if it is not having
   * puppet(fakedec) and never once used for previous stream. */
  if (output->decoder && needs_decoder) {
    gchar *decname = NULL;
    decname = gst_element_get_name (output->decoder);
    if (g_str_has_prefix (decname, "decproxy") && !output->puppet_done) {
      GST_INFO_OBJECT (dbin, "Puppet is not ready to output:%p", output);
      if (reassign) {
        can_reassign = TRUE;
      } else {
        g_free (decname);
        return;
      }
    }
    g_free (decname);
  }

  /* Check if existing config is reusable as-is by checking if
   * the existing decoder accepts the new caps, if not delete
   * it and create a new one */
  if (output->decoder) {
    gboolean can_reuse_decoder;

    if (needs_decoder) {
      /* FIXME: Force to re-use current decproxy without checking Accept CAPS query
       * when it is possible to re-assignable directly to new activated stream.
       * If we send CAPS event for new stream, it is blocked in decproxy and indeed
       * can not return anymore and pended */
      if (can_reassign) {
        can_reuse_decoder = TRUE;
      } else {
        GST_LOG_OBJECT (dbin, "Querying accept-caps to pad(%s:%s)",
            GST_DEBUG_PAD_NAME (output->decoder_sink));
        can_reuse_decoder =
            gst_pad_query_accept_caps (output->decoder_sink, new_caps);
        GST_LOG_OBJECT (dbin, "Queried accept-caps (%d) to pad(%s:%s)",
            can_reuse_decoder, GST_DEBUG_PAD_NAME (output->decoder_sink));
      }
    } else
      can_reuse_decoder = FALSE;

    if (can_reuse_decoder) {
      if (output->type & GST_STREAM_TYPE_VIDEO && output->drop_probe_id == 0) {
        GST_DEBUG_OBJECT (dbin, "Adding keyframe-waiter probe");
        output->drop_probe_id =
            gst_pad_add_probe (output->decoder_sink, GST_PAD_PROBE_TYPE_BUFFER,
            (GstPadProbeCallback) keyframe_waiter_probe, output, NULL);
      }
      GST_DEBUG_OBJECT (dbin, "Reusing existing decoder for slot: %s:%s(%p)",
          GST_DEBUG_PAD_NAME (slot->src_pad), slot);
      if (output->linked == FALSE) {

        /* FIXME: If current decproxy is re-assignable directly to new activated stream,
         * store new CAPS event to MQ's srcpad and make sure it sends to actual decoder.
         * If not, SEGMENT event is sent without CAPS event sometimes. */
        if (can_reassign) {
          GstEvent *e;
          e = gst_event_new_caps (new_caps);
          GST_DEBUG_OBJECT (slot->src_pad,
              "Re-assign %" GST_PTR_FORMAT " and storing new caps %"
              GST_PTR_FORMAT, output->decoder, new_caps);
          gst_pad_store_sticky_event (slot->src_pad, e);
          GST_LOG_OBJECT (slot->src_pad, "Stored new caps");
          needs_actual_decoder = TRUE;
        }

        gst_pad_link_full (slot->src_pad, output->decoder_sink,
            GST_PAD_LINK_CHECK_NOTHING);
        output->linked = TRUE;

        if (output->decoder && needs_actual_decoder && dbin->request_resource) {
          gchar *decname = NULL;
          decname = gst_element_get_name (output->decoder);
          if (g_str_has_prefix (decname, "decproxy") && dbin->resource_info) {
            change_actual_decoder (dbin, output);
          }
          g_free (decname);
        }
      }
#if 0
      else {
        GstEvent *e;
        GST_DEBUG_OBJECT (output->decoder_sink,
            "Resending caps %" GST_PTR_FORMAT, new_caps);
        e = gst_event_new_caps (new_caps);
        SELECTION_UNLOCK (dbin);
        gst_pad_send_event (output->decoder_sink, e);
        SELECTION_LOCK (dbin);
        GST_LOG_OBJECT (output->decoder_sink, "Sent caps");
      }
#endif
      gst_caps_unref (new_caps);
      return;
    }

    GST_DEBUG_OBJECT (dbin, "Removing old decoder(%s:%s) for slot: %s:%s(%p)",
        GST_DEBUG_PAD_NAME (output->decoder_sink),
        GST_DEBUG_PAD_NAME (slot->src_pad), slot);
    if (output->linked)
      gst_pad_unlink (slot->src_pad, output->decoder_sink);
    output->linked = FALSE;
    if (output->drop_probe_id) {
      gst_pad_remove_probe (output->decoder_sink, output->drop_probe_id);
      output->drop_probe_id = 0;
    }

    if (!gst_ghost_pad_set_target ((GstGhostPad *) output->src_pad, NULL)) {
      GST_ERROR_OBJECT (dbin, "Could not release decoder pad");
      gst_caps_unref (new_caps);
      goto cleanup;
    }

    gst_element_set_locked_state (output->decoder, TRUE);
    gst_element_set_state (output->decoder, GST_STATE_NULL);

    gst_bin_remove ((GstBin *) dbin, output->decoder);
    output->decoder = NULL;
  }

  gst_caps_unref (new_caps);

  gst_object_replace ((GstObject **) & output->decoder_sink, NULL);
  gst_object_replace ((GstObject **) & output->decoder_src, NULL);

  /* If a decoder is required, create one */
  if (needs_decoder) {
    gchar *decname = NULL;

    /* If we don't have a decoder yet, instantiate one */
    output->decoder = create_decoder (dbin, slot->active_stream);
    if (output->decoder == NULL) {
      GstCaps *caps;

      SELECTION_UNLOCK (dbin);
      /* FIXME : Should we be smarter if there's a missing decoder ?
       * Should we deactivate that stream ? */
      caps = gst_stream_get_caps (slot->active_stream);
      gst_element_post_message (GST_ELEMENT_CAST (dbin),
          gst_missing_decoder_message_new (GST_ELEMENT_CAST (dbin), caps));
      gst_caps_unref (caps);
      SELECTION_LOCK (dbin);
      goto cleanup;
    }

    decname = gst_element_get_name (output->decoder);
    GST_LOG_OBJECT (dbin, "Created decoder(%s)", decname);

    if (g_str_has_prefix (decname, "decproxy")) {
      output->puppet_done = FALSE;
      if (g_object_class_find_property (G_OBJECT_GET_CLASS (output->decoder),
              "propagate-sticky-event")) {
        g_object_set (output->decoder, "propagate-sticky-event", FALSE, NULL);
      }
    }
    g_free (decname);

    GST_DEBUG_OBJECT (dbin, "Adding Decoder(%s)",
        GST_OBJECT_NAME (output->decoder));
    if (!gst_bin_add ((GstBin *) dbin, output->decoder)) {
      GST_ERROR_OBJECT (dbin, "could not add decoder to pipeline");
      goto cleanup;
    }
    GST_DEBUG_OBJECT (dbin, "Added Decoder(%s)",
        GST_OBJECT_NAME (output->decoder));

    gst_element_sync_state_with_parent (output->decoder);
    output->decoder_sink = gst_element_get_static_pad (output->decoder, "sink");
    output->decoder_src = gst_element_get_static_pad (output->decoder, "src");
    if (output->type & GST_STREAM_TYPE_VIDEO) {
      GST_DEBUG_OBJECT (dbin, "Adding keyframe-waiter probe");
      output->drop_probe_id =
          gst_pad_add_probe (output->decoder_sink, GST_PAD_PROBE_TYPE_BUFFER,
          (GstPadProbeCallback) keyframe_waiter_probe, output, NULL);
    }

    /* FIXME: Store new caps event to MQ's srcpad and make sure it sends to actual decoder */
    decname = gst_element_get_name (output->decoder);
    if (g_str_has_prefix (decname, "decproxy")) {
      GstEvent *e;
      e = gst_event_new_caps (new_caps);
      GST_DEBUG_OBJECT (slot->src_pad, "Storing new caps %" GST_PTR_FORMAT,
          new_caps);
      gst_pad_store_sticky_event (slot->src_pad, e);
      GST_LOG_OBJECT (slot->src_pad, "Stored new caps");
      gst_event_unref (e);
    }
    g_free (decname);

    GST_DEBUG_OBJECT (dbin, "Linking (%s:%s) to (%s:%s)",
        GST_DEBUG_PAD_NAME (slot->src_pad),
        GST_DEBUG_PAD_NAME (output->decoder_sink));
    if (gst_pad_link_full (slot->src_pad, output->decoder_sink,
            GST_PAD_LINK_CHECK_NOTHING) != GST_PAD_LINK_OK) {
      GST_ERROR_OBJECT (dbin, "could not link to %s:%s",
          GST_DEBUG_PAD_NAME (output->decoder_sink));
      goto cleanup;
    }
    GST_LOG_OBJECT (dbin, "Linked (%s:%s) to (%s:%s)",
        GST_DEBUG_PAD_NAME (slot->src_pad),
        GST_DEBUG_PAD_NAME (output->decoder_sink));
    needs_actual_decoder = TRUE;
  } else {
    output->decoder_src = gst_object_ref (slot->src_pad);
    output->decoder_sink = NULL;
  }
  output->linked = TRUE;

  GST_DEBUG_OBJECT (dbin, "Exposing decoder srcpad (%s:%s)",
      GST_DEBUG_PAD_NAME (output->decoder_src));
  if (!gst_ghost_pad_set_target ((GstGhostPad *) output->src_pad,
          output->decoder_src)) {
    GST_ERROR_OBJECT (dbin, "Could not expose decoder pad");
    goto cleanup;
  }
  GST_LOG_OBJECT (dbin, "Exposed decoder srcpad (%s:%s)",
      GST_DEBUG_PAD_NAME (output->decoder_src));
  if (output->src_exposed == FALSE) {
    output->src_exposed = TRUE;
    GST_LOG_OBJECT (dbin, "Adding srcpad (%s:%s)",
        GST_DEBUG_PAD_NAME (output->src_pad));
    gst_element_add_pad (GST_ELEMENT_CAST (dbin), output->src_pad);
    GST_LOG_OBJECT (dbin, "Added srcpad (%s:%s)",
        GST_DEBUG_PAD_NAME (output->src_pad));
  }

  output->slot = slot;

  if (output->decoder && needs_actual_decoder && dbin->request_resource) {
    gchar *decname = NULL;
    decname = gst_element_get_name (output->decoder);
    if (g_str_has_prefix (decname, "decproxy") && dbin->resource_info) {
      change_actual_decoder (dbin, output);
    }
    g_free (decname);
  }

  GST_DEBUG_OBJECT (dbin,
      "Reconfigured output: %s(%p) to slot: %s:%s(%p)",
      output->decoder ? GST_ELEMENT_NAME (output->decoder) : "NONE", output,
      GST_DEBUG_PAD_NAME (slot->src_pad), slot);

  return;

cleanup:
  {
    GST_DEBUG_OBJECT (dbin, "Cleanup");
    if (output->decoder_sink) {
      gst_object_unref (output->decoder_sink);
      output->decoder_sink = NULL;
    }
    if (output->decoder_src) {
      gst_object_unref (output->decoder_src);
      output->decoder_src = NULL;
    }
    if (output->decoder) {
      gst_element_set_state (output->decoder, GST_STATE_NULL);
      gst_bin_remove ((GstBin *) dbin, output->decoder);
      output->decoder = NULL;
    }
    needs_actual_decoder = FALSE;
  }
}

static GList *
get_expected_active_selection (GstDecodebin3 * dbin,
    GstStreamCollection * collection)
{
  guint i, nb;
  GList *tmp = NULL;
  GstStreamType used_types = 0;
  GList *expected_active_selection = NULL;

  nb = gst_stream_collection_get_size (collection);

  /* 1. Is there a pending SELECT_STREAMS we can return straight away since
   *  the switch handler will take care of the pending selection */
  if (dbin->pending_select_streams) {
    GST_DEBUG_OBJECT (dbin,
        "No need to create pending selection, SELECT_STREAMS underway");
    goto beach;
  }

  /* 2. If not, are we in EXPOSE_ALL_MODE ? If so, match everything */
  GST_FIXME_OBJECT (dbin, "Implement EXPOSE_ALL_MODE");

  /* 3. If not, check if we already have some of the streams in the
   * existing active/requested selection */
  for (i = 0; i < nb; i++) {
    GstStream *stream = gst_stream_collection_get_stream (collection, i);
    const gchar *sid = gst_stream_get_stream_id (stream);
    gint request = -1;
    /* Fire select-stream signal to see if outside components want to
     * hint at which streams should be selected */
    g_signal_emit (G_OBJECT (dbin),
        gst_decodebin3_signals[SIGNAL_SELECT_STREAM], 0, collection, stream,
        &request);
    GST_DEBUG_OBJECT (dbin, "stream %s , request:%d", sid, request);
    if (request == 1 || (request == -1 && dbin->collection_posted
            && (stream_in_list (expected_active_selection, sid)
                || stream_in_list (dbin->active_selection, sid)))) {
      GstStreamType curtype = gst_stream_get_stream_type (stream);
      if (request == 1)
        GST_DEBUG_OBJECT (dbin,
            "Using stream requested by 'select-stream' signal : %s", sid);
      else
        GST_DEBUG_OBJECT (dbin,
            "Re-using stream already present in requested or active selection : %s",
            sid);
      tmp = g_list_append (tmp, (gchar *) sid);
      used_types |= curtype;
    }
  }

  /* 4. If not, match one stream of each type */
  for (i = 0; i < nb; i++) {
    GstStream *stream = gst_stream_collection_get_stream (collection, i);
    GstStreamType curtype = gst_stream_get_stream_type (stream);
    if (!(used_types & curtype)) {
      const gchar *sid = gst_stream_get_stream_id (stream);
      GST_DEBUG_OBJECT (dbin, "Selecting stream '%s' of type %s",
          sid, gst_stream_type_get_name (curtype));
      tmp = g_list_append (tmp, (gchar *) sid);
      used_types |= curtype;
    }
  }

beach:
  /* Finally set the requested selection */
  if (tmp) {
    if (expected_active_selection) {
      GST_FIXME_OBJECT (dbin,
          "Replacing non-NULL expected_active_selection, what should we do ??");
      g_list_free_full (expected_active_selection, g_free);
    }
    expected_active_selection =
        g_list_copy_deep (tmp, (GCopyFunc) g_strdup, NULL);
    dbin->selection_updated = TRUE;
    g_list_free (tmp);
    DUMP_SELECTION_LIST (dbin, expected_active_selection, "requested");
  }

  return expected_active_selection;
}

static GstStreamCollection *
priv_get_sorted_collection (GstDecodebin3 * dbin,
    GstStreamCollection * collection)
{
  GList *tmp = NULL;
  GList *unsorted_streams = NULL;
  guint i;
  const gchar *upstream_id;
  GstStreamCollection *sorted_collection = NULL;

  g_return_val_if_fail (collection != NULL, NULL);

  upstream_id = gst_stream_collection_get_upstream_id (collection);
  sorted_collection = gst_stream_collection_new (upstream_id);

  for (i = 0; i < gst_stream_collection_get_size (collection); i++) {
    GstStream *stream = gst_stream_collection_get_stream (collection, i);
    unsorted_streams = g_list_append (unsorted_streams, stream);
  }

  /* re-order streams : video, then audio, then others */
  unsorted_streams =
      g_list_sort (unsorted_streams, (GCompareFunc) sort_streams);
  for (tmp = unsorted_streams; tmp; tmp = tmp->next) {
    GstStream *stream = (GstStream *) tmp->data;
    GST_DEBUG_OBJECT (dbin, "Adding #stream(%s) to collection",
        gst_stream_get_stream_id (stream));
    gst_stream_collection_add_stream (sorted_collection,
        gst_object_ref (stream));
  }

  if (unsorted_streams)
    g_list_free (unsorted_streams);

  gst_object_unref (collection);

  return sorted_collection;
}

/* Call with SELECTION_LOCK taken */
static GstMessage *
update_active_collection (MultiQueueSlot * slot, gboolean update_selection)
{
  GstStreamCollection *collection;
  gboolean do_post = FALSE;
  GstMessage *msg = NULL;
  GstDecodebin3 *dbin = slot->dbin;

  GST_DEBUG_OBJECT (dbin, "Probe stream-start(%s) on slot(%s:%s)",
      slot->active_stream ? gst_stream_get_stream_id (slot->active_stream) :
      "<NONE>", GST_DEBUG_PAD_NAME (slot->src_pad));

  collection = get_active_collection (dbin);

  if (dbin->active_collection)
    gst_object_unref (dbin->active_collection);

  dbin->active_collection = collection;

  /* Wait for posting startup stream-collection message
   * until all input_streams arrived on multiqueue's src pad.
   * This is to prevent noisy stream-collection message
   */
  if (!dbin->collection_posted) {
    if (dbin->collection && dbin->active_collection) {
      if (gst_stream_collection_get_size (dbin->collection) ==
          gst_stream_collection_get_size (dbin->active_collection)) {
        do_post = TRUE;
      } else {
        GList *expected_active_selection =
            get_expected_active_selection (dbin, dbin->collection);
        const gchar *active_sid =
            slot->
            active_stream ? gst_stream_get_stream_id (slot->active_stream) :
            "<NONE>";

        /* FIXME : How can we maintain order of all tracks corresponding with current collection?
         * Ignore to update requested-selection if the stream's order is lower. (not activated yet) */
        if (!stream_in_list (expected_active_selection, active_sid)) {
          GST_DEBUG_OBJECT (dbin,
              "Ignore to update requested-selection for sid (%s)", active_sid);
          update_selection = FALSE;
        }
        if (expected_active_selection)
          g_list_free_full (expected_active_selection, g_free);
      }
    }
  } else {
    /* Always post if we are not in startup state */
    do_post = TRUE;
  }

  if (collection) {
    if (update_selection)
      priv_update_requested_selection (dbin, collection);

    if (do_post) {
      GST_DEBUG_OBJECT (dbin, "Created new activated collection");
      msg =
          gst_message_new_stream_collection (GST_OBJECT_CAST (dbin),
          collection);
      dbin->collection_posted = TRUE;
    }
  }

  return msg;
}

static gboolean
pad_in_list (GList * list, GstPad * pad)
{
  GList *tmp;

  for (tmp = list; tmp; tmp = tmp->next) {
    GstPad *opad = (GstPad *) tmp->data;
    if (opad == pad)
      return TRUE;
  }

  return FALSE;
}

static void
priv_append_exposed_pad (GstDecodebin3 * dbin, GstPad * pad)
{
  /* FIXME : How can we ignore the caps from typefind ?? */
  if (!dbin->request_resource) {
    if (!pad_in_list (dbin->exposed_pads, pad)) {
      GST_DEBUG_OBJECT (pad, "Adding pad(%p) to exposed pads list", pad);
      dbin->exposed_pads =
          g_list_append (dbin->exposed_pads, gst_object_ref (pad));
    }
  }
}

static void
priv_query_smart_properties (GstDecodebin3 * dbin, DecodebinInput * input)
{
  if (input->is_main) {
    GstSmartPropertiesReturn ret;
    ret =
        gst_element_get_smart_properties (GST_ELEMENT_CAST (dbin),
        "monitor-pts-continuity", &dbin->monitor_pts_continuity,
        "use-fallback-preroll", &dbin->use_fallback_preroll, "adaptive-mode",
        &dbin->adaptive_mode, "dvr-playback", &dbin->dvr_playback, NULL);
    GST_DEBUG_OBJECT (dbin,
        "response of custom query : [%d], monitor-pts-continuity = [%d], use-fallback-preroll = [%d], adaptive-mode = [%d], dvr-playback = [%d]",
        ret, dbin->monitor_pts_continuity, dbin->use_fallback_preroll,
        dbin->adaptive_mode, dbin->dvr_playback);
  }
}

static void
priv_set_monitor_pts_continuity (GstDecodebin3 * dbin, DecodebinInput * input)
{
  if (input->is_main) {
    dbin->monitor_pts_continuity = FALSE;
  }
}

static void
priv_create_new_input (GstDecodebin3 * dbin, DecodebinInput * input)
{
  g_mutex_init (&input->input_lock);
  GST_DEBUG_OBJECT (dbin, "Done");
}

/* Call with SELECTION_LOCK taken */
static void
priv_update_requested_selection (GstDecodebin3 * dbin,
    GstStreamCollection * collection)
{
  guint i, nb;
  GList *tmp = NULL;
  GstStreamType used_types = 0;

  nb = gst_stream_collection_get_size (collection);

  /* 1. Is there a pending SELECT_STREAMS we can return straight away since
   *  the switch handler will take care of the pending selection */
  if (dbin->pending_select_streams) {
    GST_DEBUG_OBJECT (dbin,
        "No need to create pending selection, SELECT_STREAMS underway");
    goto beach;
  }

  /* 2. If not, are we in EXPOSE_ALL_MODE ? If so, match everything */
  GST_FIXME_OBJECT (dbin, "Implement EXPOSE_ALL_MODE");

  /* 3. If not, check if we already have some of the streams in the
   * existing active/requested selection */
  for (i = 0; i < nb; i++) {
    GstStream *stream = gst_stream_collection_get_stream (collection, i);
    const gchar *sid = gst_stream_get_stream_id (stream);
    gint request = -1;
    /* Fire select-stream signal to see if outside components want to
     * hint at which streams should be selected */
    g_signal_emit (G_OBJECT (dbin),
        gst_decodebin3_signals[SIGNAL_SELECT_STREAM], 0, collection, stream,
        &request);
    GST_DEBUG_OBJECT (dbin, "stream %s , request:%d", sid, request);
    if (request == 1 || (request == -1 && dbin->collection_posted
            && (stream_in_list (dbin->requested_selection, sid)
                || stream_in_list (dbin->active_selection, sid)))) {
      GstStreamType curtype = gst_stream_get_stream_type (stream);
      if (request == 1)
        GST_DEBUG_OBJECT (dbin,
            "Using stream requested by 'select-stream' signal : %s", sid);
      else
        GST_DEBUG_OBJECT (dbin,
            "Re-using stream already present in requested or active selection : %s",
            sid);
      tmp = g_list_append (tmp, (gchar *) sid);
      used_types |= curtype;
    }
  }

  /* 4. If not, match one stream of each type */
  for (i = 0; i < nb; i++) {
    GstStream *stream = gst_stream_collection_get_stream (collection, i);
    GstStreamType curtype = gst_stream_get_stream_type (stream);
    if (!(used_types & curtype)) {
      const gchar *sid = gst_stream_get_stream_id (stream);
      GST_DEBUG_OBJECT (dbin, "Selecting stream '%s' of type %s",
          sid, gst_stream_type_get_name (curtype));
      tmp = g_list_append (tmp, (gchar *) sid);
      used_types |= curtype;
    }
  }

beach:
  /* Finally set the requested selection */
  if (tmp) {
    if (dbin->requested_selection) {
      GST_FIXME_OBJECT (dbin,
          "Replacing non-NULL requested_selection, what should we do ??");
      g_list_free_full (dbin->requested_selection, g_free);
    }
    dbin->requested_selection =
        g_list_copy_deep (tmp, (GCopyFunc) g_strdup, NULL);
    dbin->selection_updated = TRUE;
    g_list_free (tmp);
    DUMP_SELECTION_LIST (dbin, dbin->requested_selection, "requested");
  }
}

/* sort_streams:
 * GCompareFunc to use with lists of GstStream.
 * Sorts GstStreams by stream type and SELECT flag and stream-id
 * First video, then audio, then others.
 *
 * Return: negative if a<b, 0 if a==b, positive if a>b
 */
static gint
sort_streams (GstStream * sa, GstStream * sb)
{
  GstStreamType typea, typeb;
  GstStreamFlags flaga, flagb;
  const gchar *ida, *idb;
  gint ret = 0;

  typea = gst_stream_get_stream_type (sa);
  typeb = gst_stream_get_stream_type (sb);

  GST_LOG ("sa(%s), sb(%s)", gst_stream_get_stream_id (sa),
      gst_stream_get_stream_id (sb));

  /* Sort by stream type. First video, then audio, then others(text, container, unknown) */
  if (typea != typeb) {
    if (typea & GST_STREAM_TYPE_VIDEO)
      ret = -1;
    else if (typea & GST_STREAM_TYPE_AUDIO)
      ret = (!(typeb & GST_STREAM_TYPE_VIDEO)) ? -1 : 1;
    else if (typea & GST_STREAM_TYPE_TEXT)
      ret = (!(typeb & GST_STREAM_TYPE_VIDEO)
          && !(typeb & GST_STREAM_TYPE_AUDIO)) ? -1 : 1;
    else if (typea & GST_STREAM_TYPE_CONTAINER)
      ret = (typeb & GST_STREAM_TYPE_UNKNOWN) ? -1 : 1;
    else
      ret = 1;
  }

  if (ret != 0) {
    GST_LOG ("Sort by stream-type: %d", ret);
    return ret;
  }

  /* Sort by SELECT flag, if stream type is same. */
  flaga = gst_stream_get_stream_flags (sa);
  flagb = gst_stream_get_stream_flags (sb);

  ret =
      (flaga & GST_STREAM_FLAG_SELECT) ? ((flagb & GST_STREAM_FLAG_SELECT) ? 0 :
      -1) : ((flagb & GST_STREAM_FLAG_SELECT) ? 1 : 0);

  if (ret != 0) {
    GST_LOG ("Sort by SELECT flag: %d", ret);
    return ret;
  }

  /* Sort by stream-id, if otherwise the same. */
  ida = gst_stream_get_stream_id (sa);
  idb = gst_stream_get_stream_id (sb);
  ret = (ida) ? ((idb) ? (strcmp (ida, idb) > 0 ? 1 : -1) : -1) : 1;

  GST_LOG ("Sort by stream-id: %d", ret);

  return ret;
}

/* Call with SELECTION_LOCK taken */
static GstStreamCollection *
get_active_collection (GstDecodebin3 * dbin)
{
  GstStreamCollection *collection;
  GList *iter;
  GList *unsorted_streams = NULL;
#ifndef GST_DISABLE_GST_DEBUG
  guint i;
#endif

  collection = gst_stream_collection_new ("decodebin3");

  for (iter = dbin->slots; iter; iter = g_list_next (iter)) {
    MultiQueueSlot *slot = (MultiQueueSlot *) iter->data;
    GstStream *active_stream = slot->active_stream;

    if (active_stream)
      unsorted_streams = g_list_append (unsorted_streams, active_stream);
  }

  /* re-order streams : video, then audio, then others */
  unsorted_streams =
      g_list_sort (unsorted_streams, (GCompareFunc) sort_streams);

  for (iter = unsorted_streams; iter; iter = iter->next) {
    GstStream *stream = (GstStream *) iter->data;
    GST_DEBUG_OBJECT (dbin, "Adding #stream(%s) to collection",
        gst_stream_get_stream_id (stream));
    gst_stream_collection_add_stream (collection, gst_object_ref (stream));
  }

  if (unsorted_streams)
    g_list_free (unsorted_streams);

#ifndef GST_DISABLE_GST_DEBUG
  GST_DEBUG ("Active collection");
  GST_DEBUG ("  %d streams", gst_stream_collection_get_size (collection));
  for (i = 0; i < gst_stream_collection_get_size (collection); i++) {
    GstStream *stream = gst_stream_collection_get_stream (collection, i);
    GstTagList *taglist;
    GstCaps *caps;

    GST_DEBUG ("   Stream '%s'", gst_stream_get_stream_id (stream));
    GST_DEBUG ("     type  : %s",
        gst_stream_type_get_name (gst_stream_get_stream_type (stream)));
    GST_DEBUG ("     flags : 0x%x", gst_stream_get_stream_flags (stream));
    taglist = gst_stream_get_tags (stream);
    GST_DEBUG ("     tags  : %" GST_PTR_FORMAT, taglist);
    caps = gst_stream_get_caps (stream);
    GST_DEBUG ("     caps  : %" GST_PTR_FORMAT, caps);
    if (taglist)
      gst_tag_list_unref (taglist);
    if (caps)
      gst_caps_unref (caps);
  }
#endif

  if (!gst_stream_collection_get_size (collection)) {
    gst_object_unref (collection);
    collection = NULL;
  }

  return collection;
}

static void
priv_decodebin3_handle_message (GstDecodebin3 * dbin, GstMessage * message,
    gboolean * steal, gboolean * do_post)
{
  switch (GST_MESSAGE_TYPE (message)) {
    case GST_MESSAGE_STREAM_COLLECTION:
    {
      GstStreamCollection *collection = NULL;
      GstMessage *new_msg = NULL;

      SELECTION_LOCK (dbin);
      GST_DEBUG_OBJECT (dbin, "Steal %s", GST_MESSAGE_TYPE_NAME (message));

      *steal = TRUE;
      *do_post = FALSE;

      gst_message_parse_stream_collection (message, &collection);
      SELECTION_UNLOCK (dbin);

      if (collection) {
        DecodebinInput *input = NULL;
        INPUT_LOCK (dbin);
        input =
            find_message_parsebin (dbin,
            (GstElement *) GST_MESSAGE_SRC (message));
        INPUT_UNLOCK (dbin);

        if (input) {
          PARSE_INPUT_LOCK (input);
          SELECTION_LOCK (dbin);
          INPUT_LOCK (dbin);
          handle_stream_collection (dbin, collection,
              (GstElement *) GST_MESSAGE_SRC (message));
          INPUT_UNLOCK (dbin);
          SELECTION_UNLOCK (dbin);
          PARSE_INPUT_UNLOCK (input);
        }
      }

      SELECTION_LOCK (dbin);
      /* Never post collection here */
      if (dbin->request_resource) {
        gst_message_unref (message);
        if (collection)
          gst_object_unref (collection);
        SELECTION_UNLOCK (dbin);
        return;
      }

      if (dbin->collection && collection != dbin->collection) {
        /* Replace collection message, we most likely aggregated it */
        new_msg =
            gst_message_new_stream_collection ((GstObject *) dbin,
            dbin->collection);
        gst_message_unref (message);
        GST_FIXME_OBJECT (dbin, "Merged Collection: size(%d)",
            gst_stream_collection_get_size (dbin->collection));
      }
      SELECTION_UNLOCK (dbin);

      if (collection)
        gst_object_unref (collection);

      GST_BIN_CLASS (parent_class)->handle_message ((GstBin *) dbin, new_msg);

      break;
    }
    case GST_MESSAGE_APPLICATION:
    {
      const GstStructure *msg_structure = gst_message_get_structure (message);
      GList *tmp = NULL;

      if (!g_strrstr (gst_structure_get_name (msg_structure),
              "configured-decoder"))
        break;

      GST_DEBUG_OBJECT (dbin, "Configured decoder %" GST_PTR_FORMAT,
          GST_MESSAGE_SRC (message));

      SELECTION_LOCK (dbin);
      *steal = TRUE;
      for (tmp = dbin->output_streams; tmp; tmp = tmp->next) {
        DecodebinOutputStream *output = (DecodebinOutputStream *) tmp->data;
        if (output->decoder) {
          gchar *decname = NULL;
          decname = gst_element_get_name (output->decoder);
          GST_DEBUG_OBJECT (dbin, "output->decoder %p, from %p",
              output->decoder, GST_MESSAGE_SRC (message));
          if (g_str_has_prefix (decname, "decproxy")
              && GST_MESSAGE_SRC (message) == GST_OBJECT (output->decoder)) {
            output->puppet_done = TRUE;
          }
          g_free (decname);
        }
      }

      if (!dbin->request_resource && is_request_resource_done (dbin)) {
        /* Send request resource message */
        GST_SYS_DEBUG_OBJECT (dbin, "posting request-resource message");
        dbin->request_resource = TRUE;
        SELECTION_UNLOCK (dbin);
        if (!gst_element_post_message (GST_ELEMENT_CAST (dbin),
                gst_message_new_application (GST_OBJECT_CAST (dbin),
                    gst_structure_new_empty ("request-resource"))))
          GST_ERROR_OBJECT (dbin, "ERROR: Send request resource message");
      } else
        SELECTION_UNLOCK (dbin);

      /* Never post 'configured-decoder' msg */
      *do_post = FALSE;
      gst_message_unref (message);
      break;
    }
    default:
      break;
  }
}

/* Returns SELECTED_STREAMS message if active_selection is equal to
 * requested_selection, else NULL.
 * Must be called with LOCK taken */
static GstMessage *
priv_is_selection_done (GstDecodebin3 * dbin)
{
  GList *tmp;
  GstMessage *msg;

  if (!dbin->selection_updated)
    return NULL;

  if (!dbin->collection_posted)
    return NULL;

  GST_LOG_OBJECT (dbin, "Checking");

  if (dbin->to_activate != NULL) {
    GST_DEBUG ("Still have streams to activate");
    DUMP_SELECTION_LIST (dbin, dbin->to_activate, "<to_activate>");
    return NULL;
  }
  for (tmp = dbin->requested_selection; tmp; tmp = tmp->next) {
    GST_DEBUG ("Checking requested stream %s", (gchar *) tmp->data);
    if (!stream_in_list (dbin->active_selection, (gchar *) tmp->data)) {
      GST_DEBUG ("Not in active selection, returning");
      return NULL;
    }
  }

  GST_DEBUG_OBJECT (dbin, "Selection active, creating message");

  /* We are completely active */
  msg = gst_message_new_streams_selected ((GstObject *) dbin,
      dbin->active_collection);
  GST_MESSAGE_SEQNUM (msg) = dbin->select_streams_seqnum;
  for (tmp = dbin->output_streams; tmp; tmp = tmp->next) {
    DecodebinOutputStream *output = (DecodebinOutputStream *) tmp->data;
    if (output->slot) {
      GST_DEBUG_OBJECT (dbin, "Adding stream %s",
          gst_stream_get_stream_id (output->slot->active_stream));

      gst_message_streams_selected_add (msg, output->slot->active_stream);
      if (output->slot->old_stream) {
        const gchar *sid = gst_stream_get_stream_id (output->slot->old_stream);
        GST_DEBUG_OBJECT (dbin, "Remove old stream %s", sid);
        gst_object_unref (output->slot->old_stream);
        output->slot->old_stream = NULL;
      }
    } else
      GST_WARNING_OBJECT (dbin, "No valid slot for output %p", output);
  }
  dbin->selection_updated = FALSE;
  return msg;
}

static const gchar *
collection_has_stream (GstStreamCollection * collection, gchar * sid)
{
  guint i, len;

  len = gst_stream_collection_get_size (collection);
  for (i = 0; i < len; i++) {
    GstStream *stream = gst_stream_collection_get_stream (collection, i);
    const gchar *osid = gst_stream_get_stream_id (stream);
    if (!g_strcmp0 (sid, osid))
      return osid;
  }

  return NULL;
}

/* Must be called with SELECTION_LOCK taken */
static void
check_backend_needs_gap (GstDecodebin3 * dbin, MultiQueueSlot * slot,
    gboolean was_drained)
{
  GstElement *top = NULL;
  GstPipeline *pipe = NULL;
  GstStreamType stream_type = GST_STREAM_TYPE_UNKNOWN;
  GstStream *active_stream = NULL;
  DecodebinOutputStream *output = NULL;

  GST_FIXME_OBJECT (slot->src_pad, "Whether to checking GAP needs or not");

  output = slot->output;
  if (!output) {
    GST_LOG_OBJECT (slot->src_pad, "Not having output");
    return;
  }

  active_stream = slot->active_stream;
  if (!active_stream) {
    GST_LOG_OBJECT (slot->src_pad, "Not having active stream");
    return;
  }

  stream_type = gst_stream_get_stream_type (active_stream);
  if (!(stream_type & GST_STREAM_TYPE_VIDEO
          || stream_type & GST_STREAM_TYPE_AUDIO)) {
    GST_LOG_OBJECT (slot->src_pad, "Don't consider %s type",
        gst_stream_type_get_name (stream_type));
    return;
  }

  top = (GstElement *) dbin;
  while (GST_ELEMENT_PARENT (top))
    top = GST_ELEMENT_PARENT (top);

  pipe = (GstPipeline *) top;

  if (!pipe || GST_STATE_RETURN ((GstElement *) pipe) != GST_STATE_CHANGE_ASYNC) {
    GST_LOG_OBJECT (slot->src_pad,
        "Don't consider not in asynchronous state (%s:%s)",
        (pipe ? GST_OBJECT_NAME (pipe) : "NULL"),
        (pipe ?
            gst_element_state_change_return_get_name (GST_STATE_RETURN (
                    (GstElement *) pipe)) : "NULL"));
    return;
  }

  /* FIXME: How am I aware under 'ustream change(DSC)' ?? */
  if (dbin->collection && dbin->active_collection) {
    guint i;
    guint nb_streams;

    if (gst_stream_collection_get_size (dbin->collection) !=
        gst_stream_collection_get_size (dbin->active_collection)) {
      GST_LOG_OBJECT (slot->src_pad, "Active collection is not ready");
      return;
    }

    nb_streams = gst_stream_collection_get_size (dbin->active_collection);
    for (i = 0; i < nb_streams; i++) {
      GstStream *stream =
          gst_stream_collection_get_stream (dbin->active_collection, i);
      const gchar *sid = gst_stream_get_stream_id (stream);
      if (!collection_has_stream (dbin->collection, sid)) {
        GST_LOG_OBJECT (slot->src_pad,
            "(stream-id:%s) is not in dbin->collection", sid);
        return;
      }
    }

    nb_streams = gst_stream_collection_get_size (dbin->collection);
    for (i = 0; i < nb_streams; i++) {
      GstStream *stream =
          gst_stream_collection_get_stream (dbin->collection, i);
      const gchar *sid = gst_stream_get_stream_id (stream);
      if (!collection_has_stream (dbin->active_collection, sid)) {
        GST_LOG_OBJECT (slot->src_pad,
            "(stream-id:%s) is not in dbin->active_collection", sid);
        return;
      }
    }
  }

  /* FIXME: Consider to send GAP event to backend of decodebin3, because of no valid data */
  GST_DEBUG_OBJECT (slot->src_pad, "%s is in asynchronous state",
      GST_OBJECT_NAME (pipe));

  if (output && output->src_pad) {
    GstPad *peer = gst_pad_get_peer (output->src_pad);
    if (peer) {
      GstEvent *output_ss =
          gst_pad_get_sticky_event (peer, GST_EVENT_STREAM_START, 0);
      GstEvent *output_caps = NULL;
      GstEvent *output_seg = NULL;

      GST_SYS_WARNING_OBJECT (slot->src_pad,
          "(%s:%s) Consider to send GAP event to backend of decodebin3, because of no valid data",
          GST_DEBUG_PAD_NAME (peer));
      if (!output_ss) {
        GstEvent *input_ss = gst_pad_get_sticky_event (slot->src_pad,
            GST_EVENT_STREAM_START, 0);
        GST_SYS_WARNING_OBJECT (slot->src_pad,
            "(%s:%s) Sending missed stream-start (%p:%s) !!",
            GST_DEBUG_PAD_NAME (peer), input_ss,
            gst_stream_get_stream_id (active_stream));
        SELECTION_UNLOCK (dbin);
        gst_pad_send_event (peer, input_ss);
        SELECTION_LOCK (dbin);
        GST_SYS_WARNING_OBJECT (slot->src_pad,
            "(%s:%s) Sent missed stream-start !!", GST_DEBUG_PAD_NAME (peer));
      } else {
        gst_event_unref (output_ss);
      }

      output_caps = gst_pad_get_sticky_event (peer, GST_EVENT_CAPS, 0);
      if (!output_caps) {
        GstCaps *incaps = (GstCaps *) gst_stream_get_caps (active_stream);
        GstCaps *outcaps = NULL;
        if (incaps) {
          if (stream_type == GST_STREAM_TYPE_AUDIO)
            outcaps =
                negotiate_default_caps (dbin, incaps, output->src_pad,
                stream_type);
          else
            outcaps = gst_caps_new_empty_simple ("video/x-raw");
          GST_SYS_WARNING_OBJECT (slot->src_pad,
              "active: %d, eos: %d, flushing: %d", GST_PAD_IS_ACTIVE (peer),
              GST_PAD_IS_EOS (peer), GST_PAD_IS_FLUSHING (peer));
          GST_SYS_WARNING_OBJECT (slot->src_pad,
              "(%s:%s) Sending missed caps: %" GST_PTR_FORMAT,
              GST_DEBUG_PAD_NAME (peer), outcaps);
          SELECTION_UNLOCK (dbin);
          gst_pad_send_event (peer, gst_event_new_caps (outcaps));
          SELECTION_LOCK (dbin);
          GST_WARNING_OBJECT (slot->src_pad, "(%s:%s) Sent missed caps !!",
              GST_DEBUG_PAD_NAME (peer));
          gst_caps_unref (incaps);
        }
      } else {
        gst_event_unref (output_caps);
      }

      output_seg = gst_pad_get_sticky_event (peer, GST_EVENT_SEGMENT, 0);
      if (!output_seg && output->segment.format != GST_FORMAT_UNDEFINED) {
        GST_WARNING_OBJECT (slot->src_pad,
            "active: %d, eos: %d, flushing: %d", GST_PAD_IS_ACTIVE (peer),
            GST_PAD_IS_EOS (peer), GST_PAD_IS_FLUSHING (peer));
        GST_SYS_WARNING_OBJECT (slot->src_pad,
            "(%s:%s) Sending missed segment: %" GST_SEGMENT_FORMAT,
            GST_DEBUG_PAD_NAME (peer), &output->segment);
        SELECTION_UNLOCK (dbin);
        gst_pad_send_event (peer, gst_event_new_segment (&output->segment));
        SELECTION_LOCK (dbin);
        GST_SYS_WARNING_OBJECT (slot->src_pad, "(%s:%s) Sent missed segment !!",
            GST_DEBUG_PAD_NAME (peer));
      } else {
        gst_event_unref (output_seg);
      }

      if (!was_drained) {
        GST_SYS_WARNING_OBJECT (slot->src_pad, "(%s:%s) Sending GAP !!",
            GST_DEBUG_PAD_NAME (peer));
        SELECTION_UNLOCK (dbin);
        /* FIXME: Do we need exactly start and duration for gap ?? */
        gst_pad_send_event (peer, gst_event_new_gap (0, GST_CLOCK_TIME_NONE));
        SELECTION_LOCK (dbin);
        GST_SYS_WARNING_OBJECT (slot->src_pad, "(%s:%s) Sent GAP !!",
            GST_DEBUG_PAD_NAME (peer));
      }
      gst_object_unref (peer);
    }
  }
}

static GstPadProbeReturn
priv_multiqueue_src_probe (GstPad * pad, GstPadProbeInfo * info,
    MultiQueueSlot * slot)
{
  GstPadProbeReturn ret = GST_PAD_PROBE_OK;
  GstDecodebin3 *dbin = slot->dbin;

  if (GST_IS_EVENT (GST_PAD_PROBE_INFO_DATA (info))) {
    GstEvent *ev = GST_PAD_PROBE_INFO_EVENT (info);

    GST_DEBUG_OBJECT (pad, "Got event %p %s", ev, GST_EVENT_TYPE_NAME (ev));
    switch (GST_EVENT_TYPE (ev)) {
      case GST_EVENT_STREAM_START:
      {
        GstStream *stream = NULL;
        const gchar *stream_id;
        GstMessage *msg = NULL;
        const GstStructure *s = gst_event_get_structure (ev);
        DecodebinOutputStream *output;

        /* Drop STREAM_START events used to cleanup multiqueue */
        if (s
            && gst_structure_has_field (s,
                "decodebin3-flushing-stream-start")) {
          ret = GST_PAD_PROBE_HANDLED;
          gst_event_unref (ev);
          break;
        }

        gst_event_parse_stream (ev, &stream);
        if (stream == NULL) {
          GST_ERROR_OBJECT (pad,
              "Got a STREAM_START event without a GstStream");
          break;
        }
        slot->is_drained = FALSE;
        stream_id = gst_stream_get_stream_id (stream);
        GST_DEBUG_OBJECT (pad, "Stream Start '%s'", stream_id);
        if (slot->active_stream == NULL) {
          SELECTION_LOCK (dbin);
          slot->active_stream = stream;
          msg = update_active_collection (slot, TRUE);
          SELECTION_UNLOCK (dbin);
          if (msg) {
            GST_DEBUG_OBJECT (dbin, "Posting collection");
            gst_element_post_message ((GstElement *) slot->dbin, msg);
          }
        } else if (slot->active_stream != stream) {
          const gchar *last_sid =
              gst_stream_get_stream_id (slot->active_stream);
          GST_FIXME_OBJECT (pad, "Handle stream changes (%s => %s) !",
              last_sid, stream_id);

          SELECTION_LOCK (dbin);
          if (stream_in_list (dbin->active_selection, last_sid)) {
            GST_DEBUG_OBJECT (pad, "Update active selection");
            dbin->active_selection =
                g_list_remove (dbin->active_selection, last_sid);
            dbin->active_selection =
                g_list_append (dbin->active_selection, (gchar *) stream_id);
          }
          if ((last_sid = stream_in_list (dbin->requested_selection, last_sid))
              != NULL) {
            dbin->requested_selection =
                g_list_remove (dbin->requested_selection, last_sid);
            g_free ((gchar *) last_sid);
            dbin->requested_selection =
                g_list_append (dbin->requested_selection, g_strdup (stream_id));
            dbin->selection_updated = TRUE;
          }
          if (G_UNLIKELY (slot->old_stream))
            gst_object_unref (slot->old_stream);
          slot->old_stream = slot->active_stream;
          slot->active_stream = stream;
          msg = update_active_collection (slot, FALSE);
          SELECTION_UNLOCK (dbin);
          if (msg) {
            GST_DEBUG_OBJECT (dbin, "Posting collection");
            gst_element_post_message ((GstElement *) slot->dbin, msg);
          }
        } else
          gst_object_unref (stream);
#if 0                           /* Disabled because stream-start is pushed for every buffer on every unlinked pad */
        {
          gboolean is_active, is_requested;
          /* Quick check to see if we're in the current selection */
          /* FIXME : Re-check all slot<=>output mappings based on requested_selection */
          SELECTION_LOCK (dbin);
          GST_DEBUG_OBJECT (dbin, "Checking active selection");
          is_active = stream_in_list (dbin->active_selection, stream_id);
          GST_DEBUG_OBJECT (dbin, "Checking requested selection");
          is_requested = stream_in_list (dbin->requested_selection, stream_id);
          SELECTION_UNLOCK (dbin);
          if (is_active)
            GST_DEBUG_OBJECT (pad, "Slot in ACTIVE selection (output:%p)",
                slot->output);
          if (is_requested)
            GST_DEBUG_OBJECT (pad, "Slot in REQUESTED selection (output:%p)",
                slot->output);
          else if (slot->output) {
            GST_DEBUG_OBJECT (pad,
                "Slot needs to be deactivated ? It's no longer in requested selection");
          } else if (!is_active)
            GST_DEBUG_OBJECT (pad,
                "Slot in neither active nor requested selection");
        }
#endif
        SELECTION_LOCK (dbin);
        output = get_output_for_slot (slot);
        SELECTION_UNLOCK (dbin);
        GST_SYS_DEBUG_OBJECT (pad, "Got STREAM-START");
        if (output) {
          GST_DEBUG_OBJECT (pad, "Reset last pushed PTS");
          output->last_pushed_ts = GST_CLOCK_TIME_NONE;
          gst_segment_init (&output->segment, GST_FORMAT_UNDEFINED);
        } else {
          GST_WARNING_OBJECT (pad, "No output ???");
        }
      }
        break;
      case GST_EVENT_CAPS:
      {
        /* Configure the output slot if needed */
        DecodebinOutputStream *output;
        GstMessage *msg = NULL;
        GstCaps *caps = NULL;
        DecodebinInput *input = NULL;
        DecodebinInput *after_input = NULL;
        gboolean need_input_lock = FALSE;

        SELECTION_LOCK (dbin);
        input = get_input_for_slot (dbin, slot);
        SELECTION_UNLOCK (dbin);

        if (input) {
          need_input_lock = TRUE;
          PARSE_INPUT_LOCK (input);
        }

        SELECTION_LOCK (dbin);
        gst_event_parse_caps (ev, &caps);
        GST_DEBUG_OBJECT (dbin, "caps %" GST_PTR_FORMAT, caps);
        if (G_LIKELY (slot->active_stream))
          gst_stream_set_caps (slot->active_stream, caps);
        output = get_output_for_slot (slot);
        if (output) {
          slot->plugging_decoder = TRUE;
          priv_reconfigure_output_stream (output, slot, FALSE);
          slot->plugging_decoder = FALSE;
          msg = priv_is_selection_done (dbin);
        }
        after_input = get_input_for_slot (dbin, slot);
        if (input && after_input && after_input != input) {
          GST_WARNING_OBJECT (pad, "parsebin(%s -> %s) is changed !!",
              (input->parsebin ? GST_ELEMENT_NAME (input->parsebin) : "<NONE>"),
              (after_input->
                  parsebin ? GST_ELEMENT_NAME (after_input->parsebin) :
                  "<NONE>"));
        } else if (!input || input && !after_input) {
          GST_WARNING_OBJECT (pad, "No parsebin !!");
        }
        SELECTION_UNLOCK (dbin);
        if (need_input_lock && input && input == after_input) {
          PARSE_INPUT_UNLOCK (input);
        }
        if (msg)
          gst_element_post_message ((GstElement *) slot->dbin, msg);
        SELECTION_LOCK (dbin);
        if (!dbin->request_resource && is_request_resource_done (dbin)) {
          /* Send request resource message */
          GST_SYS_DEBUG_OBJECT (dbin, "posting request-resource message");
          dbin->request_resource = TRUE;
          SELECTION_UNLOCK (dbin);
          if (!gst_element_post_message (GST_ELEMENT_CAST (dbin),
                  gst_message_new_application (GST_OBJECT_CAST (dbin),
                      gst_structure_new_empty ("request-resource"))))
            GST_ERROR_OBJECT (dbin, "ERROR: Send request resource message");
        } else
          SELECTION_UNLOCK (dbin);
      }
        break;
      case GST_EVENT_EOS:
      {
        gboolean was_drained = slot->is_drained;
        slot->is_drained = TRUE;

        /* Custom EOS handling first */
        if (gst_mini_object_get_qdata (GST_MINI_OBJECT_CAST (ev),
                CUSTOM_EOS_QUARK)) {
          DecodebinInput *input = NULL;
          DecodebinInput *after_input = NULL;
          gboolean need_input_lock = FALSE;

          /* remove custom-eos */
          gst_mini_object_set_qdata (GST_MINI_OBJECT_CAST (ev),
              CUSTOM_EOS_QUARK, NULL, NULL);
          GST_SYS_LOG_OBJECT (pad, "Received custom EOS");
          ret = GST_PAD_PROBE_HANDLED;

          SELECTION_LOCK (dbin);
          INPUT_LOCK (dbin);
          input = get_input_for_slot (dbin, slot);

          if (input && input->parsebin
              && GST_OBJECT_PARENT (GST_OBJECT (input->parsebin)) ==
              GST_OBJECT (dbin) && input->collection) {
            need_input_lock = TRUE;
            INPUT_UNLOCK (dbin);
            SELECTION_UNLOCK (dbin);
            PARSE_INPUT_LOCK (input);
            SELECTION_LOCK (dbin);
            INPUT_LOCK (dbin);
          }

          after_input = get_input_for_slot (dbin, slot);
          if (input && after_input && after_input != input) {
            GST_WARNING_OBJECT (pad, "parsebin(%s -> %s) is changed !!",
                (input->
                    parsebin ? GST_ELEMENT_NAME (input->parsebin) : "<NONE>"),
                (after_input->
                    parsebin ? GST_ELEMENT_NAME (after_input->parsebin) :
                    "<NONE>"));
          } else if (!input || input && !after_input) {
            GST_WARNING_OBJECT (pad, "No parsebin !!");
          }

          INPUT_UNLOCK (dbin);
          if (need_input_lock && input && input == after_input) {
            PARSE_INPUT_UNLOCK (input);
          }
          if (slot->input == NULL) {
            GstMessage *msg = NULL;
            GST_DEBUG_OBJECT (pad,
                "Got custom-eos from null input stream, remove output stream");

            /* Update collection to remove current stream from collection */
            if (slot->active_stream) {
              gst_object_unref (slot->active_stream);
              slot->active_stream = NULL;
            }
            msg = update_active_collection (slot, TRUE);
            SELECTION_UNLOCK (dbin);
            if (msg) {
              GST_DEBUG_OBJECT (dbin, "Posting collection");
              gst_element_post_message ((GstElement *) slot->dbin, msg);
            }
            SELECTION_LOCK (dbin);
            /* Remove the output */
            if (slot->output) {
              DecodebinOutputStream *output = slot->output;
              dbin->output_streams =
                  g_list_remove (dbin->output_streams, output);
              free_output_stream (dbin, output);
            }
            slot->probe_id = 0;
            if (slot->upstream_probe_id)
              gst_pad_remove_probe (slot->src_pad, slot->upstream_probe_id);
            slot->upstream_probe_id = 0;
            dbin->slots = g_list_remove (dbin->slots, slot);
            if (!slot->mark_async_pool) {
              slot->mark_async_pool = TRUE;
              free_multiqueue_slot_async (dbin, slot);
            }
            ret = GST_PAD_PROBE_REMOVE;
          } else {
            if (dbin->use_fallback_preroll)
              check_backend_needs_gap (dbin, slot, was_drained);
            if (!was_drained)
              check_all_slot_for_eos (dbin);
          }
          SELECTION_UNLOCK (dbin);
          break;
        }

        GST_FIXME_OBJECT (pad, "EOS on multiqueue source pad. input:%p",
            slot->input);
        if (slot->input == NULL) {
          GstPad *peer;
          GstMessage *msg = NULL;
          GST_DEBUG_OBJECT (pad,
              "last EOS for input, forwarding and removing slot");
          peer = gst_pad_get_peer (pad);
          if (peer) {
            gst_pad_send_event (peer, ev);
            gst_object_unref (peer);
          } else {
            gst_event_unref (ev);
          }
          SELECTION_LOCK (dbin);
          /* FIXME : Shouldn't we try to re-assign the output instead of just
           * removing it ? */

          /* Update collection to remove current stream from collection */
          if (slot->active_stream) {
            gst_object_unref (slot->active_stream);
            slot->active_stream = NULL;
          }
          msg = update_active_collection (slot, TRUE);
          SELECTION_UNLOCK (dbin);
          if (msg) {
            GST_DEBUG_OBJECT (dbin, "Posting collection");
            gst_element_post_message ((GstElement *) slot->dbin, msg);
          }
          SELECTION_LOCK (dbin);
          /* Remove the output */
          if (slot->output) {
            DecodebinOutputStream *output = slot->output;
            dbin->output_streams = g_list_remove (dbin->output_streams, output);
            free_output_stream (dbin, output);
          }
          slot->probe_id = 0;
          if (slot->upstream_probe_id)
            gst_pad_remove_probe (slot->src_pad, slot->upstream_probe_id);
          slot->upstream_probe_id = 0;
          dbin->slots = g_list_remove (dbin->slots, slot);
          if (!slot->mark_async_pool) {
            slot->mark_async_pool = TRUE;
            SELECTION_UNLOCK (dbin);
            free_multiqueue_slot_async (dbin, slot);
            SELECTION_LOCK (dbin);
          }
          SELECTION_UNLOCK (dbin);
          ret = GST_PAD_PROBE_REMOVE;
        } else if (gst_mini_object_get_qdata (GST_MINI_OBJECT_CAST (ev),
                CUSTOM_FINAL_EOS_QUARK)) {
          GST_SYS_DEBUG_OBJECT (pad, "Got final eos, propagating downstream");
        } else {
          GST_SYS_DEBUG_OBJECT (pad, "Got regular eos (all_inputs_are_eos)");
          /* drop current event as eos will be sent in check_all_slot_for_eos
           * when all output streams are also eos */
          ret = GST_PAD_PROBE_DROP;
          SELECTION_LOCK (dbin);
          if (!check_all_slot_for_eos (dbin)) {
            if (dbin->use_fallback_preroll)
              check_backend_needs_gap (dbin, slot, TRUE);
          }
          SELECTION_UNLOCK (dbin);
        }
      }
        break;
      case GST_EVENT_SEGMENT:
      {
        DecodebinOutputStream *output;
        const GstSegment *segment;
        SELECTION_LOCK (dbin);
        output = get_output_for_slot (slot);
        SELECTION_UNLOCK (dbin);
        gst_event_parse_segment (ev, &segment);

        if (output && segment) {
          if (output->segment.format == GST_FORMAT_UNDEFINED) {
            if (segment->format == GST_FORMAT_BYTES) {
              GST_WARNING_OBJECT (pad, "Can only operate non-BYTES format");
              gst_event_unref (ev);
              ret = GST_PAD_PROBE_HANDLED;
              goto done;
            }
          } else if (output->segment.format != segment->format) {
            GST_WARNING_OBJECT (pad,
                "Operating in %s format but new pad has %s",
                gst_format_get_name (output->segment.format),
                gst_format_get_name (segment->format));
            gst_event_unref (ev);
            ret = GST_PAD_PROBE_HANDLED;
            goto done;
          }
        }

        if (output && segment) {
          GST_DEBUG_OBJECT (pad, "Operating in %s format",
              gst_format_get_name (segment->format));
          GST_SYS_LOG_OBJECT (pad, "New segment: %" GST_SEGMENT_FORMAT,
              segment);
          gst_segment_copy_into (segment, &output->segment);
        }
      }
        break;
      case GST_EVENT_FLUSH_START:
      {
        GST_SYS_DEBUG_OBJECT (pad, "Got FLUSH-START");
        break;
      }
      case GST_EVENT_FLUSH_STOP:
      {
        DecodebinOutputStream *output;
        SELECTION_LOCK (dbin);
        output = get_output_for_slot (slot);
        SELECTION_UNLOCK (dbin);
        GST_SYS_DEBUG_OBJECT (pad, "Got FLUSH-STOP");
        if (output) {
          GST_DEBUG_OBJECT (pad, "Reset last pushed PTS");
          output->last_pushed_ts = GST_CLOCK_TIME_NONE;
          gst_segment_init (&output->segment, GST_FORMAT_UNDEFINED);
        } else {
          GST_WARNING_OBJECT (pad, "No output ???");
        }
      }
      default:
        break;
    }
  } else if (GST_IS_QUERY (GST_PAD_PROBE_INFO_DATA (info))) {
    GstQuery *query = GST_PAD_PROBE_INFO_QUERY (info);
    switch (GST_QUERY_TYPE (query)) {
      case GST_QUERY_CAPS:
      {
        GST_DEBUG_OBJECT (pad, "Intercepting CAPS query");
        gst_query_set_caps_result (query, GST_CAPS_ANY);
        ret = GST_PAD_PROBE_HANDLED;
      }
        break;

      case GST_QUERY_ACCEPT_CAPS:
      {
        GST_DEBUG_OBJECT (pad, "Intercepting Accept Caps query");
        /* If the current decoder doesn't accept caps, we'll reconfigure
         * on the actual caps event. So accept any caps. */
        gst_query_set_accept_caps_result (query, TRUE);
        ret = GST_PAD_PROBE_HANDLED;
      }
      default:
        break;
    }
  } else if (GST_IS_BUFFER (GST_PAD_PROBE_INFO_DATA (info))) {
    GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER (info);
    DecodebinOutputStream *output;
    GstClockTimeDiff diff = GST_CLOCK_STIME_NONE;
    GstClockTime cache_time = GST_CLOCK_TIME_NONE;
    SELECTION_LOCK (dbin);
    output = get_output_for_slot (slot);
    SELECTION_UNLOCK (dbin);

    if (output && output->segment.format != GST_FORMAT_UNDEFINED) {
      GstClockTime cur_ts = GST_CLOCK_TIME_NONE;
      GstClockTime prev_ts = GST_CLOCK_TIME_NONE;

      GST_LOG_OBJECT (pad, "Got buffer: %" GST_PTR_FORMAT, buf);

      if (output->segment.rate != 1.0) {
        GST_LOG_OBJECT (pad, "PTS continuity can be monitored in 1.0 rate");
        return GST_PAD_PROBE_OK;
      }

      cur_ts = GST_BUFFER_TIMESTAMP (buf);
      prev_ts = output->last_pushed_ts;

      if (!GST_CLOCK_TIME_IS_VALID (cur_ts)
          && !GST_CLOCK_TIME_IS_VALID (prev_ts)) {
        GST_LOG_OBJECT (pad, "Both current and previous PTS should be valid");
        return GST_PAD_PROBE_OK;
      }

      /* FIXME: How to estimate caching time ??
       * 1) either buffer duration or
       * 2) time diff between current and previous or
       * 3) under maximum 250ms likes mq */
      cache_time = GST_BUFFER_DURATION (buf);

      if (GST_CLOCK_TIME_IS_VALID (cur_ts) && GST_CLOCK_TIME_IS_VALID (prev_ts)) {
        diff =
            (prev_ts > cur_ts) ? GST_CLOCK_DIFF (cur_ts,
            prev_ts) : GST_CLOCK_DIFF (prev_ts, cur_ts);
        GST_LOG_OBJECT (pad,
            "Current PTS: (%" GST_TIME_FORMAT "), Previously seen PTS: (%"
            GST_TIME_FORMAT "), diff: (%c%" GST_TIME_FORMAT ")",
            GST_TIME_ARGS (cur_ts), GST_TIME_ARGS (prev_ts),
            (prev_ts > cur_ts ? '-' : '+'), GST_TIME_ARGS (diff));

        if (!GST_CLOCK_TIME_IS_VALID (cache_time))
          cache_time =
              GST_CLOCK_STIME_IS_VALID (diff) ? (diff >
              250 * GST_MSECOND ? 250 * GST_MSECOND : diff) : 250 * GST_MSECOND;

        if (prev_ts > cur_ts) {
          GST_WARNING_OBJECT (pad,
              "Current PTS: (%" GST_TIME_FORMAT
              ") is not continuity, dropping ..., prev PTS (%" GST_TIME_FORMAT
              "), diff: (%c%" GST_TIME_FORMAT ")", GST_TIME_ARGS (cur_ts),
              GST_TIME_ARGS (prev_ts), (prev_ts > cur_ts ? '-' : '+'),
              GST_TIME_ARGS (diff));
          return GST_PAD_PROBE_DROP;
        } else if (diff < cache_time || diff > cache_time) {
          GST_WARNING_OBJECT (pad,
              "Current PTS: (%" GST_TIME_FORMAT
              ") is out of cached time, prev PTS (%" GST_TIME_FORMAT
              "), diff: (+%" GST_TIME_FORMAT ")", GST_TIME_ARGS (cur_ts),
              GST_TIME_ARGS (prev_ts), GST_TIME_ARGS (diff));
          // return GST_PAD_PROBE_DROP;
        }
      }

      if (!GST_CLOCK_TIME_IS_VALID (cache_time))
        cache_time = 250 * GST_MSECOND;
      GST_LOG_OBJECT (pad, "Estimate cache time: (%" GST_TIME_FORMAT ")",
          GST_TIME_ARGS (cache_time));

      if (GST_CLOCK_TIME_IS_VALID (cur_ts)) {
        prev_ts = cur_ts;
      } else if (GST_CLOCK_TIME_IS_VALID (prev_ts)) {
        GST_LOG_OBJECT (pad,
            "Assume current PTS based on previous PTS + cached-time");
        prev_ts += cache_time;
      }

      if (GST_CLOCK_TIME_IS_VALID (prev_ts)) {
        output->last_pushed_ts = prev_ts;
        GST_LOG_OBJECT (pad, "Lastly pushed PTS: %" GST_TIME_FORMAT,
            GST_TIME_ARGS (prev_ts));
      }
    }
  }

done:
  return ret;
}

static GstPadProbeReturn
multiqueue_upstream_src_probe (GstPad * pad, GstPadProbeInfo * info,
    MultiQueueSlot * slot)
{
  GstPadProbeReturn ret = GST_PAD_PROBE_OK;
  GstDecodebin3 *dbin = slot->dbin;

  if (GST_IS_EVENT (GST_PAD_PROBE_INFO_DATA (info))) {
    GstEvent *ev = GST_PAD_PROBE_INFO_EVENT (info);
    switch (GST_QUERY_TYPE (ev)) {
      case GST_EVENT_SEEK:
      {
        GST_SYS_DEBUG_OBJECT (pad, "Got SEEK event: %" GST_PTR_FORMAT, ev);
      }
        break;
      default:
        break;
    }
  } else if (GST_IS_QUERY (GST_PAD_PROBE_INFO_DATA (info))) {
    GstQuery *query = GST_PAD_PROBE_INFO_QUERY (info);

    switch (GST_QUERY_TYPE (query)) {
      case GST_QUERY_CAPS:
      {
        if (ongoing_change_upstream (dbin, slot)) {
          if (slot->active_stream) {
            GstCaps *slot_caps = gst_stream_get_caps (slot->active_stream);
            if (slot_caps) {
              GST_DEBUG_OBJECT (slot->src_pad,
                  "Intercepting CAPS query: %" GST_PTR_FORMAT, slot_caps);
              gst_query_set_caps_result (query, slot_caps);
              gst_caps_unref (slot_caps);
              ret = GST_PAD_PROBE_HANDLED;
            }
          }
        }
        break;
      }
#if 0
      case GST_QUERY_ACCEPT_CAPS:
      {
        GST_DEBUG_OBJECT (pad, "Intercepting Accept Caps query");
        /* If the current decoder doesn't accept caps, we'll reconfigure
         * on the actual caps event. So accept any caps. */
        gst_query_set_accept_caps_result (query, TRUE);
        ret = GST_PAD_PROBE_HANDLED;
      }
#endif
      default:
        break;
    }
  }

  return ret;
}

static void
priv_create_new_slot (GstDecodebin3 * dbin, MultiQueueSlot * slot)
{
  GstPadProbeType ptype = GST_PAD_PROBE_TYPE_INVALID;

  if (dbin->monitor_pts_continuity && (slot->type & GST_STREAM_TYPE_AUDIO))
    ptype =
        GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM |
        GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM | GST_PAD_PROBE_TYPE_BUFFER |
        GST_PAD_PROBE_TYPE_EVENT_FLUSH;
  else
    ptype =
        GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM | GST_PAD_PROBE_TYPE_EVENT_FLUSH |
        GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM;

  slot->probe_id = gst_pad_add_probe (slot->src_pad, ptype,
      (GstPadProbeCallback) priv_multiqueue_src_probe, slot, NULL);

  slot->upstream_probe_id =
      gst_pad_add_probe (slot->src_pad,
      GST_PAD_PROBE_TYPE_EVENT_UPSTREAM | GST_PAD_PROBE_TYPE_QUERY_UPSTREAM,
      (GstPadProbeCallback) multiqueue_upstream_src_probe, slot, NULL);

  slot->mark_async_pool = FALSE;
}

static void
priv_free_multiqueue_slot (GstDecodebin3 * dbin, MultiQueueSlot * slot)
{
  GST_DEBUG_OBJECT (dbin, "Freeing slot(%p)", slot);
  if (slot->upstream_probe_id)
    gst_pad_remove_probe (slot->src_pad, slot->upstream_probe_id);
  slot->upstream_probe_id = 0;
}

static gboolean
priv_handle_stream_switch (GstDecodebin3 * dbin, GList * select_streams,
    guint32 seqnum)
{
  gboolean ret = TRUE;
  GList *tmp, *next;
  /* List of slots to (de)activate. */
  GList *to_deactivate = NULL;
  GList *to_activate = NULL;
  /* List of unknown stream id, most likely means the event
   * should be sent upstream so that elements can expose the requested stream */
  GList *unknown = NULL;
  GList *to_reassign = NULL;
  GList *future_request_streams = NULL;
  GList *pending_streams = NULL;
  GList *slots_to_reassign = NULL;
  GList *new_select_streams = g_list_copy (select_streams);;
  GList *slots_to_ignore = NULL;

  SELECTION_LOCK (dbin);
  if (G_UNLIKELY (seqnum != dbin->select_streams_seqnum)) {
    GST_DEBUG_OBJECT (dbin, "New SELECT_STREAMS has arrived in the meantime");
    SELECTION_UNLOCK (dbin);
    return TRUE;
  }
  /* Remove pending select_streams */
  g_list_free (dbin->pending_select_streams);
  dbin->pending_select_streams = NULL;

  /* COMPARE the requested streams to the active and requested streams
   * on multiqueue. */

  /* First check the slots to activate and which ones are unknown */
  for (tmp = new_select_streams; tmp; tmp = next) {
    const gchar *sid = (const gchar *) tmp->data;
    MultiQueueSlot *slot;

    next = tmp->next;

    GST_DEBUG_OBJECT (dbin, "Checking stream '%s'", sid);
    slot = find_slot_for_stream_id (dbin, sid);
    /* Find the corresponding slot */
    if (slot == NULL) {
      if (stream_in_collection (dbin, (gchar *) sid)) {
        pending_streams = g_list_append (pending_streams, (gchar *) sid);
      } else {
        GST_DEBUG_OBJECT (dbin, "We don't have a slot for stream '%s'", sid);
        unknown = g_list_append (unknown, (gchar *) sid);
      }
    } else if (slot->output == NULL && slot->active_stream) {
      if (dbin->dvr_playback && !collection_has_stream (dbin->collection, sid)
          && collection_has_stream (dbin->active_collection, sid)) {
        GST_DEBUG_OBJECT (dbin,
            "Requested stream '%s' will be changed from upstream", sid);
        slots_to_ignore = g_list_append (slots_to_ignore, slot);
      } else {
        GST_DEBUG_OBJECT (dbin, "We need to activate slot %p for stream '%s')",
            slot, sid);
        to_activate = g_list_append (to_activate, slot);
      }
    } else {
      if (slot->active_stream && slot->old_stream) {
        const gchar *old_sid, *active_sid;
        old_sid = gst_stream_get_stream_id (slot->old_stream);
        active_sid = gst_stream_get_stream_id (slot->active_stream);
        if (!g_strcmp0 (sid, old_sid)) {
          GST_DEBUG_OBJECT (dbin, "Replace requested selection stream %s to %s",
              old_sid, active_sid);
          new_select_streams = g_list_remove_link (new_select_streams, tmp);
          new_select_streams = g_list_insert_before (new_select_streams, next,
              (gchar *) active_sid);

          sid = active_sid;
        }
      }
      if (slot->output == NULL && slot->active_stream) {
        GST_DEBUG_OBJECT (dbin, "We need to activate slot %p for stream '%s')",
            slot, sid);
        to_activate = g_list_append (to_activate, slot);
      } else {
        GST_DEBUG_OBJECT (dbin,
            "Stream '%s' from slot %p is already active on output %p", sid,
            slot, slot->output);
        future_request_streams =
            g_list_append (future_request_streams, (gchar *) sid);
      }
    }
  }

  for (tmp = dbin->slots; tmp; tmp = tmp->next) {
    MultiQueueSlot *slot = (MultiQueueSlot *) tmp->data;
    /* For slots that have an output, check if it's part of the streams to
     * be active */
    if (slot->output) {
      gboolean slot_to_deactivate = TRUE;

      if (slot->active_stream) {
        if (stream_in_list (select_streams,
                gst_stream_get_stream_id (slot->active_stream))) {
          slot_to_deactivate = FALSE;
        } else if (slot->old_stream) {
          if (stream_in_list (select_streams,
                  gst_stream_get_stream_id (slot->old_stream))) {
            /* Replace requested select streams to actual activated one */
            const gchar *old_sid, *active_sid;
            old_sid = gst_stream_get_stream_id (slot->old_stream);
            active_sid = gst_stream_get_stream_id (slot->active_stream);

            GST_DEBUG_OBJECT (dbin,
                "Requested stream %s will be changed to %s, accept this anyway",
                old_sid, active_sid);
            slot_to_deactivate = FALSE;
          }
        }
      }
      if (slot_to_deactivate && slot->pending_stream
          && slot->pending_stream != slot->active_stream) {
        if (stream_in_list (select_streams,
                gst_stream_get_stream_id (slot->pending_stream)))
          slot_to_deactivate = FALSE;
      }

      if (slots_to_ignore) {
        GList *tmp2 = NULL;
        for (tmp2 = slots_to_ignore; tmp2; tmp2 = tmp2->next) {
          MultiQueueSlot *ignore_slot = (MultiQueueSlot *) tmp2->data;
          if (ignore_slot->type ==
              gst_stream_get_stream_type (slot->active_stream)) {
            const gchar *active_sid;
            active_sid = gst_stream_get_stream_id (slot->active_stream);
            GST_DEBUG_OBJECT (dbin,
                "Maintain current stream '%s' instead of requested stream '%s'",
                active_sid,
                gst_stream_get_stream_id (ignore_slot->active_stream));
            future_request_streams =
                g_list_append (future_request_streams, (gchar *) active_sid);
            slot_to_deactivate = FALSE;
          }
        }
      }

      if (slot_to_deactivate) {
        GST_DEBUG_OBJECT (dbin,
            "Slot %p (%s) should be deactivated, no longer used", slot,
            slot->active_stream ? gst_stream_get_stream_id (slot->
                active_stream) : "NULL");
        to_deactivate = g_list_append (to_deactivate, slot);
      }
    }
  }

  if (to_deactivate != NULL) {
    GST_DEBUG_OBJECT (dbin, "Check if we can reassign slots");
    /* We need to compare what needs to be activated and deactivated in order
     * to determine whether there are outputs that can be transferred */
    /* Take the stream-id of the slots that are to be activated, for which there
     * is a slot of the same type that needs to be deactivated */
    tmp = to_deactivate;
    while (tmp) {
      MultiQueueSlot *slot_to_deactivate = (MultiQueueSlot *) tmp->data;
      gboolean removeit = FALSE;
      GList *tmp2, *next;
      GST_DEBUG_OBJECT (dbin,
          "Checking if slot to deactivate (%p) has a candidate slot to activate",
          slot_to_deactivate);
      for (tmp2 = to_activate; tmp2; tmp2 = tmp2->next) {
        MultiQueueSlot *slot_to_activate = (MultiQueueSlot *) tmp2->data;
        GST_DEBUG_OBJECT (dbin, "Comparing to slot %p", slot_to_activate);
        if (slot_to_activate->type == slot_to_deactivate->type) {
          GST_DEBUG_OBJECT (dbin, "Re-using");
          to_reassign = g_list_append (to_reassign, (gchar *)
              gst_stream_get_stream_id (slot_to_activate->active_stream));
          slots_to_reassign =
              g_list_append (slots_to_reassign, slot_to_deactivate);
          to_activate = g_list_remove (to_activate, slot_to_activate);
          removeit = TRUE;
          break;
        }
      }
      next = tmp->next;
      if (removeit)
        to_deactivate = g_list_delete_link (to_deactivate, tmp);
      tmp = next;
    }
  }

  for (tmp = to_deactivate; tmp; tmp = tmp->next) {
    MultiQueueSlot *slot = (MultiQueueSlot *) tmp->data;
    GST_DEBUG_OBJECT (dbin,
        "Really need to deactivate slot %p, but no available alternative",
        slot);

    slots_to_reassign = g_list_append (slots_to_reassign, slot);
  }

  /* The only slots left to activate are the ones that won't be reassigned and
   * therefore really need to have a new output created */
  for (tmp = to_activate; tmp; tmp = tmp->next) {
    MultiQueueSlot *slot = (MultiQueueSlot *) tmp->data;
    if (slot->active_stream)
      future_request_streams =
          g_list_append (future_request_streams,
          (gchar *) gst_stream_get_stream_id (slot->active_stream));
    else if (slot->pending_stream)
      future_request_streams =
          g_list_append (future_request_streams,
          (gchar *) gst_stream_get_stream_id (slot->pending_stream));
    else
      GST_ERROR_OBJECT (dbin, "No stream for slot %p !!", slot);
  }

  if (to_activate == NULL && pending_streams != NULL) {
    GST_DEBUG_OBJECT (dbin, "Stream switch requested for future collection");
    if (dbin->requested_selection)
      g_list_free_full (dbin->requested_selection, g_free);
    dbin->requested_selection =
        g_list_copy_deep (new_select_streams, (GCopyFunc) g_strdup, NULL);
    g_list_free (to_deactivate);
    g_list_free (pending_streams);
    to_deactivate = NULL;
    pending_streams = NULL;
  } else {
    if (dbin->requested_selection)
      g_list_free_full (dbin->requested_selection, g_free);
    dbin->requested_selection =
        g_list_copy_deep (future_request_streams, (GCopyFunc) g_strdup, NULL);
    dbin->requested_selection =
        g_list_concat (dbin->requested_selection,
        g_list_copy_deep (pending_streams, (GCopyFunc) g_strdup, NULL));
    if (dbin->to_activate)
      g_list_free (dbin->to_activate);
    dbin->to_activate = g_list_copy (to_reassign);
  }

  dbin->selection_updated = TRUE;
  SELECTION_UNLOCK (dbin);

  if (unknown) {
    GST_FIXME_OBJECT (dbin, "Got request for an unknown stream");
    g_list_free (unknown);
  }

  if (to_activate && !slots_to_reassign) {
    for (tmp = to_activate; tmp; tmp = tmp->next) {
      MultiQueueSlot *slot = (MultiQueueSlot *) tmp->data;
      gst_pad_add_probe (slot->src_pad, GST_PAD_PROBE_TYPE_IDLE,
          (GstPadProbeCallback) idle_reconfigure, slot, NULL);
    }
  }

  /* For all streams to deactivate, add an idle probe where we will do
   * the unassignment and switch over */
  for (tmp = slots_to_reassign; tmp; tmp = tmp->next) {
    MultiQueueSlot *slot = (MultiQueueSlot *) tmp->data;
    gst_pad_add_probe (slot->src_pad, GST_PAD_PROBE_TYPE_IDLE,
        (GstPadProbeCallback) slot_unassign_probe, slot, NULL);
  }

  if (to_deactivate)
    g_list_free (to_deactivate);
  if (to_activate)
    g_list_free (to_activate);
  if (to_reassign)
    g_list_free (to_reassign);
  if (future_request_streams)
    g_list_free (future_request_streams);
  if (pending_streams)
    g_list_free (pending_streams);
  if (slots_to_reassign)
    g_list_free (slots_to_reassign);
  if (new_select_streams)
    g_list_free (new_select_streams);
  if (slots_to_ignore)
    g_list_free (slots_to_ignore);

  return ret;
}

static GstPadProbeReturn
priv_ghost_pad_event_probe (GstDecodebin3 * dbin, GstPad * pad,
    GstEvent * event, DecodebinOutputStream * output, gboolean * steal)
{
  GstPadProbeReturn ret = GST_PAD_PROBE_OK;

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_CUSTOM_UPSTREAM:
    {
      const GstStructure *s;
      GstPad *peer;

      if (!gst_event_has_name (event, "acquired-resource")) {
        GST_DEBUG_OBJECT (pad, "Unknown custom event : %" GST_PTR_FORMAT,
            event);
        return GST_PAD_PROBE_OK;
      }

      GST_DEBUG_OBJECT (pad, "Steal event %p %s", event,
          GST_EVENT_TYPE_NAME (event));

      GST_OBJECT_LOCK (dbin);
      *steal = TRUE;
      s = gst_event_get_structure (event);
      if (dbin->resource_info) {
        gst_structure_free (dbin->resource_info);
        dbin->resource_info = NULL;
      }
      dbin->resource_info = gst_structure_copy (s);
      /* FIXME: Decodebin3 handles 'active' value when input-selector is not used.
       * Original structure can not be modified in event in probe function and
       * copied structure with 'active' is forwarded upstream. */
      gst_structure_set (dbin->resource_info, "active", G_TYPE_BOOLEAN, TRUE,
          NULL);
      GST_OBJECT_UNLOCK (dbin);

      GST_DEBUG_OBJECT (pad, "got acquired-resource : %" GST_PTR_FORMAT,
          dbin->resource_info);

      /* Send event upstream */
      if ((peer = gst_pad_get_peer (pad))) {
        GstEvent *resource_ev;
        GstStructure *resource_st;
        resource_st = gst_structure_copy (dbin->resource_info);
        resource_ev =
            gst_event_new_custom (GST_EVENT_CUSTOM_UPSTREAM, resource_st);
        gst_pad_send_event (peer, resource_ev);
        gst_object_unref (peer);
      }
      gst_event_unref (event);

      ret = GST_PAD_PROBE_HANDLED;
      break;
    }
    default:
      break;
  }

  return ret;
}

static void
priv_release_resource_related (GstDecodebin3 * dbin)
{
  GList *tmp;

  /* Reset resource related */
  if (dbin->main_input) {
    dbin->main_input->has_demuxer = FALSE;
    dbin->main_input->demuxer_no_more_pads = FALSE;
  }

  for (tmp = dbin->other_inputs; tmp; tmp = tmp->next) {
    DecodebinInput *sub_input = (DecodebinInput *) tmp->data;
    sub_input->has_demuxer = FALSE;
    sub_input->demuxer_no_more_pads = FALSE;
  }
  dbin->request_resource = FALSE;
  if (dbin->resource_info) {
    gst_structure_free (dbin->resource_info);
    dbin->resource_info = NULL;
  }
  if (dbin->exposed_pads) {
    g_list_free_full (dbin->exposed_pads, gst_object_unref);
    dbin->exposed_pads = NULL;
  }
  GST_DEBUG_OBJECT (dbin, "Done");
}

static gint
sort_compatible_ppads (gconstpointer a, gconstpointer b, gpointer udata)
{
  MultiQueueSlot *slot = (MultiQueueSlot *) udata;
  GstDecodebin3 *dbin = slot->dbin;
  PendingPad *pa = (PendingPad *) a;
  PendingPad *pb = (PendingPad *) b;
  GstStream *sa, *sb;
  GstCaps *ca, *cb;
  const gchar *ida, *idb;
  gboolean accepta, acceptb;
  GstCaps *slot_caps = NULL;
  const gchar *slot_sid;
  gchar **stream_ids;
  gchar *slot_last_sid = NULL, *ida_last_sid = NULL, *idb_last_sid = NULL;
  gint ret = 0, comp_ret1 = 0, comp_ret2 = 0;

  sa = gst_pad_get_stream (pa->pad);
  ca = gst_stream_get_caps (sa);

  sb = gst_pad_get_stream (pb->pad);
  cb = gst_stream_get_caps (sb);

  accepta = gst_pad_peer_query_accept_caps (slot->src_pad, ca);
  acceptb = gst_pad_peer_query_accept_caps (slot->src_pad, cb);

  /* Sort by which PendingPad has compatible CAPS with given slot */
  ret = accepta ? (acceptb ? 0 : -1) : (acceptb ? 1 : 0);
  if (ret != 0) {
    GST_LOG ("Sort by compatible CAPS");
    goto done;
  }

  /* Sort by which PendingPad has compatible mime-type with given slot */
  slot_caps = gst_stream_get_caps (slot->active_stream);
  if (slot_caps) {
    GstStructure *s = gst_caps_get_structure (slot_caps, 0);
    const gchar *slot_mime_type = gst_structure_get_name (s);
    GstStructure *sa = gst_caps_get_structure (ca, 0);
    const gchar *mime_typea = gst_structure_get_name (sa);
    GstStructure *sb = gst_caps_get_structure (cb, 0);
    const gchar *mime_typeb = gst_structure_get_name (sb);

    ret =
        (!g_strcmp0 (slot_mime_type, mime_typea) ? (!g_strcmp0 (slot_mime_type,
                mime_typeb) ? 0 : -1) : (!g_strcmp0 (slot_mime_type,
                mime_typeb)) ? 1 : 0);
  }

  if (ret != 0) {
    GST_LOG ("Sort by compatible mime-type");
    goto done;
  }

  ida = gst_stream_get_stream_id (sa);
  idb = gst_stream_get_stream_id (sb);

  if (!slot->active_stream || !dbin->adaptive_mode) {
    /* Sort by stream-id */
    ret = (ida) ? ((idb) ? (strcmp (ida, idb) > 0 ? 1 : -1) : -1) : 1;
    GST_LOG ("Sort by stream-id: %d, ida(%s), idb(%s)", ret, ida, idb);
    goto done;
  }

  slot_sid = gst_stream_get_stream_id (slot->active_stream);

  if (slot_sid) {
    stream_ids = g_strsplit (slot_sid, "/", -1);
    slot_last_sid = g_strdup (stream_ids[g_strv_length (stream_ids) - 1]);
    g_strfreev (stream_ids);
  }
  GST_FIXME ("slot_last_sid: %s", slot_last_sid);

  if (ida) {
    stream_ids = g_strsplit (ida, "/", -1);
    ida_last_sid = g_strdup (stream_ids[g_strv_length (stream_ids) - 1]);
    g_strfreev (stream_ids);
  }
  GST_FIXME ("ida_last_sid: %s", ida_last_sid);

  if (idb) {
    stream_ids = g_strsplit (idb, "/", -1);
    idb_last_sid = g_strdup (stream_ids[g_strv_length (stream_ids) - 1]);
    g_strfreev (stream_ids);
  }
  GST_FIXME ("idb_last_sid: %s", idb_last_sid);

  comp_ret1 = !strcmp (slot_last_sid, ida_last_sid);
  comp_ret2 = !strcmp (slot_last_sid, idb_last_sid);

  ret = comp_ret1 ? (comp_ret2 ? 0 : -1) : (comp_ret2 ? 1 : 0);
  if (ret != 0) {
    GST_LOG ("Sort by compatible stream_id. ret: %d, ida(%s), idb(%s)", ret,
        ida_last_sid, idb_last_sid);
    goto done;
  }

  /* Sort by stream-id */
  ret = (ida) ? ((idb) ? (strcmp (ida, idb) > 0 ? 1 : -1) : -1) : 1;

  GST_LOG ("Sort by stream-id: %d, ida(%s), idb(%s)", ret, ida, idb);

done:
  if (slot_caps)
    gst_caps_unref (slot_caps);
  if (ca)
    gst_caps_unref (ca);
  if (cb)
    gst_caps_unref (cb);
  g_free (slot_last_sid);
  g_free (ida_last_sid);
  g_free (idb_last_sid);

  return ret;
}

static gboolean
link_pending_pad_to_slot (DecodebinInput * input, MultiQueueSlot * slot)
{
  GstDecodebin3 *dbin;
  GList *iter;
  DecodebinInputStream *input_stream = NULL;
  GList *candidate_ppads = NULL;
  PendingPad *found_ppad = NULL;
  GstStream *pstream = NULL;
  DecodebinInputStream *old_stream = NULL;
  guint block_id = 0;

  dbin = input->dbin;
  if (G_UNLIKELY (!slot))
    return FALSE;

  PARSE_INPUT_LOCK (input);
  INPUT_LOCK (dbin);

  GST_DEBUG_OBJECT (dbin, "Finding suitable ppad for (%s:%s)",
      GST_DEBUG_PAD_NAME (slot->sink_pad));
  for (iter = g_list_last (input->pending_pads); iter; iter = iter->prev) {
    GstStream *stream;
    PendingPad *ppad = (PendingPad *) iter->data;

    stream = gst_pad_get_stream (ppad->pad);

    if (stream == NULL) {
      GST_ERROR_OBJECT (dbin, "No stream for pad ????");
      continue;
    }

    if (slot->type == gst_stream_get_stream_type (stream)) {
      gboolean needs_decoder;
      GstCaps *pcaps;

      GST_DEBUG_OBJECT (dbin, "Adding PendingPad (%s:%s) to candidate",
          GST_DEBUG_PAD_NAME (ppad->pad));
      candidate_ppads = g_list_append (candidate_ppads, ppad);

      pcaps = gst_stream_get_caps (stream);
      needs_decoder = gst_caps_can_intersect (pcaps, dbin->caps) != TRUE;

      GST_DEBUG_OBJECT (slot->sink_pad,
          "needs_decoder(%s), caps: %" GST_PTR_FORMAT,
          needs_decoder ? "TRUE" : "FALSE", pcaps);
      gst_caps_unref (pcaps);
    }
  }

  if (!candidate_ppads) {
    GST_WARNING_OBJECT (dbin, "No candidate");
    INPUT_UNLOCK (dbin);
    goto done;
  }
  INPUT_UNLOCK (dbin);

  /* Sort by compatible PendingPad with given slot */
  REASSIGN_LOCK (dbin);
  SELECTION_LOCK (dbin);
  if (candidate_ppads->data && g_list_length (candidate_ppads) > 1)
    candidate_ppads =
        g_list_sort_with_data (candidate_ppads, sort_compatible_ppads,
        (gpointer) slot);

  if (candidate_ppads->data) {
    found_ppad = (PendingPad *) candidate_ppads->data;
    GST_DEBUG_OBJECT (dbin, "Found suitable ppad(%s:%s)",
        GST_DEBUG_PAD_NAME (found_ppad->pad));
  }

  if (!found_ppad) {
    GST_DEBUG_OBJECT (slot->sink_pad, "Failed to find suitable PendingPad");
    SELECTION_UNLOCK (dbin);
    REASSIGN_UNLOCK (dbin);
    goto done;
  }

  GST_DEBUG_OBJECT (dbin,
      "Try to link pending pad (%s:%s) to unused slot (%s:%s)",
      GST_DEBUG_PAD_NAME (found_ppad->pad),
      GST_DEBUG_PAD_NAME (slot->sink_pad));
  pstream = gst_pad_get_stream (found_ppad->pad);
  block_id =
      gst_pad_add_probe (slot->sink_pad, GST_PAD_PROBE_TYPE_BLOCK_UPSTREAM,
      NULL, NULL, NULL);

  old_stream = slot->input;
  if (old_stream) {
    GST_DEBUG_OBJECT (dbin, "Removing old input stream %p (%s)", old_stream,
        old_stream->active_stream ?
        gst_stream_get_stream_id (old_stream->active_stream) : "<NONE>");

    /* Unlink from slot */
    if (old_stream->srcpad) {
      GstPad *peer;
      peer = gst_pad_get_peer (old_stream->srcpad);
      if (peer) {
        gst_pad_unlink (old_stream->srcpad, peer);
        gst_object_unref (peer);
      }
    }

    slot->input = NULL;
    GST_DEBUG_OBJECT (dbin, "slot(%s:%s) cleared",
        GST_DEBUG_PAD_NAME (slot->sink_pad));

    if (old_stream->active_stream)
      gst_object_unref (old_stream->active_stream);
    if (old_stream->pending_stream)
      gst_object_unref (old_stream->pending_stream);

    dbin->input_streams = g_list_remove (dbin->input_streams, old_stream);
    g_free (old_stream);
  }
  slot->pending_stream = NULL;

  /* Link new input stream with slot */
  SELECTION_UNLOCK (dbin);
  input_stream = create_input_stream (dbin, pstream, found_ppad->pad, input);
  SELECTION_LOCK (dbin);
  input_stream->active_stream = pstream;
  link_input_to_slot (input, input_stream, slot);

  /* Remove the buffer and event probe */
  gst_pad_remove_probe (found_ppad->pad, found_ppad->event_probe);
  if (found_ppad->gap_event) {
    gst_pad_push_event (found_ppad->pad, found_ppad->gap_event);
    found_ppad->gap_event = NULL;
  }
  gst_pad_remove_probe (found_ppad->pad, found_ppad->buffer_probe);

  SELECTION_UNLOCK (dbin);
  REASSIGN_UNLOCK (dbin);
  INPUT_LOCK (dbin);
  input->pending_pads = g_list_remove (input->pending_pads, found_ppad);
  g_free (found_ppad);
  INPUT_UNLOCK (dbin);
  SELECTION_LOCK (dbin);

  gst_pad_remove_probe (slot->sink_pad, block_id);

  SELECTION_UNLOCK (dbin);

done:
  g_list_free (candidate_ppads);
  PARSE_INPUT_UNLOCK (input);

  GST_DEBUG_OBJECT (dbin, "%s reusable slot(%s:%s) ",
      input_stream ? "Found" : "Not found",
      GST_DEBUG_PAD_NAME (slot->sink_pad));

  return input_stream ? TRUE : FALSE;
}

static void
priv_remove_exposed_pad (GstDecodebin3 * dbin, GstPad * pad)
{
  const gchar *sid;
  GList *walk, *next;

  sid = gst_pad_get_stream_id (pad);

  for (walk = dbin->exposed_pads; walk; walk = next) {
    GstPad *opad = (GstPad *) walk->data;
    const gchar *osid = gst_pad_get_stream_id (opad);

    next = g_list_next (walk);

    if (!g_strcmp0 (sid, osid)) {
      GST_DEBUG_OBJECT (pad, "Removing pad(%p:%s) in exposed pads list", opad,
          sid);
      gst_object_unref (opad);
      dbin->exposed_pads = g_list_delete_link (dbin->exposed_pads, walk);
    }
    g_free (osid);
  }
  g_free (sid);
}

static gboolean
priv_check_slot_for_eos (GstDecodebin3 * dbin, MultiQueueSlot * slot)
{
  if (slot->input && !slot->input->saw_eos) {
    /* inputstream got new stream-start */
    GST_DEBUG_OBJECT (dbin, "slot(%s:%s) haven't seen EOS",
        GST_DEBUG_PAD_NAME (slot->src_pad));
    return FALSE;
  }

  return TRUE;
}

static void
priv_remove_input_stream (GstDecodebin3 * dbin, GstPad * pad,
    DecodebinInputStream * input)
{
  MultiQueueSlot *slot;
  DecodebinInput *inp;
  GST_DEBUG_OBJECT (pad, "stream %p", input);

  SELECTION_LOCK (dbin);
  slot = get_slot_for_input (dbin, input);
  inp = input->input;
  SELECTION_UNLOCK (dbin);

  if (inp->pending_pads == NULL) {
    GST_DEBUG_OBJECT (pad, "Remove input stream %p", input);

    SELECTION_LOCK (dbin);
    remove_input_stream (dbin, input);
    if (slot && g_list_find (dbin->slots, slot) && slot->is_drained) {
      GstMessage *msg = NULL;

      if (dbin->dvr_playback && slot->output && slot->active_stream) {
        GList *tmp;
        GstStream *future_stream = NULL;
        for (tmp = dbin->requested_selection; tmp; tmp = tmp->next) {
          GstStream *req_stream =
              find_stream_in_collection (dbin->active_collection,
              (gchar *) tmp->data);
          if (req_stream) {
            const gchar *req_stream_id = gst_stream_get_stream_id (req_stream);
            GstStreamType req_stream_type =
                gst_stream_get_stream_type (req_stream);
            MultiQueueSlot *req_slot =
                find_slot_for_stream_id (dbin, req_stream_id);
            if ((req_stream_type ==
                    gst_stream_get_stream_type (slot->active_stream))
                && (g_strcmp0 (req_stream_id,
                        gst_stream_get_stream_id (slot->active_stream))
                    && req_slot && !req_slot->is_drained && !req_slot->output)) {
              GST_DEBUG_OBJECT (slot->src_pad,
                  "We Found future stream(%s) in active-collection",
                  gst_stream_get_stream_id (req_stream));
              future_stream = req_stream;
              break;
            }
          }
        }
        if (future_stream) {
          const gchar *future_streamid =
              gst_stream_get_stream_id (future_stream);
          dbin->to_activate =
              g_list_append (dbin->to_activate, (gchar *) future_streamid);
          GST_DEBUG_OBJECT (slot->src_pad, "Directly re-assign slot to '%s'",
              future_streamid);
          SELECTION_UNLOCK (dbin);
          reassign_slot (dbin, slot);
          SELECTION_LOCK (dbin);
        }
      }

      /* if slot is still there and already drained, remove it in here */
      if (slot->active_stream) {
        gst_object_unref (slot->active_stream);
        slot->active_stream = NULL;
      }
      msg = update_active_collection (slot, TRUE);
      SELECTION_UNLOCK (dbin);
      if (msg) {
        GST_DEBUG_OBJECT (dbin, "poting collection");
        gst_element_post_message ((GstElement *) slot->dbin, msg);
      }
      SELECTION_LOCK (dbin);
      if (slot->output) {
        DecodebinOutputStream *output = slot->output;
        GST_DEBUG_OBJECT (pad, "Multiqueue was drained, Remove output stream");
        dbin->output_streams = g_list_remove (dbin->output_streams, output);
        free_output_stream (dbin, output);
      }
      GST_DEBUG_OBJECT (pad, "No pending pad, Remove multiqueue slot");
      if (slot->probe_id)
        gst_pad_remove_probe (slot->src_pad, slot->probe_id);
      slot->probe_id = 0;
      slot->upstream_probe_id = 0;
      dbin->slots = g_list_remove (dbin->slots, slot);
      if (!slot->mark_async_pool) {
        slot->mark_async_pool = TRUE;
        free_multiqueue_slot_async (dbin, slot);
      }
    }
    SELECTION_UNLOCK (dbin);
  } else {
    input->srcpad = NULL;
    if (input->input_buffer_probe_id)
      gst_pad_remove_probe (pad, input->input_buffer_probe_id);
    input->input_buffer_probe_id = 0;
    link_pending_pad_to_slot (inp, slot);
  }
}

static GstCaps *
expected_output_tmpl_caps (GstCaps * input_caps)
{
  GList *decoders = NULL;
  GList *filtered = NULL;
  GstElementFactory *factory = NULL;
  const GList *templates;
  GstCaps *tmpl_caps = NULL;

  decoders =
      gst_element_factory_list_get_elements
      (GST_ELEMENT_FACTORY_TYPE_DECODER, GST_RANK_MARGINAL);

  if (decoders == NULL) {
    GST_WARNING ("Cannot find any of decoders");
    goto fail;
  }

  GST_DEBUG ("got factory list %p", decoders);
  gst_plugin_feature_list_debug (decoders);

  decoders = g_list_sort (decoders, gst_plugin_feature_rank_compare_func);

  if (!(filtered =
          gst_element_factory_list_filter (decoders, input_caps, GST_PAD_SINK,
              FALSE))) {
    gchar *tmp = gst_caps_to_string (input_caps);
    GST_WARNING ("Cannot find any decoder for caps %s", tmp);
    g_free (tmp);

    goto fail;
  }

  GST_DEBUG ("got filtered list %p", filtered);

  gst_plugin_feature_list_debug (filtered);

  factory = GST_ELEMENT_FACTORY_CAST (g_list_nth_data (filtered, 0));

  /* Note that decproxy can not be a child element in decproxy bin */
  if (!g_strcmp0 (gst_plugin_feature_get_name (g_list_nth_data (filtered, 0)),
          "decproxy")) {
    if (g_list_length (filtered) > 1)
      factory = GST_ELEMENT_FACTORY_CAST (g_list_nth_data (filtered, 1));
    else
      factory = NULL;
  }

  if (factory == NULL) {
    GST_WARNING ("factory is null");
    goto fail;
  }

  GST_DEBUG ("expected actual decoder: %" GST_PTR_FORMAT, factory);
  templates = gst_element_factory_get_static_pad_templates (factory);
  while (templates) {
    GstStaticPadTemplate *template = (GstStaticPadTemplate *) templates->data;

    if (template->direction == GST_PAD_SRC
        && template->presence == GST_PAD_ALWAYS) {
      tmpl_caps = gst_static_caps_get (&template->static_caps);
    }
    templates = g_list_next (templates);
  }

fail:
  if (decoders)
    gst_plugin_feature_list_free (decoders);
  if (filtered)
    gst_plugin_feature_list_free (filtered);

  return tmpl_caps;
}

static gboolean
is_valid_value (const gchar * prop, gint value, GstStructure * s)
{
  GValue v1 = { 0, };
  const GValue *v2;
  gboolean ret = TRUE;

  g_value_init (&v1, G_TYPE_INT);
  g_value_set_int (&v1, value);
  v2 = gst_structure_get_value (s, prop);

  if (v2 && !gst_value_is_fixed (v2) && !gst_value_is_subset (&v1, v2))
    ret = FALSE;

  g_value_unset (&v1);

  return ret;
}

static GstCaps *
negotiate_default_caps (GstDecodebin3 * dbin, GstCaps * input_caps,
    GstPad * srcpad, GstStreamType stream_type)
{
  GstCaps *caps, *templcaps;
  gint i;
  gint channels = 0;
  gint rate;
  guint64 channel_mask = 0;
  gint caps_size;
  GstStructure *structure;

  templcaps = expected_output_tmpl_caps (input_caps);
  GST_DEBUG_OBJECT (dbin, "expected templcaps : %" GST_PTR_FORMAT, templcaps);
  caps = gst_pad_peer_query_caps (srcpad, templcaps);
  if (caps)
    gst_caps_unref (templcaps);
  else
    caps = templcaps;
  templcaps = NULL;

  if (!caps || gst_caps_is_empty (caps) || gst_caps_is_any (caps))
    goto caps_error;

  GST_DEBUG_OBJECT (dbin, "peer caps  %" GST_PTR_FORMAT, caps);

  /* before fixating, try to use whatever upstream provided */
  caps = gst_caps_make_writable (caps);
  caps_size = gst_caps_get_size (caps);
  if (input_caps) {
    GstCaps *sinkcaps = input_caps;
    GstStructure *structure = gst_caps_get_structure (sinkcaps, 0);
    if (stream_type == GST_STREAM_TYPE_AUDIO) {
      if (gst_structure_get_int (structure, "rate", &rate)) {
        for (i = 0; i < caps_size; i++) {
          if (is_valid_value ("rate", rate, gst_caps_get_structure (caps, i)))
            gst_structure_set (gst_caps_get_structure (caps, i), "rate",
                G_TYPE_INT, rate, NULL);
        }
      }

      if (gst_structure_get_int (structure, "channels", &channels)) {
        for (i = 0; i < caps_size; i++) {
          if (is_valid_value ("channels", channels,
                  gst_caps_get_structure (caps, i)))
            gst_structure_set (gst_caps_get_structure (caps, i), "channels",
                G_TYPE_INT, channels, NULL);
        }
      }

      if (gst_structure_get (structure, "channel-mask", GST_TYPE_BITMASK,
              &channel_mask, NULL)) {
        for (i = 0; i < caps_size; i++) {
          gst_structure_set (gst_caps_get_structure (caps, i), "channel-mask",
              GST_TYPE_BITMASK, channel_mask, NULL);
        }
      }
    } else if (stream_type == GST_STREAM_TYPE_VIDEO) {
      gint width, height;
      gint par_n, par_d;
      gint fps_n, fps_d;

      if (gst_structure_get_int (structure, "width", &width)) {
        for (i = 0; i < caps_size; i++) {
          gst_structure_set (gst_caps_get_structure (caps, i), "width",
              G_TYPE_INT, width, NULL);
        }
      }

      if (gst_structure_get_int (structure, "height", &height)) {
        for (i = 0; i < caps_size; i++) {
          gst_structure_set (gst_caps_get_structure (caps, i), "height",
              G_TYPE_INT, height, NULL);
        }
      }

      if (gst_structure_get_fraction (structure, "framerate", &fps_n, &fps_d)) {
        for (i = 0; i < caps_size; i++) {
          gst_structure_set (gst_caps_get_structure (caps, i), "framerate",
              GST_TYPE_FRACTION, fps_n, fps_d, NULL);
        }
      }

      if (gst_structure_get_fraction (structure, "pixel-aspect-ratio", &par_n,
              &par_d)) {
        for (i = 0; i < caps_size; i++) {
          gst_structure_set (gst_caps_get_structure (caps, i),
              "pixel-aspect-ratio", GST_TYPE_FRACTION, par_n, par_d, NULL);
        }
      }
    }
  }

  if (stream_type == GST_STREAM_TYPE_AUDIO) {
    for (i = 0; i < caps_size; i++) {
      structure = gst_caps_get_structure (caps, i);
      gst_structure_fixate_field_nearest_int (structure,
          "channels", GST_AUDIO_DEF_CHANNELS);
      gst_structure_fixate_field_nearest_int (structure,
          "rate", GST_AUDIO_DEF_RATE);

    }

    caps = gst_caps_fixate (caps);
    structure = gst_caps_get_structure (caps, 0);

    /* Need to add a channel-mask if channels > 2 */
    gst_structure_get_int (structure, "channels", &channels);
    if (channels > 2 && !gst_structure_has_field (structure, "channel-mask")) {
      channel_mask = gst_audio_channel_get_fallback_mask (channels);
      if (channel_mask != 0) {
        gst_structure_set (structure, "channel-mask",
            GST_TYPE_BITMASK, channel_mask, NULL);
      } else {
        GST_WARNING_OBJECT (dbin, "No default channel-mask for %d channels",
            channels);
      }
    }

    gst_structure_set (structure, "from-decodebin3",
        G_TYPE_BOOLEAN, TRUE, NULL);

  } else if (stream_type == GST_STREAM_TYPE_VIDEO) {
    for (i = 0; i < caps_size; i++) {
      structure = gst_caps_get_structure (caps, i);
      /* Random 1280x720@30 for fixation */
      gst_structure_fixate_field_nearest_int (structure, "width", 1280);
      gst_structure_fixate_field_nearest_int (structure, "height", 720);
      gst_structure_fixate_field_nearest_fraction (structure, "framerate", 30,
          1);
      if (gst_structure_has_field (structure, "pixel-aspect-ratio")) {
        gst_structure_fixate_field_nearest_fraction (structure,
            "pixel-aspect-ratio", 1, 1);
      } else {
        gst_structure_set (structure, "pixel-aspect-ratio", GST_TYPE_FRACTION,
            1, 1, NULL);
      }
    }
  }

  caps = gst_caps_fixate (caps);

  if (!caps)
    goto caps_error;

  GST_INFO_OBJECT (dbin,
      "Chose default caps %" GST_PTR_FORMAT " for initial negotiation", caps);

  return caps;

caps_error:
  {
    if (caps)
      gst_caps_unref (caps);
    return NULL;
  }
}

static gboolean
ongoing_change_upstream (GstDecodebin3 * dbin, MultiQueueSlot * slot)
{
  if (!dbin->collection_posted)
    return FALSE;

  if (dbin->collection && dbin->active_collection) {
    guint i;
    guint nb_streams;

    if (gst_stream_collection_get_size (dbin->collection) !=
        gst_stream_collection_get_size (dbin->active_collection)) {
      GST_LOG_OBJECT (slot->src_pad,
          "Not equal in between dbin->collection and dbin->active_collection");
      return TRUE;
    }

    nb_streams = gst_stream_collection_get_size (dbin->active_collection);
    for (i = 0; i < nb_streams; i++) {
      GstStream *stream =
          gst_stream_collection_get_stream (dbin->active_collection, i);
      const gchar *sid = gst_stream_get_stream_id (stream);
      if (!collection_has_stream (dbin->collection, sid)) {
        GST_LOG_OBJECT (slot->src_pad,
            "(stream-id:%s) is not exist in dbin->collection", sid);
        return TRUE;
      }
    }

    nb_streams = gst_stream_collection_get_size (dbin->collection);
    for (i = 0; i < nb_streams; i++) {
      GstStream *stream =
          gst_stream_collection_get_stream (dbin->collection, i);
      const gchar *sid = gst_stream_get_stream_id (stream);
      if (!collection_has_stream (dbin->active_collection, sid)) {
        GST_LOG_OBJECT (slot->src_pad,
            "(stream-id:%s) is not exist in dbin->active_collection", sid);
        return TRUE;
      }
    }
  }

  return FALSE;
}
