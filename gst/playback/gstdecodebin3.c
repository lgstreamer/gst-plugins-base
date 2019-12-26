/* GStreamer
 *
 * Copyright (C) <2015> Centricular Ltd
 *  @author: Edward Hervey <edward@centricular.com>
 *  @author: Jan Schmidt <jan@centricular.com>
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

#include <glib.h>
#include <glib-object.h>
#include <glib/gprintf.h>
#include <gst/gst.h>
#include <gst/pbutils/pbutils.h>
#include <gst/gst-i18n-plugin.h>

#include "gstplayback.h"
#include "gstplay-enum.h"
#include "gstrawcaps.h"

/**
 * SECTION:element-decodebin3
 * @title: decodebin3
 *
 * #GstBin that auto-magically constructs a decoding pipeline using available
 * decoders and demuxers via auto-plugging. The output is raw audio, video
 * or subtitle streams.
 *
 * decodebin3 differs from the previous decodebin (decodebin2) in important ways:
 *
 * * supports publication and selection of stream information via
 * GstStreamCollection messages and #GST_EVENT_SELECT_STREAM events.
 *
 * * dynamically switches stream connections internally, and
 * reuses decoder elements when stream selections change, so that in
 * the normal case it maintains 1 decoder of each type (video/audio/subtitle)
 * and only creates new elements when streams change and an existing decoder
 * is not capable of handling the new format.
 *
 * * supports multiple input pads for the parallel decoding of auxilliary streams
 * not muxed with the primary stream.
 *
 * * does not handle network stream buffering. decodebin3 expects that network stream
 * buffering is handled upstream, before data is passed to it.
 *
 * <emphasis>decodebin3 is still experimental API and a technology preview.
 * Its behaviour and exposed API is subject to change.</emphasis>
 *
 */

/**
 * Global design
 *
 * 1) From sink pad to elementary streams (GstParseBin)
 *
 * The input sink pads are fed to GstParseBin. GstParseBin will feed them
 * through typefind. When the caps are detected (or changed) we recursively
 * figure out which demuxer, parser or depayloader is needed until we get to
 * elementary streams.
 *
 * All elementary streams (whether decoded or not, whether exposed or not) are
 * fed through multiqueue. There is only *one* multiqueue in decodebin3.
 *
 * => MultiQueue is the cornerstone.
 * => No buffering before multiqueue
 *
 * 2) Elementary streams
 *
 * After GstParseBin, there are 3 main components:
 *  1) Input Streams (provided by GstParseBin)
 *  2) Multiqueue slots
 *  3) Output Streams
 *
 * Input Streams correspond to the stream coming from GstParseBin and that gets
 * fed into a multiqueue slot.
 *
 * Output Streams correspond to the combination of a (optional) decoder and an
 * output ghostpad. Output Streams can be moved from one multiqueue slot to
 * another, can reconfigure itself (different decoders), and can be
 * added/removed depending on the configuration (all streams outputted, only one
 * of each type, ...).
 *
 * Multiqueue slots correspond to a pair of sink/src pad from multiqueue. For
 * each 'active' Input Stream there is a corresponding slot.
 * Slots might have different streams on input and output (due to internal
 * buffering).
 *
 * Due to internal queuing/buffering/..., all those components (might) behave
 * asynchronously. Therefore probes will be used on each component source pad to
 * detect various key-points:
 *  * EOS :
 *     the stream is done => Mark that component as done, optionally freeing/removing it
 *  * STREAM_START :
 *     a new stream is starting => link it further if needed
 *
 * 3) Gradual replacement
 *
 * If the caps change at any point in decodebin (input sink pad, demuxer output,
 * multiqueue output, ..), we gradually replace (if needed) the following elements.
 *
 * This is handled by the probes in various locations:
 *  a) typefind output
 *  b) multiqueue input (source pad of Input Streams)
 *  c) multiqueue output (source pad of Multiqueue Slots)
 *  d) final output (target of source ghostpads)
 *
 * When CAPS event arrive at those points, one of three things can happen:
 * a) There is no elements downstream yet, just create/link-to following elements
 * b) There are downstream elements, do a ACCEPT_CAPS query
 *  b.1) The new CAPS are accepted, keep current configuration
 *  b.2) The new CAPS are not accepted, remove following elements then do a)
 *
 *    Components:
 *
 *                                                   MultiQ     Output
 *                     Input(s)                      Slots      Streams
 *  /-------------------------------------------\   /-----\  /------------- \
 *
 * +-------------------------------------------------------------------------+
 * |                                                                         |
 * | +---------------------------------------------+                         |
 * | |   GstParseBin(s)                            |                         |
 * | |                +--------------+             |  +-----+                |
 * | |                |              |---[parser]-[|--| Mul |---[ decoder ]-[|
 * |]--[ typefind ]---|  demuxer(s)  |------------[|  | ti  |                |
 * | |                |  (if needed) |---[parser]-[|--| qu  |                |
 * | |                |              |---[parser]-[|--| eu  |---[ decoder ]-[|
 * | |                +--------------+             |  +------             ^  |
 * | +---------------------------------------------+        ^             |  |
 * |                                               ^        |             |  |
 * +-----------------------------------------------+--------+-------------+--+
 *                                                 |        |             |
 *                                                 |        |             |
 *                                       Probes  --/--------/-------------/
 *
 * ATOMIC SWITCHING
 *
 * We want to ensure we re-use decoders when switching streams. This takes place
 * at the multiqueue output level.
 *
 * MAIN CONCEPTS
 *  1) Activating a stream (i.e. linking a slot to an output) is only done within
 *    the streaming thread in the multiqueue_src_probe() and only if the
      stream is in the REQUESTED selection.
 *  2) Deactivating a stream (i.e. unlinking a slot from an output) is also done
 *    within the stream thread, but only in a purposefully called IDLE probe
 *    that calls reassign_slot().
 *
 * Based on those two principles, 3 "selection" of streams (stream-id) are used:
 * 1) requested_selection
 *    All streams within that list should be activated
 * 2) active_selection
 *    List of streams that are exposed by decodebin
 * 3) to_activate
 *    List of streams that will be moved to requested_selection in the
 *    reassign_slot() method (i.e. once a stream was deactivated, and the output
 *    was retargetted)
 */


GST_DEBUG_CATEGORY_STATIC (decodebin3_debug);
#define GST_CAT_DEFAULT decodebin3_debug

#define GST_TYPE_DECODEBIN3	 (gst_decodebin3_get_type ())

#define EXTRA_DEBUG 1

#define CUSTOM_FINAL_EOS_QUARK _custom_final_eos_quark_get ()
#define CUSTOM_FINAL_EOS_QUARK_DATA "custom-final-eos"
static GQuark
_custom_final_eos_quark_get (void)
{
  static gsize g_quark;

  if (g_once_init_enter (&g_quark)) {
    gsize quark =
        (gsize) g_quark_from_static_string ("decodebin3-custom-final-eos");
    g_once_init_leave (&g_quark, quark);
  }
  return g_quark;
}

typedef struct _GstDecodebin3 GstDecodebin3;
typedef struct _GstDecodebin3Class GstDecodebin3Class;

typedef struct _DecodebinInputStream DecodebinInputStream;
typedef struct _DecodebinInput DecodebinInput;
typedef struct _DecodebinOutputStream DecodebinOutputStream;

#define GST_DECODEBIN3_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_CLASS((obj),GST_TYPE_DECODEBIN3, GstDecodebin3Class))
typedef struct _MultiQueueSlot MultiQueueSlot;

struct _GstDecodebin3
{
  GstBin bin;

  /* input_lock protects the following variables */
  GMutex input_lock;
  /* Main input (static sink pad) */
  DecodebinInput *main_input;
  /* Supplementary input (request sink pads) */
  GList *other_inputs;
  /* counter for input */
  guint32 input_counter;
  /* Current stream group_id (default : GST_GROUP_ID_INVALID) */
  /* FIXME : Needs to be resetted appropriately (when upstream changes ?) */
  guint32 current_group_id;
  /* End of variables protected by input_lock */

  GstElement *multiqueue;

  /* selection_lock protects access to following variables */
  GMutex selection_lock;
  GList *input_streams;         /* List of DecodebinInputStream for active collection */
  GList *output_streams;        /* List of DecodebinOutputStream used for output */
  GList *slots;                 /* List of MultiQueueSlot */
  guint slot_id;

  /* Active collection */
  GstStreamCollection *collection;
  /* requested selection of stream-id to activate post-multiqueue */
  GList *requested_selection;
  /* list of stream-id currently activated in output */
  GList *active_selection;
  /* List of stream-id that need to be activated (after a stream switch for ex) */
  GList *to_activate;
  /* Pending select streams event */
  guint32 select_streams_seqnum;
  /* pending list of streams to select (from downstream) */
  GList *pending_select_streams;
  /* TRUE if requested_selection was updated, will become FALSE once
   * it has fully transitioned to active */
  gboolean selection_updated;
  /* End of variables protected by selection_lock */

  /* List of pending collections.
   * FIXME : Is this really needed ? */
  GList *pending_collection;

  /* Factories */
  GMutex factories_lock;
  guint32 factories_cookie;
  /* All DECODABLE factories */
  GList *factories;
  /* Only DECODER factories */
  GList *decoder_factories;
  /* DECODABLE but not DECODER factories */
  GList *decodable_factories;

  /* counters for pads */
  guint32 apadcount, vpadcount, tpadcount, opadcount;

  /* Properties */
  GstCaps *caps;

  GstStreamCollection *active_collection;       /* active collection (from output) */
  /* TRUE if any stream-collection message was posted to parent bin */
  gboolean collection_posted;

  /* For resource related */
  GstStructure *resource_info;
  gboolean request_resource;
  /* exposed pads(streams) from parsebin */
  GList *exposed_pads;

  /* Check whether PTS is continuity before pushing decoder'input */
  gboolean monitor_pts_continuity;

  /* Check whether to pushing gap event or not */
  gboolean use_fallback_preroll;

  /* Check adaptive mode */
  gboolean adaptive_mode;

  /* check dvr mode */
  gboolean dvr_playback;
};

struct _GstDecodebin3Class
{
  GstBinClass class;

    gint (*select_stream) (GstDecodebin3 * dbin,
      GstStreamCollection * collection, GstStream * stream);

  /* signal fired when we found a pad that we cannot decode */
  void (*unknown_type) (GstElement * element, GstPad * pad, GstCaps * caps);

  /* Virtual Functions for webOS Platform */
  void (*priv_decodebin3_init) (GstDecodebin3 * dbin);
  void (*priv_decodebin3_dispose) (GstDecodebin3 * dbin);
  void (*priv_parsebin_init) (GstDecodebin3 * dbin, DecodebinInput * input);
  GstStreamCollection *(*priv_get_sorted_collection) (GstDecodebin3 * dbin,
      GstStreamCollection * collection);
  void (*priv_append_exposed_pad) (GstDecodebin3 * dbin, GstPad * pad);
  void (*priv_query_smart_properties) (GstDecodebin3 * dbin,
      DecodebinInput * input);
  void (*priv_set_monitor_pts_continuity) (GstDecodebin3 * dbin,
      DecodebinInput * input);
  void (*priv_create_new_input) (GstDecodebin3 * dbin, DecodebinInput * input);
  void (*priv_update_requested_selection) (GstDecodebin3 * dbin,
      GstStreamCollection * collection);
  void (*priv_decodebin3_handle_message) (GstDecodebin3 * dbin,
      GstMessage * message, gboolean * steal, gboolean * do_post);
  GstMessage *(*priv_is_selection_done) (GstDecodebin3 * dbin);
  void (*priv_create_new_slot) (GstDecodebin3 * dbin, MultiQueueSlot * slot);
  void (*priv_free_multiqueue_slot) (GstDecodebin3 * dbin,
      MultiQueueSlot * slot);
    gboolean (*priv_handle_stream_switch) (GstDecodebin3 * dbin,
      GList * select_streams, guint32 seqnum);
    GstPadProbeReturn (*priv_ghost_pad_event_probe) (GstDecodebin3 * dbin,
      GstPad * pad, GstEvent * event, DecodebinOutputStream * output,
      gboolean * steal);
  void (*priv_release_resource_related) (GstDecodebin3 * dbin);
  void (*priv_remove_exposed_pad) (GstDecodebin3 * dbin, GstPad * pad);
    gboolean (*priv_check_slot_for_eos) (GstDecodebin3 * dbin,
      MultiQueueSlot * slot);
  void (*priv_remove_input_stream) (GstDecodebin3 * dbin, GstPad * pad,
      DecodebinInputStream * input);
  void (*priv_reconfigure_output_stream) (DecodebinOutputStream * output,
      MultiQueueSlot * slot, gboolean reassign);
};

/* Input of decodebin, controls input pad and parsebin */
struct _DecodebinInput
{
  GstDecodebin3 *dbin;

  gboolean is_main;

  GstPad *ghost_sink;
  GstPad *parsebin_sink;

  GstStreamCollection *collection;      /* Active collection */

  guint group_id;

  GstElement *parsebin;

  gulong pad_added_sigid;
  gulong pad_removed_sigid;
  gulong drained_sigid;

  /* TRUE if the input got drained
   * FIXME : When do we reset it if re-used ?
   */
  gboolean drained;

  /* HACK : Remove these fields */
  /* List of PendingPad structures */
  GList *pending_pads;

  GMutex input_lock;

  /* For resource related */
  gboolean has_demuxer;
  gboolean demuxer_no_more_pads;
};

/* Multiqueue Slots */
typedef struct _MultiQueueSlot
{
  guint id;

  GstDecodebin3 *dbin;
  /* Type of stream handled by this slot */
  GstStreamType type;

  /* Linked input and output */
  DecodebinInputStream *input;
  DecodebinInput *parent_input;

  /* pending => last stream received on sink pad */
  GstStream *pending_stream;
  /* active => last stream outputted on source pad */
  GstStream *active_stream;

  GstPad *sink_pad, *src_pad;

  /* id of the MQ src_pad event probe */
  gulong probe_id;

  gboolean is_drained;

  DecodebinOutputStream *output;

  /* temporal active stream but to be deactivated with selection done */
  GstStream *old_stream;

  gulong upstream_probe_id;

  gboolean mark_async_pool;

  gboolean plugging_decoder;
} MultiQueueSlot;

/* Streams that are exposed downstream (i.e. output) */
struct _DecodebinOutputStream
{
  GstDecodebin3 *dbin;
  /* The type of stream handled by this output stream */
  GstStreamType type;

  /* The slot to which this output stream is currently connected to */
  MultiQueueSlot *slot;

  GstElement *decoder;          /* Optional */
  GstPad *decoder_sink, *decoder_src;
  gboolean linked;

  /* ghostpad */
  GstPad *src_pad;
  /* Flag if ghost pad is exposed */
  gboolean src_exposed;

  /* keyframe dropping probe */
  gulong drop_probe_id;

  /* For resource related */
  gboolean puppet_done;

  GstSegment segment;
  GstClockTime last_pushed_ts;

  gboolean got_flush_start;
  gboolean got_flush_stop;
};

/* Pending pads from parsebin */
typedef struct _PendingPad
{
  GstDecodebin3 *dbin;
  DecodebinInput *input;
  GstPad *pad;

  gulong buffer_probe;
  gulong event_probe;
  gboolean saw_eos;
  GstEvent *gap_event;
} PendingPad;

/* properties */
enum
{
  PROP_0,
  PROP_CAPS
};

/* signals */
enum
{
  SIGNAL_SELECT_STREAM,
  SIGNAL_ABOUT_TO_FINISH,
  SIGNAL_UNKNOWN_TYPE,
  LAST_SIGNAL
};
static guint gst_decodebin3_signals[LAST_SIGNAL] = { 0 };

#define SELECTION_LOCK(dbin) G_STMT_START {				\
    GST_LOG_OBJECT (dbin,						\
		    "selection locking from thread %p",			\
		    g_thread_self ());					\
    g_mutex_lock (&dbin->selection_lock);				\
    GST_LOG_OBJECT (dbin,						\
		    "selection locked from thread %p",			\
		    g_thread_self ());					\
  } G_STMT_END

#define SELECTION_UNLOCK(dbin) G_STMT_START {				\
    GST_LOG_OBJECT (dbin,						\
		    "selection unlocking from thread %p",		\
		    g_thread_self ());					\
    g_mutex_unlock (&dbin->selection_lock);				\
  } G_STMT_END

#define INPUT_LOCK(dbin) G_STMT_START {				\
    GST_LOG_OBJECT (dbin,						\
		    "input locking from thread %p",			\
		    g_thread_self ());					\
    g_mutex_lock (&dbin->input_lock);				\
    GST_LOG_OBJECT (dbin,						\
		    "input locked from thread %p",			\
		    g_thread_self ());					\
  } G_STMT_END

#define INPUT_UNLOCK(dbin) G_STMT_START {				\
    GST_LOG_OBJECT (dbin,						\
		    "input unlocking from thread %p",		\
		    g_thread_self ());					\
    g_mutex_unlock (&dbin->input_lock);				\
  } G_STMT_END

#ifndef GST_DISABLE_GST_DEBUG
#define DUMP_SELECTION_LIST(dbin, list, name) G_STMT_START {			\
    GList *tmp;									\
    for (tmp = list; tmp; tmp = tmp->next) {					\
      gchar *sid = (gchar *) tmp->data;						\
      GST_LOG_OBJECT (dbin, "#stream-id(%s) in %s selection", sid, name);	\
    }										\
  } G_STMT_END
#else
#define DUMP_SELECTION_LIST(dbin, list, name) (void) 0
#endif


GType gst_decodebin3_get_type (void);
#define gst_decodebin3_parent_class parent_class
G_DEFINE_TYPE (GstDecodebin3, gst_decodebin3, GST_TYPE_BIN);

static GstStaticCaps default_raw_caps = GST_STATIC_CAPS (DEFAULT_RAW_CAPS);

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);

static GstStaticPadTemplate request_sink_template =
GST_STATIC_PAD_TEMPLATE ("sink_%u",
    GST_PAD_SINK,
    GST_PAD_REQUEST,
    GST_STATIC_CAPS_ANY);

static GstStaticPadTemplate video_src_template =
GST_STATIC_PAD_TEMPLATE ("video_%u",
    GST_PAD_SRC,
    GST_PAD_SOMETIMES,
    GST_STATIC_CAPS_ANY);

static GstStaticPadTemplate audio_src_template =
GST_STATIC_PAD_TEMPLATE ("audio_%u",
    GST_PAD_SRC,
    GST_PAD_SOMETIMES,
    GST_STATIC_CAPS_ANY);

static GstStaticPadTemplate text_src_template =
GST_STATIC_PAD_TEMPLATE ("text_%u",
    GST_PAD_SRC,
    GST_PAD_SOMETIMES,
    GST_STATIC_CAPS_ANY);

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE ("src_%u",
    GST_PAD_SRC,
    GST_PAD_SOMETIMES,
    GST_STATIC_CAPS_ANY);


static void gst_decodebin3_dispose (GObject * object);
static void gst_decodebin3_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_decodebin3_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

static gboolean parsebin_autoplug_continue_cb (GstElement *
    parsebin, GstPad * pad, GstCaps * caps, GstDecodebin3 * dbin);

static gint
gst_decodebin3_select_stream (GstDecodebin3 * dbin,
    GstStreamCollection * collection, GstStream * stream)
{
  GST_LOG_OBJECT (dbin, "default select-stream, returning -1");

  return -1;
}

static GstPad *gst_decodebin3_request_new_pad (GstElement * element,
    GstPadTemplate * temp, const gchar * name, const GstCaps * caps);
static void gst_decodebin3_handle_message (GstBin * bin, GstMessage * message);
static GstStateChangeReturn gst_decodebin3_change_state (GstElement * element,
    GstStateChange transition);
static gboolean gst_decodebin3_send_event (GstElement * element,
    GstEvent * event);

static void gst_decode_bin_update_factories_list (GstDecodebin3 * dbin);
#if 0
static gboolean have_factory (GstDecodebin3 * dbin, GstCaps * caps,
    GstElementFactoryListType ftype);
#endif

static void free_input (GstDecodebin3 * dbin, DecodebinInput * input);
static void free_input_async (GstDecodebin3 * dbin, DecodebinInput * input);
static DecodebinInput *create_new_input (GstDecodebin3 * dbin, gboolean main);
static gboolean set_input_group_id (DecodebinInput * input, guint32 * group_id);

static void reconfigure_output_stream (DecodebinOutputStream * output,
    MultiQueueSlot * slot);
static void free_output_stream (GstDecodebin3 * dbin,
    DecodebinOutputStream * output);
static DecodebinOutputStream *create_output_stream (GstDecodebin3 * dbin,
    GstStreamType type);

static GstPadProbeReturn slot_unassign_probe (GstPad * pad,
    GstPadProbeInfo * info, MultiQueueSlot * slot);
static gboolean reassign_slot (GstDecodebin3 * dbin, MultiQueueSlot * slot);
static MultiQueueSlot *get_slot_for_input (GstDecodebin3 * dbin,
    DecodebinInputStream * input);
static void link_input_to_slot (DecodebinInput * parent_input,
    DecodebinInputStream * input, MultiQueueSlot * slot);
static void free_multiqueue_slot (GstDecodebin3 * dbin, MultiQueueSlot * slot);
static void free_multiqueue_slot_async (GstDecodebin3 * dbin,
    MultiQueueSlot * slot);

static GstStreamCollection *get_merged_collection (GstDecodebin3 * dbin);
static void update_requested_selection (GstDecodebin3 * dbin);
static GstPadProbeReturn keyframe_waiter_probe (GstPad * pad,
    GstPadProbeInfo * info, DecodebinOutputStream * output);
static GstElement *create_decoder (GstDecodebin3 * dbin, GstStream * stream);
static const gchar *stream_in_list (GList * list, const gchar * sid);
static void handle_stream_collection (GstDecodebin3 * dbin,
    GstStreamCollection * collection, GstElement * child);
static DecodebinOutputStream *get_output_for_slot (MultiQueueSlot * slot);
static gboolean check_all_slot_for_eos (GstDecodebin3 * dbin);
static MultiQueueSlot *find_slot_for_stream_id (GstDecodebin3 * dbin,
    const gchar * sid);
static const gchar *stream_in_collection (GstDecodebin3 * dbin, gchar * sid);
static GstPadProbeReturn idle_reconfigure (GstPad * pad, GstPadProbeInfo * info,
    MultiQueueSlot * slot);
static DecodebinInput *find_message_parsebin (GstDecodebin3 * dbin,
    GstElement * child);
static DecodebinInput *get_input_for_slot (GstDecodebin3 * dbin,
    MultiQueueSlot * slot);
static GstStream *find_stream_in_collection (GstStreamCollection * collection,
    const gchar * sid);

/* FIXME: Really make all the parser stuff a self-contained helper object */
#include "gstdecodebin3-parse.c"

#ifdef HAVE_PRIV_FUNC
#include "gstdecodebin3-tv.c"
#endif

static gboolean
_gst_int_accumulator (GSignalInvocationHint * ihint,
    GValue * return_accu, const GValue * handler_return, gpointer dummy)
{
  gint res = g_value_get_int (handler_return);

  if (!(ihint->run_type & G_SIGNAL_RUN_CLEANUP))
    g_value_set_int (return_accu, res);

  if (res == -1)
    return TRUE;

  return FALSE;
}

static void
unknown_type_cb (GstElement * element, GstPad * pad, GstCaps * caps,
    GstDecodebin3 * dbin)
{
  gchar *capsstr;
  GstDecodebin3Class *klass = GST_DECODEBIN3_GET_CLASS (dbin);

  GST_DEBUG_OBJECT (dbin, "unknown-type signaled");

  if (G_LIKELY (klass->priv_remove_exposed_pad))
    klass->priv_remove_exposed_pad (dbin, pad);

  capsstr = gst_caps_to_string (caps);
  GST_ELEMENT_WARNING (dbin, STREAM, CODEC_NOT_FOUND,
      (_("No decoder available for type \'%s\'."), capsstr), (NULL));
  g_free (capsstr);
}

static void
gst_decodebin3_class_init (GstDecodebin3Class * klass)
{
  GObjectClass *gobject_klass = (GObjectClass *) klass;
  GstElementClass *element_class = (GstElementClass *) klass;
  GstBinClass *bin_klass = (GstBinClass *) klass;

  gobject_klass->dispose = gst_decodebin3_dispose;
  gobject_klass->set_property = gst_decodebin3_set_property;
  gobject_klass->get_property = gst_decodebin3_get_property;

  /* FIXME : ADD PROPERTIES ! */
  g_object_class_install_property (gobject_klass, PROP_CAPS,
      g_param_spec_boxed ("caps", "Caps",
          "The caps on which to stop decoding. (NULL = default)",
          GST_TYPE_CAPS, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /* FIXME : ADD SIGNALS ! */
  /**
   * GstDecodebin3::select-stream
   * @decodebin: a #GstDecodebin3
   * @collection: a #GstStreamCollection
   * @stream: a #GstStream
   *
   * This signal is emitted whenever @decodebin needs to decide whether
   * to expose a @stream of a given @collection.
   *
   * Returns: 1 if the stream should be selected, 0 if it shouldn't be selected.
   * A value of -1 (default) lets @decodebin decide what to do with the stream.
   * */
  gst_decodebin3_signals[SIGNAL_SELECT_STREAM] =
      g_signal_new ("select-stream", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET (GstDecodebin3Class, select_stream),
      _gst_int_accumulator, NULL, g_cclosure_marshal_generic,
      G_TYPE_INT, 2, GST_TYPE_STREAM_COLLECTION, GST_TYPE_STREAM);

  /**
   * GstDecodebin3::about-to-finish:
   *
   * This signal is emitted when the data for the selected URI is
   * entirely buffered and it is safe to specify anothe URI.
   */
  gst_decodebin3_signals[SIGNAL_ABOUT_TO_FINISH] =
      g_signal_new ("about-to-finish", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, g_cclosure_marshal_generic, G_TYPE_NONE,
      0, G_TYPE_NONE);

  /**
   * GstDecodeBin3::unknown-type:
   * @bin: The decodebin3.
   * @pad: the new pad containing caps that cannot be resolved to a 'final'.
   * stream type.
   * @caps: the #GstCaps of the pad that cannot be resolved.
   *
   * This signal is emitted when a pad for which there is no further possible
   * decoding is added to the decodebin3.
   */
  gst_decodebin3_signals[SIGNAL_UNKNOWN_TYPE] =
      g_signal_new ("unknown-type", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET (GstDecodebin3Class, unknown_type),
      NULL, NULL, g_cclosure_marshal_generic, G_TYPE_NONE, 2,
      GST_TYPE_PAD, GST_TYPE_CAPS);

  element_class->request_new_pad =
      GST_DEBUG_FUNCPTR (gst_decodebin3_request_new_pad);
  element_class->change_state = GST_DEBUG_FUNCPTR (gst_decodebin3_change_state);
  element_class->send_event = GST_DEBUG_FUNCPTR (gst_decodebin3_send_event);

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&sink_template));
  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&request_sink_template));
  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&video_src_template));
  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&audio_src_template));
  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&text_src_template));
  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&src_template));

  gst_element_class_set_static_metadata (element_class,
      "Decoder Bin 3", "Generic/Bin/Decoder",
      "Autoplug and decode to raw media",
      "Edward Hervey <edward@centricular.com>");

  bin_klass->handle_message = gst_decodebin3_handle_message;

  klass->select_stream = gst_decodebin3_select_stream;

#ifdef HAVE_PRIV_FUNC
  klass->priv_decodebin3_init = priv_decodebin3_init;
  klass->priv_decodebin3_dispose = priv_decodebin3_dispose;
  klass->priv_parsebin_init = priv_parsebin_init;
  klass->priv_get_sorted_collection = priv_get_sorted_collection;
  klass->priv_append_exposed_pad = priv_append_exposed_pad;
  klass->priv_query_smart_properties = priv_query_smart_properties;
  klass->priv_set_monitor_pts_continuity = priv_set_monitor_pts_continuity;
  klass->priv_create_new_input = priv_create_new_input;
  klass->priv_update_requested_selection = priv_update_requested_selection;
  klass->priv_decodebin3_handle_message = priv_decodebin3_handle_message;
  klass->priv_is_selection_done = priv_is_selection_done;
  klass->priv_create_new_slot = priv_create_new_slot;
  klass->priv_free_multiqueue_slot = priv_free_multiqueue_slot;
  klass->priv_handle_stream_switch = priv_handle_stream_switch;
  klass->priv_ghost_pad_event_probe = priv_ghost_pad_event_probe;
  klass->priv_release_resource_related = priv_release_resource_related;
  klass->priv_remove_exposed_pad = priv_remove_exposed_pad;
  klass->priv_check_slot_for_eos = priv_check_slot_for_eos;
  klass->priv_remove_input_stream = priv_remove_input_stream;
  klass->priv_reconfigure_output_stream = priv_reconfigure_output_stream;
#endif
}

static void
gst_decodebin3_init (GstDecodebin3 * dbin)
{
  GstDecodebin3Class *klass = GST_DECODEBIN3_GET_CLASS (dbin);

  /* Create main input */
  dbin->main_input = create_new_input (dbin, TRUE);

  dbin->multiqueue = gst_element_factory_make ("multiqueue", NULL);
  g_object_set (dbin->multiqueue, "sync-by-running-time", TRUE,
      "max-size-buffers", 0, "use-interleave", TRUE, NULL);
  gst_bin_add ((GstBin *) dbin, dbin->multiqueue);

  dbin->current_group_id = GST_GROUP_ID_INVALID;

  g_mutex_init (&dbin->factories_lock);
  g_mutex_init (&dbin->selection_lock);
  g_mutex_init (&dbin->input_lock);

  dbin->caps = gst_static_caps_get (&default_raw_caps);

  if (G_LIKELY (klass->priv_decodebin3_init))
    klass->priv_decodebin3_init (dbin);

  GST_OBJECT_FLAG_SET (dbin, GST_BIN_FLAG_STREAMS_AWARE);
}

static void
gst_decodebin3_dispose (GObject * object)
{
  GstDecodebin3 *dbin = (GstDecodebin3 *) object;
  GList *walk, *next;
  GstDecodebin3Class *klass = GST_DECODEBIN3_GET_CLASS (dbin);

  if (dbin->factories)
    gst_plugin_feature_list_free (dbin->factories);
  if (dbin->decoder_factories)
    g_list_free (dbin->decoder_factories);
  if (dbin->decodable_factories)
    g_list_free (dbin->decodable_factories);
  g_list_free_full (dbin->requested_selection, g_free);
  g_list_free (dbin->active_selection);
  g_list_free (dbin->to_activate);
  g_list_free (dbin->pending_select_streams);
  g_clear_object (&dbin->collection);

  free_input (dbin, dbin->main_input);

  for (walk = dbin->other_inputs; walk; walk = next) {
    DecodebinInput *input = walk->data;

    next = g_list_next (walk);

    free_input (dbin, input);
    dbin->other_inputs = g_list_delete_link (dbin->other_inputs, walk);
  }

  if (G_LIKELY (klass->priv_decodebin3_dispose))
    klass->priv_decodebin3_dispose (dbin);

  G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
gst_decodebin3_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstDecodebin3 *dbin = (GstDecodebin3 *) object;

  /* FIXME : IMPLEMENT */
  switch (prop_id) {
    case PROP_CAPS:
      GST_OBJECT_LOCK (dbin);
      if (dbin->caps)
        gst_caps_unref (dbin->caps);
      dbin->caps = g_value_dup_boxed (value);
      GST_OBJECT_UNLOCK (dbin);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_decodebin3_get_property (GObject * object, guint prop_id, GValue * value,
    GParamSpec * pspec)
{
  GstDecodebin3 *dbin = (GstDecodebin3 *) object;

  /* FIXME : IMPLEMENT */
  switch (prop_id) {
    case PROP_CAPS:
      GST_OBJECT_LOCK (dbin);
      g_value_set_boxed (value, dbin->caps);
      GST_OBJECT_UNLOCK (dbin);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static gboolean
parsebin_autoplug_continue_cb (GstElement * parsebin, GstPad * pad,
    GstCaps * caps, GstDecodebin3 * dbin)
{
  GstDecodebin3Class *klass = GST_DECODEBIN3_GET_CLASS (dbin);
  GST_DEBUG_OBJECT (pad, "caps %" GST_PTR_FORMAT, caps);

  if (G_LIKELY (klass->priv_append_exposed_pad))
    klass->priv_append_exposed_pad (dbin, pad);

  /* If it matches our target caps, expose it */
  if (gst_caps_can_intersect (caps, dbin->caps))
    return FALSE;

  return TRUE;
}

/* This method should be called whenever a STREAM_START event
 * comes out of a given parsebin.
 * The caller shall replace the group_id if the function returns TRUE */
static gboolean
set_input_group_id (DecodebinInput * input, guint32 * group_id)
{
  GstDecodebin3 *dbin = input->dbin;

  if (input->group_id != *group_id) {
    if (input->group_id != GST_GROUP_ID_INVALID)
      GST_WARNING_OBJECT (dbin,
          "Group id changed (%" G_GUINT32_FORMAT " -> %" G_GUINT32_FORMAT
          ") on input %p ", input->group_id, *group_id, input);
    input->group_id = *group_id;
  }

  if (*group_id != dbin->current_group_id) {
    if (dbin->current_group_id == GST_GROUP_ID_INVALID) {
      GST_DEBUG_OBJECT (dbin, "Setting current group id to %" G_GUINT32_FORMAT,
          *group_id);
      dbin->current_group_id = *group_id;
    }
    *group_id = dbin->current_group_id;
    return TRUE;
  }

  return FALSE;
}

static void
parsebin_drained_cb (GstElement * parsebin, DecodebinInput * input)
{
  GstDecodebin3 *dbin = input->dbin;
  gboolean all_drained;
  GList *tmp;

  GST_WARNING_OBJECT (dbin, "input %p drained", input);
  input->drained = TRUE;

  all_drained = dbin->main_input->drained;
  for (tmp = dbin->other_inputs; tmp; tmp = tmp->next) {
    DecodebinInput *data = (DecodebinInput *) tmp->data;

    all_drained &= data->drained;
  }

  if (all_drained) {
    GST_WARNING_OBJECT (dbin, "All inputs drained. Posting about-to-finish");
    g_signal_emit (dbin, gst_decodebin3_signals[SIGNAL_ABOUT_TO_FINISH], 0,
        NULL);
  }
}

/* Call with INPUT_LOCK taken */
static gboolean
ensure_input_parsebin (GstDecodebin3 * dbin, DecodebinInput * input)
{
  gboolean set_state = FALSE;
  GstDecodebin3Class *klass = GST_DECODEBIN3_GET_CLASS (dbin);

  if (input->parsebin == NULL) {
    input->parsebin = gst_element_factory_make ("parsebin", NULL);
    if (input->parsebin == NULL)
      goto no_parsebin;
    input->parsebin = gst_object_ref (input->parsebin);
    input->parsebin_sink = gst_element_get_static_pad (input->parsebin, "sink");
    input->pad_added_sigid =
        g_signal_connect (input->parsebin, "pad-added",
        (GCallback) parsebin_pad_added_cb, input);
    input->pad_removed_sigid =
        g_signal_connect (input->parsebin, "pad-removed",
        (GCallback) parsebin_pad_removed_cb, input);
    input->drained_sigid =
        g_signal_connect (input->parsebin, "drained",
        (GCallback) parsebin_drained_cb, input);
    g_signal_connect (input->parsebin, "autoplug-continue",
        (GCallback) parsebin_autoplug_continue_cb, dbin);
    g_signal_connect (input->parsebin, "unknown-type",
        G_CALLBACK (unknown_type_cb), dbin);

    if (G_LIKELY (klass->priv_parsebin_init))
      klass->priv_parsebin_init (dbin, input);
  }

  if (GST_OBJECT_PARENT (GST_OBJECT (input->parsebin)) != GST_OBJECT (dbin)) {
    gst_bin_add (GST_BIN (dbin), input->parsebin);
    set_state = TRUE;
  }

  gst_ghost_pad_set_target (GST_GHOST_PAD (input->ghost_sink),
      input->parsebin_sink);
  if (set_state)
    gst_element_sync_state_with_parent (input->parsebin);

  if (G_LIKELY (klass->priv_query_smart_properties))
    klass->priv_query_smart_properties (dbin, input);

  return TRUE;

  /* ERRORS */
no_parsebin:
  {
    gst_element_post_message ((GstElement *) dbin,
        gst_missing_element_message_new ((GstElement *) dbin, "parsebin"));
    return FALSE;
  }
}

static GstPadLinkReturn
gst_decodebin3_input_pad_link (GstPad * pad, GstObject * parent, GstPad * peer)
{
  GstDecodebin3 *dbin = (GstDecodebin3 *) parent;
  GstPadLinkReturn res = GST_PAD_LINK_OK;
  DecodebinInput *input;

  GST_LOG_OBJECT (parent, "Got link on input pad %" GST_PTR_FORMAT
      ". Creating parsebin if needed", pad);

  if ((input = g_object_get_data (G_OBJECT (pad), "decodebin.input")) == NULL)
    goto fail;

  INPUT_LOCK (dbin);
  if (!ensure_input_parsebin (dbin, input))
    res = GST_PAD_LINK_REFUSED;
  INPUT_UNLOCK (dbin);

  return res;
fail:
  GST_ERROR_OBJECT (parent, "Failed to retrieve input state from ghost pad");
  return GST_PAD_LINK_REFUSED;
}

static gboolean
check_reconfigure_decoder (DecodebinInput * input)
{
  GstDecodebin3 *dbin = input->dbin;
  GList *tmp;
  gboolean reconfigure = FALSE;

  GST_DEBUG_OBJECT (dbin, "parsebin(%s)",
      input->parsebin ? GST_ELEMENT_NAME (input->parsebin) : "<NONE>");

  for (tmp = dbin->slots; tmp; tmp = tmp->next) {
    MultiQueueSlot *slot = (MultiQueueSlot *) tmp->data;
    DecodebinInputStream *inputstream = (DecodebinInputStream *) slot->input;

    if (inputstream && inputstream->input == input && slot->plugging_decoder) {
      GST_DEBUG_OBJECT (slot->sink_pad,
          "In progress for reconfigure decoder: %d", slot->plugging_decoder);
      if (slot->plugging_decoder) {
        reconfigure = TRUE;
        break;
      }
    }
  }

  return reconfigure;
}

/* Drop reconfigure event during decoder is configured */
static GstPadProbeReturn
event_reconfigure_drop_probe (GstPad * pad, GstPadProbeInfo * info,
    DecodebinInput * input)
{
  GstPadProbeReturn ret = GST_PAD_PROBE_OK;

  if (GST_IS_EVENT (GST_PAD_PROBE_INFO_DATA (info))) {
    GstEvent *event = GST_PAD_PROBE_INFO_EVENT (info);
    GST_LOG_OBJECT (pad, "See event: %" GST_PTR_FORMAT, event);
    if (GST_EVENT_TYPE (event) == GST_EVENT_RECONFIGURE) {
      gboolean reconfigure = check_reconfigure_decoder (input);
      GST_LOG_OBJECT (pad, "reconfiguring decoder: %d", reconfigure);
      if (reconfigure) {
        GST_LOG_OBJECT (pad, "stop forwarding event reconfigure");
        gst_event_unref (event);
        ret = GST_PAD_PROBE_HANDLED;
      }
    }
  }

  return ret;
}

/* Drop duration query during _input_pad_unlink */
static GstPadProbeReturn
query_duration_drop_probe (GstPad * pad, GstPadProbeInfo * info,
    DecodebinInput * input)
{
  GstPadProbeReturn ret = GST_PAD_PROBE_OK;

  if (GST_IS_QUERY (GST_PAD_PROBE_INFO_DATA (info))) {
    GstQuery *query = GST_PAD_PROBE_INFO_QUERY (info);
    if (GST_QUERY_TYPE (query) == GST_QUERY_DURATION) {
      GST_LOG_OBJECT (pad, "stop forwarding query duration");
      ret = GST_PAD_PROBE_HANDLED;
    }
  }

  return ret;
}

static void
gst_decodebin3_input_pad_unlink (GstPad * pad, GstObject * parent)
{
  GstDecodebin3 *dbin = (GstDecodebin3 *) parent;
  DecodebinInput *input;

  GST_LOG_OBJECT (parent, "Got unlink on input pad %" GST_PTR_FORMAT
      ". Removing parsebin.", pad);

  if ((input = g_object_get_data (G_OBJECT (pad), "decodebin.input")) == NULL)
    goto fail;

  PARSE_INPUT_LOCK (input);
  INPUT_LOCK (dbin);
  GST_DEBUG_OBJECT (dbin, "Removing input (%p)", input);
  if (input->parsebin == NULL) {
    INPUT_UNLOCK (dbin);
    PARSE_INPUT_UNLOCK (input);
    return;
  }

  if (GST_OBJECT_PARENT (GST_OBJECT (input->parsebin)) == GST_OBJECT (dbin)) {
    GstStreamCollection *collection = NULL;
    gulong event_probe_id = 0;

    gulong probe_id = gst_pad_add_probe (input->parsebin_sink,
        GST_PAD_PROBE_TYPE_QUERY_UPSTREAM,
        (GstPadProbeCallback) query_duration_drop_probe, input, NULL);
    if (dbin->dvr_playback)
      event_probe_id = gst_pad_add_probe (input->parsebin_sink,
          GST_PAD_PROBE_TYPE_EVENT_UPSTREAM,
          (GstPadProbeCallback) event_reconfigure_drop_probe, input, NULL);

    /* Clear stream-collection corresponding to current INPUT and post new
     * stream-collection message, if needed */
    if (input->collection) {
      gst_object_unref (input->collection);
      input->collection = NULL;
    }

    collection = get_merged_collection (dbin);
    INPUT_UNLOCK (dbin);
    SELECTION_LOCK (dbin);
    if (collection && collection != dbin->collection) {
      GstMessage *msg;
      GST_DEBUG_OBJECT (dbin, "Update Stream Collection");

      if (dbin->collection)
        gst_object_unref (dbin->collection);
      dbin->collection = collection;

      msg = NULL;
/* FIXME : Do we need to post new collection and update request-selection ?? */
/*
      gst_message_new_stream_collection ((GstObject *) dbin, dbin->collection);
      SELECTION_UNLOCK (dbin);
      gst_element_post_message (GST_ELEMENT_CAST (dbin), msg);
      update_requested_selection (dbin);
    } else
      SELECTION_UNLOCK (dbin);
*/
    }
    SELECTION_UNLOCK (dbin);

    INPUT_LOCK (dbin);
    gst_bin_remove (GST_BIN (dbin), input->parsebin);
    INPUT_UNLOCK (dbin);
    gst_element_set_state (input->parsebin, GST_STATE_NULL);
    INPUT_LOCK (dbin);
    g_signal_handler_disconnect (input->parsebin, input->pad_removed_sigid);
    g_signal_handler_disconnect (input->parsebin, input->pad_added_sigid);
    g_signal_handler_disconnect (input->parsebin, input->drained_sigid);
    gst_pad_remove_probe (input->parsebin_sink, probe_id);
    if (event_probe_id)
      gst_pad_remove_probe (input->parsebin_sink, event_probe_id);
    gst_object_unref (input->parsebin);
    gst_object_unref (input->parsebin_sink);

    input->parsebin = NULL;
    input->parsebin_sink = NULL;
    dbin->monitor_pts_continuity = FALSE;

    if (!input->is_main) {
      dbin->other_inputs = g_list_remove (dbin->other_inputs, input);
      free_input_async (dbin, input);
    }
  }
  INPUT_UNLOCK (dbin);
  PARSE_INPUT_UNLOCK (input);
  return;

fail:
  GST_ERROR_OBJECT (parent, "Failed to retrieve input state from ghost pad");
  return;
}

static void
free_input (GstDecodebin3 * dbin, DecodebinInput * input)
{
  GST_DEBUG ("Freeing input %p", input);
  gst_ghost_pad_set_target (GST_GHOST_PAD (input->ghost_sink), NULL);
  gst_element_remove_pad (GST_ELEMENT (dbin), input->ghost_sink);
  if (input->parsebin) {
    g_signal_handler_disconnect (input->parsebin, input->pad_removed_sigid);
    g_signal_handler_disconnect (input->parsebin, input->pad_added_sigid);
    g_signal_handler_disconnect (input->parsebin, input->drained_sigid);
    gst_element_set_state (input->parsebin, GST_STATE_NULL);
    gst_object_unref (input->parsebin);
    gst_object_unref (input->parsebin_sink);
  }
  if (input->collection)
    gst_object_unref (input->collection);
  g_free (input);
}

static void
free_input_async (GstDecodebin3 * dbin, DecodebinInput * input)
{
  GST_LOG_OBJECT (dbin, "pushing input %p on thread pool to free", input);
  gst_element_call_async (GST_ELEMENT_CAST (dbin),
      (GstElementCallAsyncFunc) free_input, input, NULL);
}

/* Call with INPUT_LOCK taken */
static DecodebinInput *
create_new_input (GstDecodebin3 * dbin, gboolean main)
{
  DecodebinInput *input;
  GstDecodebin3Class *klass = GST_DECODEBIN3_GET_CLASS (dbin);

  input = g_new0 (DecodebinInput, 1);
  input->dbin = dbin;
  input->is_main = main;
  input->group_id = GST_GROUP_ID_INVALID;
  if (main)
    input->ghost_sink = gst_ghost_pad_new_no_target ("sink", GST_PAD_SINK);
  else {
    gchar *pad_name = g_strdup_printf ("sink_%u", dbin->input_counter++);
    input->ghost_sink = gst_ghost_pad_new_no_target (pad_name, GST_PAD_SINK);
    g_free (pad_name);
  }
  g_object_set_data (G_OBJECT (input->ghost_sink), "decodebin.input", input);
  gst_pad_set_link_function (input->ghost_sink, gst_decodebin3_input_pad_link);
  gst_pad_set_unlink_function (input->ghost_sink,
      gst_decodebin3_input_pad_unlink);

  gst_pad_set_active (input->ghost_sink, TRUE);
  gst_element_add_pad ((GstElement *) dbin, input->ghost_sink);

  if (G_LIKELY (klass->priv_create_new_input))
    klass->priv_create_new_input (dbin, input);

  return input;

}

static GstPad *
gst_decodebin3_request_new_pad (GstElement * element, GstPadTemplate * temp,
    const gchar * name, const GstCaps * caps)
{
  GstDecodebin3 *dbin = (GstDecodebin3 *) element;
  DecodebinInput *input;
  GstPad *res = NULL;

  /* We are ignoring names for the time being, not sure it makes any sense
   * within the context of decodebin3 ... */
  input = create_new_input (dbin, FALSE);
  if (input) {
    INPUT_LOCK (dbin);
    dbin->other_inputs = g_list_append (dbin->other_inputs, input);
    res = input->ghost_sink;
    INPUT_UNLOCK (dbin);
  }

  return res;
}

/* Must be called with factories lock! */
static void
gst_decode_bin_update_factories_list (GstDecodebin3 * dbin)
{
  guint cookie;

  cookie = gst_registry_get_feature_list_cookie (gst_registry_get ());
  if (!dbin->factories || dbin->factories_cookie != cookie) {
    GList *tmp;
    if (dbin->factories)
      gst_plugin_feature_list_free (dbin->factories);
    if (dbin->decoder_factories)
      g_list_free (dbin->decoder_factories);
    if (dbin->decodable_factories)
      g_list_free (dbin->decodable_factories);
    dbin->factories =
        gst_element_factory_list_get_elements
        (GST_ELEMENT_FACTORY_TYPE_DECODABLE, GST_RANK_MARGINAL);
    dbin->factories =
        g_list_sort (dbin->factories, gst_plugin_feature_rank_compare_func);
    dbin->factories_cookie = cookie;

    /* Filter decoder and other decodables */
    dbin->decoder_factories = NULL;
    dbin->decodable_factories = NULL;
    for (tmp = dbin->factories; tmp; tmp = tmp->next) {
      GstElementFactory *fact = (GstElementFactory *) tmp->data;
      if (gst_element_factory_list_is_type (fact,
              GST_ELEMENT_FACTORY_TYPE_DECODER))
        dbin->decoder_factories = g_list_append (dbin->decoder_factories, fact);
      else
        dbin->decodable_factories =
            g_list_append (dbin->decodable_factories, fact);
    }
  }
}

/* Must be called with appropriate lock if list is a protected variable */
static const gchar *
stream_in_list (GList * list, const gchar * sid)
{
  GList *tmp;

#if EXTRA_DEBUG
  for (tmp = list; tmp; tmp = tmp->next) {
    gchar *osid = (gchar *) tmp->data;
    GST_DEBUG ("Checking %s against %s", sid, osid);
  }
#endif

  for (tmp = list; tmp; tmp = tmp->next) {
    const gchar *osid = (gchar *) tmp->data;
    if (!g_strcmp0 (sid, osid))
      return osid;
  }

  return NULL;
}

static void
update_requested_selection (GstDecodebin3 * dbin)
{
  guint i, nb;
  GList *tmp = NULL;
  GstStreamType used_types = 0;
  GstStreamCollection *collection;

  /* 1. Is there a pending SELECT_STREAMS we can return straight away since
   *  the switch handler will take care of the pending selection */
  SELECTION_LOCK (dbin);
  if (dbin->pending_select_streams) {
    GST_DEBUG_OBJECT (dbin,
        "No need to create pending selection, SELECT_STREAMS underway");
    goto beach;
  }

  collection = dbin->collection;
  if (G_UNLIKELY (collection == NULL)) {
    GST_DEBUG_OBJECT (dbin, "No current GstStreamCollection");
    goto beach;
  }
  nb = gst_stream_collection_get_size (collection);

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
    if (request == 1 || (request == -1
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
  }
  SELECTION_UNLOCK (dbin);
}

/* Call with INPUT_LOCK taken */
static GstStreamCollection *
get_merged_collection (GstDecodebin3 * dbin)
{
  gboolean needs_merge = FALSE;
  GstStreamCollection *res = NULL;
  GList *tmp;
  guint i, nb_stream;

  /* First check if we need to do a merge or just return the only collection */
  res = dbin->main_input->collection;

  for (tmp = dbin->other_inputs; tmp; tmp = tmp->next) {
    DecodebinInput *input = (DecodebinInput *) tmp->data;
    if (input->collection) {
      if (res) {
        needs_merge = TRUE;
        break;
      }
      res = input->collection;
    }
  }

  if (!needs_merge) {
    GST_DEBUG_OBJECT (dbin, "No need to merge, returning %p", res);
    return res ? gst_object_ref (res) : NULL;
  }

  /* We really need to create a new collection */
  /* FIXME : Some numbering scheme maybe ?? */
  res = gst_stream_collection_new ("decodebin3");
  if (dbin->main_input->collection) {
    nb_stream = gst_stream_collection_get_size (dbin->main_input->collection);
    GST_DEBUG_OBJECT (dbin, "main input %p %d", dbin->main_input, nb_stream);
    for (i = 0; i < nb_stream; i++) {
      GstStream *stream =
          gst_stream_collection_get_stream (dbin->main_input->collection, i);
      gst_stream_collection_add_stream (res, gst_object_ref (stream));
    }
  }

  for (tmp = dbin->other_inputs; tmp; tmp = tmp->next) {
    DecodebinInput *input = (DecodebinInput *) tmp->data;
    GST_DEBUG_OBJECT (dbin, "input %p , collection %p", input,
        input->collection);
    if (input->collection) {
      nb_stream = gst_stream_collection_get_size (input->collection);
      GST_DEBUG_OBJECT (dbin, "nb_stream : %d", nb_stream);
      for (i = 0; i < nb_stream; i++) {
        GstStream *stream =
            gst_stream_collection_get_stream (input->collection, i);
        gst_stream_collection_add_stream (res, gst_object_ref (stream));
      }
    }
  }

  return res;
}

/* Must be called with SELECTION_LOCK */
static DecodebinInput *
get_input_for_slot (GstDecodebin3 * dbin, MultiQueueSlot * slot)
{
  DecodebinInput *input = NULL;
  GList *tmp;

  GST_DEBUG_OBJECT (dbin, "Finding for input for slot(%p)", slot);

  if (slot->input && slot->input->input) {
    GST_DEBUG_OBJECT (dbin, "Still alive inputstream(%p) and input(%p)",
        slot->input, slot->input->input);
    input = slot->input->input;
    goto done;
  }

  if (slot->parent_input == dbin->main_input) {
    GST_DEBUG_OBJECT (dbin,
        "inputstream is not exist but, found in main input(%p)",
        dbin->main_input);
    input = dbin->main_input;
    goto done;
  }

  for (tmp = dbin->other_inputs; tmp; tmp = tmp->next) {
    DecodebinInput *cur = (DecodebinInput *) tmp->data;
    if (slot->parent_input == cur) {
      GST_DEBUG_OBJECT (dbin,
          "inputstream is not exist but, found in other input(%p)", cur);
      input = cur;
      break;
    }
  }

done:
  if (input)
    GST_DEBUG_OBJECT (dbin, "Found input: %s",
        input->parsebin ? GST_ELEMENT_NAME (input->parsebin) : "<NONE>");
  else
    GST_DEBUG_OBJECT (dbin, "No found input for slot(%p)", slot);

  return input;
}

/* Call with INPUT_LOCK taken */
static DecodebinInput *
find_message_parsebin (GstDecodebin3 * dbin, GstElement * child)
{
  DecodebinInput *input = NULL;
  GstElement *parent = gst_object_ref (child);
  GList *tmp;

  do {
    GstElement *next_parent;

    GST_DEBUG_OBJECT (dbin, "parent %s",
        parent ? GST_ELEMENT_NAME (parent) : "<NONE>");

    if (parent == dbin->main_input->parsebin) {
      input = dbin->main_input;
      break;
    }
    for (tmp = dbin->other_inputs; tmp; tmp = tmp->next) {
      DecodebinInput *cur = (DecodebinInput *) tmp->data;
      if (parent == cur->parsebin) {
        input = cur;
        break;
      }
    }
    next_parent = (GstElement *) gst_element_get_parent (parent);
    gst_object_unref (parent);
    parent = next_parent;

  } while (parent && parent != (GstElement *) dbin);

  if (parent)
    gst_object_unref (parent);

  return input;
}

static const gchar *
stream_in_collection (GstDecodebin3 * dbin, gchar * sid)
{
  guint i, len;

  if (dbin->collection == NULL)
    return NULL;
  len = gst_stream_collection_get_size (dbin->collection);
  for (i = 0; i < len; i++) {
    GstStream *stream = gst_stream_collection_get_stream (dbin->collection, i);
    const gchar *osid = gst_stream_get_stream_id (stream);
    if (!g_strcmp0 (sid, osid))
      return osid;
  }

  return NULL;
}

/* Call with INPUT_LOCK taken */
static void
handle_stream_collection (GstDecodebin3 * dbin,
    GstStreamCollection * collection, GstElement * child)
{
#ifndef GST_DISABLE_GST_DEBUG
  const gchar *upstream_id;
  guint i;
#endif
  DecodebinInput *input = find_message_parsebin (dbin, child);

  if (!input) {
    GST_DEBUG_OBJECT (dbin,
        "Couldn't find corresponding input, most likely shutting down");
    return;
  }

  /* Replace collection in input */
  if (input->collection)
    gst_object_unref (input->collection);
  input->collection = gst_object_ref (collection);
  GST_DEBUG_OBJECT (dbin, "Setting collection %p on input %p", collection,
      input);

  /* Merge collection if needed */
  collection = get_merged_collection (dbin);
  if (collection) {
    GstDecodebin3Class *klass = GST_DECODEBIN3_GET_CLASS (dbin);
    if (G_LIKELY (klass->priv_get_sorted_collection))
      collection = klass->priv_get_sorted_collection (dbin, collection);
  }
#ifndef GST_DISABLE_GST_DEBUG
  /* Just some debugging */
  upstream_id = gst_stream_collection_get_upstream_id (collection);
  GST_DEBUG ("Received Stream Collection. Upstream_id : %s", upstream_id);
  GST_DEBUG ("From input %p", input);
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

  /* Store collection for later usage */
  if (dbin->collection == NULL) {
    dbin->collection = collection;
  } else {
    /* We need to check who emitted this collection (the owner).
     * If we already had a collection from that user, this one is an update,
     * that is to say that we need to figure out how we are going to re-use
     * the streams/slot */
    GST_FIXME_OBJECT (dbin, "New collection but already had one ...");
    /* FIXME : When do we switch from pending collection to active collection ?
     * When all streams from active collection are drained in multiqueue output ? */
    gst_object_unref (dbin->collection);
    dbin->collection = collection;
    /* dbin->pending_collection = */
    /*     g_list_append (dbin->pending_collection, collection); */
  }
}

static void
gst_decodebin3_handle_message (GstBin * bin, GstMessage * message)
{
  GstDecodebin3 *dbin = (GstDecodebin3 *) bin;
  gboolean posting_collection = FALSE;
  GstDecodebin3Class *klass = GST_DECODEBIN3_GET_CLASS (dbin);

  GST_DEBUG_OBJECT (bin, "Got Message %p:%s", message,
      GST_MESSAGE_TYPE_NAME (message));

  if (G_LIKELY (klass->priv_decodebin3_handle_message)) {
    gboolean steal = FALSE;
    gboolean do_post = TRUE;
    klass->priv_decodebin3_handle_message (dbin, message, &steal, &do_post);
    if (steal) {
      if (do_post) {
        GST_FIXME_OBJECT (dbin, "Steal and Post message %" GST_PTR_FORMAT,
            message);
        GST_BIN_CLASS (parent_class)->handle_message (bin, message);
        return;
      } else {
        GST_FIXME_OBJECT (dbin, "Steal and dump message %" GST_PTR_FORMAT,
            message);
        return;
      }
    }
  }

  switch (GST_MESSAGE_TYPE (message)) {
    case GST_MESSAGE_STREAM_COLLECTION:
    {
      GstStreamCollection *collection = NULL;
      gst_message_parse_stream_collection (message, &collection);
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
      if (dbin->collection && collection != dbin->collection) {
        /* Replace collection message, we most likely aggregated it */
        GstMessage *new_msg;
        new_msg =
            gst_message_new_stream_collection ((GstObject *) dbin,
            dbin->collection);
        gst_message_unref (message);
        message = new_msg;
      }
      SELECTION_UNLOCK (dbin);

      if (collection)
        gst_object_unref (collection);
      break;
    }
    default:
      break;
  }

  GST_BIN_CLASS (parent_class)->handle_message (bin, message);

  if (posting_collection) {
    /* Figure out a selection for that collection */
    update_requested_selection (dbin);
  }
}

static DecodebinOutputStream *
find_free_compatible_output (GstDecodebin3 * dbin, GstStream * stream)
{
  GList *tmp;
  GstStreamType stype = gst_stream_get_stream_type (stream);

  for (tmp = dbin->output_streams; tmp; tmp = tmp->next) {
    DecodebinOutputStream *output = (DecodebinOutputStream *) tmp->data;
    if (output->type == stype && output->slot && output->slot->active_stream) {
      GstStream *tstream = output->slot->active_stream;
      if (!stream_in_list (dbin->requested_selection,
              (gchar *) gst_stream_get_stream_id (tstream))) {
        return output;
      }
    }
  }

  return NULL;
}

static GstStream *
find_stream_in_collection (GstStreamCollection * collection, const gchar * sid)
{
  guint i;
  guint nb_streams = gst_stream_collection_get_size (collection);
  GstStream *stream = NULL;

  for (i = 0; i < nb_streams; i++) {
    stream = gst_stream_collection_get_stream (collection, i);
    if (!g_strcmp0 (gst_stream_get_stream_id (stream), sid))
      return stream;
  }

  return NULL;
}

/* Give a certain slot, figure out if it should be linked to an
 * output stream
 * CALL WITH SELECTION LOCK TAKEN !*/
static DecodebinOutputStream *
get_output_for_slot (MultiQueueSlot * slot)
{
  GstDecodebin3 *dbin = slot->dbin;
  DecodebinOutputStream *output = NULL;
  const gchar *stream_id;
  GstCaps *caps;
  gchar *id_in_list = NULL;

  /* If we already have a configured output, just use it */
  if (slot->output != NULL)
    return slot->output;

  /*
   * FIXME
   *
   * This method needs to be split into multiple parts
   *
   * 1) Figure out whether stream should be exposed or not
   *   This is based on autoplug-continue, EXPOSE_ALL_MODE, or presence
   *   in the default stream attribution
   *
   * 2) Figure out whether an output stream should be created, whether
   *   we can re-use the output stream already linked to the slot, or
   *   whether we need to get re-assigned another (currently used) output
   *   stream.
   */

  stream_id = gst_stream_get_stream_id (slot->active_stream);
  caps = gst_stream_get_caps (slot->active_stream);
  GST_DEBUG_OBJECT (dbin, "stream %s , %" GST_PTR_FORMAT, stream_id, caps);
  gst_caps_unref (caps);

  /* 0. Emit autoplug-continue signal for pending caps ? */
  GST_FIXME_OBJECT (dbin, "emit autoplug-continue");

  /* 1. if in EXPOSE_ALL_MODE, just accept */
  GST_FIXME_OBJECT (dbin, "Handle EXPOSE_ALL_MODE");

#if 0
  /* FIXME : The idea around this was to avoid activating a stream for
   *     which we have no decoder. Unfortunately it is way too
   *     expensive. Need to figure out a better solution */
  /* 2. Is there a potential decoder (if one is required) */
  if (!gst_caps_can_intersect (caps, dbin->caps)
      && !have_factory (dbin, (GstCaps *) caps,
          GST_ELEMENT_FACTORY_TYPE_DECODER)) {
    GST_WARNING_OBJECT (dbin, "Don't have a decoder for %" GST_PTR_FORMAT,
        caps);
    SELECTION_UNLOCK (dbin);
    gst_element_post_message (GST_ELEMENT_CAST (dbin),
        gst_missing_decoder_message_new (GST_ELEMENT_CAST (dbin), caps));
    SELECTION_LOCK (dbin);
    return NULL;
  }
#endif

  /* 3. In default mode check if we should expose */
  id_in_list = (gchar *) stream_in_list (dbin->requested_selection, stream_id);
  if (id_in_list) {
    /* Check if we can steal an existing output stream we could re-use.
     * that is:
     * * an output stream whose slot->stream is not in requested
     * * and is of the same type as this stream
     */
    output = find_free_compatible_output (dbin, slot->active_stream);
    if (output) {
      if (dbin->dvr_playback) {
        if (stream_in_list (dbin->to_activate, (gchar *) stream_id)) {
          GST_DEBUG_OBJECT (dbin, "<to-activate> list has already sid(%s)",
              (gchar *) stream_id);
          return NULL;
        } else {
          GstStreamType to_activate_type =
              gst_stream_get_stream_type (slot->active_stream);
          GList *tmp = NULL;
          for (tmp = dbin->to_activate; tmp; tmp = tmp->next) {
            GstStream *stream =
                find_stream_in_collection (dbin->active_collection,
                (gchar *) tmp->data);
            if (stream
                && gst_stream_get_stream_type (stream) == to_activate_type) {
              GST_DEBUG_OBJECT (slot->src_pad,
                  "New stream(%s) is requested, old stream(%s) to be deactivated, Removing to-activate list",
                  (gchar *) stream_id, (gchar *) tmp->data);
              dbin->to_activate =
                  g_list_remove (dbin->to_activate, (gchar *) tmp->data);
            }
          }
        }
      }

      /* Move this output from its current slot to this slot */
      dbin->to_activate =
          g_list_append (dbin->to_activate, (gchar *) stream_id);
      dbin->requested_selection =
          g_list_remove (dbin->requested_selection, id_in_list);
      g_free (id_in_list);
      DUMP_SELECTION_LIST (dbin, dbin->to_activate, "<to_activate>");
      DUMP_SELECTION_LIST (dbin, dbin->requested_selection, "requested");
      SELECTION_UNLOCK (dbin);
      GST_DEBUG_OBJECT (output->slot->src_pad,
          "Adding probe for re-assign slot for sid(%s)", stream_id);
      gst_pad_add_probe (output->slot->src_pad, GST_PAD_PROBE_TYPE_IDLE,
          (GstPadProbeCallback) slot_unassign_probe, output->slot, NULL);
      SELECTION_LOCK (dbin);
      return NULL;
    }

    output = create_output_stream (dbin, slot->type);
    output->slot = slot;
    GST_DEBUG ("Linking slot %p to new output %p", slot, output);
    slot->output = output;
    dbin->active_selection =
        g_list_append (dbin->active_selection, (gchar *) stream_id);
    DUMP_SELECTION_LIST (dbin, dbin->active_selection, "active");
  } else
    GST_DEBUG ("Not creating any output for slot %p", slot);

  return output;
}

/* Returns SELECTED_STREAMS message if active_selection is equal to
 * requested_selection, else NULL.
 * Must be called with LOCK taken */
static GstMessage *
is_selection_done (GstDecodebin3 * dbin)
{
  GList *tmp;
  GstMessage *msg;

  if (!dbin->selection_updated)
    return NULL;

  GST_LOG_OBJECT (dbin, "Checking");

  if (dbin->to_activate != NULL) {
    GST_DEBUG ("Still have streams to activate");
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
  msg = gst_message_new_streams_selected ((GstObject *) dbin, dbin->collection);
  GST_MESSAGE_SEQNUM (msg) = dbin->select_streams_seqnum;
  for (tmp = dbin->output_streams; tmp; tmp = tmp->next) {
    DecodebinOutputStream *output = (DecodebinOutputStream *) tmp->data;
    if (output->slot) {
      GST_DEBUG_OBJECT (dbin, "Adding stream %s",
          gst_stream_get_stream_id (output->slot->active_stream));

      gst_message_streams_selected_add (msg, output->slot->active_stream);
    } else
      GST_WARNING_OBJECT (dbin, "No valid slot for output %p", output);
  }
  dbin->selection_updated = FALSE;
  return msg;
}

/* Must be called with SELECTION_LOCK taken */
static gboolean
check_all_slot_for_eos (GstDecodebin3 * dbin)
{
  gboolean all_drained = TRUE;
  GList *iter;

  GST_DEBUG_OBJECT (dbin, "check slot for eos");

  for (iter = dbin->slots; iter; iter = iter->next) {
    MultiQueueSlot *slot = iter->data;

    if (!slot->output)
      continue;

    if (slot->is_drained) {
      GstDecodebin3Class *klass = GST_DECODEBIN3_GET_CLASS (dbin);
      if (G_LIKELY (klass->priv_check_slot_for_eos)) {
        if ((all_drained = klass->priv_check_slot_for_eos (dbin, slot)) != TRUE)
          break;
      }
      GST_LOG_OBJECT (slot->sink_pad, "slot %p is drained", slot);
      continue;
    }

    all_drained = FALSE;
    break;
  }

  if (all_drained) {
    INPUT_LOCK (dbin);
    if (!pending_pads_are_eos (dbin->main_input))
      all_drained = FALSE;

    if (all_drained) {
      for (iter = dbin->other_inputs; iter; iter = iter->next) {
        if (!pending_pads_are_eos ((DecodebinInput *) iter->data)) {
          all_drained = FALSE;
          break;
        }
      }
    }
    INPUT_UNLOCK (dbin);
  }

  if (all_drained) {
    GST_DEBUG_OBJECT (dbin,
        "All active slots are drained, and no pending input, push EOS");

    for (iter = dbin->input_streams; iter; iter = iter->next) {
      DecodebinInputStream *input = (DecodebinInputStream *) iter->data;
      GstPad *peer = gst_pad_get_peer (input->srcpad);

      /* Send EOS to all slots */
      if (peer) {
        GstEvent *stream_start, *eos;
        stream_start =
            gst_pad_get_sticky_event (input->srcpad, GST_EVENT_STREAM_START, 0);

        /* First forward a custom STREAM_START event to reset the EOS status (if any) */
        if (stream_start) {
          GstStructure *s;
          GstEvent *custom_stream_start = gst_event_copy (stream_start);
          gst_event_unref (stream_start);
          s = (GstStructure *) gst_event_get_structure (custom_stream_start);
          gst_structure_set (s, "decodebin3-flushing-stream-start",
              G_TYPE_BOOLEAN, TRUE, NULL);
          GST_DEBUG_OBJECT (peer, "Pushing custom stream-start: %",
              GST_PTR_FORMAT, custom_stream_start);
          gst_pad_send_event (peer, custom_stream_start);
        } else if (GST_PAD_IS_EOS (peer)) {
          GList *tmp;
          /* FIXME: We temporarily get stream-start event from multiqueue slot if input
           * stream does not contain. */
          GST_DEBUG_OBJECT (peer,
              "Generating custom stream-start event when input stream does not contain");
          for (tmp = dbin->slots; tmp; tmp = tmp->next) {
            MultiQueueSlot *slot = (MultiQueueSlot *) tmp->data;
            if (slot->input == input) {
              GST_LOG_OBJECT (dbin, "Found slot: %d", slot->id);
              if (slot->active_stream) {
                GstStream *slot_stream = slot->active_stream;
                GstStructure *s;
                const gchar *stream_id = gst_stream_get_stream_id (slot_stream);

                GST_LOG_OBJECT (slot->src_pad, "Found stream-start(%s)",
                    stream_id);
                GstEvent *custom_stream_start =
                    gst_event_new_stream_start (stream_id);

                gst_event_set_stream (custom_stream_start, slot_stream);

                s = (GstStructure *)
                    gst_event_get_structure (custom_stream_start);
                gst_structure_set (s, "decodebin3-flushing-stream-start",
                    G_TYPE_BOOLEAN, TRUE, NULL);
                GST_DEBUG_OBJECT (peer, "Pushing custom stream-start: %",
                    GST_PTR_FORMAT, custom_stream_start);
                gst_pad_send_event (peer, custom_stream_start);
              }
            }
          }
        }

        eos = gst_event_new_eos ();
        gst_mini_object_set_qdata (GST_MINI_OBJECT_CAST (eos),
            CUSTOM_FINAL_EOS_QUARK, (gchar *) CUSTOM_FINAL_EOS_QUARK_DATA,
            NULL);
        GST_DEBUG_OBJECT (peer, "Pushing final eos: %" GST_PTR_FORMAT, eos);
        gst_pad_send_event (peer, eos);
        gst_object_unref (peer);
      } else
        GST_DEBUG_OBJECT (dbin, "no output");
    }
  }

  return all_drained;
}

static GstPadProbeReturn
multiqueue_src_probe (GstPad * pad, GstPadProbeInfo * info,
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
        const GstStructure *s = gst_event_get_structure (ev);

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
        GST_DEBUG_OBJECT (pad, "Stream Start '%s'",
            gst_stream_get_stream_id (stream));
        if (slot->active_stream == NULL) {
          slot->active_stream = stream;
        } else if (slot->active_stream != stream) {
          GST_FIXME_OBJECT (pad, "Handle stream changes (%s => %s) !",
              gst_stream_get_stream_id (slot->active_stream),
              gst_stream_get_stream_id (stream));
          gst_object_unref (slot->active_stream);
          slot->active_stream = stream;
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
      }
        break;
      case GST_EVENT_CAPS:
      {
        /* Configure the output slot if needed */
        DecodebinOutputStream *output;
        GstMessage *msg = NULL;
        SELECTION_LOCK (dbin);
        output = get_output_for_slot (slot);
        if (output) {
          reconfigure_output_stream (output, slot);
          msg = is_selection_done (dbin);
        }
        SELECTION_UNLOCK (dbin);
        if (msg)
          gst_element_post_message ((GstElement *) slot->dbin, msg);
      }
        break;
      case GST_EVENT_EOS:
      {
        gboolean was_drained = slot->is_drained;
        slot->is_drained = TRUE;

        /* Custom EOS handling first */
        if (gst_mini_object_get_qdata (GST_MINI_OBJECT_CAST (ev),
                CUSTOM_EOS_QUARK)) {
          /* remove custom-eos */
          gst_mini_object_set_qdata (GST_MINI_OBJECT_CAST (ev),
              CUSTOM_EOS_QUARK, NULL, NULL);
          GST_LOG_OBJECT (pad, "Received custom EOS");
          ret = GST_PAD_PROBE_HANDLED;
          SELECTION_LOCK (dbin);
          if (slot->input == NULL) {
            GST_DEBUG_OBJECT (pad,
                "Got custom-eos from null input stream, remove output stream");
            /* Remove the output */
            if (slot->output) {
              DecodebinOutputStream *output = slot->output;
              dbin->output_streams =
                  g_list_remove (dbin->output_streams, output);
              free_output_stream (dbin, output);
            }
            slot->probe_id = 0;
            dbin->slots = g_list_remove (dbin->slots, slot);
            free_multiqueue_slot_async (dbin, slot);
            ret = GST_PAD_PROBE_REMOVE;
          } else if (!was_drained) {
            check_all_slot_for_eos (dbin);
          }
          SELECTION_UNLOCK (dbin);
          break;
        }

        GST_FIXME_OBJECT (pad, "EOS on multiqueue source pad. input:%p",
            slot->input);
        if (slot->input == NULL) {
          GstPad *peer;
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
          /* Remove the output */
          if (slot->output) {
            DecodebinOutputStream *output = slot->output;
            dbin->output_streams = g_list_remove (dbin->output_streams, output);
            free_output_stream (dbin, output);
          }
          slot->probe_id = 0;
          dbin->slots = g_list_remove (dbin->slots, slot);
          SELECTION_UNLOCK (dbin);

          free_multiqueue_slot_async (dbin, slot);
          ret = GST_PAD_PROBE_REMOVE;
        } else if (gst_mini_object_get_qdata (GST_MINI_OBJECT_CAST (ev),
                CUSTOM_FINAL_EOS_QUARK)) {
          GST_DEBUG_OBJECT (pad, "Got final eos, propagating downstream");
        } else {
          GST_DEBUG_OBJECT (pad, "Got regular eos (all_inputs_are_eos)");
          /* drop current event as eos will be sent in check_all_slot_for_eos
           * when all output streams are also eos */
          ret = GST_PAD_PROBE_DROP;
          SELECTION_LOCK (dbin);
          check_all_slot_for_eos (dbin);
          SELECTION_UNLOCK (dbin);
        }
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
  }

  return ret;
}

/* Create a new multiqueue slot for the given type
 *
 * It is up to the caller to know whether that slot is needed or not
 * (and release it when no longer needed) */
static MultiQueueSlot *
create_new_slot (GstDecodebin3 * dbin, GstStreamType type)
{
  MultiQueueSlot *slot;
  GstIterator *it = NULL;
  GValue item = { 0, };
  GstDecodebin3Class *klass = GST_DECODEBIN3_GET_CLASS (dbin);

  GST_DEBUG_OBJECT (dbin, "Creating new slot for type %s",
      gst_stream_type_get_name (type));
  slot = g_new0 (MultiQueueSlot, 1);
  slot->dbin = dbin;

  slot->id = dbin->slot_id++;

  slot->type = type;
  slot->sink_pad = gst_element_get_request_pad (dbin->multiqueue, "sink_%u");
  if (slot->sink_pad == NULL)
    goto fail;

  it = gst_pad_iterate_internal_links (slot->sink_pad);
  if (!it || (gst_iterator_next (it, &item)) != GST_ITERATOR_OK
      || ((slot->src_pad = g_value_dup_object (&item)) == NULL)) {
    GST_ERROR ("Couldn't get srcpad from multiqueue for sink pad %s:%s",
        GST_DEBUG_PAD_NAME (slot->src_pad));
    goto fail;
  }
  gst_iterator_free (it);
  g_value_reset (&item);

  g_object_set (slot->sink_pad, "group-id", (guint) type, NULL);

  /* Add event probe */
  if (G_LIKELY (klass->priv_create_new_slot))
    klass->priv_create_new_slot (dbin, slot);
  else
    slot->probe_id =
        gst_pad_add_probe (slot->src_pad,
        GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM |
        GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM,
        (GstPadProbeCallback) multiqueue_src_probe, slot, NULL);

  GST_DEBUG ("Created new slot %u (%p) (%s:%s)", slot->id, slot,
      GST_DEBUG_PAD_NAME (slot->src_pad));

  dbin->slots = g_list_append (dbin->slots, slot);

  return slot;

  /* ERRORS */
fail:
  {
    if (slot->sink_pad)
      gst_element_release_request_pad (dbin->multiqueue, slot->sink_pad);
    g_free (slot);
    return NULL;
  }
}

/* Must be called with SELECTION_LOCK */
static MultiQueueSlot *
get_slot_for_input (GstDecodebin3 * dbin, DecodebinInputStream * input)
{
  GList *tmp;
  MultiQueueSlot *empty_slot = NULL;
  GstStreamType input_type = 0;
  gchar *stream_id = NULL;

  GST_DEBUG_OBJECT (dbin, "input %p (stream %p %s)",
      input, input->active_stream,
      input->active_stream ? gst_stream_get_stream_id (input->
          active_stream) : "");

  if (input->active_stream) {
    input_type = gst_stream_get_stream_type (input->active_stream);
    stream_id = (gchar *) gst_stream_get_stream_id (input->active_stream);
  }

  /* Go over existing slots and check if there is already one for it */
  for (tmp = dbin->slots; tmp; tmp = tmp->next) {
    MultiQueueSlot *slot = (MultiQueueSlot *) tmp->data;
    /* Already used input, return that one */
    if (slot->input == input) {
      GST_DEBUG_OBJECT (dbin, "Returning already specified slot %d", slot->id);
      return slot;
    }
  }

  /* Go amongst all unused slots of the right type and try to find a candidate */
  for (tmp = g_list_last (dbin->slots); tmp; tmp = tmp->prev) {
    MultiQueueSlot *slot = (MultiQueueSlot *) tmp->data;
    if (slot->input == NULL && input_type == slot->type) {
      /* Remember this empty slot for later */
      empty_slot = slot;
      /* Check if available slot is of the same stream_id */
      GST_LOG_OBJECT (dbin, "Checking candidate slot %d (active_stream:%p)",
          slot->id, slot->active_stream);
      if (stream_id && slot->active_stream) {
        gchar *ostream_id =
            (gchar *) gst_stream_get_stream_id (slot->active_stream);
        GST_DEBUG_OBJECT (dbin, "Checking slot %d %s against %s", slot->id,
            ostream_id, stream_id);
        if (!g_strcmp0 (stream_id, ostream_id))
          break;
      }
    }
  }

  if (empty_slot) {
    GST_DEBUG_OBJECT (dbin, "Re-using existing unused slot %d", empty_slot->id);
    empty_slot->input = input;
    return empty_slot;
  }

  if (input_type)
    return create_new_slot (dbin, input_type);

  return NULL;
}

static void
link_input_to_slot (DecodebinInput * parent_input, DecodebinInputStream * input,
    MultiQueueSlot * slot)
{
  if (slot->input != NULL && slot->input != input) {
    GST_ERROR_OBJECT (slot->dbin,
        "Trying to link input to an already used slot");
    return;
  }
  GST_DEBUG_OBJECT (slot->sink_pad, "Linking to input(%s:%s)",
      GST_DEBUG_PAD_NAME (input->srcpad));
  gst_pad_link_full (input->srcpad, slot->sink_pad, GST_PAD_LINK_CHECK_NOTHING);
  slot->pending_stream = input->active_stream;
  slot->input = input;
  slot->parent_input = parent_input;
  GST_DEBUG_OBJECT (slot->sink_pad, "Assigned to input stream(%p)", input);
}

#if 0
static gboolean
have_factory (GstDecodebin3 * dbin, GstCaps * caps,
    GstElementFactoryListType ftype)
{
  gboolean ret = FALSE;
  GList *res;

  g_mutex_lock (&dbin->factories_lock);
  gst_decode_bin_update_factories_list (dbin);
  if (ftype == GST_ELEMENT_FACTORY_TYPE_DECODER)
    res =
        gst_element_factory_list_filter (dbin->decoder_factories,
        caps, GST_PAD_SINK, TRUE);
  else
    res =
        gst_element_factory_list_filter (dbin->decodable_factories,
        caps, GST_PAD_SINK, TRUE);
  g_mutex_unlock (&dbin->factories_lock);

  if (res) {
    ret = TRUE;
    gst_plugin_feature_list_free (res);
  }

  return ret;
}
#endif

static GstElement *
create_element (GstDecodebin3 * dbin, GstStream * stream,
    GstElementFactoryListType ftype)
{
  GList *res;
  GstElement *element = NULL;
  GstCaps *caps;
  GList *tmp;

  g_mutex_lock (&dbin->factories_lock);
  gst_decode_bin_update_factories_list (dbin);
  caps = gst_stream_get_caps (stream);
  if (ftype == GST_ELEMENT_FACTORY_TYPE_DECODER)
    res =
        gst_element_factory_list_filter (dbin->decoder_factories,
        caps, GST_PAD_SINK, TRUE);
  else
    res =
        gst_element_factory_list_filter (dbin->decodable_factories,
        caps, GST_PAD_SINK, TRUE);
  g_mutex_unlock (&dbin->factories_lock);

  if (res) {
    for (tmp = res; tmp; tmp = tmp->next) {
      GstElementFactory *factory = (GstElementFactory *) tmp->data;
      GST_LOG ("got factory : %" GST_PTR_FORMAT, factory);
    }

    element =
        gst_element_factory_create ((GstElementFactory *) res->data, NULL);
    GST_DEBUG ("Created element '%s'", GST_ELEMENT_NAME (element));
    gst_plugin_feature_list_free (res);
  } else {
    GST_DEBUG ("Could not find an element for caps %" GST_PTR_FORMAT, caps);
  }

  gst_caps_unref (caps);
  return element;
}

/* FIXME : VERY NAIVE. ASSUMING FIRST ONE WILL WORK */
static GstElement *
create_decoder (GstDecodebin3 * dbin, GstStream * stream)
{
  return create_element (dbin, stream, GST_ELEMENT_FACTORY_TYPE_DECODER);
}

static GstPadProbeReturn
keyframe_waiter_probe (GstPad * pad, GstPadProbeInfo * info,
    DecodebinOutputStream * output)
{
  GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER (info);
  /* If we have a keyframe, remove the probe and let all data through */
  /* FIXME : HANDLE HEADER BUFFER ?? */
  if (!GST_BUFFER_FLAG_IS_SET (buf, GST_BUFFER_FLAG_DELTA_UNIT) ||
      GST_BUFFER_FLAG_IS_SET (buf, GST_BUFFER_FLAG_HEADER)) {
    GST_DEBUG_OBJECT (pad,
        "Buffer is keyframe or header, letting through and removing probe");
    output->drop_probe_id = 0;
    return GST_PAD_PROBE_REMOVE;
  }
  GST_SYS_DEBUG_OBJECT (pad, "Buffer is not a keyframe, dropping");
  return GST_PAD_PROBE_DROP;
}

static void
reconfigure_output_stream (DecodebinOutputStream * output,
    MultiQueueSlot * slot)
{
  GstDecodebin3 *dbin = output->dbin;
  GstCaps *new_caps = (GstCaps *) gst_stream_get_caps (slot->active_stream);
  gboolean needs_decoder;

  needs_decoder = gst_caps_can_intersect (new_caps, dbin->caps) != TRUE;

  GST_DEBUG_OBJECT (dbin,
      "Reconfiguring output %p to slot %p, needs_decoder:%d", output, slot,
      needs_decoder);

  /* FIXME : Maybe make the output un-hook itself automatically ? */
  if (output->slot != NULL && output->slot != slot) {
    GST_WARNING_OBJECT (dbin,
        "Output still linked to another slot (%p)", output->slot);
    gst_caps_unref (new_caps);
    return;
  }

  /* Check if existing config is reusable as-is by checking if
   * the existing decoder accepts the new caps, if not delete
   * it and create a new one */
  if (output->decoder) {
    gboolean can_reuse_decoder;

    if (needs_decoder) {
      can_reuse_decoder =
          gst_pad_query_accept_caps (output->decoder_sink, new_caps);
    } else
      can_reuse_decoder = FALSE;

    if (can_reuse_decoder) {
      if (output->type & GST_STREAM_TYPE_VIDEO && output->drop_probe_id == 0) {
        GST_DEBUG_OBJECT (dbin, "Adding keyframe-waiter probe");
        output->drop_probe_id =
            gst_pad_add_probe (slot->src_pad, GST_PAD_PROBE_TYPE_BUFFER,
            (GstPadProbeCallback) keyframe_waiter_probe, output, NULL);
      }
      GST_DEBUG_OBJECT (dbin, "Reusing existing decoder for slot %p", slot);
      if (output->linked == FALSE) {
        gst_pad_link_full (slot->src_pad, output->decoder_sink,
            GST_PAD_LINK_CHECK_NOTHING);
        output->linked = TRUE;
      }
      gst_caps_unref (new_caps);
      return;
    }

    GST_DEBUG_OBJECT (dbin, "Removing old decoder for slot %p", slot);

    if (output->linked)
      gst_pad_unlink (slot->src_pad, output->decoder_sink);
    output->linked = FALSE;
    if (output->drop_probe_id) {
      gst_pad_remove_probe (slot->src_pad, output->drop_probe_id);
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
    if (!gst_bin_add ((GstBin *) dbin, output->decoder)) {
      GST_ERROR_OBJECT (dbin, "could not add decoder to pipeline");
      goto cleanup;
    }
    output->decoder_sink = gst_element_get_static_pad (output->decoder, "sink");
    output->decoder_src = gst_element_get_static_pad (output->decoder, "src");
    if (output->type & GST_STREAM_TYPE_VIDEO) {
      GST_DEBUG_OBJECT (dbin, "Adding keyframe-waiter probe");
      output->drop_probe_id =
          gst_pad_add_probe (slot->src_pad, GST_PAD_PROBE_TYPE_BUFFER,
          (GstPadProbeCallback) keyframe_waiter_probe, output, NULL);
    }
    if (gst_pad_link_full (slot->src_pad, output->decoder_sink,
            GST_PAD_LINK_CHECK_NOTHING) != GST_PAD_LINK_OK) {
      GST_ERROR_OBJECT (dbin, "could not link to %s:%s",
          GST_DEBUG_PAD_NAME (output->decoder_sink));
      goto cleanup;
    }
  } else {
    output->decoder_src = gst_object_ref (slot->src_pad);
    output->decoder_sink = NULL;
  }
  output->linked = TRUE;
  if (!gst_ghost_pad_set_target ((GstGhostPad *) output->src_pad,
          output->decoder_src)) {
    GST_ERROR_OBJECT (dbin, "Could not expose decoder pad");
    goto cleanup;
  }
  if (output->src_exposed == FALSE) {
    output->src_exposed = TRUE;
    gst_element_add_pad (GST_ELEMENT_CAST (dbin), output->src_pad);
  }

  if (output->decoder)
    gst_element_sync_state_with_parent (output->decoder);

  output->slot = slot;
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
  }
}

static GstPadProbeReturn
idle_reconfigure (GstPad * pad, GstPadProbeInfo * info, MultiQueueSlot * slot)
{
  GstMessage *msg = NULL;
  DecodebinOutputStream *output;

  SELECTION_LOCK (slot->dbin);
  output = get_output_for_slot (slot);

  GST_DEBUG_OBJECT (pad, "output : %p", output);

  if (output) {
    GstDecodebin3Class *klass = GST_DECODEBIN3_GET_CLASS (slot->dbin);
    if (G_LIKELY (klass->priv_reconfigure_output_stream))
      klass->priv_reconfigure_output_stream (output, slot, TRUE);
    else
      reconfigure_output_stream (output, slot);

    if (G_LIKELY (klass->priv_is_selection_done))
      msg = klass->priv_is_selection_done (slot->dbin);
    else
      msg = is_selection_done (slot->dbin);
  }
  SELECTION_UNLOCK (slot->dbin);

  if (slot->dbin->dvr_playback && slot->dbin->to_activate != NULL) {
    DUMP_SELECTION_LIST (slot->dbin, slot->dbin->to_activate, "<to_activate>");
    GST_DEBUG_OBJECT (slot->src_pad,
        "Adding probe for re-assign slot for remained sid(s)");
    gst_pad_add_probe (slot->src_pad, GST_PAD_PROBE_TYPE_IDLE,
        (GstPadProbeCallback) slot_unassign_probe, slot, NULL);
  }

  if (msg)
    gst_element_post_message ((GstElement *) slot->dbin, msg);

  return GST_PAD_PROBE_REMOVE;
}

static MultiQueueSlot *
find_slot_for_stream_id (GstDecodebin3 * dbin, const gchar * sid)
{
  GList *tmp;

  for (tmp = dbin->slots; tmp; tmp = tmp->next) {
    MultiQueueSlot *slot = (MultiQueueSlot *) tmp->data;
    const gchar *stream_id;
    if (slot->active_stream) {
      stream_id = gst_stream_get_stream_id (slot->active_stream);
      if (!g_strcmp0 (sid, stream_id))
        return slot;
    }
    if (slot->pending_stream && slot->pending_stream != slot->active_stream) {
      stream_id = gst_stream_get_stream_id (slot->pending_stream);
      if (!g_strcmp0 (sid, stream_id))
        return slot;
    }
    if (slot->old_stream) {
      stream_id = gst_stream_get_stream_id (slot->old_stream);
      if (!g_strcmp0 (sid, stream_id))
        return slot;
    }
  }

  return NULL;
}

/* This function handles the reassignment of a slot. Call this from
 * the streaming thread of a slot. */
static gboolean
reassign_slot (GstDecodebin3 * dbin, MultiQueueSlot * slot)
{
  DecodebinOutputStream *output;
  MultiQueueSlot *target_slot = NULL;
  GList *tmp;
  const gchar *sid, *tsid;

  SELECTION_LOCK (dbin);
  output = slot->output;

  if (G_UNLIKELY (slot->active_stream == NULL)) {
    GST_DEBUG_OBJECT (slot->src_pad,
        "Called on inactive slot (active_stream == NULL)");
    SELECTION_UNLOCK (dbin);
    return FALSE;
  }

  if (G_UNLIKELY (output == NULL)) {
    GST_DEBUG_OBJECT (slot->src_pad,
        "Slot doesn't have any output to be removed");
    SELECTION_UNLOCK (dbin);
    return FALSE;
  }

  sid = gst_stream_get_stream_id (slot->active_stream);
  GST_DEBUG_OBJECT (slot->src_pad, "slot %s %p", sid, slot);

  /* Recheck whether this stream is still in the list of streams to deactivate */
  if (stream_in_list (dbin->requested_selection, sid)) {
    /* Stream is in the list of requested streams, don't remove */
    SELECTION_UNLOCK (dbin);
    GST_DEBUG_OBJECT (slot->src_pad,
        "Stream '%s' doesn't need to be deactivated", sid);
    return FALSE;
  }

  DUMP_SELECTION_LIST (dbin, dbin->requested_selection,
      "##### BEFORE requested");
  DUMP_SELECTION_LIST (dbin, dbin->to_activate, "##### BEFORE to-activate");
  DUMP_SELECTION_LIST (dbin, dbin->active_selection, "##### BEFEORE active");

  /* Unlink slot from output */
  /* FIXME : Handle flushing ? */
  /* FIXME : Handle outputs without decoders */
  GST_DEBUG_OBJECT (slot->src_pad, "Unlinking from decoder %p",
      output->decoder_sink);
  if (output->decoder_sink)
    gst_pad_unlink (slot->src_pad, output->decoder_sink);
  output->linked = FALSE;
  slot->output = NULL;
  output->slot = NULL;

  /* Remove sid from active selection */
  for (tmp = dbin->active_selection; tmp; tmp = tmp->next)
    if (!g_strcmp0 (sid, tmp->data)) {
      dbin->active_selection = g_list_delete_link (dbin->active_selection, tmp);
      break;
    }

  if (dbin->dvr_playback
      && !stream_in_list (dbin->requested_selection, (gchar *) sid)) {
    GstStreamType deactivated_type =
        gst_stream_get_stream_type (slot->active_stream);
    GstStream *requested_stream = NULL;
    GstStream *to_activate_stream = NULL;

    for (tmp = dbin->requested_selection; tmp; tmp = tmp->next) {
      GstStream *stream = find_stream_in_collection (dbin->active_collection,
          (gchar *) tmp->data);
      if (stream && gst_stream_get_stream_type (stream) == deactivated_type) {
        GST_DEBUG_OBJECT (slot->src_pad,
            "We Found requested stream(%s) in active-collection",
            gst_stream_get_stream_id (stream));
        requested_stream = stream;
        break;
      }
    }

    if (requested_stream) {
      for (tmp = dbin->to_activate; tmp; tmp = tmp->next) {
        GstStream *stream = find_stream_in_collection (dbin->active_collection,
            ((gchar *) tmp->data));
        if (stream && gst_stream_get_stream_type (stream) == deactivated_type) {
          GST_DEBUG_OBJECT (slot->src_pad,
              "We Found to-activate stream(%s) in active-collection",
              gst_stream_get_stream_id (stream));
          to_activate_stream = stream;
          break;
        }
      }

      if (requested_stream && to_activate_stream) {
        const gchar *requested_sid =
            gst_stream_get_stream_id (requested_stream);
        const gchar *to_activate_sid =
            gst_stream_get_stream_id (to_activate_stream);

        if (g_strcmp0 (requested_sid, to_activate_sid)) {
          GST_DEBUG_OBJECT (slot->src_pad, "Replace to_activate (%s) to (%s)",
              to_activate_sid, requested_sid);
          dbin->to_activate =
              g_list_remove (dbin->to_activate, to_activate_sid);
          dbin->to_activate = g_list_append (dbin->to_activate, requested_sid);
        }
      }
    }
  }

  /* Can we re-assign this output to a requested stream ? */
  GST_DEBUG_OBJECT (slot->src_pad, "Attempting to re-assing output stream");
  DUMP_SELECTION_LIST (dbin, dbin->to_activate, "##### <to_activate>");
  for (tmp = dbin->to_activate; tmp; tmp = tmp->next) {
    MultiQueueSlot *tslot = find_slot_for_stream_id (dbin, tmp->data);
    GST_LOG_OBJECT (tslot->src_pad, "Checking slot %p (output:%p , stream:%s)",
        tslot, tslot->output, gst_stream_get_stream_id (tslot->active_stream));
    if (tslot && tslot->type == output->type && tslot->output == NULL) {
      GST_DEBUG_OBJECT (tslot->src_pad, "Using as reassigned slot");
      target_slot = tslot;
      tsid = tmp->data;
      /* Pass target stream id to requested selection */
      if (!stream_in_list (dbin->requested_selection, tsid)) {
        dbin->requested_selection =
            g_list_append (dbin->requested_selection, g_strdup (tmp->data));
        DUMP_SELECTION_LIST (dbin, dbin->requested_selection, "requested");
      }
      dbin->to_activate = g_list_remove (dbin->to_activate, tmp->data);
      break;
    }
  }

  if (target_slot) {
    GST_DEBUG_OBJECT (slot->src_pad, "Assigning output to slot %p '%s'",
        target_slot, tsid);
    target_slot->output = output;
    output->slot = target_slot;
    dbin->active_selection =
        g_list_append (dbin->active_selection, (gchar *) tsid);
    DUMP_SELECTION_LIST (dbin, dbin->active_selection, "active");
    SELECTION_UNLOCK (dbin);

    /* Wakeup the target slot so that it retries to send events/buffers
     * thereby triggering the output reconfiguration codepath */
    gst_pad_add_probe (target_slot->src_pad, GST_PAD_PROBE_TYPE_IDLE,
        (GstPadProbeCallback) idle_reconfigure, target_slot, NULL);
    /* gst_pad_send_event (target_slot->src_pad, gst_event_new_reconfigure ()); */
  } else {
    GstMessage *msg;

    dbin->output_streams = g_list_remove (dbin->output_streams, output);
    free_output_stream (dbin, output);
    msg = is_selection_done (slot->dbin);
    SELECTION_UNLOCK (dbin);

    if (msg)
      gst_element_post_message ((GstElement *) slot->dbin, msg);
  }

  DUMP_SELECTION_LIST (dbin, dbin->requested_selection,
      "##### AFTER requested");
  DUMP_SELECTION_LIST (dbin, dbin->to_activate, "##### AFTER to-activate");
  DUMP_SELECTION_LIST (dbin, dbin->active_selection, "AFTER BEFEORE active");

  return TRUE;
}

/* Idle probe called when a slot should be unassigned from its output stream.
 * This is needed to ensure nothing is flowing when unlinking the slot.
 *
 * Also, this method will search for a pending stream which could re-use
 * the output stream. */
static GstPadProbeReturn
slot_unassign_probe (GstPad * pad, GstPadProbeInfo * info,
    MultiQueueSlot * slot)
{
  GstDecodebin3 *dbin = slot->dbin;

  reassign_slot (dbin, slot);

  return GST_PAD_PROBE_REMOVE;
}

static gboolean
handle_stream_switch (GstDecodebin3 * dbin, GList * select_streams,
    guint32 seqnum)
{
  gboolean ret = TRUE;
  GList *tmp;
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
  for (tmp = select_streams; tmp; tmp = tmp->next) {
    const gchar *sid = (const gchar *) tmp->data;
    MultiQueueSlot *slot;
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
    } else if (slot->output == NULL) {
      GST_DEBUG_OBJECT (dbin, "We need to activate slot %p for stream '%s')",
          slot, sid);
      to_activate = g_list_append (to_activate, slot);
    } else {
      GST_DEBUG_OBJECT (dbin,
          "Stream '%s' from slot %p is already active on output %p", sid, slot,
          slot->output);
      future_request_streams =
          g_list_append (future_request_streams, (gchar *) sid);
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
                gst_stream_get_stream_id (slot->active_stream)))
          slot_to_deactivate = FALSE;
      }
      if (slot_to_deactivate && slot->pending_stream
          && slot->pending_stream != slot->active_stream) {
        if (stream_in_list (select_streams,
                gst_stream_get_stream_id (slot->pending_stream)))
          slot_to_deactivate = FALSE;
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
        g_list_copy_deep (select_streams, (GCopyFunc) g_strdup, NULL);
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

  return ret;
}

static void
mark_all_output_flush (GstDecodebin3 * dbin, gboolean mark)
{
  GList *tmp;

  for (tmp = dbin->output_streams; tmp; tmp = tmp->next) {
    DecodebinOutputStream *output = (DecodebinOutputStream *) tmp->data;

    output->got_flush_start = mark;
    output->got_flush_stop = mark;
  }
}

static void
check_all_output_for_flush (GstDecodebin3 * dbin)
{
  GList *tmp;

  for (tmp = dbin->output_streams; tmp; tmp = tmp->next) {
    DecodebinOutputStream *output = (DecodebinOutputStream *) tmp->data;
    MultiQueueSlot *slot;
    GstEvent *flush_event;
    GstPad *sinkpad;
    GstPad *peer;

    slot = output->slot;
    sinkpad = slot->sink_pad;
    peer = gst_pad_get_peer (output->src_pad);
    GST_DEBUG_OBJECT (output->src_pad, "peerpad(%s:%s)",
        GST_DEBUG_PAD_NAME (peer));

    if (!output->got_flush_start) {
      flush_event = gst_event_new_flush_start ();
      GST_DEBUG_OBJECT (sinkpad, "Sending: %" GST_PTR_FORMAT, flush_event);
      gst_pad_send_event (sinkpad, flush_event);

      if (peer != NULL && !GST_PAD_IS_FLUSHING (peer)) {
        flush_event = gst_event_new_flush_start ();
        GST_DEBUG_OBJECT (peer, "Forced to pushing: %" GST_PTR_FORMAT,
            flush_event);
        gst_pad_send_event (peer, flush_event);
      }
    }

    if (!output->got_flush_stop) {
      flush_event = gst_event_new_flush_stop (TRUE);
      GST_DEBUG_OBJECT (sinkpad, "Sending: %" GST_PTR_FORMAT, flush_event);
      gst_pad_send_event (sinkpad, flush_event);

      if (peer != NULL && GST_PAD_IS_FLUSHING (peer)) {
        flush_event = gst_event_new_flush_stop (TRUE);
        GST_DEBUG_OBJECT (peer, "Forced to pushing: %" GST_PTR_FORMAT,
            flush_event);
        gst_pad_send_event (peer, flush_event);
      }
    }

    gst_object_unref (peer);
  }
}

static gboolean
output_srcpad_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  GstDecodebin3 *dbin = (GstDecodebin3 *) parent;
  DecodebinOutputStream *output = NULL;
  GList *tmp;
  gboolean ret = TRUE;

  for (tmp = dbin->output_streams; tmp; tmp = tmp->next) {
    DecodebinOutputStream *out = (DecodebinOutputStream *) tmp->data;
    if (out->src_pad == pad) {
      output = out;
      break;
    }
  }

  GST_LOG_OBJECT (pad, "Got event %p %s", event, GST_EVENT_TYPE_NAME (event));

  if (!output)
    goto no_output;

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_SEEK:
    {
      GstFlowReturn flow_ret;
      GstEvent *copy;
      DecodebinInput *input = dbin->main_input;

      GST_DEBUG_OBJECT (pad, "Seeing Seek:%" GST_PTR_FORMAT, event);
      GST_DEBUG_OBJECT (pad, "parsebin_sink(%s:%s)",
          GST_DEBUG_PAD_NAME (input->parsebin_sink));

      copy = gst_event_copy (event);

      mark_all_output_flush (dbin, FALSE);

      GST_DEBUG_OBJECT (pad, "Pushing Seek event: %p", event);
      ret = gst_pad_push_event (input->parsebin_sink, event);
      flow_ret = GST_PAD_LAST_FLOW_RETURN (input->parsebin_sink);
      GST_DEBUG_OBJECT (pad, "Pushed Seek event: ret: %d, pad_flow_return: %s",
          ret, gst_flow_get_name (flow_ret));
      if (ret) {
        gst_event_unref (copy);
        check_all_output_for_flush (dbin);
      }
      return ret;
    }
      break;
    default:
      break;
  }

forward:
  GST_DEBUG_OBJECT (pad, "Pushing event: %" GST_PTR_FORMAT, event);
  ret = gst_pad_event_default (pad, parent, event);
  GST_DEBUG_OBJECT (pad, "Pushed event: %d", ret);
  return ret;

no_output:
  GST_DEBUG_OBJECT (pad, "No corresponding output");
  goto forward;
}

static GstPadProbeReturn
output_srcpad_flush_probe (GstPad * pad, GstPadProbeInfo * info,
    DecodebinOutputStream * output)
{
  GstPadProbeReturn ret = GST_PAD_PROBE_OK;
  GstDecodebin3 *dbin = output->dbin;
  GstEvent *event = GST_PAD_PROBE_INFO_EVENT (info);

  GST_LOG_OBJECT (pad, "Got event %p %s", event, GST_EVENT_TYPE_NAME (event));

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_FLUSH_START:
    {
      GST_DEBUG_OBJECT (pad, "Got FLUSH-START");
      output->got_flush_start = TRUE;
    }
      break;
    case GST_EVENT_FLUSH_STOP:
    {
      GST_DEBUG_OBJECT (pad, "Got FLUSH-STOP");
      output->got_flush_stop = TRUE;
    }
      break;
    default:
      break;
  }

  return ret;
}

static GstPadProbeReturn
ghost_pad_event_probe (GstPad * pad, GstPadProbeInfo * info,
    DecodebinOutputStream * output)
{
  GstPadProbeReturn ret = GST_PAD_PROBE_OK;
  GstDecodebin3 *dbin = output->dbin;
  GstEvent *event = GST_PAD_PROBE_INFO_EVENT (info);
  GstDecodebin3Class *klass = GST_DECODEBIN3_GET_CLASS (dbin);

  GST_DEBUG_OBJECT (pad, "Got event %p %s", event, GST_EVENT_TYPE_NAME (event));

  if (G_LIKELY (klass->priv_ghost_pad_event_probe)) {
    gboolean steal = FALSE;
    ret = klass->priv_ghost_pad_event_probe (dbin, pad, event, output, &steal);
    if (steal)
      return ret;
  }

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_SELECT_STREAMS:
    {
      GstPad *peer;
      GList *streams = NULL;
      guint32 seqnum = gst_event_get_seqnum (event);

      SELECTION_LOCK (dbin);
      if (seqnum == dbin->select_streams_seqnum) {
        SELECTION_UNLOCK (dbin);
        GST_DEBUG_OBJECT (pad,
            "Already handled/handling that SELECT_STREAMS event");
        gst_event_unref (event);
        ret = GST_PAD_PROBE_HANDLED;
        break;
      }
      dbin->select_streams_seqnum = seqnum;
      if (dbin->pending_select_streams != NULL) {
        GST_LOG_OBJECT (dbin, "Replacing pending select streams");
        g_list_free (dbin->pending_select_streams);
        dbin->pending_select_streams = NULL;
      }
      gst_event_parse_select_streams (event, &streams);
      dbin->pending_select_streams = g_list_copy (streams);
      SELECTION_UNLOCK (dbin);

      /* Send event upstream */
      if ((peer = gst_pad_get_peer (pad))) {
        gst_pad_send_event (peer, event);
        gst_object_unref (peer);
      } else {
        gst_event_unref (event);
      }
      /* Finally handle the switch */
      if (streams) {
        if (G_LIKELY (klass->priv_handle_stream_switch)) {
          klass->priv_handle_stream_switch (dbin, streams, seqnum);
          g_list_free_full (streams, g_free);
          ret = GST_PAD_PROBE_HANDLED;
          break;
        }
        handle_stream_switch (dbin, streams, seqnum);
        g_list_free_full (streams, g_free);
      }
      ret = GST_PAD_PROBE_HANDLED;
    }
      break;
    default:
      break;
  }

  return ret;
}

static gboolean
gst_decodebin3_send_event (GstElement * element, GstEvent * event)
{
  GST_DEBUG_OBJECT (element, "event %s", GST_EVENT_TYPE_NAME (event));
  if (GST_EVENT_TYPE (event) == GST_EVENT_SELECT_STREAMS) {
    GstDecodebin3 *dbin = (GstDecodebin3 *) element;
    GList *streams = NULL;
    guint32 seqnum = gst_event_get_seqnum (event);

    SELECTION_LOCK (dbin);
    if (seqnum == dbin->select_streams_seqnum) {
      SELECTION_UNLOCK (dbin);
      GST_DEBUG_OBJECT (dbin,
          "Already handled/handling that SELECT_STREAMS event");
      return TRUE;
    }
    dbin->select_streams_seqnum = seqnum;
    if (dbin->pending_select_streams != NULL) {
      GST_LOG_OBJECT (dbin, "Replacing pending select streams");
      g_list_free (dbin->pending_select_streams);
      dbin->pending_select_streams = NULL;
    }
    gst_event_parse_select_streams (event, &streams);
    dbin->pending_select_streams = g_list_copy (streams);
    SELECTION_UNLOCK (dbin);

    /* FIXME : We don't have an upstream ?? */
#if 0
    /* Send event upstream */
    if ((peer = gst_pad_get_peer (pad))) {
      gst_pad_send_event (peer, event);
      gst_object_unref (peer);
    }
#endif
    /* Finally handle the switch */
    if (streams) {
      GstDecodebin3Class *klass = GST_DECODEBIN3_GET_CLASS (dbin);
      if (G_LIKELY (klass->priv_handle_stream_switch)) {
        klass->priv_handle_stream_switch (dbin, streams, seqnum);
        g_list_free_full (streams, g_free);
        gst_event_unref (event);
        return TRUE;
      }
      handle_stream_switch (dbin, streams, seqnum);
      g_list_free_full (streams, g_free);
    }

    gst_event_unref (event);
    return TRUE;
  }
  return GST_ELEMENT_CLASS (parent_class)->send_event (element, event);
}


static void
free_multiqueue_slot (GstDecodebin3 * dbin, MultiQueueSlot * slot)
{
  GstDecodebin3Class *klass = GST_DECODEBIN3_GET_CLASS (dbin);

  GST_DEBUG_OBJECT (dbin, "Releasing slot(%s:%s)",
      GST_DEBUG_PAD_NAME (slot->sink_pad));

  if (slot->probe_id)
    gst_pad_remove_probe (slot->src_pad, slot->probe_id);
  if (slot->input) {
    if (slot->input->srcpad)
      gst_pad_unlink (slot->input->srcpad, slot->sink_pad);
  }

  if (G_LIKELY (klass->priv_free_multiqueue_slot))
    klass->priv_free_multiqueue_slot (dbin, slot);

  gst_element_release_request_pad (dbin->multiqueue, slot->sink_pad);
  gst_object_replace ((GstObject **) & slot->sink_pad, NULL);
  gst_object_replace ((GstObject **) & slot->src_pad, NULL);
  gst_object_replace ((GstObject **) & slot->active_stream, NULL);
  gst_object_replace ((GstObject **) & slot->old_stream, NULL);

  g_free (slot);
}

static void
free_multiqueue_slot_async (GstDecodebin3 * dbin, MultiQueueSlot * slot)
{
  GST_LOG_OBJECT (dbin, "pushing multiqueue slot(%p) on thread pool to free",
      slot);
  gst_element_call_async (GST_ELEMENT_CAST (dbin),
      (GstElementCallAsyncFunc) free_multiqueue_slot, slot, NULL);
}

/* Create a DecodebinOutputStream for a given type
 * Note: It will be empty initially, it needs to be configured
 * afterwards */
static DecodebinOutputStream *
create_output_stream (GstDecodebin3 * dbin, GstStreamType type)
{
  DecodebinOutputStream *res = g_new0 (DecodebinOutputStream, 1);
  gchar *pad_name;
  const gchar *prefix;
  GstStaticPadTemplate *templ;
  GstPadTemplate *ptmpl;
  guint32 *counter;
  GstPad *internal_pad;

  GST_DEBUG_OBJECT (dbin, "Created new output stream %p for type %s",
      res, gst_stream_type_get_name (type));

  res->type = type;
  res->dbin = dbin;

  if (type & GST_STREAM_TYPE_VIDEO) {
    templ = &video_src_template;
    counter = &dbin->vpadcount;
    prefix = "video";
  } else if (type & GST_STREAM_TYPE_AUDIO) {
    templ = &audio_src_template;
    counter = &dbin->apadcount;
    prefix = "audio";
  } else if (type & GST_STREAM_TYPE_TEXT) {
    templ = &text_src_template;
    counter = &dbin->tpadcount;
    prefix = "text";
  } else {
    templ = &src_template;
    counter = &dbin->opadcount;
    prefix = "src";
  }

  pad_name = g_strdup_printf ("%s_%u", prefix, *counter);
  *counter += 1;
  ptmpl = gst_static_pad_template_get (templ);
  res->src_pad = gst_ghost_pad_new_no_target_from_template (pad_name, ptmpl);
  gst_object_unref (ptmpl);
  g_free (pad_name);
  gst_pad_set_active (res->src_pad, TRUE);
  /* Put an event probe on the internal proxy pad to detect upstream
   * events */
  internal_pad =
      (GstPad *) gst_proxy_pad_get_internal ((GstProxyPad *) res->src_pad);
  gst_pad_add_probe (internal_pad,
      GST_PAD_PROBE_TYPE_EVENT_UPSTREAM,
      (GstPadProbeCallback) ghost_pad_event_probe, res, NULL);
  gst_pad_add_probe (res->src_pad, GST_PAD_PROBE_TYPE_EVENT_FLUSH,
      (GstPadProbeCallback) output_srcpad_flush_probe, res, NULL);
  if (dbin->adaptive_mode)
    gst_pad_set_event_function (res->src_pad, output_srcpad_event);
  gst_object_unref (internal_pad);

  dbin->output_streams = g_list_append (dbin->output_streams, res);
  res->last_pushed_ts = GST_CLOCK_TIME_NONE;
  gst_segment_init (&res->segment, GST_FORMAT_UNDEFINED);

  return res;
}

static void
free_output_stream (GstDecodebin3 * dbin, DecodebinOutputStream * output)
{
  if (output->slot) {
    if (output->decoder_sink && output->decoder)
      gst_pad_unlink (output->slot->src_pad, output->decoder_sink);

    output->slot->output = NULL;
    output->slot = NULL;
  }
  gst_object_replace ((GstObject **) & output->decoder_sink, NULL);
  gst_ghost_pad_set_target ((GstGhostPad *) output->src_pad, NULL);
  gst_object_replace ((GstObject **) & output->decoder_src, NULL);
  if (output->src_exposed) {
    gst_element_remove_pad ((GstElement *) dbin, output->src_pad);
  }
  if (output->decoder) {
    gst_element_set_locked_state (output->decoder, TRUE);
    gst_element_set_state (output->decoder, GST_STATE_NULL);
    gst_bin_remove ((GstBin *) dbin, output->decoder);
  }
  g_free (output);
}

static GstStateChangeReturn
gst_decodebin3_change_state (GstElement * element, GstStateChange transition)
{
  GstDecodebin3 *dbin = (GstDecodebin3 *) element;
  GstStateChangeReturn ret;

  /* Upwards */
  switch (transition) {
    default:
      break;
  }
  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);
  if (ret == GST_STATE_CHANGE_FAILURE)
    goto beach;

  switch (transition) {
    case GST_STATE_CHANGE_PAUSED_TO_READY:
    {
      GList *tmp;
      GstDecodebin3Class *klass = GST_DECODEBIN3_GET_CLASS (dbin);

      /* Free output streams */
      for (tmp = dbin->output_streams; tmp; tmp = tmp->next) {
        DecodebinOutputStream *output = (DecodebinOutputStream *) tmp->data;
        free_output_stream (dbin, output);
      }
      g_list_free (dbin->output_streams);
      dbin->output_streams = NULL;
      /* Free multiqueue slots */
      for (tmp = dbin->slots; tmp; tmp = tmp->next) {
        MultiQueueSlot *slot = (MultiQueueSlot *) tmp->data;
        free_multiqueue_slot (dbin, slot);
      }
      g_list_free (dbin->slots);
      dbin->slots = NULL;
      dbin->current_group_id = GST_GROUP_ID_INVALID;
      /* Free inputs */
      if (G_LIKELY (klass->priv_release_resource_related))
        klass->priv_release_resource_related (dbin);
    }
      break;
    default:
      break;
  }
beach:
  return ret;
}

gboolean
gst_decodebin3_plugin_init (GstPlugin * plugin)
{
  GST_DEBUG_CATEGORY_INIT (decodebin3_debug, "decodebin3", 0, "decoder bin");

  return gst_element_register (plugin, "decodebin3", GST_RANK_NONE,
      GST_TYPE_DECODEBIN3);
}
