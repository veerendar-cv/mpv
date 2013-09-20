#ifndef MPV_VAAPI_H
#define MPV_VAAPI_H

#include <stdbool.h>
#include <inttypes.h>
#include <va/va.h>
#include <va/va_x11.h>

/* Compatibility glue with VA-API >= 0.31 */
#if defined VA_CHECK_VERSION
#if VA_CHECK_VERSION(0,31,0)
#define vaPutImage2             vaPutImage
#define vaAssociateSubpicture2  vaAssociateSubpicture
#endif
#endif

/* Compatibility glue with VA-API >= 0.34 */
#if VA_CHECK_VERSION(0,34,0)
#include <va/va_compat.h>
#endif

/* Compatibility glue with upstream libva */
#ifndef VA_SDS_VERSION
#define VA_SDS_VERSION          0
#endif

/* Compatibility glue with VA-API >= 0.30 */
#ifndef VA_INVALID_ID
#define VA_INVALID_ID           0xffffffff
#endif
#ifndef VA_FOURCC
#define VA_FOURCC(ch0, ch1, ch2, ch3)           \
    ((uint32_t)(uint8_t)(ch0) |                 \
     ((uint32_t)(uint8_t)(ch1) << 8) |          \
     ((uint32_t)(uint8_t)(ch2) << 16) |         \
     ((uint32_t)(uint8_t)(ch3) << 24 ))
#endif
#if defined VA_SRC_BT601 && defined VA_SRC_BT709
# define USE_VAAPI_COLORSPACE 1
#else
# define USE_VAAPI_COLORSPACE 0
#endif

/* Compatibility glue with VA-API >= 0.31.1 */
#ifndef VA_SRC_SMPTE_240
#define VA_SRC_SMPTE_240        0x00000040
#endif
#if defined VA_FILTER_SCALING_MASK
# define USE_VAAPI_SCALING 1
#else
# define USE_VAAPI_SCALING 0
#endif

#ifndef VA_FOURCC_YV12
#define VA_FOURCC_YV12 0x32315659
#endif
#ifndef VA_FOURCC_IYUV
#define VA_FOURCC_IYUV 0x56555949
#endif
#ifndef VA_FOURCC_I420
#define VA_FOURCC_I420 VA_FOURCC('I', '4', '2', '0')
#endif
#ifndef VA_FOURCC_NV12
#define VA_FOURCC_NV12 VA_FOURCC('N', 'V', '1', '2')
#endif

#define VA_STR_FOURCC(fcc) \
    (const char[]){(fcc), (fcc) >> 8u, (fcc) >> 16u, (fcc) >> 24u, 0}

#include "mpvcore/mp_msg.h"
#include "mp_image.h"

static inline bool check_va_status(VAStatus status, const char *msg)
{
    if (status != VA_STATUS_SUCCESS) {
        mp_msg(MSGT_VO, MSGL_ERR, "[vaapi] %s: %s\n", msg, vaErrorStr(status));
        return false;
    }
    return true;
}

static inline int get_va_colorspace_flag(enum mp_csp csp)
{
#if USE_VAAPI_COLORSPACE
    switch (csp) {
    case MP_CSP_BT_601:         return VA_SRC_BT601;
    case MP_CSP_BT_709:         return VA_SRC_BT709;
    case MP_CSP_SMPTE_240M:     return VA_SRC_SMPTE_240;
    }
#endif
    return 0;
}

struct mp_vaapi_ctx {
    VADisplay display;
    struct va_image_formats *image_formats;
    void *priv; // for VO
};

struct va_surface_pool;
struct va_image_formats;

struct va_surface {
    VASurfaceID id;      // VA_INVALID_ID if unallocated
    int w, h, rt_format; // parameters of allocated image (0/0/-1 unallocated)

    struct va_surface_priv *p;
};

enum mp_imgfmt           va_fourcc_to_imgfmt(uint32_t fourcc);
uint32_t                 va_fourcc_from_imgfmt(int imgfmt);
struct va_image_formats *va_image_formats_alloc(VADisplay display);
void                     va_image_formats_release(struct va_image_formats *formats);
VAImageFormat *          va_image_format_from_imgfmt(const struct va_image_formats *formats, int imgfmt);
bool                     va_image_map(VADisplay display, VAImage *image, struct mp_image *mpi);
bool                     va_image_unmap(VADisplay display, VAImage *image);

// ctx can be NULL. pool can be shared by specifying the same ctx value.
struct va_surface_pool * va_surface_pool_alloc(VADisplay display, int rt_format);
void                     va_surface_pool_release(struct va_surface_pool *pool);
void                     va_surface_pool_releasep(struct va_surface_pool **pool);
void                     va_surface_pool_clear(struct va_surface_pool *pool);
bool                     va_surface_pool_reserve(struct va_surface_pool *pool, int count, int w, int h);
int                      va_surface_pool_rt_format(const struct va_surface_pool *pool);
struct va_surface *      va_surface_pool_get(struct va_surface_pool *pool, int w, int h);
struct va_surface *      va_surface_pool_get_by_imgfmt(struct va_surface_pool *pool, const struct va_image_formats *formats, int imgfmt, int w, int h);
struct mp_image *        va_surface_pool_get_wrapped(struct va_surface_pool *pool, const struct va_image_formats *formats, int imgfmt, int w, int h);

void                     va_surface_release(struct va_surface *surface);
void                     va_surface_releasep(struct va_surface **surface);
struct va_surface *      va_surface_in_mp_image(struct mp_image *mpi);
struct mp_image *        va_surface_wrap(struct va_surface *surface); // takes ownership
VASurfaceID              va_surface_id(const struct va_surface *surface);
VASurfaceID              va_surface_id_in_mp_image(const struct mp_image *mpi);
bool                     va_surface_upload(struct va_surface *surface, const struct mp_image *mpi);
struct mp_image *        va_surface_download(const struct va_surface *surface, const struct va_image_formats *formats);

#endif
