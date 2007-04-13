#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <stdint.h>
#ifdef WIN32
#define _REST(ms) rest(ms)
#else
#include <unistd.h>
#define _REST(ms) usleep(1000*ms)
#endif
#include <float.h> /* XXX: for DBL_EPSILON but dunno if non-BSD OSes have it */
#include <allegro.h>
#include "readfile.h"
#include "errquit.h"

#define SCRWIDTH 640
#define SCRHEIGHT 480
#define RATE 44100

#define READKEY(val,ascii) do {		\
	while (!keypressed())		\
		_REST(12);		\
	val = readkey();		\
	ascii = val & 0xff;		\
	val >>= 8;			\
} while(0)

static int16_t *samples;
static int numsamples;
static int zoom = 1; /* Number of samples in each pixel. */
static int pos = 0; /* Sample value at leftmost pixel. */
static int logdisp = 0; /* Use logarithmic display? */
static BITMAP *buffer;

enum
{
	BLACK = 0, BLUE, GREEN, CYAN, RED, MAGENTA, BROWN,
	LIGHT_GRAY, GRAY, LIGHT_BLUE, LIGHT_GREEN, LIGHT_CYAN,
	LIGHT_RED, LIGHT_MAGENTA, YELLOW, WHITE
};

#define SCREEN_BG GRAY
#define CHANNEL_BG BLACK
#define CHANNEL_PEAK_COLOR LIGHT_GREEN
#define CHANNEL_RMS_COLOR CYAN
#define CHANNEL_DCLINE LIGHT_GRAY
#define MARKER_FG WHITE
#define MARKER_TEXT WHITE

#define RMS_MIN_SAMPLES ((int)(RATE * 0.001))

/*
 * Get the minimum and maximum of num samples, starting at start.
 * odd == 1 means get right-channel samples, otherwise left.
 */
static void getminmax(int odd, int start, int num, int *pmin, int *pmax)
{
	int min = 32768, max = -32767;
	int ix;
	int samp[2];

#define GETSAMP(n,s)				\
	if ((n) >= numsamples)			\
		(s) = 0;			\
	else					\
		(s) = samples[(n)*2 + odd];

	if (num == 0)
	{
		*pmin = *pmax = 0;
		return;
	}

	if (num & 1)
	{
		GETSAMP(start, min)
		max = min;
		start++, num--;
	}

	for (ix = 0; ix < num; ix += 2)
	{
		GETSAMP(start+ix, samp[0])
		GETSAMP(start+ix+1, samp[1])
		if (samp[1] < samp[0])
		{
			if (samp[1] < min)
				min = samp[1];
			if (samp[0] > max)
				max = samp[0];
		}
		else
		{
			if (samp[0] < min)
				min = samp[0];
			if (samp[1] > max)
				max = samp[1];
		}
	}

	*pmin = min;
	*pmax = max;
}

/* Calculate the RMS average and return it. */
static double calcrms(int odd, int start, int num)
{
	int ix;
	int samp;
	double total = 0.0;

	for (ix = 0; ix < num; ix++)
	{
		double f;

		GETSAMP(start+ix, samp)
		f = (samp / 32768.0);
		total += f * f;
	}
	return sqrt(total / num);
}

static int lastpeaky1, lastpeaky2;
static int lastrmsy1, lastrmsy2;
static int chan_i;

/* Draw a column of a channel of audio. */
static void drawcolumn(int x, int top, int height, int min, int max, int rms)
{
	int y1, y2;
	const int lasty1 = rms ? lastrmsy1 : lastpeaky1;
	const int lasty2 = rms ? lastrmsy2 : lastpeaky2;

	if (!logdisp) /* Linear display. */
	{
		int ycenter = top + height/2;
		y1 = ycenter - (max * height/2 / 32768);
		y2 = ycenter - (min * height/2 / 32768);
	}
	else /* Logarithmic display. */
	{
		double maxdb, mindb;

		/* Avoid negative infinity when taking logs. */
		if (max == 0) max = 1;
		if (min == 0) min = 1;

		maxdb = 20.0 * log10((double)abs(max) / 32768.0);
		mindb = 20.0 * log10((double)abs(min) / 32768.0);

		/*
		 * XXX is this clamping necessary? should it be?
		 * Note: 96 dB is maximum dynamic range of 16-bit
		 * samples, in theory.
		 */
		if (maxdb > 0.0) maxdb = 0.0;
		if (mindb < -96.0) mindb = -96.0;

		y1 = (int)(top - mindb * height / 96.0);
		y2 = (int)(top - maxdb * height / 96.0);
	}

	/* Make y1 the one on top. */
	if (y1 > y2)
	{
		const int tmp = y2;
		y2 = y1;
		y1 = tmp;
	}

	/*
	 * Connect this line to the last line, to avoid the
	 * "scatterplot" look when zoomed in.
	 */
	if (chan_i > 0)
	{
		if (y2 < lasty1 - 1)
			y2 = lasty1 - 1;
		else if (y1 > lasty2 + 1)
			y1 = lasty2 + 1;
	}

	vline(buffer, x, y1, y2, rms ? CHANNEL_RMS_COLOR : CHANNEL_PEAK_COLOR);

	if (rms)
	{
		lastrmsy1 = y1;
		lastrmsy2 = y2;
	}
	else
	{
		lastpeaky1 = y1;
		lastpeaky2 = y2;
	}
}

/*
 * Draw a channel of audio.
 * top = topmost pixel, height = height, left = leftmost pixel,
 * wzoom = number of samples in 1 pixel column, cols = number of columns,
 * odd = use odd (1) or even (0) samples, start = starting sample number.
 */
static void drawchannel(int top, int height, int left, int wzoom, int cols,
	int odd, int start)
{
	int max, min;
	int skiprms;

	if (!(height & 1)) height--; /* force an odd height */

	lastpeaky1 = lastpeaky2 = lastrmsy1 = lastrmsy2 = top + height/2;

	/* If there are too few samples for each column, skip RMS. */
	skiprms = wzoom < RMS_MIN_SAMPLES;

	rectfill(buffer, left, top, left + cols - 1, top + height - 1,
		CHANNEL_BG);

	if (!logdisp)
	{
		hline(buffer, left, top + height/2, left + cols - 1,
			CHANNEL_DCLINE);
	}

	for (chan_i = 0; chan_i < cols; chan_i++)
	{
		double rmsval;
		int rmsint;

		getminmax(odd, start + chan_i*wzoom, wzoom, &min, &max);
		drawcolumn(chan_i + left, top, height, min, max, 0);

		if (!skiprms)
		{
			rmsval = calcrms(odd, start + chan_i*wzoom, wzoom);
			rmsint = (int)(32768.0 * rmsval);
			drawcolumn(chan_i + left, top, height,
				-rmsint, rmsint, 1);
		}
	}
}

static const char *makemarker(double timepos, double interval)
{
	int decimals;
	double n;
	static char buf[20];

	/*
	 * Count the number of decimals in this interval value:
	 * 1.0    : 0
	 * 0.1    : 1
	 * 0.01   : 2
	 * 0.001  : 3
	 * 0.0001 : 4, etc.
	 */
	for (n = interval, decimals = 0; n < 1.0; n *= 10, decimals++)
		;

	snprintf(buf, sizeof buf, "%.*f", decimals, timepos);
	return buf;
}

#define FONTHEIGHT 8
#define FONTWIDTH 8

static void drawtimemarkers(int top, int left, int width, int start, int num)
{
	double totaltime;
	double markerinterval = 1000.0;
	double secsperpixel;
	double t;
	double startsecs, endsecs; /* start and end of visible part, in secs */

	totaltime = (double)num / RATE; /* time in seconds */
	secsperpixel = totaltime / width;
	startsecs = (double)start / RATE;
	endsecs = (double)(start + num) / RATE;

	/*
	 * Divide markerinterval by 10 as long as there's still room
	 * for each marker without overlapping the next, or getting too close.
	 */
	while ((1+strlen(makemarker(endsecs, markerinterval/10))) * FONTWIDTH
		< markerinterval/10/secsperpixel)
	{
		markerinterval /= 10;
	}

	/* Find the first marker interval, where it should be. */
	t = (double)start / RATE;
	if (fmod(t, markerinterval) > DBL_EPSILON)
		t += markerinterval - fmod(t, markerinterval);
	for (; t < endsecs; t += markerinterval)
	{
		int x = (int)((t - startsecs)/secsperpixel) + left;
		vline(buffer, x, top - FONTHEIGHT/4, top + 3*FONTHEIGHT/4,
			MARKER_FG);
		textout_ex(buffer, font, makemarker(t, markerinterval),
			x + 2, top, MARKER_TEXT, -1);
	}
}

static void draw(void)
{
	int ch;

	clear_to_color(buffer, SCREEN_BG);

	for (ch = 0; ch < 2; ch++)
	{
		drawchannel(ch * SCRHEIGHT/2, SCRHEIGHT/2 - FONTHEIGHT,
			0, zoom, SCRWIDTH, ch, pos);
		drawtimemarkers((ch+1) * SCRHEIGHT/2 - FONTHEIGHT, 0,
			SCRWIDTH, pos, zoom*SCRWIDTH);
	}

	scare_mouse();
	vsync();
	blit(buffer, screen, 0, 0, 0, 0, SCRWIDTH, SCRHEIGHT);
	unscare_mouse();
}

/* How many presses of an arrow key it takes to move a whole screen. */
#define SCREEN_INTERVAL 10

/* Return 1 to quit. */
static int cycle(void)
{
	int keyascii, keyval;

	draw();
	READKEY(keyval, keyascii);
	if (keyval == KEY_PGUP)
		pos -= SCRWIDTH * zoom;
	else if (keyval == KEY_PGDN)
		pos += SCRWIDTH * zoom;
	else if (keyval == KEY_LEFT)
		pos -= SCRWIDTH * zoom / SCREEN_INTERVAL;
	else if (keyval == KEY_RIGHT)
		pos += SCRWIDTH * zoom / SCREEN_INTERVAL;
	else if (keyval == KEY_HOME)
		pos = 0;
	else if (keyval == KEY_END)
		pos = numsamples - SCRWIDTH*zoom;
	else if (keyval == KEY_UP) /* zoom in */
	{
		pos += SCRWIDTH*zoom/2;
		zoom /= 2;
		if (zoom < 1)
			zoom = 1;
		pos -= SCRWIDTH*zoom/2;
	}
	else if (keyval == KEY_DOWN) /* zoom out */
	{
		pos += SCRWIDTH*zoom/2;
		zoom *= 2;
		if (zoom > numsamples/SCRWIDTH)
			zoom = numsamples/SCRWIDTH;
		pos -= SCRWIDTH*zoom/2;
	}
	else if (keyascii == 'l' || keyascii == 'L') /* toggle log view */
		logdisp = !logdisp;
	else if (keyval == KEY_ESC) /* quit */
		return 1;

	if (pos > numsamples - SCRWIDTH*zoom)
		pos = numsamples - SCRWIDTH*zoom;
	if (pos < 0)
		pos = 0;

	return 0;
}

int main(int argc, char *argv[])
{
	void *data;
	int datalen;
	FILE *fp;

	if (allegro_init() != 0)
	{
		fprintf(stderr, "cannot initialize Allegro\n");
		exit(EXIT_FAILURE);
	}
	if (install_keyboard() != 0)
		errquit("can't install keyboard handler: %s", allegro_error);
	if (install_timer() != 0)
		errquit("can't install timers: %s", allegro_error);
	if (set_gfx_mode(GFX_AUTODETECT_WINDOWED, SCRWIDTH, SCRHEIGHT, 0, 0)
		!= 0)
	{
		errquit("can't set graphics mode: %s", allegro_error);
	}
	buffer = create_bitmap(SCRWIDTH, SCRHEIGHT);
	if (buffer == NULL)
		errquit("can't create buffer: %s", allegro_error);
	show_mouse(screen);

	if (argc != 2)
		errquit("incorrect usage; usage: viewwav file.wav");
	if ((fp = fopen(argv[1], "rb")) == NULL)
		errquit("cannot open %s", argv[1]);
	data = readfile(fp, &datalen);
	/* XXX todo: swap if big endian */
	fclose(fp);

	samples = data;
	numsamples = datalen / (2 * sizeof (int16_t));
	while (!cycle())
		;
	return 0;
}
