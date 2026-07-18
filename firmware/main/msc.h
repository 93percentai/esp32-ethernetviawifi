#pragma once

/*
 * The MSC class callbacks (tud_msc_*) live in msc.c and are only referenced by
 * the TinyUSB library. Calling msc_register() from app startup forces msc.c.obj
 * to be pulled from the static archive so those callbacks are linked in.
 */
void msc_register(void);

/* Call after SD ownership changes so the USB host re-reads capacity/contents. */
void msc_notify_media_changed(void);
