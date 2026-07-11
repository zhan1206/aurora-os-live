/*
 * drm.h - Direct Rendering Manager / Kernel Mode Setting interface
 *
 * Provides framebuffer management, display mode setting, and basic
 * 2D rendering primitives for AuroraOS.
 *
 * Uses UEFI GOP framebuffer if available, otherwise falls back to
 * VGA text mode.
 */
#ifndef DRM_H
#define DRM_H

#include <stdint.h>
#include <stddef.h>

/* Maximum number of framebuffers, CRTCs, and connectors */
#define DRM_MAX_FBS        8
#define DRM_MAX_CRTCS      4
#define DRM_MAX_CONNECTORS 4
#define DRM_MAX_MODES     16

/* DRM ioctl command codes */
#define DRM_IOCTL_BASE              0x64
#define DRM_IOCTL_VERSION           (DRM_IOCTL_BASE + 0x00)
#define DRM_IOCTL_MODE_GETRESOURCES (DRM_IOCTL_BASE + 0x01)
#define DRM_IOCTL_MODE_GETCONNECTOR (DRM_IOCTL_BASE + 0x02)
#define DRM_IOCTL_MODE_SETCRTC      (DRM_IOCTL_BASE + 0x03)
#define DRM_IOCTL_MODE_PAGE_FLIP    (DRM_IOCTL_BASE + 0x04)
#define DRM_IOCTL_MODE_DIRTYFB      (DRM_IOCTL_BASE + 0x05)

/* Display mode flags */
#define DRM_MODE_FLAG_PHSYNC   (1 << 0)
#define DRM_MODE_FLAG_NHSYNC   (1 << 1)
#define DRM_MODE_FLAG_PVSYNC   (1 << 2)
#define DRM_MODE_FLAG_NVSYNC   (1 << 3)
#define DRM_MODE_FLAG_INTERLACE (1 << 4)

/* Connector types */
#define DRM_MODE_CONNECTOR_Unknown    0
#define DRM_MODE_CONNECTOR_VGA        1
#define DRM_MODE_CONNECTOR_DVII       2
#define DRM_MODE_CONNECTOR_DVID       3
#define DRM_MODE_CONNECTOR_HDMIA      4
#define DRM_MODE_CONNECTOR_HDMIB      5
#define DRM_MODE_CONNECTOR_eDP        6
#define DRM_MODE_CONNECTOR_DisplayPort 7
#define DRM_MODE_CONNECTOR_BuiltIn    8

/* Connector status */
#define DRM_MODE_CONNECTED        1
#define DRM_MODE_DISCONNECTED     2
#define DRM_MODE_UNKNOWNCONNECTION 3

/* ================================================================
 * Structures
 * ================================================================ */

/* Display mode (timing information) */
struct drm_mode {
    uint32_t clock;         /* pixel clock in kHz */
    uint16_t hdisplay;      /* horizontal visible area */
    uint16_t hsync_start;
    uint16_t hsync_end;
    uint16_t htotal;
    uint16_t vdisplay;      /* vertical visible area */
    uint16_t vsync_start;
    uint16_t vsync_end;
    uint16_t vtotal;
    uint32_t flags;         /* DRM_MODE_FLAG_* */
    char     name[32];      /* human-readable mode name */
};

/* Framebuffer */
struct drm_framebuffer {
    uint32_t fb_id;         /* unique framebuffer ID */
    uint32_t width;
    uint32_t height;
    uint32_t pitch;         /* bytes per line */
    uint32_t bpp;           /* bits per pixel */
    void    *buffer;        /* pixel data */
    size_t   size;          /* total buffer size in bytes */
    int      refcount;
};

/* CRTC (Cathode Ray Tube Controller) */
struct drm_crtc {
    uint32_t                crtc_id;
    struct drm_framebuffer *framebuffer;  /* current framebuffer */
    struct drm_mode        *mode;         /* current mode */
    int32_t                 x;            /* x offset */
    int32_t                 y;            /* y offset */
    int                     active;
};

/* Connector */
struct drm_connector {
    uint32_t         connector_id;
    uint32_t         connector_type;
    int              connected;
    struct drm_mode  modes[DRM_MAX_MODES];
    int              num_modes;
    struct drm_mode *preferred_mode;
    uint32_t         mm_width;
    uint32_t         mm_height;
};

/* DRM device */
struct drm_device {
    struct drm_framebuffer  framebuffers[DRM_MAX_FBS];
    int                     num_fbs;

    struct drm_crtc         crtcs[DRM_MAX_CRTCS];
    int                     num_crtcs;

    struct drm_connector    connectors[DRM_MAX_CONNECTORS];
    int                     num_connectors;

    /* Framebuffer info from UEFI GOP */
    void    *gop_fb;         /* physical framebuffer address */
    uint32_t gop_width;
    uint32_t gop_height;
    uint32_t gop_pitch;
    uint32_t gop_bpp;
    int      gop_available;

    /* Double buffering */
    void    *back_buffer;    /* back buffer for page flipping */
    int      using_double_buf;
};

/* ================================================================
 * Built-in font (8x16 bitmap)
 * ================================================================ */

/* DRM font glyph: 8 pixels wide, 16 pixels tall */
#define DRM_FONT_WIDTH   8
#define DRM_FONT_HEIGHT 16
#define DRM_FONT_GLYPHS 128

/* ================================================================
 * Public API
 * ================================================================ */

/* Initialize the DRM subsystem. Called during boot. */
void drm_init(void);

/* Initialize DRM with UEFI GOP framebuffer info. */
void drm_init_gop(void *fb_addr, uint32_t width, uint32_t height,
                  uint32_t pitch, uint32_t bpp);

/* Create a framebuffer. Returns fb_id or -1 on failure. */
int drm_fb_create(uint32_t width, uint32_t height, uint32_t bpp);

/* Destroy a framebuffer. */
int drm_fb_destroy(int fb_id);

/* Fill a rectangle on a framebuffer with a solid color. */
void drm_fb_fill_rect(int fb_id, int x, int y, int w, int h, uint32_t color);

/* Draw a character on a framebuffer using the built-in font. */
void drm_fb_draw_char(int fb_id, int x, int y, char c, uint32_t fg, uint32_t bg);

/* Draw a string on a framebuffer. */
void drm_fb_draw_text(int fb_id, int x, int y, const char *text,
                      uint32_t fg, uint32_t bg);

/* Present a framebuffer to the screen (double-buffer flip). */
void drm_fb_present(int fb_id);

/* Set the display mode on a CRTC. */
int drm_mode_set(int crtc_id, int fb_id, struct drm_mode *mode);

/* Detect connected displays. Returns number of connected connectors. */
int drm_connector_detect(void);

/* Get the DRM device for direct access. */
struct drm_device *drm_get_device(void);

/* Clear the entire screen. */
void drm_clear_screen(uint32_t color);

#endif /* DRM_H */