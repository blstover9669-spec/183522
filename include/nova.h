/*
 * nova.h - umbrella header for NovaOS applications.
 *
 *   #include <nova.h>
 *
 * gives you the whole public API: graphics, windows/events, widgets, theme,
 * filesystem, app registry, system services and the libc/libm subset.
 * See docs/API.md for the guide.
 */
#ifndef NOVA_H
#define NOVA_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>
#include <limits.h>

#include <nova/types.h>
#include <nova/gfx.h>
#include <nova/input.h>
#include <nova/wm.h>
#include <nova/ui.h>
#include <nova/theme.h>
#include <nova/app.h>
#include <nova/vfs.h>
#include <nova/sys.h>
#include <nova/icons.h>
#include <nova/shell.h>
#include <nova/service.h>

#endif
