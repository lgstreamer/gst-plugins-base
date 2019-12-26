/* GStreamer
 * Copyright (C) <2018> Jinuk Jeon <jinuk.jeon@lge.com>
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

#include "gstsubtitlemeta.h"

GType
gst_subtitle_data_meta_api_get_type (void)
{
  static volatile GType type;
  static const gchar *tags[] =
      { GST_META_TAG_SUBTITLE_STR, GST_META_TAG_SUBTITLE_DATA_STR, NULL };

  if (g_once_init_enter (&type)) {
    GType _type = gst_meta_api_type_register ("GstSubtitleDataMetaAPI", tags);
    g_once_init_leave (&type, _type);
  }
  return type;
}

gboolean
gst_subtitle_data_meta_init (GstMeta * meta, gpointer params,
    GstBuffer * buffer)
{
  GstSubtitleDataMeta *data_meta = (GstSubtitleDataMeta *) meta;

  data_meta->period_start = 0;
  data_meta->subtitle_index = 0;
  data_meta->offset = 0;
  return TRUE;
}

void
gst_subtitle_data_meta_free (GstMeta * meta, GstBuffer * buffer)
{
  /*nothing to free really */
}

const GstMetaInfo *
gst_subtitle_data_meta_get_info (void)
{
  static const GstMetaInfo *meta_info = NULL;

  if (g_once_init_enter (&meta_info)) {
    const GstMetaInfo *meta =
        gst_meta_register (GST_SUBTITLE_DATA_META_API_TYPE,
        "GstSubtitleDataMeta",
        sizeof (GstSubtitleDataMeta), gst_subtitle_data_meta_init,
        gst_subtitle_data_meta_free, (GstMetaTransformFunction) NULL);
    g_once_init_leave (&meta_info, meta);
  }
  return meta_info;
}

GstSubtitleDataMeta *
gst_buffer_add_subtitle_data_meta (GstBuffer * buffer,
    guint64 period_start, gint64 offset, guint subtitle_index)
{
  GstSubtitleDataMeta *meta;

  g_return_val_if_fail (period_start >= 0, NULL);
  g_return_val_if_fail (subtitle_index >= 0, NULL);

  meta =
      (GstSubtitleDataMeta *) gst_buffer_add_meta (buffer,
      GST_SUBTITLE_DATA_META_INFO, NULL);

  meta->period_start = period_start;
  meta->subtitle_index = subtitle_index;
  meta->offset = offset;

  return meta;
}
