#pragma once

#include <cstdint>
#include <cstddef>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sk_surface_t sk_surface_t;
typedef struct sk_canvas_t sk_canvas_t;
typedef struct sk_paint_t sk_paint_t;
typedef struct sk_font_t sk_font_t;
typedef struct sk_typeface_t sk_typeface_t;
typedef struct sk_rrect_t sk_rrect_t;
typedef struct sk_surfaceprops_t sk_surfaceprops_t;
typedef struct gr_direct_context_t gr_direct_context_t;
typedef void (*sk_surface_raster_release_proc)(void* pixels, void* context);

typedef uint32_t sk_color_t;

typedef enum {
    SK_COLORTYPE_UNKNOWN,
    SK_COLORTYPE_ALPHA_8,
    SK_COLORTYPE_RGB_565,
    SK_COLORTYPE_ARGB_4444,
    SK_COLORTYPE_RGBA_8888,
    SK_COLORTYPE_RGB_888X,
    SK_COLORTYPE_BGRA_8888,
    SK_COLORTYPE_RGBA_1010102,
    SK_COLORTYPE_BGRA_1010102,
    SK_COLORTYPE_RGB_101010X,
    SK_COLORTYPE_BGR_101010X,
    SK_COLORTYPE_GRAY_8,
    SK_COLORTYPE_RGBA_F16_NORM,
    SK_COLORTYPE_RGBA_F16,
    SK_COLORTYPE_RGBA_F32,
} sk_colortype_t;

typedef enum {
    SK_ALPHATYPE_UNKNOWN,
    SK_ALPHATYPE_OPAQUE,
    SK_ALPHATYPE_PREMUL,
    SK_ALPHATYPE_UNPREMUL,
} sk_alphatype_t;

// Matches native libSkiaSharp.dll memory layout exactly (colorspace pointer at offset 0)
typedef struct {
    void* colorspace;
    int32_t width;
    int32_t height;
    int32_t colorType;
    int32_t alphaType;
} sk_imageinfo_t;

typedef struct {
    float left;
    float top;
    float right;
    float bottom;
} sk_rect_t;

typedef struct {
    float scaleX, skewX, transX;
    float skewY, scaleY, transY;
    float persp0, persp1, persp2;
} sk_matrix_t;

typedef enum {
    SK_PAINT_STYLE_FILL = 0,
    SK_PAINT_STYLE_STROKE = 1,
    SK_PAINT_STYLE_STROKE_AND_FILL = 2,
} sk_paint_style_t;

typedef enum {
    SK_TEXT_ENCODING_UTF8 = 0,
    SK_TEXT_ENCODING_UTF16 = 1,
    SK_TEXT_ENCODING_UTF32 = 2,
    SK_TEXT_ENCODING_GLYPH_ID = 3,
} sk_text_encoding_t;

// Surface & Canvas
sk_surface_t* sk_surface_new_raster_direct(
    const sk_imageinfo_t* info, 
    void* pixels, 
    size_t rowBytes, 
    sk_surface_raster_release_proc releaseProc, 
    void* context, 
    const sk_surfaceprops_t* props);

void sk_surface_unref(sk_surface_t* surface);
sk_canvas_t* sk_surface_get_canvas(sk_surface_t* surface);
void sk_canvas_flush(sk_canvas_t* canvas);
void sk_canvas_clear(sk_canvas_t* canvas, sk_color_t color);
int sk_canvas_save(sk_canvas_t* canvas);
void sk_canvas_restore(sk_canvas_t* canvas);
void sk_canvas_translate(sk_canvas_t* canvas, float dx, float dy);
void sk_canvas_scale(sk_canvas_t* canvas, float sx, float sy);
void sk_canvas_concat(sk_canvas_t* canvas, const sk_matrix_t* matrix);

// Shapes
void sk_canvas_draw_rect(sk_canvas_t* canvas, const sk_rect_t* rect, const sk_paint_t* paint);
void sk_canvas_draw_circle(sk_canvas_t* canvas, float cx, float cy, float rad, const sk_paint_t* paint);
void sk_canvas_draw_rrect(sk_canvas_t* canvas, const sk_rrect_t* rrect, const sk_paint_t* paint);

// Rounded rect
sk_rrect_t* sk_rrect_new(void);
void sk_rrect_delete(sk_rrect_t* rrect);
void sk_rrect_set_rect_xy(sk_rrect_t* rrect, const sk_rect_t* rect, float xRad, float yRad);

// Paint
sk_paint_t* sk_paint_new(void);
void sk_paint_delete(sk_paint_t* paint);
void sk_paint_set_color(sk_paint_t* paint, sk_color_t color);
void sk_paint_set_style(sk_paint_t* paint, sk_paint_style_t style);
void sk_paint_set_stroke_width(sk_paint_t* paint, float width);
void sk_paint_set_antialias(sk_paint_t* paint, bool antialias);

// Font & Text
sk_font_t* sk_font_new(void);
void sk_font_delete(sk_font_t* font);
void sk_font_set_size(sk_font_t* font, float size);
void sk_font_set_typeface(sk_font_t* font, sk_typeface_t* typeface);
sk_typeface_t* sk_typeface_create_default(void);
sk_typeface_t* sk_typeface_create_from_name(const char* familyName, const int* style);
void sk_typeface_unref(sk_typeface_t* typeface);
void sk_canvas_draw_simple_text(
    sk_canvas_t* canvas, 
    const void* text, 
    size_t byteLength, 
    sk_text_encoding_t encoding, 
    float x, 
    float y, 
    const sk_font_t* font, 
    const sk_paint_t* paint);

// Context
void gr_direct_context_flush_and_submit(gr_direct_context_t* context, bool sync);

#ifdef __cplusplus
}
#endif
