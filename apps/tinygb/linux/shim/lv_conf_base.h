/**
 * lv_conf.h — LVGL v9 configuration for iPod nano 7 homebrew.
 *
 * Tuned for a freestanding ARM Cortex-A8 build with no libc:
 *   - LVGL's built-in TLSF malloc, string and sprintf are enabled so we
 *     don't depend on the C runtime.
 *   - 32 bpp internal (XRGB8888 — see LV_COLOR_DEPTH below). The flush
 *     callback drops the X byte and packs RGB888 into the MIPI long-
 *     write payload (panel is configured 24bpp by the OS at boot).
 *   - Partial render mode, 16 KB draw buffer parked at a fixed high-
 *     RAM address (hb_lvgl.c HB_LVGL_BUF_ADDR), so a single app fits
 *     LVGL + its widgets in the ~256 KB load window.
 *   - Default theme on (gives the nice flat-modern look) but with a
 *     curated subset of widgets to keep binary size manageable.
 *
 * Apps opt into LVGL with LVGL_ENABLE=1 in their Makefile, see
 * sdk/hb_app.mk.
 */

#ifndef LV_CONF_H
#define LV_CONF_H

/*====================
   COLOR SETTINGS
 *====================*/

/* 32 bpp internal (XRGB8888). 4 bytes per pixel doubles the draw
   buffer cost vs RGB565, but eliminates the visible banding on
   gradients and the gradient-cards we paint on N7G's panel are
   the most obvious visual win. Flush callback writes XRGB8888
   directly into the GPU aligned buffer.
   We tried LV_COLOR_DEPTH=16 + NEON blend paths — it panicked the
   device on launch (likely NEON register state across OS re-entry).
   Revisit when we have an GPU-2D LVGL draw backend. */
#define LV_COLOR_DEPTH 32

/*=========================
   STDLIB WRAPPER SETTINGS
 *=========================*/

#define LV_USE_STDLIB_MALLOC    LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_STRING    LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_SPRINTF   LV_STDLIB_BUILTIN

#define LV_STDINT_INCLUDE       <stdint.h>
#define LV_STDDEF_INCLUDE       <stddef.h>
#define LV_STDBOOL_INCLUDE      <stdbool.h>
#define LV_INTTYPES_INCLUDE     "hb_lvgl_inttypes.h"
#define LV_LIMITS_INCLUDE       "hb_lvgl_limits.h"
#define LV_STDARG_INCLUDE       <stdarg.h>

#if LV_USE_STDLIB_MALLOC == LV_STDLIB_BUILTIN
    /* 64 KB TLSF pool placed at high-RAM 0x09190000 on device (see
       hb_app.ld / hb_lvgl.c). Keeping it out of .bss means the app
       .bin doesn't carry 64 KB of zero-fill that would otherwise
       overwrite OS .text during SCSI upload. */
    #ifdef HB_LV_RELOC
        /* Relocatable LVGL surface blob (operator-new arena, hb_reloc): the TLSF
           pool is LVGL's OWN static work_mem array (LV_MEM_ADR=0), so it lands in
           the image's .bss, part of the SAME arena hb_reloc allocates. One
           contiguous OS-heap block, nothing separate to overlap. .bss is NOLOAD,
           so the 640 KB pool doesn't bloat the .hbapp. This is why we run a STOCK
           upstream LVGL (no fork patch needed): the pool is configured purely
           through this header. */
        #define LV_MEM_SIZE (640 * 1024U)
        #define LV_MEM_ADR 0
    #else
        /* Pool lives at 0x09140000..0x091E0000 = 640 KB, about 1800 widgets
           safe at roughly 350 B each. */
        #define LV_MEM_SIZE (640 * 1024U)
        #define LV_MEM_ADR 0x09140000
    #endif
    #define LV_MEM_POOL_EXPAND_SIZE 0
#endif

/*====================
   HAL SETTINGS
 *====================*/

#define LV_DEF_REFR_PERIOD  8           /* ~120 Hz cap — we have GPU-fast flips,
                                           let animations run at the panel
                                           refresh rather than be throttled */
#define LV_DPI_DEF          150         /* iPod nano 7 ≈ 200 dpi, but 150 keeps widgets compact */

/*=========================
 * OPERATING SYSTEM
 *=========================*/

#define LV_USE_OS   LV_OS_NONE

/*========================
 * RENDERING CONFIGURATION
 *========================*/

#define LV_DRAW_BUF_STRIDE_ALIGN                1
#define LV_DRAW_BUF_ALIGN                       4
#define LV_DRAW_TRANSFORM_USE_MATRIX            0
#define LV_DRAW_LAYER_SIMPLE_BUF_SIZE           (64 * 1024)
                                                /* LVGL benchmark flagged 16 KB as too small for opa_layered
                                                 * (need >31 KB for the 240×432 panel). 64 KB allocates from the
                                                 * 640 KB TLSF pool on demand for layered/opacity composites. */
#define LV_DRAW_LAYER_MAX_MEMORY                0
#define LV_DRAW_THREAD_STACK_SIZE               (8 * 1024)

#define LV_USE_DRAW_SW                          1
#if LV_USE_DRAW_SW == 1
    #define LV_DRAW_SW_SUPPORT_RGB565           1
    #define LV_DRAW_SW_SUPPORT_RGB565_SWAPPED   0
    #define LV_DRAW_SW_SUPPORT_RGB565A8         0
    #define LV_DRAW_SW_SUPPORT_RGB888           0
    #define LV_DRAW_SW_SUPPORT_XRGB8888         1
    #define LV_DRAW_SW_SUPPORT_ARGB8888         0
    #define LV_DRAW_SW_SUPPORT_ARGB8888_PREMULTIPLIED 0
    #define LV_DRAW_SW_SUPPORT_L8               0
    #define LV_DRAW_SW_SUPPORT_AL88             0
    #define LV_DRAW_SW_SUPPORT_A8               1
    #define LV_DRAW_SW_SUPPORT_I1               0

    #define LV_DRAW_SW_DRAW_UNIT_CNT            1
    #define LV_USE_DRAW_SW_ASM                  LV_DRAW_SW_ASM_NONE
    #define LV_USE_DRAW_SW_COMPLEX_GRADIENTS    0
#endif

#define LV_USE_NATIVE_HELIUM_ASM    0

/*=======================
 * FEATURE CONFIGURATION
 *=======================*/

#define LV_USE_OBJ_PROPERTY                 0
#define LV_USE_OBJ_PROPERTY_NAME            0
#define LV_USE_OBJ_NAME                     0
#define LV_USE_OBJ_ID                       0
#define LV_USE_OBJ_ID_BUILTIN               0

#define LV_VG_LITE_USE_BOX_SHADOW           0
#define LV_VG_LITE_GRAD_CACHE_CNT           0
#define LV_VG_LITE_STROKE_CACHE_CNT         0

/*-------------
 * Logging
 *-----------*/
#define LV_USE_LOG      0

/*-------------
 * Asserts
 *-----------*/
#define LV_USE_ASSERT_NULL          0
#define LV_USE_ASSERT_MALLOC        0
#define LV_USE_ASSERT_STYLE         0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_USE_ASSERT_OBJ           0
#define LV_ASSERT_HANDLER_INCLUDE   <stdint.h>
#define LV_ASSERT_HANDLER           while(1);

/*-------------
 * Debug
 *-----------*/
#define LV_USE_REFR_DEBUG            0
#define LV_USE_LAYER_DEBUG           0
#define LV_USE_PARALLEL_DRAW_DEBUG   0

/*-------------
 * Others
 *-----------*/
#define LV_ENABLE_GLOBAL_CUSTOM     0
#if LV_ENABLE_GLOBAL_CUSTOM
    #define LV_GLOBAL_CUSTOM_INCLUDE <stdint.h>
#endif

#define LV_CACHE_DEF_SIZE                       0
#define LV_IMAGE_HEADER_CACHE_DEF_CNT           0
#define LV_GRADIENT_MAX_STOPS                   2
#define LV_COLOR_MIX_ROUND_OFS                  0
#define LV_OBJ_STYLE_CACHE                      0
#define LV_USE_OBJ_TYPE_CACHE                   0
#define LV_USE_OBJ_TYPE_INHERITANCE_CACHE       0

/*=====================
 *  COMPILER SETTINGS
 *====================*/

#define LV_BIG_ENDIAN_SYSTEM        0
#define LV_ATTRIBUTE_TICK_INC
#define LV_ATTRIBUTE_TIMER_HANDLER
#define LV_ATTRIBUTE_FLUSH_READY
#define LV_ATTRIBUTE_MEM_ALIGN_SIZE 4
#define LV_ATTRIBUTE_MEM_ALIGN
#define LV_ATTRIBUTE_LARGE_CONST
#define LV_ATTRIBUTE_LARGE_RAM_ARRAY
#define LV_ATTRIBUTE_FAST_MEM
#define LV_EXPORT_CONST_INT(int_value)  struct _silence_gcc_warning

#define LV_USE_FLOAT 0
#define LV_USE_MATRIX 0
#define LV_USE_PRIVATE_API 0

/*==================
 *   FONT USAGE
 *===================*/

#define LV_FONT_MONTSERRAT_8    0
#define LV_FONT_MONTSERRAT_10   0
#define LV_FONT_MONTSERRAT_12   0
#define LV_FONT_MONTSERRAT_14   1
#define LV_FONT_MONTSERRAT_16   1
#define LV_FONT_MONTSERRAT_18   0
#define LV_FONT_MONTSERRAT_20   1
#define LV_FONT_MONTSERRAT_22   0
#define LV_FONT_MONTSERRAT_24   1
#define LV_FONT_MONTSERRAT_26   1     /* used by lvgl_demos/benchmark */
#define LV_FONT_MONTSERRAT_28   1
#define LV_FONT_MONTSERRAT_30   0
#define LV_FONT_MONTSERRAT_32   0
#define LV_FONT_MONTSERRAT_34   0
#define LV_FONT_MONTSERRAT_36   1
#define LV_FONT_MONTSERRAT_38   0
#define LV_FONT_MONTSERRAT_40   0
#define LV_FONT_MONTSERRAT_42   0
#define LV_FONT_MONTSERRAT_44   0
#define LV_FONT_MONTSERRAT_46   0
#define LV_FONT_MONTSERRAT_48   1
#define LV_FONT_MONTSERRAT_28_COMPRESSED 0
#define LV_FONT_DEJAVU_16_PERSIAN_HEBREW 0
#define LV_FONT_SIMSUN_14_CJK            0
#define LV_FONT_SIMSUN_16_CJK            0
#define LV_FONT_SOURCE_HAN_SANS_SC_14_CJK 0
#define LV_FONT_UNSCII_8        0
#define LV_FONT_UNSCII_16       0
#define LV_FONT_CUSTOM_DECLARE

#define LV_FONT_DEFAULT &lv_font_montserrat_16
#define LV_FONT_FMT_TXT_LARGE   0
#define LV_USE_FONT_COMPRESSED  0
#define LV_USE_FONT_PLACEHOLDER 1

/*=================
 *  TEXT SETTINGS
 *=================*/

#define LV_TXT_ENC LV_TXT_ENC_UTF8
#define LV_TXT_BREAK_CHARS " ,.;:-_)]}"
#define LV_TXT_LINE_BREAK_LONG_LEN  0
#define LV_TXT_LINE_BREAK_LONG_PRE_MIN_LEN  3
#define LV_TXT_LINE_BREAK_LONG_POST_MIN_LEN 3
#define LV_TXT_COLOR_CMD "#"
#define LV_USE_BIDI 0
#if LV_USE_BIDI
    #define LV_BIDI_BASE_DIR_DEF LV_BASE_DIR_AUTO
#endif
#define LV_USE_ARABIC_PERSIAN_CHARS 0

/*==================
 * WIDGETS
 *==================*/

#define LV_WIDGETS_HAS_DEFAULT_VALUE 1

#define LV_USE_ANIMIMG    1     /* used by lvgl_demos/widgets */
#define LV_USE_ARC        1
#define LV_USE_ARCLABEL   0
#define LV_USE_BAR        1
#define LV_USE_BUTTON     1
#define LV_USE_BUTTONMATRIX 1
#define LV_USE_CALENDAR   1     /* used by lvgl_demos/{stress,keypad_encoder} */
#define LV_USE_CANVAS     1
#define LV_USE_CHART      1     /* used by lvgl_demos/{widgets,benchmark} */
#define LV_USE_CHECKBOX   1
#define LV_USE_DROPDOWN   1     /* used by lvgl_demos/widgets */
#define LV_USE_IMAGE      1
#define LV_USE_IMAGEBUTTON 0
#define LV_USE_KEYBOARD   1
#define LV_USE_LABEL      1
#if LV_USE_LABEL
    #define LV_LABEL_TEXT_SELECTION 0
    #define LV_LABEL_LONG_TXT_HINT 1
    #define LV_LABEL_WAIT_CHAR_COUNT 3
#endif
#define LV_USE_LED        0
#define LV_USE_LINE       1
#define LV_USE_LIST       1
#define LV_USE_LOTTIE     0
#define LV_USE_MENU       0
#define LV_USE_MSGBOX     1     /* used by lvgl_demos/stress */
#define LV_USE_ROLLER     1
#define LV_USE_SCALE      1     /* used by lvgl_demos/widgets */
#define LV_USE_SLIDER     1
#define LV_USE_SPAN       1     /* used by lvgl_demos/widgets */
#define LV_USE_SPINBOX    1     /* used by lvgl_demos/keypad_encoder */
#define LV_USE_SPINNER    1
#define LV_USE_SWITCH     1
#define LV_USE_TABLE      1     /* used by lvgl_demos/benchmark */
#define LV_USE_TABVIEW    1
#define LV_USE_TEXTAREA   1
#if LV_USE_TEXTAREA != 0
    #define LV_TEXTAREA_DEF_PWD_SHOW_TIME 1500
#endif
#define LV_USE_TILEVIEW   1
#define LV_USE_WIN        1     /* used by lvgl_demos/stress */
#define LV_USE_3DTEXTURE  0

/*==================
 * THEMES
 *==================*/

/* Default theme is bigger (~6 KB code) but gives us the modern flat
   look with shadows / rounded corners; simple theme is plain rects. */
#define LV_USE_THEME_DEFAULT 1
#if LV_USE_THEME_DEFAULT
    #define LV_THEME_DEFAULT_DARK 1
    #define LV_THEME_DEFAULT_GROW 0
    #define LV_THEME_DEFAULT_TRANSITION_TIME 0
#endif

#define LV_USE_THEME_SIMPLE 1
#define LV_USE_THEME_MONO   0

/*==================
 * LAYOUTS
 *==================*/

#define LV_USE_FLEX 1
#define LV_USE_GRID 1

/*====================
 * 3rd party libraries
 *====================*/

#define LV_FS_DEFAULT_DRIVER_LETTER '\0'
#define LV_USE_FS_STDIO 0
#define LV_USE_FS_POSIX 0
#define LV_USE_FS_WIN32 0
#define LV_USE_FS_FATFS 0
#define LV_USE_FS_MEMFS 0
#define LV_USE_FS_LITTLEFS 0
#define LV_USE_FS_ARDUINO_ESP_LITTLEFS 0
#define LV_USE_FS_ARDUINO_SD 0
#define LV_USE_FS_UEFI 0
#define LV_USE_LZ4_INTERNAL  0
#define LV_USE_LZ4_EXTERNAL  0
#define LV_USE_SVG          0
#define LV_USE_LODEPNG      0
#define LV_USE_LIBPNG       0
#define LV_USE_BMP          0
#define LV_USE_TJPGD        0
#define LV_USE_LIBJPEG_TURBO 0
#define LV_USE_GIF          0
#define LV_USE_QRCODE       1
#define LV_USE_BARCODE      1
#define LV_USE_FREETYPE     0
#define LV_USE_TINY_TTF     0
#define LV_USE_RLOTTIE      0
#define LV_USE_VECTOR_GRAPHIC  0
#define LV_USE_THORVG_INTERNAL 0
#define LV_USE_THORVG_EXTERNAL 0
#define LV_USE_FFMPEG       0
#define LV_USE_PROPERTY_TRANSITION 0

/*==================
 * OTHERS
 *==================*/

#define LV_USE_SNAPSHOT     0
/* Perf instrumentation (sysmon perf backend + live FPS/CPU/mem overlays). OFF by
 * default — it registers per-frame display RENDER_START/READY handlers + a timer,
 * which add overhead and clutter; the lv_demo_benchmark also needs the sysmon
 * subject to report. Opt in per app with `LV_PERF := 1` in the app Makefile
 * (-DHB_LV_PERF; uses a separate LVGL archive so regular apps stay clean). Works
 * on the surface runtime (the 2nd-render fault was specific to the old blob). */
#ifdef HB_LV_PERF
#define LV_USE_SYSMON       1
#define LV_USE_PERF_MONITOR 1     /* live FPS/CPU overlay        */
#define LV_USE_MEM_MONITOR  1     /* TLSF heap-usage overlay     */
#else
#define LV_USE_SYSMON       0
#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR  0
#endif
#define LV_USE_PROFILER     0
#define LV_USE_MONKEY       0
#define LV_USE_GRIDNAV      0
#define LV_USE_FRAGMENT     0
#define LV_USE_IMGFONT      0
#define LV_USE_OBSERVER     1
#define LV_USE_IME_PINYIN   0
#define LV_USE_FILE_EXPLORER 0
#define LV_USE_FONT_MANAGER  0
#define LV_USE_TIMELINE      0
#define LV_USE_BIN_DECODER   1
#define LV_USE_XML           0
#define LV_USE_TRANSLATION   0
#define LV_USE_NUTTX         0
#define LV_USE_TEST          0

/*==================
 * DEVICES
 *==================*/
#define LV_USE_SDL              0
#define LV_USE_X11              0
#define LV_USE_WAYLAND          0
#define LV_USE_LINUX_FBDEV      0
#define LV_USE_LINUX_DRM        0
#define LV_USE_TFT_ESPI         0
#define LV_USE_ST7735           0
#define LV_USE_ST7789           0
#define LV_USE_ST7796           0
#define LV_USE_ILI9341          0
#define LV_USE_FT81X            0
#define LV_USE_GENERIC_MIPI     0
#define LV_USE_WINDOWS          0
#define LV_USE_UEFI             0
#define LV_USE_OPENGLES         0
#define LV_USE_NUTTX_LCD        0
#define LV_USE_RENESAS_GLCDC    0
#define LV_USE_ST_LTDC          0
#define LV_USE_QNX              0
#define LV_USE_EVDEV            0
#define LV_USE_LIBINPUT         0
#define LV_USE_XKB              0
#define LV_USE_NUTTX_TOUCHSCREEN 0
#define LV_USE_NUTTX_LIBUV      0
#define LV_USE_NUTTX_INDEV      0
#define LV_USE_GLFW3            0
#define LV_USE_EGL              0

/*==================
* EXAMPLES
*==================*/

#define LV_BUILD_EXAMPLES 0

/*==================
* DEMOS
*==================*/
#define LV_USE_DEMO_WIDGETS         1
#define LV_USE_DEMO_BENCHMARK       1
#define LV_USE_DEMO_STRESS          1
#define LV_USE_DEMO_KEYPAD_AND_ENCODER 1

#endif /* LV_CONF_H */
