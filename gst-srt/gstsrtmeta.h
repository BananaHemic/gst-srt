#ifndef __GST_SRTMETA_H__
#define __GST_SRTMETA_H__

#include <gst/gst.h>
#include <gst/gstmeta.h>

G_BEGIN_DECLS

typedef struct _GstSrtMeta GstSrtMeta;

struct _GstSrtMeta {
    GstMeta meta;
    guint64 src_time;
};

GType gst_meta_marking_api_get_type (void);
const GstMetaInfo* gst_meta_marking_get_info (void);
#define GST_SRT_META_GET(buf) ((GstSrtMeta *)gst_buffer_get_meta(buf,gst_meta_marking_api_get_type()))
#define GST_SRT_META_ADD(buf) ((GstSrtMeta *)gst_buffer_add_meta(buf,gst_meta_marking_get_info(),(gpointer)NULL))

G_END_DECLS
#endif
