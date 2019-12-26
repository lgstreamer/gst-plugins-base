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

/* blacklisted field for taglist, we don't need to add it to GstStream */
static const gchar *blacklisted_tag_fields[] =
    { GST_TAG_BITRATE, GST_TAG_MINIMUM_BITRATE, GST_TAG_MAXIMUM_BITRATE,
  NULL
};

#define COMPOSITOR_LOCK(parsebin) G_STMT_START {                           \
    GST_LOG_OBJECT (parsebin,                                              \
                   "compositor locking from thread %p",                        \
                   g_thread_self ());                                  \
    g_mutex_lock (&GST_PARSE_BIN_CAST(parsebin)->compositor_lock);                \
    GST_LOG_OBJECT (parsebin,                                              \
                   "compositor lock from thread %p",                   \
                   g_thread_self ());                                  \
} G_STMT_END

#define COMPOSITOR_UNLOCK(parsebin) G_STMT_START {                         \
    GST_LOG_OBJECT (parsebin,                                              \
                   "compositor unlocking from thread %p",              \
                   g_thread_self ());                                  \
    g_mutex_unlock (&GST_PARSE_BIN_CAST(parsebin)->compositor_lock);              \
} G_STMT_END


static GstPad *get_sink_pad_to_compositor (GstElement * element);
static gboolean is_compositor_factory (GstElementFactory * factory);
static GstElement *gst_parse_chain_get_compositor (GstParseGroup * group,
    GstStreamType type);
static GstPadProbeReturn gst_demux_peer_event (GstPad * pad,
    GstPadProbeInfo * info, gpointer user_data);
static void free_delayed_to_null (GstElement * delayed);

/* Virtual Funtions */
static void priv_parse_bin_init (GstParseBin * parse_bin);
static void priv_parse_bin_dispose (GstParseBin * parse_bin);
static void priv_parse_bin_finalize (GstParseBin * parse_bin);
static void priv_analyze_new_pad (GstParseBin * parsebin, GstElement * src,
    GstPad * pad, GstCaps * caps, GstParseChain * chain);
static gboolean priv_connect_pad (GstParseBin * parsebin, GstElement * src,
    GstParsePad * parsepad, GstPad * pad, GstCaps * caps,
    GValueArray * factories, GstParseChain * chain, gchar ** deadend_details);
static void priv_query_smart_properties (GstParseBin * parse_bin);
static void priv_parse_chain_new (GstParseChain * chain);
static void priv_parse_chain_free (GstParseChain * chain);
static gboolean priv_have_composite_stream (GstParseChain * chain);
static void priv_build_fallback_elements (GstParseBin * parsebin,
    GList * endpads);
static void priv_parse_bin_expose_compositor (GstParseBin * parsebin);
static void priv_parse_pad_update_tags (GstParsePad * parsepad,
    GstTagList * tags);
static void priv_update_compsite_stream (GstParsePad * parsepad,
    const gchar * stream_id);
static void priv_parse_pad_add_probe (GstParsePad * parsepad,
    GstProxyPad * ppad);
static GstPadProbeReturn priv_parse_pad_event (GstParsePad * parsepad,
    GstEvent * event, gboolean * steal);
static gboolean priv_check_delayed_set_to_null (GstParseChain * chain,
    GstElement * element);

static void
free_delayed_to_null (GstElement * delayed)
{
  GST_DEBUG_OBJECT (delayed, "Removing ...");
  gst_element_set_state (delayed, GST_STATE_NULL);
  gst_object_unref (delayed);
}

static gboolean
is_compositor_factory (GstElementFactory * factory)
{
  if (strstr (gst_element_factory_get_metadata (factory,
              GST_ELEMENT_METADATA_KLASS), "Parser")) {
    const GList *tmp;
    gboolean has_request_sinkpad = FALSE;
    gint num_always_srcpads = 0;

    for (tmp = gst_element_factory_get_static_pad_templates (factory);
        tmp; tmp = tmp->next) {
      GstStaticPadTemplate *template = tmp->data;

      if (template->direction == GST_PAD_SINK) {
        if (template->presence == GST_PAD_REQUEST) {
          has_request_sinkpad = TRUE;
        }
      } else if (template->direction == GST_PAD_SRC) {
        if (template->presence == GST_PAD_ALWAYS) {
          if (num_always_srcpads >= 0)
            num_always_srcpads++;
        } else {
          num_always_srcpads = -1;
        }
      }
    }

    if (has_request_sinkpad == TRUE && num_always_srcpads == 1) {
      GST_DEBUG_OBJECT (factory, "is a compositor.");
      return TRUE;
    }
  }

  return FALSE;
}

static GstPadProbeReturn
gst_demux_peer_event (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  GstEvent *event = GST_PAD_PROBE_INFO_EVENT (info);
  GstParsePad *parsepad = (GstParsePad *) user_data;

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_TAG:{
      GstTagList *tags;
      gst_event_parse_tag (event, &tags);
      GST_DEBUG_OBJECT (pad,
          "got TAG event : %" GST_PTR_FORMAT ", parsepad : %" GST_PTR_FORMAT,
          tags, parsepad);
      if (parsepad)
        gst_parse_pad_update_tags (parsepad, tags);
      break;
    }
    default:
      break;
  }

  return GST_PAD_PROBE_OK;
}

static void
priv_parse_bin_init (GstParseBin * parse_bin)
{
  parse_bin->have_compositor = FALSE;
  parse_bin->have_composite_stream = FALSE;

  g_mutex_init (&parse_bin->compositor_lock);
  parse_bin->flush_probed = FALSE;
  parse_bin->on_dsc = FALSE;
  g_mutex_init (&parse_bin->dsc_lock);
  g_cond_init (&parse_bin->dsc_cond);
  parse_bin->adaptive_mode = FALSE;
}

static void
priv_parse_bin_dispose (GstParseBin * parse_bin)
{
  g_free (parse_bin->fallback_element);
  parse_bin->fallback_element = NULL;

  g_list_free_full (parse_bin->delayed_to_null, free_delayed_to_null);
  parse_bin->delayed_to_null = NULL;
  g_mutex_clear (&parse_bin->dsc_lock);
  g_cond_clear (&parse_bin->dsc_cond);
}

static void
priv_parse_bin_finalize (GstParseBin * parse_bin)
{
  g_mutex_clear (&parse_bin->compositor_lock);
}

static void parse_pad_set_target (GstParsePad * parsepad, GstPad * target);

static void
priv_analyze_new_pad (GstParseBin * parsebin, GstElement * src, GstPad * pad,
    GstCaps * caps, GstParseChain * chain)
{
  gboolean apcontinue = TRUE;
  GValueArray *factories = NULL, *result = NULL;
  GstParsePad *parsepad;
  GstElementFactory *factory;
  const gchar *classification;
  gboolean is_parser_converter = FALSE;
  gboolean res;
  gchar *deadend_details = NULL;

  GST_DEBUG_OBJECT (parsebin, "Pad %s:%s caps:%" GST_PTR_FORMAT,
      GST_DEBUG_PAD_NAME (pad), caps);

  if (chain->elements
      && src != ((GstParseElement *) chain->elements->data)->element
      && src != ((GstParseElement *) chain->elements->data)->capsfilter) {
    GST_ERROR_OBJECT (parsebin,
        "New pad from not the last element in this chain");
    return;
  }

  if (chain->demuxer) {
    GstParseGroup *group;
    GstParseChain *oldchain = chain;
    GstParseElement *demux = (chain->elements ? chain->elements->data : NULL);

    if (chain->current_pad)
      gst_object_unref (chain->current_pad);
    chain->current_pad = NULL;

    /* we are adding a new pad for a demuxer (see is_demuxer_element(),
     * start a new chain for it */
    CHAIN_MUTEX_LOCK (oldchain);
    group = gst_parse_chain_get_current_group (chain);
    if (group && !g_list_find (group->children, chain)) {
      chain = gst_parse_chain_new (parsebin, group, pad, caps);
      group->children = g_list_prepend (group->children, chain);
    }
    CHAIN_MUTEX_UNLOCK (oldchain);
    if (!group) {
      GST_WARNING_OBJECT (parsebin, "No current group");
      return;
    }

    /* If this is not a dynamic pad demuxer, we're no-more-pads
     * already before anything else happens
     */
    if (demux == NULL || !demux->no_more_pads_id)
      group->no_more_pads = TRUE;
  }

  /* From here on we own a reference to the caps as
   * we might create new caps below and would need
   * to unref them later */
  if (caps)
    gst_caps_ref (caps);

  if ((caps == NULL) || gst_caps_is_empty (caps))
    goto unknown_type;

  if (gst_caps_is_any (caps))
    goto any_caps;

  if (chain->type == GST_STREAM_TYPE_UNKNOWN) {
    chain->type = guess_stream_type_from_caps (caps);
    GST_DEBUG_OBJECT (parsebin, "chain : %p, chain->type : %d", chain,
        chain->type);
  }

  if (chain->compositor != NULL && chain->main_input)
    goto no_expose_pad;

  if (!chain->current_pad)
    chain->current_pad = gst_parse_pad_new (parsebin, chain);

  parsepad = gst_object_ref (chain->current_pad);
  gst_pad_set_active (GST_PAD_CAST (parsepad), TRUE);
  parse_pad_set_target (parsepad, pad);

  /* 1. Emit 'autoplug-continue' the result will tell us if this pads needs
   * further autoplugging. Only do this for fixed caps, for unfixed caps
   * we will later come here again from the notify::caps handler. The
   * problem with unfixed caps is that, we can't reliably tell if the output
   * is e.g. accepted by a sink because only parts of the possible final
   * caps might be accepted by the sink. */
  if (gst_caps_is_fixed (caps))
    g_signal_emit (G_OBJECT (parsebin),
        gst_parse_bin_signals[SIGNAL_AUTOPLUG_CONTINUE], 0, parsepad, caps,
        &apcontinue);
  else
    apcontinue = TRUE;

  /* 1.a if autoplug-continue is FALSE or caps is a raw format, goto pad_is_final */
  if (!apcontinue)
    goto expose_pad;

  /* 1.b For Parser/Converter that can output different stream formats
   * we insert a capsfilter with the sorted caps of all possible next
   * elements and continue with the capsfilter srcpad */
  factory = gst_element_get_factory (src);
  classification =
      gst_element_factory_get_metadata (factory, GST_ELEMENT_METADATA_KLASS);
  is_parser_converter = (strstr (classification, "Parser")
      && strstr (classification, "Converter"));

  /* FIXME: We just need to be sure that the next element is not a parser */
  /* 1.c when the caps are not fixed yet, we can't be sure what element to
   * connect. We delay autoplugging until the caps are fixed */
  if (!is_parser_converter && !gst_caps_is_fixed (caps)) {
    goto non_fixed;
  } else if (!is_parser_converter) {
    gst_caps_unref (caps);
    caps = gst_pad_get_current_caps (pad);
    if (!caps) {
      GST_DEBUG_OBJECT (parsebin,
          "No final caps set yet, delaying autoplugging");
      gst_object_unref (parsepad);
      goto setup_caps_delay;
    }
  }

  /* 1.d else get the factories and if there's no compatible factory goto
   * unknown_type */
  g_signal_emit (G_OBJECT (parsebin),
      gst_parse_bin_signals[SIGNAL_AUTOPLUG_FACTORIES], 0, parsepad, caps,
      &factories);

  /* NULL means that we can expose the pad */
  if (factories == NULL)
    goto expose_pad;

  /* if the array is empty, we have a type for which we have no parser */
  if (factories->n_values == 0) {
    /* if not we have a unhandled type with no compatible factories */
    g_value_array_free (factories);
    gst_object_unref (parsepad);
    goto unknown_type;
  }

  /* 1.e sort some more. */
  g_signal_emit (G_OBJECT (parsebin),
      gst_parse_bin_signals[SIGNAL_AUTOPLUG_SORT], 0, parsepad, caps, factories,
      &result);
  if (result) {
    g_value_array_free (factories);
    factories = result;
  }

  /* 1.g now get the factory template caps and insert the capsfilter if this
   * is a parser/converter
   */
  if (is_parser_converter) {
    GstCaps *filter_caps;
    gint i;
    GstPad *p;
    GstParseElement *pelem;

    g_assert (chain->elements != NULL);
    pelem = (GstParseElement *) chain->elements->data;

    filter_caps = gst_caps_new_empty ();
    for (i = 0; i < factories->n_values; i++) {
      GstElementFactory *factory =
          g_value_get_object (g_value_array_get_nth (factories, i));
      GstCaps *tcaps, *intersection;
      const GList *tmps;

      GST_DEBUG ("Trying factory %s",
          gst_plugin_feature_get_name (GST_PLUGIN_FEATURE (factory)));

      if (gst_element_get_factory (src) == factory ||
          gst_element_factory_list_is_type (factory,
              GST_ELEMENT_FACTORY_TYPE_PARSER)) {
        GST_DEBUG ("Skipping factory");
        continue;
      }

      for (tmps = gst_element_factory_get_static_pad_templates (factory); tmps;
          tmps = tmps->next) {
        GstStaticPadTemplate *st = (GstStaticPadTemplate *) tmps->data;
        if (st->direction != GST_PAD_SINK || st->presence != GST_PAD_ALWAYS)
          continue;
        tcaps = gst_static_pad_template_get_caps (st);
        intersection =
            gst_caps_intersect_full (tcaps, caps, GST_CAPS_INTERSECT_FIRST);
        filter_caps = gst_caps_merge (filter_caps, intersection);
        gst_caps_unref (tcaps);
      }
    }

    /* Append the parser caps to prevent any not-negotiated errors */
    filter_caps = gst_caps_merge (filter_caps, gst_caps_ref (caps));

    pelem->capsfilter = gst_element_factory_make ("capsfilter", NULL);
    g_object_set (G_OBJECT (pelem->capsfilter), "caps", filter_caps, NULL);
    gst_caps_unref (filter_caps);
    gst_element_set_state (pelem->capsfilter, GST_STATE_PAUSED);
    gst_bin_add (GST_BIN_CAST (parsebin), gst_object_ref (pelem->capsfilter));

    parse_pad_set_target (parsepad, NULL);
    p = gst_element_get_static_pad (pelem->capsfilter, "sink");
    gst_pad_link_full (pad, p, GST_PAD_LINK_CHECK_NOTHING);
    gst_object_unref (p);
    p = gst_element_get_static_pad (pelem->capsfilter, "src");
    parse_pad_set_target (parsepad, p);
    pad = p;

    gst_caps_unref (caps);

    caps = gst_pad_get_current_caps (pad);
    if (!caps) {
      GST_DEBUG_OBJECT (parsebin,
          "No final caps set yet, delaying autoplugging");
      gst_object_unref (parsepad);
      g_value_array_free (factories);
      goto setup_caps_delay;
    }
  }

  /* 1.h else continue autoplugging something from the list. */
  GST_LOG_OBJECT (pad, "Let's continue discovery on this pad");
  res =
      priv_connect_pad (parsebin, src, parsepad, pad, caps, factories, chain,
      &deadend_details);

  /* Need to unref the capsfilter srcpad here if
   * we inserted a capsfilter */
  if (is_parser_converter)
    gst_object_unref (pad);

  gst_object_unref (parsepad);
  g_value_array_free (factories);

  if (!res)
    goto unknown_type;

  gst_caps_unref (caps);

  return;

expose_pad:
  {
    GST_LOG_OBJECT (parsebin, "Pad is final. autoplug-continue:%d", apcontinue);
    expose_pad (parsebin, src, parsepad, pad, caps, chain);
    gst_object_unref (parsepad);
    gst_caps_unref (caps);
    return;
  }

unknown_type:
  {
    GST_LOG_OBJECT (pad, "Unknown type, posting message and firing signal");

    chain->deadend_details = deadend_details;
    chain->deadend = TRUE;
    chain->drained = TRUE;
    chain->endcaps = caps;
    gst_object_replace ((GstObject **) & chain->current_pad, NULL);

    gst_element_post_message (GST_ELEMENT_CAST (parsebin),
        gst_missing_decoder_message_new (GST_ELEMENT_CAST (parsebin), caps));

    g_signal_emit (G_OBJECT (parsebin),
        gst_parse_bin_signals[SIGNAL_UNKNOWN_TYPE], 0, pad, caps);

    /* Try to expose anything */
    EXPOSE_LOCK (parsebin);
    if (parsebin->parse_chain) {
      if (gst_parse_chain_is_complete (parsebin->parse_chain)) {
        gst_parse_bin_expose (parsebin);
      }
    }
    EXPOSE_UNLOCK (parsebin);

    if (src == parsebin->typefind) {
      if (!caps || gst_caps_is_empty (caps)) {
        GST_SYS_ERROR_OBJECT (parsebin, "Could not determine type of stream");
        GST_ELEMENT_ERROR (parsebin, STREAM, TYPE_NOT_FOUND,
            (_("Could not determine type of stream")), (NULL));
      }
    }
    return;
  }
no_expose_pad:
  {
    GST_DEBUG_OBJECT (pad, "No need to expose pad. (chain : %p)", chain);

    chain->deadend = TRUE;
    chain->endcaps = NULL;
    gst_caps_unref (caps);
    gst_object_replace ((GstObject **) & chain->current_pad, NULL);

#if 0
    /* Try to expose anything */
    EXPOSE_LOCK (parsebin);
    if (gst_parse_chain_is_complete (parsebin->parse_chain)) {
      gst_parse_bin_expose (parsebin);
    }
    EXPOSE_UNLOCK (parsebin);
#endif
    return;
  }
#if 1
non_fixed:
  {
    GST_DEBUG_OBJECT (pad, "pad has non-fixed caps delay autoplugging");
    gst_object_unref (parsepad);
    goto setup_caps_delay;
  }
#endif
any_caps:
  {
    GST_DEBUG_OBJECT (pad, "pad has ANY caps, delaying auto-plugging");
    goto setup_caps_delay;
  }
setup_caps_delay:
  {
    GstPendingPad *ppad;

    /* connect to caps notification */
    CHAIN_MUTEX_LOCK (chain);
    GST_LOG_OBJECT (parsebin, "Chain %p has now %d dynamic pads", chain,
        g_list_length (chain->pending_pads));
    ppad = g_slice_new0 (GstPendingPad);
    ppad->pad = gst_object_ref (pad);
    ppad->chain = chain;
    ppad->event_probe_id =
        gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
        pad_event_cb, ppad, NULL);
    chain->pending_pads = g_list_prepend (chain->pending_pads, ppad);
    ppad->notify_caps_id = g_signal_connect (pad, "notify::caps",
        G_CALLBACK (caps_notify_cb), chain);
    CHAIN_MUTEX_UNLOCK (chain);

    /* If we're here because we have a Parser/Converter
     * we have to unref the pad */
    if (is_parser_converter)
      gst_object_unref (pad);
    if (caps)
      gst_caps_unref (caps);

    return;
  }
}

static gboolean is_simple_demuxer_factory (GstElementFactory * factory);
static void add_error_filter (GstParseBin * parsebin, GstElement * element);
static void remove_error_filter (GstParseBin * parsebin, GstElement * element,
    GstMessage ** error);
static gchar *error_message_to_string (GstMessage * msg);
static gboolean send_sticky_events (GstParseBin * parsebin, GstPad * pad);

static gboolean
continue_autoplug_chain (GstParseChain * chain, GstPad * pad, GstCaps * caps)
{
  GstParseBin *parsebin = chain->parsebin;
  GstStructure *caps_structure;
  const gchar *structure_name;
  const gchar *secure_area;
  gboolean res = FALSE;
  gboolean is_parsed = TRUE;

  caps_structure = gst_caps_get_structure (caps, 0);
  structure_name = gst_structure_get_name (caps_structure);

  if (g_strrstr (structure_name, "cenc")) {
    GST_DEBUG_OBJECT (parsebin,
        "chain(%p), pad(%s:%s), continue autoplug for secure stream", chain,
        GST_DEBUG_PAD_NAME (pad));
    res = TRUE;
    goto done;
  }
  GST_DEBUG_OBJECT (parsebin, "chain(%p), pad(%s:%s), It's clear stream", chain,
      GST_DEBUG_PAD_NAME (pad));

  secure_area = gst_structure_get_string (caps_structure, "secure_area");
  gst_structure_get_boolean (caps_structure, "parsed", &is_parsed);
  GST_DEBUG_OBJECT (parsebin, "secure_area(%s), is_parsed(%d)",
      secure_area ? secure_area : "NULL", is_parsed);

  if (secure_area && !g_strcmp0 (secure_area, "svp") && is_parsed) {
    GST_DEBUG_OBJECT (parsebin,
        "It's clear stream. But, it uses secure video path and already parsed. Expose pad");
    res = FALSE;
  } else
    res = TRUE;

done:
  return res;
}

static gboolean
priv_connect_pad (GstParseBin * parsebin, GstElement * src,
    GstParsePad * parsepad, GstPad * pad, GstCaps * caps,
    GValueArray * factories, GstParseChain * chain, gchar ** deadend_details)
{
  gboolean res = FALSE;
  GString *error_details = NULL;
  gboolean is_demuxer = chain->parent && !chain->elements;      /* First pad after the demuxer */
  GstElement *parent = NULL;
  GstElement *last_element = NULL;

  g_return_val_if_fail (factories != NULL, FALSE);
  g_return_val_if_fail (factories->n_values > 0, FALSE);

  GST_DEBUG_OBJECT (parsebin,
      "pad %s:%s , chain:%p, %d factories, caps %" GST_PTR_FORMAT,
      GST_DEBUG_PAD_NAME (pad), chain, factories->n_values, caps);

  if (chain->elements
      && (last_element =
          ((GstParseElement *) chain->elements->data)->element)) {
    const gchar *secure_area;
    if ((secure_area =
            gst_structure_get_string (gst_caps_get_structure (caps, 0),
                "secure_area")) != NULL) {
      GST_DEBUG_OBJECT (parsebin, "Found secure area(%s) !!", secure_area);
      if (!g_strcmp0 (secure_area, "svp")) {
        GST_DEBUG_OBJECT (parsebin,
            "We reach the last element in this chain. Expose pad(%s:%s)",
            GST_DEBUG_PAD_NAME (pad));
        /* expose the pad, we already have parser and converter */
        expose_pad (parsebin, src, parsepad, pad, caps, chain);
        res = TRUE;
        goto beach;
      }
    }
  }

  if (!continue_autoplug_chain (chain, pad, caps)) {
    /* expose the pad */
    expose_pad (parsebin, src, parsepad, pad, caps, chain);
    chain->parsed = TRUE;
    res = TRUE;
    goto beach;
  }

  /* 1. is element demuxer or parser */
  if (is_demuxer) {
    gboolean need_compositor = FALSE;

    gst_structure_get_boolean (gst_caps_get_structure (caps, 0),
        "need-compositor", &need_compositor);
    if (need_compositor) {
      chain->need_compositor = TRUE;
      parsebin->have_compositor = TRUE;
    }
  }

  error_details = g_string_new ("");

  /* 2. Try to create an element and link to it */
  while (factories->n_values > 0) {
    GstAutoplugSelectResult ret;
    GstElementFactory *factory;
    GstParseElement *pelem;
    GstElement *element;
    GstPad *sinkpad;
    GParamSpec *pspec;
    gboolean subtitle;
    GList *to_connect = NULL;
    gboolean is_parser_converter = FALSE, is_simple_demuxer = FALSE;
    gboolean no_need_to_create = FALSE, is_compositor_type = FALSE;

    /* Set parsepad target to pad again, it might've been unset
     * below but we came back here because something failed
     */
    parse_pad_set_target (parsepad, pad);

    /* take first factory */
    factory = g_value_get_object (g_value_array_get_nth (factories, 0));
    /* Remove selected factory from the list. */
    g_value_array_remove (factories, 0);

    GST_LOG_OBJECT (src, "trying factory %" GST_PTR_FORMAT, factory);

    /* Check if the caps are really supported by the factory. The
     * factory list is non-empty-subset filtered while caps
     * are only accepted by a pad if they are a subset of the
     * pad caps.
     *
     * FIXME: Only do this for fixed caps here. Non-fixed caps
     * can happen if a Parser/Converter was autoplugged before
     * this. We then assume that it will be able to convert to
     * everything that the parser would want.
     *
     * A subset check will fail here because the parser caps
     * will be generic and while the parser will only
     * support a subset of the parser caps.
     */
    if (gst_caps_is_fixed (caps)) {
      const GList *templs;
      gboolean skip = FALSE;

      templs = gst_element_factory_get_static_pad_templates (factory);

      while (templs) {
        GstStaticPadTemplate *templ = (GstStaticPadTemplate *) templs->data;

        if (templ->direction == GST_PAD_SINK) {
          GstCaps *templcaps = gst_static_caps_get (&templ->static_caps);

          if (!gst_caps_is_subset (caps, templcaps)) {
            GST_DEBUG_OBJECT (src,
                "caps %" GST_PTR_FORMAT " not subset of %" GST_PTR_FORMAT, caps,
                templcaps);
            gst_caps_unref (templcaps);
            skip = TRUE;
            break;
          }

          gst_caps_unref (templcaps);
        }
        templs = g_list_next (templs);
      }
      if (skip)
        continue;
    }

    /* If the factory is for a parser we first check if the factory
     * was already used for the current chain. If it was used already
     * we would otherwise create an infinite loop here because the
     * parser apparently accepts its own output as input.
     * This is only done for parsers because it's perfectly valid
     * to have other element classes after each other because a
     * parser is the only one that does not change the data. A
     * valid example for this would be multiple id3demux in a row.
     */
    is_parser_converter = strstr (gst_element_factory_get_metadata (factory,
            GST_ELEMENT_METADATA_KLASS), "Parser") != NULL;
    is_simple_demuxer = is_simple_demuxer_factory (factory);

    if (is_parser_converter) {
      gboolean skip = FALSE;
      GList *l;

      CHAIN_MUTEX_LOCK (chain);
      for (l = chain->elements; l; l = l->next) {
        GstParseElement *pelem = (GstParseElement *) l->data;
        GstElement *otherelement = pelem->element;

        if (gst_element_get_factory (otherelement) == factory) {
          skip = TRUE;
          break;
        }
      }

      if (!skip && chain->parent && chain->parent->parent) {
        GstParseChain *parent_chain = chain->parent->parent;
        GstParseElement *pelem =
            parent_chain->elements ? parent_chain->elements->data : NULL;

        if (pelem && gst_element_get_factory (pelem->element) == factory)
          skip = TRUE;
      }
      CHAIN_MUTEX_UNLOCK (chain);
      if (skip) {
        GST_DEBUG_OBJECT (parsebin,
            "Skipping factory '%s' because it was already used in this chain",
            gst_plugin_feature_get_name (GST_PLUGIN_FEATURE_CAST (factory)));
        continue;
      }

    }

    if (strstr (gst_element_factory_get_metadata (factory,
                GST_ELEMENT_METADATA_KLASS), "Parser")) {
      gboolean need_compositor = FALSE;
      gboolean is_parsed = FALSE;

      gst_structure_get_boolean (gst_caps_get_structure (caps, 0),
          "need-compositor", &need_compositor);
      GST_DEBUG_OBJECT (factory, "need-compositor : %d", need_compositor);

      gst_structure_get_boolean (gst_caps_get_structure (caps, 0),
          "parsed", &is_parsed);

      is_compositor_type = is_compositor_factory (factory);

      if (is_compositor_type && (!need_compositor || !is_parsed)) {
        GST_DEBUG_OBJECT (parsebin,
            "Skipping factory '%s' because it is not compositor type",
            gst_plugin_feature_get_name (GST_PLUGIN_FEATURE_CAST (factory)));
        continue;
      }
    }

    GST_DEBUG_OBJECT (factory, "compositor type : %d (chain : %p)",
        is_compositor_type, chain);

    /* Expose pads if the next factory is a decoder */
    if (gst_element_factory_list_is_type (factory,
            GST_ELEMENT_FACTORY_TYPE_DECODER)) {
      ret = GST_AUTOPLUG_SELECT_EXPOSE;
    } else {
      /* emit autoplug-select to see what we should do with it. */
      g_signal_emit (G_OBJECT (parsebin),
          gst_parse_bin_signals[SIGNAL_AUTOPLUG_SELECT],
          0, parsepad, caps, factory, &ret);
    }

    switch (ret) {
      case GST_AUTOPLUG_SELECT_TRY:
        GST_DEBUG_OBJECT (parsebin, "autoplug select requested try");
        break;
      case GST_AUTOPLUG_SELECT_EXPOSE:
        GST_DEBUG_OBJECT (parsebin, "autoplug select requested expose");
        /* expose the pad, we don't have the source element */
        expose_pad (parsebin, src, parsepad, pad, caps, chain);
        res = TRUE;
        goto beach;
      case GST_AUTOPLUG_SELECT_SKIP:
        GST_DEBUG_OBJECT (parsebin, "autoplug select requested skip");
        continue;
      default:
        GST_WARNING_OBJECT (parsebin, "autoplug select returned unhandled %d",
            ret);
        break;
    }

    /* 2.0. Unlink pad */
    parse_pad_set_target (parsepad, NULL);

    if (is_compositor_type) {
      /* FIXME: */
      COMPOSITOR_LOCK (parsebin);
      element = gst_parse_chain_get_compositor (chain->parent, chain->type);
      GST_DEBUG_OBJECT (factory, "obtain compositor = %p (chain : %p)", element,
          chain);
      if (!element) {
        if ((element = gst_element_factory_create (factory, NULL)) == NULL) {
          GST_WARNING_OBJECT (parsebin, "Could not create an element from %s",
              gst_plugin_feature_get_name (GST_PLUGIN_FEATURE (factory)));
          COMPOSITOR_UNLOCK (parsebin);
          continue;
        }
      } else
        no_need_to_create = TRUE;

      /* FIXME: */
      chain->compositor = element;
      if (no_need_to_create)
        chain->main_input = FALSE;
      else
        chain->main_input = TRUE;
    }
    /* 2.1. Try to create an element */
    else if ((element = gst_element_factory_create (factory, NULL)) == NULL) {
      GST_WARNING_OBJECT (parsebin, "Could not create an element from %s",
          gst_plugin_feature_get_name (GST_PLUGIN_FEATURE (factory)));
      g_string_append_printf (error_details,
          "Could not create an element from %s\n",
          gst_plugin_feature_get_name (GST_PLUGIN_FEATURE (factory)));
      continue;
    }

    /* Filter errors, this will prevent the element from causing the pipeline
     * to error while we test it using READY state. */
    add_error_filter (parsebin, element);

    /* We don't yet want the bin to control the element's state */
    gst_element_set_locked_state (element, TRUE);

    if (!no_need_to_create) {
      GST_DEBUG_OBJECT (element, "try to add bin = %p", chain);
      /* ... add it ... */
      if (!(gst_bin_add (GST_BIN_CAST (parsebin), element))) {
        GST_WARNING_OBJECT (parsebin, "Couldn't add %s to the bin",
            GST_ELEMENT_NAME (element));
        remove_error_filter (parsebin, element, NULL);
        g_string_append_printf (error_details, "Couldn't add %s to the bin\n",
            GST_ELEMENT_NAME (element));
        gst_object_unref (element);

        if (is_compositor_type) {
          COMPOSITOR_UNLOCK (parsebin);
        }
        continue;
      }
    }

    /* Find its sink pad. */
    if (is_compositor_type) {
      GST_DEBUG_OBJECT (element, "try to get request sinkpad, (chain : %p)",
          chain);
      COMPOSITOR_UNLOCK (parsebin);
      if (!(sinkpad = get_sink_pad_to_compositor (element))) {
        GST_WARNING_OBJECT (parsebin, "Element %s doesn't have a sink pad",
            GST_ELEMENT_NAME (element));
        remove_error_filter (parsebin, element, NULL);
        gst_bin_remove (GST_BIN (parsebin), element);
        continue;
      }
    } else {
      sinkpad = NULL;
      GST_OBJECT_LOCK (element);
      if (element->sinkpads != NULL)
        sinkpad = gst_object_ref (element->sinkpads->data);
      GST_OBJECT_UNLOCK (element);

      if (sinkpad == NULL) {
        GST_WARNING_OBJECT (parsebin, "Element %s doesn't have a sink pad",
            GST_ELEMENT_NAME (element));
        remove_error_filter (parsebin, element, NULL);
        g_string_append_printf (error_details,
            "Element %s doesn't have a sink pad", GST_ELEMENT_NAME (element));
        gst_bin_remove (GST_BIN (parsebin), element);
        continue;
      }
    }

    /* FIXME: In order to update all taglist to player until load complete,
     * we have to probe tag event and emit stream-ready signal on peerpad of demux's srcpad */
    if ((parent = gst_pad_get_parent_element (pad))
        && is_demuxer_element (parent)) {
      GST_DEBUG_OBJECT (sinkpad,
          "add probe to detect tag event, parsepad: %" GST_PTR_FORMAT,
          parsepad);
      gst_pad_add_probe (GST_PAD_CAST (sinkpad),
          GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, gst_demux_peer_event, parsepad,
          NULL);
    }
    if (parent)
      gst_object_unref (parent);

    /* ... and try to link */
    if ((gst_pad_link_full (pad, sinkpad,
                GST_PAD_LINK_CHECK_NOTHING)) != GST_PAD_LINK_OK) {
      GST_WARNING_OBJECT (parsebin, "Link failed on pad %s:%s",
          GST_DEBUG_PAD_NAME (sinkpad));
      remove_error_filter (parsebin, element, NULL);
      g_string_append_printf (error_details, "Link failed on pad %s:%s",
          GST_DEBUG_PAD_NAME (sinkpad));
      gst_object_unref (sinkpad);
      gst_bin_remove (GST_BIN (parsebin), element);
      continue;
    }

    /* ... activate it ... */
    if (!no_need_to_create) {
      if ((gst_element_set_state (element,
                  GST_STATE_READY)) == GST_STATE_CHANGE_FAILURE) {
        GstMessage *error_msg;

        GST_WARNING_OBJECT (parsebin, "Couldn't set %s to READY",
            GST_ELEMENT_NAME (element));
        remove_error_filter (parsebin, element, &error_msg);

        if (error_msg) {
          gchar *error_string = error_message_to_string (error_msg);
          g_string_append_printf (error_details,
              "Couldn't set %s to READY:\n%s", GST_ELEMENT_NAME (element),
              error_string);
          gst_message_unref (error_msg);
          g_free (error_string);
        } else {
          g_string_append_printf (error_details, "Couldn't set %s to READY",
              GST_ELEMENT_NAME (element));
        }
        gst_object_unref (sinkpad);
        gst_bin_remove (GST_BIN (parsebin), element);
        continue;
      }
    }

    /* check if we still accept the caps on the pad after setting
     * the element to READY */
    if (!gst_pad_query_accept_caps (sinkpad, caps)) {
      GstMessage *error_msg;

      GST_WARNING_OBJECT (parsebin, "Element %s does not accept caps",
          GST_ELEMENT_NAME (element));

      remove_error_filter (parsebin, element, &error_msg);

      if (error_msg) {
        gchar *error_string = error_message_to_string (error_msg);
        g_string_append_printf (error_details,
            "Element %s does not accept caps:\n%s", GST_ELEMENT_NAME (element),
            error_string);
        gst_message_unref (error_msg);
        g_free (error_string);
      } else {
        g_string_append_printf (error_details,
            "Element %s does not accept caps", GST_ELEMENT_NAME (element));
      }

      gst_element_set_state (element, GST_STATE_NULL);
      gst_object_unref (sinkpad);
      gst_bin_remove (GST_BIN (parsebin), element);
      continue;
    }

    gst_object_unref (sinkpad);
    GST_LOG_OBJECT (parsebin, "linked on pad %s:%s", GST_DEBUG_PAD_NAME (pad));

    CHAIN_MUTEX_LOCK (chain);
    pelem = g_slice_new0 (GstParseElement);
    pelem->element = gst_object_ref (element);
    pelem->capsfilter = NULL;
    chain->elements = g_list_prepend (chain->elements, pelem);
    chain->demuxer = is_demuxer_element (element);

    /* If we plugging a parser, mark the chain as parsed */
    chain->parsed |= is_parser_converter;

    if (chain->need_compositor) {
      GST_DEBUG_OBJECT (parsebin, "Set parsed FALSE to continue auto-plugging");
      chain->parsed = FALSE;
    }

    CHAIN_MUTEX_UNLOCK (chain);

    /* Set connection-speed property if needed */
    if (chain->demuxer) {
      GParamSpec *pspec;

      if ((pspec = g_object_class_find_property (G_OBJECT_GET_CLASS (element),
                  "connection-speed"))) {
        guint64 speed = parsebin->connection_speed / 1000;
        gboolean wrong_type = FALSE;

        if (G_PARAM_SPEC_TYPE (pspec) == G_TYPE_PARAM_UINT) {
          GParamSpecUInt *pspecuint = G_PARAM_SPEC_UINT (pspec);

          speed = CLAMP (speed, pspecuint->minimum, pspecuint->maximum);
        } else if (G_PARAM_SPEC_TYPE (pspec) == G_TYPE_PARAM_INT) {
          GParamSpecInt *pspecint = G_PARAM_SPEC_INT (pspec);

          speed = CLAMP (speed, pspecint->minimum, pspecint->maximum);
        } else if (G_PARAM_SPEC_TYPE (pspec) == G_TYPE_PARAM_UINT64) {
          GParamSpecUInt64 *pspecuint = G_PARAM_SPEC_UINT64 (pspec);

          speed = CLAMP (speed, pspecuint->minimum, pspecuint->maximum);
        } else if (G_PARAM_SPEC_TYPE (pspec) == G_TYPE_PARAM_INT64) {
          GParamSpecInt64 *pspecint = G_PARAM_SPEC_INT64 (pspec);

          speed = CLAMP (speed, pspecint->minimum, pspecint->maximum);
        } else {
          GST_WARNING_OBJECT (parsebin,
              "The connection speed property %" G_GUINT64_FORMAT " of type %s"
              " is not usefull not setting it", speed,
              g_type_name (G_PARAM_SPEC_TYPE (pspec)));
          wrong_type = TRUE;
        }

        if (!wrong_type) {
          GST_DEBUG_OBJECT (parsebin,
              "setting connection-speed=%" G_GUINT64_FORMAT
              " to demuxer element", speed);

          g_object_set (element, "connection-speed", speed, NULL);
        }
      }
    }

    /* try to configure the subtitle encoding property when we can */
    pspec = g_object_class_find_property (G_OBJECT_GET_CLASS (element),
        "subtitle-encoding");
    if (pspec && G_PARAM_SPEC_VALUE_TYPE (pspec) == G_TYPE_STRING) {
      SUBTITLE_LOCK (parsebin);
      GST_DEBUG_OBJECT (parsebin,
          "setting subtitle-encoding=%s to element", parsebin->encoding);
      g_object_set (G_OBJECT (element), "subtitle-encoding", parsebin->encoding,
          NULL);
      SUBTITLE_UNLOCK (parsebin);
      subtitle = TRUE;
    } else {
      subtitle = FALSE;
    }

    /* link this element further */
    to_connect = connect_element (parsebin, pelem, chain);

    if ((is_simple_demuxer || is_parser_converter) && to_connect) {
      GList *l;
      for (l = to_connect; l; l = g_list_next (l)) {
        GstPad *opad = GST_PAD_CAST (l->data);
        GstCaps *ocaps;

        ocaps = get_pad_caps (opad);
        priv_analyze_new_pad (parsebin, pelem->element, opad, ocaps, chain);
        if (ocaps)
          gst_caps_unref (ocaps);

        gst_object_unref (opad);
      }
      g_list_free (to_connect);
      to_connect = NULL;
    }

    /* Bring the element to the state of the parent */

    /* First lock element's sinkpad stream lock so no data reaches
     * the possible new element added when caps are sent by element
     * while we're still sending sticky events */
    GST_PAD_STREAM_LOCK (sinkpad);

    if ((is_compositor_type ? no_need_to_create : !no_need_to_create) &&
        ((gst_element_set_state (element,
                    GST_STATE_PAUSED)) == GST_STATE_CHANGE_FAILURE)) {
      GstParseElement *dtmp = NULL;
      GstElement *tmp = NULL;
      GstMessage *error_msg;

      GST_PAD_STREAM_UNLOCK (sinkpad);

      GST_WARNING_OBJECT (parsebin, "Couldn't set %s to PAUSED",
          GST_ELEMENT_NAME (element));

      g_list_free_full (to_connect, (GDestroyNotify) gst_object_unref);
      to_connect = NULL;

      remove_error_filter (parsebin, element, &error_msg);

      if (error_msg) {
        gchar *error_string = error_message_to_string (error_msg);
        g_string_append_printf (error_details, "Couldn't set %s to PAUSED:\n%s",
            GST_ELEMENT_NAME (element), error_string);
        gst_message_unref (error_msg);
        g_free (error_string);
      } else {
        g_string_append_printf (error_details, "Couldn't set %s to PAUSED",
            GST_ELEMENT_NAME (element));
      }

      /* Remove all elements in this chain that were just added. No
       * other thread could've added elements in the meantime */
      CHAIN_MUTEX_LOCK (chain);
      do {
        GList *l;

        dtmp = chain->elements->data;
        tmp = dtmp->element;

        /* Disconnect any signal handlers that might be connected
         * in connect_element() or analyze_pad() */
        if (dtmp->pad_added_id)
          g_signal_handler_disconnect (tmp, dtmp->pad_added_id);
        if (dtmp->pad_removed_id)
          g_signal_handler_disconnect (tmp, dtmp->pad_removed_id);
        if (dtmp->no_more_pads_id)
          g_signal_handler_disconnect (tmp, dtmp->no_more_pads_id);

        for (l = chain->pending_pads; l;) {
          GstPendingPad *pp = l->data;
          GList *n;

          if (GST_PAD_PARENT (pp->pad) != tmp) {
            l = l->next;
            continue;
          }

          gst_pending_pad_free (pp);

          /* Remove element from the list, update list head and go to the
           * next element in the list */
          n = l->next;
          chain->pending_pads = g_list_delete_link (chain->pending_pads, l);
          l = n;
        }

        if (dtmp->capsfilter) {
          gst_bin_remove (GST_BIN (parsebin), dtmp->capsfilter);
          gst_element_set_state (dtmp->capsfilter, GST_STATE_NULL);
          gst_object_unref (dtmp->capsfilter);
        }

        gst_bin_remove (GST_BIN (parsebin), tmp);
        gst_element_set_state (tmp, GST_STATE_NULL);

        gst_object_unref (tmp);
        g_slice_free (GstParseElement, dtmp);

        chain->elements = g_list_delete_link (chain->elements, chain->elements);
      } while (tmp != element);
      CHAIN_MUTEX_UNLOCK (chain);

      continue;
    } else {
      send_sticky_events (parsebin, pad);
      /* Everything went well, the spice must flow now */
      GST_PAD_STREAM_UNLOCK (sinkpad);
    }

    /* Remove error filter now, from now on we can't gracefully
     * handle errors of the element anymore */
    remove_error_filter (parsebin, element, NULL);

    /* Now let the bin handle the state */
    gst_element_set_locked_state (element, FALSE);

    if (subtitle) {
      SUBTITLE_LOCK (parsebin);
      /* we added the element now, add it to the list of subtitle-encoding
       * elements when we can set the property */
      parsebin->subtitles = g_list_prepend (parsebin->subtitles, element);
      SUBTITLE_UNLOCK (parsebin);
    }

    if (to_connect) {
      GList *l;
      for (l = to_connect; l; l = g_list_next (l)) {
        GstPad *opad = GST_PAD_CAST (l->data);
        GstCaps *ocaps;

        ocaps = get_pad_caps (opad);
        priv_analyze_new_pad (parsebin, pelem->element, opad, ocaps, chain);
        if (ocaps)
          gst_caps_unref (ocaps);

        gst_object_unref (opad);
      }
      g_list_free (to_connect);
      to_connect = NULL;
    }

    res = TRUE;
    break;
  }

beach:
  if (error_details)
    *deadend_details = g_string_free (error_details, (error_details->len == 0
            || res));
  else
    *deadend_details = NULL;

  return res;
}

static void
priv_query_smart_properties (GstParseBin * parse_bin)
{
  GstSmartPropertiesReturn ret;
  ret =
      gst_element_get_smart_properties (GST_ELEMENT_CAST (parse_bin),
      "fallback-element", &parse_bin->fallback_element,
      "adaptive-mode", &parse_bin->adaptive_mode, NULL);

  GST_INFO_OBJECT (parse_bin,
      "Result of smart properties: [%d], fallback-element: [%s]", ret,
      parse_bin->fallback_element);
}

static GstElement *
gst_parse_chain_get_compositor (GstParseGroup * group, GstStreamType type)
{
  GList *l;
  GstElement *compositor = NULL;

  for (l = group->children; l; l = l->next) {
    GstParseChain *chain = l->data;

    if (type != chain->type)
      continue;

    if (chain->compositor) {
      compositor = chain->compositor;
      goto out;
    }
  }

out:
  GST_DEBUG_OBJECT (group->parsebin, "Group: %p, type: %d, compositor: %p",
      group, type, compositor);
  return compositor;
}

static void
priv_parse_chain_new (GstParseChain * chain)
{
  chain->type = GST_STREAM_TYPE_UNKNOWN;
  chain->compositor = NULL;
  chain->main_input = FALSE;
  chain->need_compositor = FALSE;
  chain->fallback_element = NULL;
}

static void
priv_parse_chain_free (GstParseChain * chain)
{
  chain->type = GST_STREAM_TYPE_UNKNOWN;
  chain->compositor = NULL;
  chain->main_input = FALSE;
  chain->need_compositor = FALSE;
  chain->fallback_element = NULL;
}

static gboolean
priv_have_composite_stream (GstParseChain * chain)
{
  if (chain->parsebin->have_compositor
      && !chain->parsebin->have_composite_stream) {
    return FALSE;
  }
  return TRUE;
}

static void
build_fallback_element_internal (GstParseBin * parsebin, GstParsePad * parsepad,
    const gchar * name)
{
  GstPad *old_target;
  GstParseChain *chain = NULL;

  old_target = gst_ghost_pad_get_target (GST_GHOST_PAD_CAST (parsepad));
  chain = parsepad->chain;

  GST_DEBUG_OBJECT (parsebin, "old target %" GST_PTR_FORMAT, old_target);

  if (old_target) {
    GstParseElement *pelem = NULL;
    GstPad *p = NULL;

    gst_ghost_pad_set_target (GST_GHOST_PAD_CAST (parsepad), NULL);
    chain->fallback_element = gst_element_factory_make (name, NULL);
    gst_element_set_locked_state (chain->fallback_element, TRUE);

    g_object_set (G_OBJECT (chain->fallback_element), "stream-type",
        chain->type, NULL);

    gst_bin_add (GST_BIN_CAST (parsebin), chain->fallback_element);

    p = gst_element_get_static_pad (chain->fallback_element, "sink");
    gst_pad_link_full (old_target, p, GST_PAD_LINK_CHECK_NOTHING);
    gst_element_set_state (chain->fallback_element, GST_STATE_READY);

    CHAIN_MUTEX_LOCK (chain);
    pelem = g_slice_new0 (GstParseElement);
    pelem->element = gst_object_ref (chain->fallback_element);
    pelem->capsfilter = NULL;
    chain->elements = g_list_prepend (chain->elements, pelem);
    chain->demuxer = FALSE;
    CHAIN_MUTEX_UNLOCK (chain);

    GST_PAD_STREAM_LOCK (p);
    gst_element_set_state (chain->fallback_element, GST_STATE_PAUSED);
    GST_PAD_STREAM_UNLOCK (p);
    gst_element_set_locked_state (chain->fallback_element, FALSE);
    gst_object_unref (p);

    p = gst_element_get_static_pad (chain->fallback_element, "src");
    gst_ghost_pad_set_target (GST_GHOST_PAD_CAST (parsepad), p);

    /* FIXME: We have to change state to playing when parsebin is reconfigured in running time */
    if (GST_STATE (GST_ELEMENT (parsebin)) == GST_STATE_PLAYING) {
      GST_DEBUG_OBJECT (chain->fallback_element,
          "Try to change to playing state same as its parsebin");
      gst_element_set_state (chain->fallback_element, GST_STATE_PLAYING);
    }

    gst_object_unref (p);
    gst_object_unref (old_target);
  }
}

static void
priv_build_fallback_elements (GstParseBin * parsebin, GList * endpads)
{
  GList *tmp;

  for (tmp = endpads; tmp; tmp = tmp->next) {
    GstParsePad *parsepad = tmp->data;
    build_fallback_element_internal (parsebin, parsepad,
        parsebin->fallback_element);
  }
}

static void
priv_parse_bin_expose_compositor (GstParseBin * parsebin)
{
  /* FIXME: We have to wait until compositor have combined GstStream */
  if (gst_parse_chain_is_complete (parsebin->parse_chain)) {
    GST_INFO_OBJECT (parsebin,
        "Expose group after compositor have combined GstStream");
    if (!gst_parse_bin_expose (parsebin))
      GST_WARNING_OBJECT (parsebin, "Couldn't expose group");
  }
}

static void
priv_parse_pad_update_tags (GstParsePad * parsepad, GstTagList * tags)
{
  if (tags && gst_tag_list_get_scope (tags) == GST_TAG_SCOPE_STREAM
      && parsepad->active_stream) {
    GstTagList *oldtags = NULL, *newtags = NULL, *copy = NULL;
    gint i = 0;

    copy = gst_tag_list_copy (tags);

    /* Filter blacklist fields out */
    for (i = 0; blacklisted_tag_fields[i]; i++)
      gst_tag_list_remove_tag (copy, blacklisted_tag_fields[i]);

    oldtags = gst_stream_get_tags (parsepad->active_stream);

    GST_DEBUG_OBJECT (parsepad,
        "old tags: %" GST_PTR_FORMAT ", Storing new tags: %" GST_PTR_FORMAT
        " on stream %" GST_PTR_FORMAT, oldtags, copy, parsepad->active_stream);

    newtags = gst_tag_list_merge (oldtags, copy, GST_TAG_MERGE_REPLACE);
    gst_stream_set_tags (parsepad->active_stream, newtags);
    GST_DEBUG_OBJECT (parsepad, "Equal: %d, Stored new tags %" GST_PTR_FORMAT
        " on stream %" GST_PTR_FORMAT, oldtags ?
        gst_tag_list_is_equal (oldtags, newtags) : FALSE,
        newtags, parsepad->active_stream);

    if (oldtags)
      gst_tag_list_unref (oldtags);
    if (copy)
      gst_tag_list_unref (copy);
    if (newtags)
      gst_tag_list_unref (newtags);
  }
}

static void
priv_update_compsite_stream (GstParsePad * parsepad, const gchar * stream_id)
{
  if (parsepad->parsebin->have_compositor && strstr (stream_id, "composite")
      && !parsepad->parsebin->have_composite_stream) {
    GST_INFO_OBJECT (parsepad, "Got composite GstStream");
    parsepad->parsebin->have_composite_stream = TRUE;
  }
}

static gboolean gst_parse_chain_clear_drained (GstParseChain * chain);
/* gst_parse_chain/group_clear_drained:
 *
 * Goes up the chains/groups and clear drained marks
 * until it finds the top-level chain
 */
static gboolean
gst_parse_group_clear_drained (GstParseGroup * group)
{
  GST_LOG ("clear drained flag on group %p", group);

  group->drained = FALSE;

  if (group->parent)
    return gst_parse_chain_clear_drained (group->parent);

  return TRUE;
}

static gboolean
gst_parse_chain_clear_drained (GstParseChain * chain)
{
  GST_LOG ("clear drained flag on chain %p", chain);

  chain->drained = FALSE;

  if (chain->parent)
    return gst_parse_group_clear_drained (chain->parent);

  return TRUE;
}

static void
priv_parse_pad_add_probe (GstParsePad * parsepad, GstProxyPad * ppad)
{
  GST_LOG_OBJECT (parsepad, "Adding event probe on internal pad %"
      GST_PTR_FORMAT, ppad);
  gst_pad_add_probe (GST_PAD_CAST (ppad),
      GST_PAD_PROBE_TYPE_EVENT_BOTH | GST_PAD_PROBE_TYPE_EVENT_FLUSH,
      gst_parse_pad_event, parsepad, NULL);
}

static GstPadProbeReturn
priv_parse_pad_event (GstParsePad * parsepad, GstEvent * event,
    gboolean * steal)
{
  gboolean res = GST_PAD_PROBE_OK;

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_FLUSH_STOP:{
      GST_DEBUG_OBJECT (parsepad, "flush-stop, clear drained mark");
      CHAIN_MUTEX_LOCK (parsepad->chain);
      gst_parse_chain_clear_drained (parsepad->chain);
      *steal = TRUE;
      GST_DEBUG_OBJECT (parsepad, "remove delayed elements");
      g_list_free_full (parsepad->parsebin->delayed_to_null,
          free_delayed_to_null);
      parsepad->parsebin->delayed_to_null = NULL;
      CHAIN_MUTEX_UNLOCK (parsepad->chain);
      break;
    }
    case GST_EVENT_CUSTOM_UPSTREAM:{
      const GstStructure *s = NULL;
      GList *iter = NULL;
      const gchar *cenc_stream_id = NULL;
      GstPad *pad = NULL;
      GstElement *elem = NULL;
      GList *next = NULL;

      if (!gst_event_has_name (event, "unload-cenc")) {
        GST_DEBUG_OBJECT (event, "Unknown custom event");
        break;
      }

      s = gst_event_get_structure (event);

      /* Get stream-id */
      cenc_stream_id = gst_structure_get_string (s, "stream-id");
      GST_DEBUG_OBJECT (parsepad, "stream-id from event: %s", cenc_stream_id);

      /* Remove cenc with corresponding with cenc_stream_id */
      iter = parsepad->parsebin->delayed_to_null;
      while (iter != NULL && iter->data != NULL) {
        elem = (GstElement *) iter->data;
        next = iter->next;
        pad = gst_element_get_static_pad (elem, "src");
        if (pad) {
          gchar *stream_id = gst_pad_get_stream_id (pad);
          if (!g_strcmp0 (cenc_stream_id, stream_id)) {
            gchar *name = gst_element_get_name (elem);
            GST_DEBUG_OBJECT (event, "Found the element: %s, Removing..", name);

            /* remove from list */
            gst_element_set_state (elem, GST_STATE_NULL);
            gst_object_unref (elem);
            parsepad->parsebin->delayed_to_null
                =
                g_list_delete_link (parsepad->parsebin->delayed_to_null, iter);

            gst_object_unref (pad);
            g_free (name);
            g_free (stream_id);
            break;
          }
          g_free (stream_id);
          gst_object_unref (pad);
        }
        iter = next;
      }
      *steal = TRUE;
      res = GST_PAD_PROBE_DROP;
      break;
    }
    default:
      break;
  }

  return res;
}

static GstPad *
get_sink_pad_to_compositor (GstElement * element)
{
  GstPad *pad = NULL;

  pad = gst_element_get_request_pad (element, "sink_%u");

  return pad;
}

static gboolean
priv_check_delayed_set_to_null (GstParseChain * chain, GstElement * element)
{
  GstParseBin *parsebin;
  GstElementFactory *factory;
  const gchar *classification;
  GstPad *pad = NULL;
  gboolean ret = FALSE;

  parsebin = chain->parsebin;
  factory = gst_element_get_factory (element);
  classification =
      gst_element_factory_get_metadata (factory, GST_ELEMENT_METADATA_KLASS);

  if (chain->last_caps)
    goto last_caps;

  /* Check last parsed caps */
  pad = gst_element_get_static_pad (element, "src");
  if (pad) {
    chain->last_caps = get_pad_caps (pad);
    if (chain->last_caps)
      GST_LOG_OBJECT (pad, "Current CAPS: %" GST_PTR_FORMAT, chain->last_caps);
    gst_object_unref (pad);
  }

  if (!chain->last_caps) {
    GstQuery *query = gst_query_new_caps (NULL);
    if (gst_element_query (element, query)) {
      gst_query_parse_caps_result (query, &chain->last_caps);
      if (chain->last_caps) {
        gst_caps_ref (chain->last_caps);
        GST_LOG_OBJECT (element, "Current Query CAPS: %" GST_PTR_FORMAT,
            chain->last_caps);
      }
    }
    gst_query_unref (query);
  }

  if (!chain->last_caps) {
    GST_LOG_OBJECT (element, "No last caps");
    goto done;
  }

last_caps:
  {
    gchar *caps_str = gst_caps_to_string (chain->last_caps);

    GST_LOG_OBJECT (parsebin, "last caps: %s", caps_str);
    if (strstr (classification, "Decryptor") && !parsebin->shutdown
        && g_strrstr (caps_str, "secure_area")) {
      GST_DEBUG_OBJECT (parsebin, "Adding (%s) to 'delayed_to_null' LIST",
          GST_OBJECT_NAME (element));
      parsebin->delayed_to_null =
          g_list_prepend (parsebin->delayed_to_null, element);
      ret = TRUE;
    }
    g_free (caps_str);
  }

done:
  {
    GST_LOG_OBJECT (parsebin, "(%s) %s, Klass(%s), Type(%s), shutdown(%d)",
        GST_OBJECT_NAME (element),
        (ret ? "don't unload right now" : "set to NULL"), classification,
        gst_stream_type_get_name (chain->type), parsebin->shutdown);

    return ret;
  }
}
