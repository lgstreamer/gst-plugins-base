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

static ChildSrcPadInfo *priv_find_pending_pad_for_slot (GstURISourceBin *
    urisrc, OutputSlotInfo * slot);
static GstPadProbeReturn priv_demux_pad_events (GstURISourceBin * urisrc,
    GstPad * pad, GstEvent * ev, ChildSrcPadInfo * child_info,
    gboolean * steal);
static OutputSlotInfo *priv_get_output_slot_for_multiappsrc (GstURISourceBin *
    urisrc);
static void priv_multiappsrc_connect_signals (GstURISourceBin * urisrc);

static gboolean
_stream_caps_is_compatible (GstStream * pending_stream, GstStream * slot_stream)
{
  GstCaps *pending_caps, *slot_caps;
  gboolean ret = FALSE;
  const GstStructure *pending_str, *slot_str;
  const gchar *pending_name, *slot_name;

  pending_caps = gst_stream_get_caps (pending_stream);
  slot_caps = gst_stream_get_caps (slot_stream);

  /* We expect pending pad has caps always */
  if (pending_caps == NULL) {
    GST_LOG ("pending pad has no caps, ignore");
    ret = FALSE;
    goto done;
  }

  /* Slot has no caps, we can accept pending stream */
  if (slot_caps == NULL) {
    GST_LOG ("slot has no caps, accept this");
    ret = TRUE;
    goto done;
  }

  pending_str = gst_caps_get_structure (pending_caps, 0);
  slot_str = gst_caps_get_structure (slot_caps, 0);

  if (!pending_str || !slot_str) {
    GST_FIXME ("caps has no structure, what should we do??");
    ret = FALSE;
    goto done;
  }

  pending_name = gst_structure_get_name (pending_str);
  slot_name = gst_structure_get_name (slot_str);

  /* Accecpt identical mime-type */
  if (!g_strcmp0 (pending_name, slot_name)) {
    GST_LOG ("identical mime-type %s, accept", pending_name);
    ret = TRUE;
  }


done:
  if (pending_caps)
    gst_caps_unref (pending_caps);
  if (slot_caps)
    gst_caps_unref (slot_caps);

  return ret;
}

static gboolean
_stream_type_is_compatible (GstStream * pending_stream, GstStream * slot_stream)
{
  GstStreamType pending_type, slot_type;

  pending_type = gst_stream_get_stream_type (pending_stream);
  slot_type = gst_stream_get_stream_type (slot_stream);

  if (pending_type == slot_type) {
    GST_LOG ("compatible stream type %s",
        gst_stream_type_get_name (pending_type));
    return TRUE;
  }

  GST_LOG ("pending pad's stream type %s is not compatible with %s",
      gst_stream_type_get_name (pending_type),
      gst_stream_type_get_name (slot_type));
  return FALSE;
}

/* TODO: Find alternative way to find compatible tag  */
static gboolean
_stream_tags_is_compatible (GstStream * pending_stream, GstStream * slot_stream)
{
  GstTagList *pending_tags, *slot_tags;
  gchar *pending_lang = NULL, *slot_lang = NULL;
  gchar *pending_code = NULL, *slot_code = NULL;
  gboolean ret = FALSE;

  pending_tags = gst_stream_get_tags (pending_stream);
  slot_tags = gst_stream_get_tags (slot_stream);

  if (!pending_tags || !slot_tags)
    goto done;

  gst_tag_list_get_string (pending_tags, GST_TAG_LANGUAGE_NAME, &pending_lang);
  if (!pending_lang) {
    gst_tag_list_get_string (pending_tags, GST_TAG_LANGUAGE_CODE,
        &pending_code);
  }

  gst_tag_list_get_string (slot_tags, GST_TAG_LANGUAGE_NAME, &slot_lang);
  if (!slot_lang) {
    gst_tag_list_get_string (pending_tags, GST_TAG_LANGUAGE_CODE, &slot_code);
  }

  if (pending_lang && slot_lang && !g_strcmp0 (pending_lang, slot_lang)) {
    GST_LOG ("compatible language name %s", pending_lang);
    ret = TRUE;
  } else if (pending_code && slot_code && !g_strcmp0 (pending_code, slot_code)) {
    GST_LOG ("compatible language code %s", pending_code);
    ret = TRUE;
  }

done:
  if (pending_tags)
    gst_tag_list_unref (pending_tags);
  if (slot_tags)
    gst_tag_list_unref (slot_tags);
  g_free (slot_code);
  g_free (slot_lang);
  g_free (pending_code);
  g_free (pending_lang);

  return ret;
}

/**
 * priv_find_pending_pad_for_slot:
 * @urisrc: a #GstURISourceBin
 * @slot: a #OutputSlotInfo
 *
 * Returns: the #ChildSrcPadInfo can be compatible with given slot
 */
static ChildSrcPadInfo *
priv_find_pending_pad_for_slot (GstURISourceBin * urisrc, OutputSlotInfo * slot)
{
  GList *cur, *cand_caps = NULL, *cand_type = NULL, *cand_tags = NULL;
  ChildSrcPadInfo *in_info = slot->linked_info;
  ChildSrcPadInfo *ret = NULL;
  GstCaps *slot_caps = gst_pad_get_current_caps (slot->sinkpad);

  /* Find suitable demuxer's pad for the slot
   * if slot or demuxer's pad has no GstStream object, figure out just using caps
   * otherwise, find by following rule until there is only one candidate pad:
   * 1. Compare mime-type
   * 2. Compare GstStreamType
   * 3. Compare language using TagList
   */

  GST_DEBUG_OBJECT (urisrc,
      "Looking for a pending pad with caps %" GST_PTR_FORMAT, slot_caps);

  for (cur = urisrc->pending_pads; cur != NULL; cur = g_list_next (cur)) {
    GstPad *pending = (GstPad *) (cur->data);
    ChildSrcPadInfo *cur_info = NULL;
    if ((cur_info =
            g_object_get_data (G_OBJECT (pending),
                "urisourcebin.srcpadinfo"))) {
      /* Don't re-link to the same pad in case of EOS while still pending */
      if (in_info == cur_info)
        continue;

      if (slot_caps == NULL) {
        ret = cur_info;
        goto done;
      }

      if (!in_info->stream || !cur_info->stream) {
        /* GstStream objects are not available, Select only by caps */
        if (gst_caps_is_equal (slot_caps, cur_info->cur_caps)) {
          ret = cur_info;
          goto done;
        } else if (gst_caps_is_subset_structure (cur_info->cur_caps,
                gst_caps_get_structure (slot_caps, 0))) {
          GST_LOG ("Found matching pads through caps subset structure.");
          ret = cur_info;
          goto done;
        }
      } else if (_stream_caps_is_compatible (cur_info->stream, in_info->stream)) {
        cand_caps = g_list_append (cand_caps, cur_info);
      }
    }
  }

  if (!cand_caps)
    goto done;

  if (g_list_length (cand_caps) == 1) {
    ret = (ChildSrcPadInfo *) cand_caps->data;
    goto done;
  }

  /* 2. Multiple candidate streams exist */
  for (cur = cand_caps; cur; cur = g_list_next (cur)) {
    ChildSrcPadInfo *tmp = (ChildSrcPadInfo *) cur->data;

    if (_stream_type_is_compatible (tmp->stream, in_info->stream))
      cand_type = g_list_append (cand_type, tmp);
    if (_stream_tags_is_compatible (tmp->stream, in_info->stream))
      cand_tags = g_list_append (cand_tags, tmp);
  }

  if (g_list_length (cand_type) == 1) {
    ret = (ChildSrcPadInfo *) cand_type->data;
    goto done;
  }

  if (cand_tags != NULL && g_list_length (cand_tags) == 1) {
    ret = (ChildSrcPadInfo *) cand_tags->data;
    goto done;
  }

  /* Give up for finding suitable pending pads, return the first pending pad in list */
  ret = (ChildSrcPadInfo *) cand_caps->data;

done:
  if (slot_caps)
    gst_caps_unref (slot_caps);
  if (cand_caps)
    g_list_free (cand_caps);

  if (ret)
    GST_DEBUG_OBJECT (urisrc, "Found suitable pending pad %" GST_PTR_FORMAT
        " with caps %" GST_PTR_FORMAT " to link to this output slot",
        ret->demux_src_pad, ret->cur_caps);

  return ret;
}

/**
 * priv_demux_pad_events:
 * @urisrc: a #GstURISourceBin
 * @pad: a #GstPad
 * @ev: a #GstEvent
 * @child_info: a #ChildSrcPadInfo
 * @steal: whether to check steal or not
 *
 * Called with GST_URI_SOURCE_BIN_LOCK held
 *
 * Returns: #GST_PAD_PROBE_OK
 */
static GstPadProbeReturn
priv_demux_pad_events (GstURISourceBin * urisrc, GstPad * pad,
    GstEvent * ev, ChildSrcPadInfo * child_info, gboolean * steal)
{
  GstPadProbeReturn ret = GST_PAD_PROBE_OK;

  switch (GST_EVENT_TYPE (ev)) {
    case GST_EVENT_STREAM_START:
    {
      GstStream *stream = gst_pad_get_stream (pad);
      if (stream) {
#ifndef GST_DISABLE_GST_DEBUG
        GstCaps *caps;
        GstTagList *taglist;

        GST_DEBUG_OBJECT (pad, "   Stream '%s'",
            gst_stream_get_stream_id (stream));
        GST_DEBUG_OBJECT (pad, "     type  : %s",
            gst_stream_type_get_name (gst_stream_get_stream_type (stream)));
        GST_DEBUG_OBJECT (pad, "     flags : 0x%x",
            gst_stream_get_stream_flags (stream));
        taglist = gst_stream_get_tags (stream);
        GST_DEBUG_OBJECT (pad, "     tags  : %" GST_PTR_FORMAT, taglist);
        caps = gst_stream_get_caps (stream);
        GST_DEBUG_OBJECT (pad, "     caps  : %" GST_PTR_FORMAT, caps);
        if (taglist)
          gst_tag_list_unref (taglist);
        if (caps)
          gst_caps_unref (caps);
#endif
        gst_object_replace ((GstObject **) & child_info->stream,
            (GstObject *) stream);
        gst_object_unref (stream);
      }

      child_info->output_slot->is_eos = FALSE;
      *steal = TRUE;
    }
      break;
    default:
      *steal = FALSE;
      break;
  }

  return ret;
}

/**
 * priv_get_output_slot_for_multiappsrc:
 * @urisrc: a #GstURISourceBin
 *
 * Called with lock held
 *
 * Returns: #OutputSlotInfo
 */
static OutputSlotInfo *
priv_get_output_slot_for_multiappsrc (GstURISourceBin * urisrc)
{
  OutputSlotInfo *slot;
  GstPad *srcpad;
  GstElement *queue;

  queue = gst_element_factory_make ("queue", NULL);
  if (!queue)
    goto no_buffer_element;

  g_object_set (queue, "max-size-buffers", 2, NULL);

  slot = g_new0 (OutputSlotInfo, 1);
  slot->queue = queue;

  /* Set the slot onto the queue (needed in buffering msg handling) */
  g_object_set_data (G_OBJECT (queue), "urisourcebin.slotinfo", slot);

  /* save queue pointer so we can remove it later */
  urisrc->out_slots = g_slist_prepend (urisrc->out_slots, slot);

  gst_bin_add (GST_BIN_CAST (urisrc), queue);
  gst_element_sync_state_with_parent (queue);

  slot->sinkpad = gst_element_get_static_pad (queue, "sink");

  /* get the new raw srcpad */
  srcpad = gst_element_get_static_pad (queue, "src");
  g_object_set_data (G_OBJECT (srcpad), "urisourcebin.slotinfo", slot);

  slot->srcpad = create_output_pad (urisrc, srcpad);

  gst_object_unref (srcpad);

  return slot;

no_buffer_element:
  {
    post_missing_plugin_error (GST_ELEMENT_CAST (urisrc), "queue");
    return NULL;
  }
}

static void
priv_multiappsrc_connect_signals (GstURISourceBin * urisrc)
{
  /* FIXME: maybe we can filter out using klass meta, but let's use hard coded
   * uri type comparison because MultiAppSrc is the unique case */
  GST_DEBUG_OBJECT (urisrc->source, "signal connect for multiappsrc pads");
  g_signal_connect (urisrc->source,
      "pad-added", G_CALLBACK (new_demuxer_pad_added_cb), urisrc);
  g_signal_connect (urisrc->source,
      "pad-removed", G_CALLBACK (pad_removed_cb), urisrc);

}
