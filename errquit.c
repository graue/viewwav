#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <allegro.h>

void errquit(const char *msg, ...)
{
	char str[1100];
	va_list argptr;

	va_start(argptr, msg);
	vsnprintf(str, sizeof str, msg, argptr);
	va_end(argptr);

	if (screen != NULL)
		set_gfx_mode(GFX_TEXT, 0, 0, 0, 0);
	allegro_message(str);
	exit(EXIT_FAILURE);
}
