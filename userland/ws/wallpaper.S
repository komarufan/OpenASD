/*
 * wallpaper.S — embeds the desktop wallpaper (raw 1280x800 XRGB8888 pixels,
 * one uint32_t per pixel, byte order matches the framebuffer / ws color
 * convention) directly into the ws binary via .incbin.
 *
 * wallpaper.bin is raw little-endian uint32 pixels (B,G,R,0 bytes = 0x00RRGGBB),
 * one per pixel, row-major at the framebuffer resolution (1280x800).  To swap
 * the wallpaper, regenerate that blob from an image and rebuild ws.
 */
        .section __TEXT,__const
        .globl  _wallpaper_data
        .p2align 2
_wallpaper_data:
        .incbin "wallpaper.bin"
