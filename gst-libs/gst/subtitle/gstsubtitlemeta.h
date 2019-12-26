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

#ifndef __GST_SUBTITLE_META_H__
#define __GST_SUBTITLE_META_H__

#include <gst/subtitle/subtitle.h>

G_BEGIN_DECLS
#define GST_SUBTITLE_DATA_META_API_TYPE (gst_subtitle_data_meta_api_get_type())
#define GST_SUBTITLE_DATA_META_INFO (gst_subtitle_data_meta_get_info())
typedef struct _GstSubtitleDataMeta GstSubtitleDataMeta;

/* metadata for subtitle index and subtitle periodstarttime */
struct _GstSubtitleDataMeta
{
  GstMeta meta;

  guint64 period_start;
  guint   subtitle_index;
  gint64  offset;
};

GST_SUBTITLE_API GType gst_subtitle_data_meta_api_get_type (void);

GST_SUBTITLE_API const GstMetaInfo *gst_subtitle_data_meta_get_info (void);

#define gst_buffer_get_subtitle_data_meta(buf) \
    ((GstSubtitleDataMeta*)gst_buffer_get_meta ((buf), GST_SUBTITLE_DATA_META_API_TYPE))

GST_SUBTITLE_API
    GstSubtitleDataMeta * gst_buffer_add_subtitle_data_meta (GstBuffer *
    buffer, guint64 period_start, gint64 offset, guint subtitle_index);

gboolean gst_subtitle_data_meta_init (GstMeta * meta, gpointer params,
    GstBuffer * buffer);

void gst_subtitle_data_meta_free (GstMeta * meta, GstBuffer * buffer);

G_END_DECLS
#endif /* __GST_SUBTITLE_META_H__ */
