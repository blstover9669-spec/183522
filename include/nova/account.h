/*
 * nova/account.h - the account (name, profile picture, password) and the start
 * screen (NovaOS 2.5).
 *
 *   #include <nova/account.h>       (not included by nova.h)
 *
 * NovaOS has one account. On an installed drive it is set up by the drive's
 * first start (the installation menu: account name, profile picture, optional
 * password) and changed in Settings > Account; it is kept on the drive in
 * /system/config/account (the password only as a salted SHA-256 hash, never as
 * text) and /system/config/account.bmp (a copy of a picture chosen from
 * Pictures). ISO starts (the live system) always use "Guest" with the person
 * icon and no password, and it cannot be changed there.
 *
 * The start screen (kernel/gui/startscreen.c) shows the account every time
 * NovaOS starts, before the desktop: the time and date slide down to the top
 * left, the account name is at the middle top with the picture under it, then
 * a Start button - or, with a password, a password box that must be typed.
 */
#ifndef NOVA_ACCOUNT_H
#define NOVA_ACCOUNT_H

#include <nova/types.h>
#include <nova/gfx.h>

#define ACCOUNT_NAME_MAX   32      /* characters of the account name            */
#define ACCOUNT_PASS_MAX   64      /* characters of a password                  */

/* Profile pictures: the person icon (default), the built-in pictures (all drawn by
   NovaOS code) and a picture chosen from Pictures. */
enum {
    ACCOUNT_PIC_PERSON = 0,        /* the classic person icon                   */
    ACCOUNT_PIC_STAR,
    ACCOUNT_PIC_MOUNTAINS,
    ACCOUNT_PIC_FLOWER,
    ACCOUNT_PIC_PLANET,
    ACCOUNT_PIC_CAT,
    ACCOUNT_PIC_BUILTIN,           /* number of built-in pictures (incl. person) */
    ACCOUNT_PIC_FILE = 100         /* a picture from Pictures (its copy)        */
};

/* errors (negative) */
#define ACCOUNT_OK          0
#define ACCOUNT_EGUEST     -1      /* ISO start: the Guest account can't change  */
#define ACCOUNT_ENAME      -2      /* empty or invalid name                      */
#define ACCOUNT_EWRONG     -3      /* the current password is wrong              */
#define ACCOUNT_EPICTURE   -4      /* the picture could not be opened            */
#define ACCOUNT_EIO        -5      /* writing the account file failed            */
#define ACCOUNT_EINVAL     -6

/* ---- the account (kernel/core/account.c) ------------------------------------------- */
bool        account_is_guest(void);        /* ISO start: "Guest", person icon, no password */
const char *account_name(void);
int         account_picture(void);         /* ACCOUNT_PIC_*                                */
bool        account_has_password(void);
bool        account_check_password(const char *password);
int         account_set_name(const char *name);
int         account_set_picture(int pic);  /* ACCOUNT_PIC_PERSON .. ACCOUNT_PIC_BUILTIN-1   */
/* A BMP/PPM picture (e.g. from /home/user/Pictures): a square copy is kept on the drive. */
int         account_set_picture_file(const char *path);
/* Set, change or remove (password NULL or "") the password; `current` must be the
   current password when one is set. */
int         account_set_password(const char *current, const char *password);
const char *account_strerror(int err);
/* A change counter: grows whenever the account changes (redraw what shows it). */
uint32_t    account_serial(void);

/* ---- pictures (kernel/gui/icons.c) ---------------------------------------------------- */
/* The account's picture, round, size x size at x,y (alpha blended). */
void        account_draw_picture(surface_t *s, int x, int y, int size);
/* A built-in picture (ACCOUNT_PIC_PERSON ...), round. */
void        account_draw_builtin(int pic, surface_t *s, int x, int y, int size);
/* Any picture surface, centre-cropped to a square and drawn round. */
void        account_draw_image(const surface_t *img, surface_t *s, int x, int y, int size);
const char *account_builtin_name(int pic);  /* "Person", "Star" ...                       */

/* ---- the start screen (kernel/gui/startscreen.c) ----------------------------------------- */
/* True while the start screen is shown or still to come at this start. */
bool        startscreen_pending(void);
/* Run fn(ctx) once NovaOS has opened - after the start screen, when the desktop shows
   (soon, from a timer, when there is no start screen at this start or it has passed). */
void        startscreen_when_done(void (*fn)(void *ctx), void *ctx);

#endif
