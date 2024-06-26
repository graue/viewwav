#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <stdint.h>
#ifdef WIN32
#define _REST(ms) rest(ms)
#else
#include <unistd.h>
#define _REST(ms) usleep(1000*ms)
#endif

#ifdef __OpenBSD__
#include <float.h> /* XXX: for DBL_EPSILON but dunno if non-BSD OSes have it */
#endif

#include <allegro.h>
#include "xm.h"
#include "readfile.h"
#include "errquit.h"
#include "binmode.h"

/* XXX */
#ifndef DBL_EPSILON
#define DBL_EPSILON 2.2204460492503131E-16
#endif

#undef MIN
#undef MAX
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define MAX(a,b) ((a) > (b) ? (a) : (b))

#define DEF_SCRWIDTH 800
#define DEF_SCRHEIGHT 600
#define DEF_RATE 44100

static int samprate = DEF_RATE;

#define READKEY(val,ascii) do {		\
	while (!keypressed())		\
		_REST(12);		\
	val = readkey();		\
	ascii = val & 0xff;		\
	val >>= 8;			\
} while(0)

static int scrwidth = DEF_SCRWIDTH;
static int scrheight = DEF_SCRHEIGHT;

static int16_t *samples;
static int numsamples; // Number of samples per channel.
static int zoom = 1; /* Number of samples in each pixel. */
static int vzoom = 0; /* Amplitude shown as 2^this times real amplitude. */
static int pos = 0; /* Sample value at leftmost pixel. */
static int logdisp = 0; /* Use logarithmic display? */
static int peakdisp = 1; /* Show peaks? */
static int rmsdisp = 0; /* Show RMS averages? */
static BITMAP *buffer;

#define VZOOM_MIN 0
#define VZOOM_MAX 15

/* Peak and RMS info about blocks. */
#define SAMPLES_PER_BLOCK 1024
static int numblocks = 0;
#define PEAK_FILLED_IN(chan) (1<<(2*(chan)))
#define SOS_FILLED_IN(chan) (1<<(2*(chan)+1))
typedef struct
{
	double sumofsquares[2];
	int max[2], min[2];
	unsigned char filledin; // The two flags OR'd together.
} block_t;
static block_t *blocks;

enum
{
	BLACK = 0, BLUE, GREEN, CYAN, RED, MAGENTA, BROWN,
	LIGHT_GRAY, GRAY, LIGHT_BLUE, LIGHT_GREEN, LIGHT_CYAN,
	LIGHT_RED, LIGHT_MAGENTA, YELLOW, WHITE
};

#define SCREEN_BG GRAY
#define CHANNEL_BG BLACK
#define CHANNEL_LOG_PEAK_COLOR LIGHT_GREEN
#define CHANNEL_LIN_PEAK_COLOR LIGHT_GREEN
#define CHANNEL_LOG_RMS_COLOR LIGHT_CYAN
#define CHANNEL_LIN_RMS_COLOR CYAN
#define CHANNEL_DCLINE_COLOR LIGHT_GRAY
#define CHANNEL_LOGGUIDE_COLOR_MAJOR (makecol(120, 120, 120))
#define CHANNEL_LOGGUIDE_COLOR_MINOR (makecol(92, 92, 92))
#define CHANNEL_LOGGUIDE_SPACING 6 // 6 dB between guide lines.
#define CHANNEL_LOGGUIDE_MAJOR_SPACING (2*CHANNEL_LOGGUIDE_SPACING)
#define MAX_DB_RANGE 96.0
#define MARKER_FG WHITE
#define MARKER_TEXT WHITE

#define RMS_MIN_SAMPLES(rate) ((int)(rate * 0.001))

#define GETSAMP(n,s,o)				\
	if ((n) >= numsamples)			\
		(s) = 0;			\
	else					\
		(s) = samples[(n)*2 + o];

#define SAMP_DIV_FLOAT 32768.0 // Divide by this to convert to a float.
#define MAXSAMP 32767
#define MINSAMP -32768

// Get the minimum and maximum of num samples, starting at start.
// odd == 1 means get right-channel samples, otherwise left.
// This "raw" version does not use blocks.
static void getminmax_raw(int odd, int start, int num, int *pmin, int *pmax)
{
	int min = MAXSAMP, max = MINSAMP;
	int ix;
	int samp[2];

	if (num == 0)
	{
		*pmin = *pmax = 0;
		return;
	}

	if (num & 1)
	{
		GETSAMP(start, min, odd)
		max = min;
		start++, num--;
	}

	for (ix = 0; ix < num; ix += 2)
	{
		GETSAMP(start+ix, samp[0], odd)
		GETSAMP(start+ix+1, samp[1], odd)
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

// Like getminmax_raw, but taking a start and end parameter for convenience.
static void getminmax_raw_se(int odd, int start, int end, int *pmin, int *pmax)
{
	getminmax_raw(odd, start, end - start + 1, pmin, pmax);
}

// Calculate the sum of squares of samples and return it,
// without using blocks to speed up the process.
static double calcsos_raw(int odd, int start, int num)
{
	int ix;
	int samp;
	double total = 0.0;
	const int end = start + num - 1;

	if (num <= 0)
		return 0.0;

	for (ix = start; ix <= end; ix++)
	{
		double f;

		GETSAMP(ix, samp, odd)
		f = samp / SAMP_DIV_FLOAT;
		total += f * f;
	}
	return total;
}

// Like calcsos_raw(), but takes a first and last sample value
// to consider, for convenience.
static double calcsos_raw_se(int odd, int start, int end)
{
	return calcsos_raw(odd, start, end - start + 1);
}

// Get the minimum and maximum of num samples, starting at start.
// odd == 1 means get right-channel samples, otherwise left.
static void getminmax(int odd, int start, int num, int *pmin, int *pmax)
{
	int min = MAXSAMP, max = MINSAMP;
	int block;
	int startblock, endblock;
	int end;
	int blockmin, blockmax;
	int tmpmin, tmpmax;

	if (start + num > numsamples)
		num = numsamples - start;
	if (num <= 0)
	{
		*pmin = *pmax = 0;
		return;
	}
	end = start + num - 1;

	startblock = start / SAMPLES_PER_BLOCK;
	endblock = (start + num - 1) / SAMPLES_PER_BLOCK;

	assert(startblock >= 0);
	assert(endblock < numblocks);
	assert((endblock+1)*SAMPLES_PER_BLOCK > end);

	for (block = startblock; block <= endblock; block++)
	{
		int blkfirst, blklast;

		blkfirst = block * SAMPLES_PER_BLOCK;
		blklast = (block+1) * SAMPLES_PER_BLOCK - 1;
		if (blklast >= numsamples)
			blklast = numsamples - 1;

		if (!(blocks[block].filledin & PEAK_FILLED_IN(odd)))
		{
			// Fill in the block, including the relevant samples.

			blockmin = MAXSAMP;
			blockmax = MINSAMP;
			if (blkfirst < start)
			{
				getminmax_raw_se(odd, blkfirst, start - 1,
					&blockmin, &blockmax);
			}
			if (blklast > end)
			{
				getminmax_raw_se(odd, end + 1, blklast,
					&tmpmin, &tmpmax);
				blockmin = MIN(blockmin, tmpmin);
				blockmax = MAX(blockmax, tmpmax);
			}
			getminmax_raw_se(odd, MAX(blkfirst, start),
				MIN(blklast, end), &tmpmin, &tmpmax);
			min = MIN(min, tmpmin);
			max = MAX(max, tmpmax);
			blockmin = MIN(blockmin, tmpmin);
			blockmax = MAX(blockmax, tmpmax);

			// Fill in the sum in the block.
			blocks[block].min[odd] = blockmin;
			blocks[block].max[odd] = blockmax;
			blocks[block].filledin |= PEAK_FILLED_IN(odd);
		}
		else if (blkfirst >= start && blklast <= end)
		{
			// The block is filled in and entirely contained
			// within the range of samples we're interested in.
			min = MIN(min, blocks[block].min[odd]);
			max = MAX(max, blocks[block].max[odd]);
		}
		else
		{
			// Block's filled in, but we can't use it.
			// We're not interested in all of it.
			getminmax_raw_se(odd, MAX(blkfirst, start),
				MIN(blklast, end), &tmpmin, &tmpmax);
			min = MIN(min, tmpmin);
			max = MAX(max, tmpmax);
		}
	}

	*pmin = min;
	*pmax = max;
}

// Calculate the sum of squares of samples and return it.
static double calcsos(int odd, int start, int num)
{
	double total = 0.0;
	int block;
	int startblock, endblock;
	int end;
	double blocktotal;

	if (start + num > numsamples)
		num = numsamples - start;
	if (num <= 0)
		return 0.0;
	end = start + num - 1;

	startblock = start / SAMPLES_PER_BLOCK;
	endblock = (start + num - 1) / SAMPLES_PER_BLOCK;

	assert(startblock >= 0);
	assert(endblock < numblocks);
	assert((endblock+1)*SAMPLES_PER_BLOCK > end);

	for (block = startblock; block <= endblock; block++)
	{
		int blkfirst, blklast;

		blkfirst = block * SAMPLES_PER_BLOCK;
		blklast = (block+1) * SAMPLES_PER_BLOCK - 1;
		if (blklast >= numsamples)
			blklast = numsamples - 1;

		if (!(blocks[block].filledin & SOS_FILLED_IN(odd)))
		{
			// Fill in the block, adding the relevant samples.

			double common;

			blocktotal = 0.0;
			if (blkfirst < start)
			{
				blocktotal += calcsos_raw_se(odd, blkfirst,
					start - 1);
			}
			if (blklast > end)
			{
				blocktotal += calcsos_raw_se(odd, end + 1,
					blklast);
			}
			common = calcsos_raw_se(odd, MAX(blkfirst, start),
				MIN(blklast, end));
			blocktotal += common;
			total += common;

			// Fill in the sum in the block.
			blocks[block].sumofsquares[odd] = blocktotal;
			blocks[block].filledin |= SOS_FILLED_IN(odd);
		}
		else if (blkfirst >= start && blklast <= end)
		{
			// The block is filled in and entirely contained
			// within the range of samples we're interested in.
			total += blocks[block].sumofsquares[odd];
		}
		else
		{
			// Block's filled in, but we can't use it.
			// We're not interested in all of it.
			total += calcsos_raw_se(odd, MAX(blkfirst, start),
				MIN(blklast, end));
		}
	}

	return total;
}

// Calculate the RMS average and return it.
static double calcrms(int odd, int start, int num)
{
	return sqrt(calcsos(odd, start, num) / num);
}

// Initialize the blocks. Note we don't fill them in yet.
static void initblocks(void)
{
	numblocks = (numsamples + SAMPLES_PER_BLOCK - 1) / SAMPLES_PER_BLOCK;
	blocks = xm(sizeof *blocks, numblocks);
	memset(blocks, 0, sizeof *blocks * numblocks);
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
	int color;

	if (!logdisp) /* Linear display. */
	{
		int scalecount;
		int ycenter = top + height/2;

		for (scalecount = vzoom; scalecount > 0; scalecount--)
		{
			min *= 2;
			max *= 2;
		}

		y1 = ycenter - (int)(max * height/2 / SAMP_DIV_FLOAT);
		y2 = ycenter - (int)(min * height/2 / SAMP_DIV_FLOAT);
	}
	else /* Logarithmic display. */
	{
		double maxdb, mindb;

		/* Avoid negative infinity when taking logs. */
		if (max == 0) max = 1;
		if (min == 0) min = 1;

		maxdb = 20.0 * log10((double)abs(max) / SAMP_DIV_FLOAT);
		mindb = 20.0 * log10((double)abs(min) / SAMP_DIV_FLOAT);

		// When drawing logarithmic peak graphs, don't allow
		// asymmetry to fubar the display.
		if (!rms)
		{
			if (mindb < maxdb)
				mindb = maxdb;
			else
				maxdb = mindb;
		}

		/*
		 * XXX is this clamping necessary? should it be?
		 * Note: 96 dB is maximum dynamic range of 16-bit
		 * samples, in theory.
		 */
		if (maxdb > 0.0) maxdb = 0.0;
		if (mindb < -MAX_DB_RANGE) mindb = -MAX_DB_RANGE;

		y1 = (int)(top - mindb * height / MAX_DB_RANGE);
		y2 = (int)(top - maxdb * height / MAX_DB_RANGE);
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

	if (rms)
	{
		if (logdisp) color = CHANNEL_LOG_RMS_COLOR;
		else color = CHANNEL_LIN_RMS_COLOR;
	}
	else if (logdisp) color = CHANNEL_LOG_PEAK_COLOR;
	else color = CHANNEL_LIN_PEAK_COLOR;

	vline(buffer, x, y1, y2, color);

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

	set_clip_rect(buffer, left, top, left + cols - 1, top + height - 1);

	if (!(height & 1)) height--; /* force an odd height */

	lastpeaky1 = lastpeaky2 = lastrmsy1 = lastrmsy2 = top + height/2;

	/* If there are too few samples for each column, skip RMS. */
	skiprms = rmsdisp ? (wzoom < RMS_MIN_SAMPLES(samprate)) : 1;

	rectfill(buffer, left, top, left + cols - 1, top + height - 1,
		CHANNEL_BG);

	if (logdisp)
	{
		int ix;
		for (ix = CHANNEL_LOGGUIDE_SPACING; ix < MAX_DB_RANGE;
			ix += CHANNEL_LOGGUIDE_SPACING)
		{
			const int y = (int)(top + ix * height / MAX_DB_RANGE);
			hline(buffer, left, y, left + cols - 1,
				ix % CHANNEL_LOGGUIDE_MAJOR_SPACING == 0
					? CHANNEL_LOGGUIDE_COLOR_MAJOR
					: CHANNEL_LOGGUIDE_COLOR_MINOR);
		}
	}
	else
	{
		hline(buffer, left, top + height/2, left + cols - 1,
			CHANNEL_DCLINE_COLOR);
	}

	for (chan_i = 0; chan_i < cols; chan_i++)
	{
		double rmsval;
		int rmsint;

		if (peakdisp)
		{
			getminmax(odd, start + chan_i*wzoom, wzoom, &min, &max);
			drawcolumn(chan_i + left, top, height, min, max, 0);
		}

		if (!skiprms)
		{
			rmsval = calcrms(odd, start + chan_i*wzoom, wzoom);
			rmsint = (int)(SAMP_DIV_FLOAT * rmsval);
			drawcolumn(chan_i + left, top, height,
				-rmsint, rmsint, 1);
		}
	}

	set_clip_rect(buffer, 0, 0, buffer->w, buffer->h);
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

	totaltime = (double)num / samprate; /* time in seconds */
	secsperpixel = totaltime / width;
	startsecs = (double)start / samprate;
	endsecs = (double)(start + num) / samprate;

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
	t = (double)start / samprate;
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
		drawchannel(ch * scrheight/2, scrheight/2 - FONTHEIGHT,
			0, zoom, scrwidth, ch, pos);
		drawtimemarkers((ch+1) * scrheight/2 - FONTHEIGHT, 0,
			scrwidth, pos, zoom*scrwidth);
	}

	scare_mouse();
	vsync();
	blit(buffer, screen, 0, 0, 0, 0, scrwidth, scrheight);
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
		pos -= scrwidth * zoom;
	else if (keyval == KEY_PGDN)
		pos += scrwidth * zoom;
	else if (keyval == KEY_LEFT)
		pos -= scrwidth * zoom / SCREEN_INTERVAL;
	else if (keyval == KEY_RIGHT)
		pos += scrwidth * zoom / SCREEN_INTERVAL;
	else if (keyval == KEY_HOME)
		pos = 0;
	else if (keyval == KEY_END)
		pos = numsamples - scrwidth*zoom;
	else if (keyval == KEY_UP) /* zoom in */
	{
		pos += scrwidth*zoom/2;
		zoom /= 2;
		if (zoom < 1)
			zoom = 1;
		pos -= scrwidth*zoom/2;
	}
	else if (keyval == KEY_DOWN) /* zoom out */
	{
		pos += scrwidth*zoom/2;
		zoom *= 2;
		if (zoom > numsamples/scrwidth)
			zoom = numsamples/scrwidth;
		pos -= scrwidth*zoom/2;
	}
	else if (keyval == KEY_F3) /* vertical zoom out */
	{
		if (vzoom > VZOOM_MIN)
			vzoom--;
	}
	else if (keyval == KEY_F4) /* vertical zoom in */
	{
		if (vzoom < VZOOM_MAX)
			vzoom++;
	}
	else if (tolower(keyascii) == 'l') /* toggle log view */
		logdisp = !logdisp;
	else if (tolower(keyascii) == 'p') /* peak display */
		peakdisp = !peakdisp;
	else if (tolower(keyascii) == 'r') /* rms display */
		rmsdisp = !rmsdisp;
	else if (keyval == KEY_ESC) /* quit */
		return 1;

	if (pos > numsamples - scrwidth*zoom)
		pos = numsamples - scrwidth*zoom;
	if (pos < 0)
		pos = 0;

	return 0;
}

static void initfromwav(char *data, uint32_t datalen) {
	uint16_t channels;
	uint16_t formattag;
	uint16_t bitdepth;
	uint32_t fmtchunklen;
	uint32_t offset;
	uint32_t chunklen;
	uint32_t wavsamplerate;
	int i;

	/*
	 * TODO: support wav files with lengths (of both file and data chunk)
	 * given as 0xFFFFFFFF by simply reading until end of file.
	 * This is what madplay produces on stdout.
	 */
	if (datalen < 44 || memcmp(data, "RIFF", 4) ||
		memcmp(&data[8], "WAVEfmt ", 8) ||
		le32toh(*(uint32_t *)&data[4]) + 8 != datalen) {
		errquit("invalid .wav file");
	}
	fmtchunklen = le32toh(*(uint32_t *)&data[16]);
	formattag = le16toh(*(uint16_t *)&data[20]);
	channels = le16toh(*(uint16_t *)&data[22]);
	wavsamplerate = le32toh(*(uint32_t *)&data[24]);
	bitdepth = le16toh(*(uint16_t *)&data[34]);
	if (formattag == 0xFFFE)
		formattag = le16toh(*(uint16_t *)&data[44]);
	if (formattag != 0x0001)
		errquit("non-PCM wav data: format tag %u", formattag);
	if (wavsamplerate > 384000)
		errquit("unsupported sample rate %u", wavsamplerate);
	samprate = (int)wavsamplerate;
	if (channels != 2)
		errquit("non-stereo wav files not supported");

	/* Find data chunk */
	offset = 16 + 4 + fmtchunklen;
	while (1) {
		if (offset + 8 > datalen)
			errquit("wav has no data chunk");

		chunklen = le32toh(*(uint32_t *)&data[offset + 4]);
		if (memcmp(&data[offset], "data", 4) == 0) break;
		offset += chunklen;
	}
	offset += 8; /* Skip data chunk ID and len */
	if (offset + chunklen != datalen)
		errquit("invalid .wav file: data chunk wrong size");
	numsamples = chunklen / channels / (bitdepth / 8);
	if (bitdepth == 16) {
		samples = (int16_t *)&data[offset];
		for (i = 0; i < numsamples; i++)
			samples[i] = le16toh(samples[i]);
	} else if (bitdepth == 24) {
		samples = xm(2, numsamples * channels);
		for (i = 0; i < numsamples * channels; i++) {
			samples[i] = (int16_t)data[offset + i*3 + 1]
				+ ((int16_t)data[offset + i*3 + 2]<<8);
		}
	} else errquit("unsupported bit depth: %u", bitdepth);
}

static void usage(void)
{
	errquit("usage: viewwav [-width X] [-height Y] [-forceraw] filename");
}

int main(int argc, char *argv[])
{
	char *data;
	char *filename;
	uint32_t datalen;
	FILE *fp;
	const char *str;
	bool iswav;
	bool forceraw = false;
	int i;

	// Default sample rate based on environment variable.
	str = getenv("RATE");
	if (str == NULL)
		str = getenv("SR");
	if (str != NULL && atoi(str) > 0)
		samprate = atoi(str);

	if (allegro_init() != 0)
	{
		fprintf(stderr, "cannot initialize Allegro\n");
		exit(EXIT_FAILURE);
	}
	if (install_keyboard() != 0)
		errquit("can't install keyboard handler: %s", allegro_error);
	if (install_timer() != 0)
		errquit("can't install timers: %s", allegro_error);

	if (argc < 2)
		usage();
	argc--, argv++;

	while (argc > 1)
	{
		if (!strcmp("-width", *argv) && argc > 2)
		{
			scrwidth = atoi(argv[1]);
			if (scrwidth < 5) errquit("screen width too small");
			argc -= 2, argv += 2;
		}
		else if (!strcmp("-height", *argv) && argc > 2)
		{
			scrheight = atoi(argv[1]);
			if (scrheight < 5) errquit("screen width too small");
			argc -= 2, argv += 2;
		}
		else if (!strcmp("-forceraw", *argv))
		{
			forceraw = true;
			argc--, argv++;
		}
		else usage();
	}

	filename = *argv;
	if (strcmp("-", filename) == 0)
	{
		fp = stdin;
		SET_BINARY_MODE
	}
	else if ((fp = fopen(filename, "rb")) == NULL)
		errquit("cannot open %s", filename);

	if (set_gfx_mode(GFX_AUTODETECT_WINDOWED, scrwidth, scrheight, 0, 0)
		!= 0)
	{
		errquit("can't set graphics mode: %s", allegro_error);
	}
	buffer = create_bitmap(scrwidth, scrheight);
	if (buffer == NULL)
		errquit("can't create buffer: %s", allegro_error);
	show_mouse(screen);

	data = readfile(fp, &datalen);
	if (fp != stdin)
		fclose(fp);

	/* Is it a wav file? On stdin, sniff; from file, check extension. */
	if (!forceraw && fp == stdin && datalen > 1000 &&
		memcmp(data, "RIFF", 4) == 0 &&
		memcmp((data + 8), "WAVEfmt ", 8) == 0)
	{
		iswav = true;
	}
	else if (!forceraw && strlen(filename) > 4 && datalen > 1000 &&
		strcasecmp(&filename[strlen(filename) - 4], ".wav") == 0)
	{
		iswav = true;
	}
	else iswav = false;

	if (iswav)
	{
		initfromwav(data, datalen);
	}
	else
	{
		samples = (int16_t *)data;
		numsamples = datalen / (2 * sizeof (int16_t));
		for (i = 0; i < numsamples; i++)
			samples[i] = le16toh(samples[i]);
	}
	initblocks();
	while (!cycle())
		;
	return 0;
}
