/*
 * zz_app_text.c - marks where the apps' code starts (NovaOS 2.5).
 *
 * The Makefile links the kernel's objects first (kernel/ sorted by path, this
 * file last) and the apps (apps/) after them, so every function after
 * kernel_text_end() up to the end of .text is app code. The file protection in
 * kernel/fs/vfs.c uses it to tell a call from an app (the Terminal, Files,
 * Notepad ...) from a call by NovaOS itself. Keep the name sorting last in kernel/.
 */
#include "kernel.h"

void kernel_text_end(void);
void kernel_text_end(void) { __asm__ volatile(""); }
