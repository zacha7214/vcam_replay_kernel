// SPDX-License-Identifier: GPL-2.0
/*
 * vcam_feed - push an image sequence into the vcam_replay driver (Phase 1).
 *
 * Reads a directory of frames (binary PGM "P5" for GREY, or headerless .raw
 * of exactly sizeimage bytes), configures the driver, then writes one frame
 * per period on an absolute CLOCK_MONOTONIC schedule so the push rate does
 * not drift. The driver's hrtimer independently replays the most recent
 * frame to V4L2 at its own fps, so pushing and delivery are decoupled.
 *
 * Usage:
 *   vcam_feed [-d /dev/vcam0] [-r fps] [-f GREY|YUYV] [-w W] [-H h]
 *             [-l] [-s] <frame-dir>
 *     -r fps     push and playback rate (default 30)
 *     -w/-H      frame size, required for .raw input (PGM is self-sizing)
 *     -f         pixel format (default GREY)
 *     -l         loop the sequence forever
 *     -s         print driver stats once per second
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <time.h>
#include <ctype.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

#include "../driver/vcam_uapi.h"

#define FOURCC(a, b, c, d) \
	((uint32_t)(a) | ((uint32_t)(b) << 8) | \
	 ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define FMT_GREY FOURCC('G', 'R', 'E', 'Y')
#define FMT_YUYV FOURCC('Y', 'U', 'Y', 'V')

static int cmpstr(const void *a, const void *b)
{
	return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* Minimal binary PGM (P5) reader; returns malloc'd pixel data. */
static uint8_t *read_pgm(FILE *f, unsigned *w, unsigned *h)
{
	unsigned vals[3], n = 0;
	uint8_t *data;
	int c;

	if (fgetc(f) != 'P' || fgetc(f) != '5')
		return NULL;
	while (n < 3) {
		c = fgetc(f);
		if (c == '#') {		/* comment to end of line */
			while ((c = fgetc(f)) != '\n' && c != EOF)
				;
		} else if (isdigit(c)) {
			unsigned v = 0;

			while (isdigit(c)) {
				v = v * 10 + (c - '0');
				c = fgetc(f);
			}
			vals[n++] = v;
		} else if (c == EOF) {
			return NULL;
		}
	}
	/* c already consumed the single whitespace after maxval */
	if (vals[2] > 255)
		return NULL;	/* 16-bit PGM not supported */
	*w = vals[0];
	*h = vals[1];
	data = malloc((size_t)vals[0] * vals[1]);
	if (!data)
		return NULL;
	if (fread(data, 1, (size_t)vals[0] * vals[1], f) !=
	    (size_t)vals[0] * vals[1]) {
		free(data);
		return NULL;
	}
	return data;
}

static uint8_t *load_frame(const char *path, uint32_t fourcc,
			   unsigned *w, unsigned *h, size_t *len)
{
	const char *ext = strrchr(path, '.');
	uint8_t *data = NULL;
	FILE *f = fopen(path, "rb");

	if (!f) {
		perror(path);
		return NULL;
	}
	if (ext && !strcasecmp(ext, ".pgm")) {
		data = read_pgm(f, w, h);
		if (data)
			*len = (size_t)*w * *h;
		if (data && fourcc == FMT_YUYV) {
			/* expand grey to YUYV so mono PGMs work either way */
			size_t px = (size_t)*w * *h;
			uint8_t *yuyv = malloc(px * 2);

			if (yuyv) {
				for (size_t i = 0; i < px; i++) {
					yuyv[i * 2] = data[i];
					yuyv[i * 2 + 1] = 0x80;
				}
				free(data);
				data = yuyv;
				*len = px * 2;
			} else {
				free(data);
				data = NULL;
			}
		}
	} else {		/* raw: trust -w/-H, read whole file */
		struct stat st;

		if (!fstat(fileno(f), &st)) {
			data = malloc(st.st_size);
			if (data &&
			    fread(data, 1, st.st_size, f) !=
				    (size_t)st.st_size) {
				free(data);
				data = NULL;
			}
			*len = st.st_size;
		}
	}
	fclose(f);
	if (!data)
		fprintf(stderr, "failed to load %s\n", path);
	return data;
}

int main(int argc, char **argv)
{
	const char *dev = "/dev/vcam0";
	const char *dir;
	unsigned width = 0, height = 0, fps = 30;
	uint32_t fourcc = FMT_GREY;
	int loop = 0, show_stats = 0;
	int opt, fd;

	while ((opt = getopt(argc, argv, "d:r:f:w:H:lsh")) != -1) {
		switch (opt) {
		case 'd': dev = optarg; break;
		case 'r': fps = atoi(optarg); break;
		case 'w': width = atoi(optarg); break;
		case 'H': height = atoi(optarg); break;
		case 'f':
			if (!strcasecmp(optarg, "YUYV"))
				fourcc = FMT_YUYV;
			else if (!strcasecmp(optarg, "GREY"))
				fourcc = FMT_GREY;
			else {
				fprintf(stderr, "unknown format %s\n", optarg);
				return 1;
			}
			break;
		case 'l': loop = 1; break;
		case 's': show_stats = 1; break;
		default:
			fprintf(stderr,
				"usage: %s [-d ctldev] [-r fps] [-f GREY|YUYV] [-w W] [-H h] [-l] [-s] <frame-dir>\n",
				argv[0]);
			return 1;
		}
	}
	if (optind >= argc || !fps) {
		fprintf(stderr, "missing frame directory (or fps 0)\n");
		return 1;
	}
	dir = argv[optind];

	/* collect and sort frame filenames */
	char **files = NULL;
	size_t nfiles = 0, cap = 0;
	DIR *d = opendir(dir);
	struct dirent *de;

	if (!d) {
		perror(dir);
		return 1;
	}
	while ((de = readdir(d))) {
		const char *ext = strrchr(de->d_name, '.');

		if (!ext || (strcasecmp(ext, ".pgm") &&
			     strcasecmp(ext, ".raw")))
			continue;
		if (nfiles == cap) {
			cap = cap ? cap * 2 : 256;
			files = realloc(files, cap * sizeof(*files));
		}
		if (asprintf(&files[nfiles], "%s/%s", dir, de->d_name) < 0)
			return 1;
		nfiles++;
	}
	closedir(d);
	if (!nfiles) {
		fprintf(stderr, "no .pgm/.raw frames in %s\n", dir);
		return 1;
	}
	qsort(files, nfiles, sizeof(*files), cmpstr);

	/* size the stream from the first frame if -w/-H not given */
	if (!width || !height) {
		size_t len;
		unsigned w = 0, h = 0;
		uint8_t *probe = load_frame(files[0], fourcc, &w, &h, &len);

		if (!probe || !w || !h) {
			fprintf(stderr,
				"cannot determine frame size; use -w/-H for raw input\n");
			return 1;
		}
		free(probe);
		width = w;
		height = h;
	}

	size_t sizeimage = (size_t)width * height *
			   (fourcc == FMT_YUYV ? 2 : 1);

	fd = open(dev, O_WRONLY);
	if (fd < 0) {
		perror(dev);
		return 1;
	}

	struct vcam_format vfmt = {
		.width = width, .height = height, .fourcc = fourcc,
	};
	if (ioctl(fd, VCAM_IOC_S_FORMAT, &vfmt)) {
		perror("VCAM_IOC_S_FORMAT (is the video device streaming?)");
		return 1;
	}
	struct vcam_fps vfps = { .num = fps, .den = 1 };
	if (ioctl(fd, VCAM_IOC_S_FPS, &vfps)) {
		perror("VCAM_IOC_S_FPS");
		return 1;
	}
	printf("feeding %zu frames %ux%u @ %u fps from %s into %s%s\n",
	       nfiles, width, height, fps, dir, dev,
	       loop ? " (looping)" : "");

	/* absolute-time pacing: next = start + n * period, no drift */
	struct timespec next;
	uint64_t period_ns = 1000000000ull / fps;
	uint64_t pushed = 0;
	time_t last_stat = 0;

	clock_gettime(CLOCK_MONOTONIC, &next);
	do {
		for (size_t i = 0; i < nfiles; i++) {
			unsigned w = width, h = height;
			size_t len;
			uint8_t *frame = load_frame(files[i], fourcc, &w, &h,
						    &len);

			if (!frame)
				continue;
			if (len != sizeimage) {
				fprintf(stderr,
					"%s: %zu bytes, expected %zu, skipping\n",
					files[i], len, sizeimage);
				free(frame);
				continue;
			}

			next.tv_nsec += period_ns;
			while (next.tv_nsec >= 1000000000) {
				next.tv_nsec -= 1000000000;
				next.tv_sec++;
			}
			clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next,
					NULL);

			if (write(fd, frame, len) != (ssize_t)len)
				perror("write");
			free(frame);
			pushed++;

			if (show_stats && time(NULL) != last_stat) {
				struct vcam_stats st;

				last_stat = time(NULL);
				if (!ioctl(fd, VCAM_IOC_G_STATS, &st))
					printf("pushed=%llu submitted=%llu delivered=%llu no_buf=%llu seq=%llu\n",
					       (unsigned long long)pushed,
					       (unsigned long long)st.frames_submitted,
					       (unsigned long long)st.frames_delivered,
					       (unsigned long long)st.ticks_no_buffer,
					       (unsigned long long)st.sequence);
			}
		}
	} while (loop);

	close(fd);
	return 0;
}
