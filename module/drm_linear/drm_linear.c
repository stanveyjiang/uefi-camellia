// SPDX-License-Identifier: GPL-2.0-only
/*
 * drm_linear.c - take DRM master, create a linear XRGB8888 dumb buffer and
 * set it as the active CRTC framebuffer, then stay alive.
 *
 * Run after stopping SurfaceFlinger and the MTK composer HAL (so DRM master
 * can be acquired). Optional argv[2] is a little-endian XRGB color.
 * Build: aarch64-linux-gnu-gcc -static -O2 -o drm_linear drm_linear.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>

static int xioctl(int fd, unsigned long req, void *arg)
{
	int r;
	do {
		r = ioctl(fd, req, arg);
	} while (r < 0 && errno == EINTR);
	return r;
}

static uint32_t *status_pixels;
static size_t status_pixel_count;
static uint32_t status_width;
static uint32_t status_height;
static uint32_t status_pitch_pixels;
static volatile sig_atomic_t requested_stage;

static void paint_status_stage(sig_atomic_t stage)
{
	uint32_t color;
	size_t i;

	switch (stage) {
	case 1:
		color = 0xFF00FF00; /* green: module loader check starting */
		break;
	case 2:
		color = 0xFF0000FF; /* blue: module loader check passed */
		break;
	case 3:
		color = 0xFF00FFFF; /* cyan: FD/display/copy preflight passed */
		break;
	case 4:
		color = 0xFFFFFFFF; /* white: page-map preflight passed */
		break;
	case 5:
		color = 0xFFFFFF00; /* yellow: final jump starting */
		break;
	case 6:
		color = 0xFFFFA500; /* orange: copy-check module unloaded */
		break;
	case 7:
		color = 0xFF800080; /* purple: IOVA/physical map check passed */
		break;
	case 8:
	default:
		color = 0xFFFF00FF; /* magenta: preflight/final load failed */
		break;
	}

	for (i = 0; i < status_pixel_count; i++)
		status_pixels[i] = color;
	msync(status_pixels, status_pixel_count * sizeof(uint32_t), MS_SYNC);
}

static void paint_failure_code(int bar_count)
{
	uint32_t bar_width;
	uint32_t x;
	uint32_t y;
	int bar;

	if (bar_count < 1)
		bar_count = 1;
	if (bar_count > 20)
		bar_count = 20;

	for (x = 0; x < status_pixel_count; x++)
		status_pixels[x] = 0xFF000000;

	bar_width = status_width / (uint32_t)(bar_count * 4);
	if (bar_width < 6)
		bar_width = 6;

	for (bar = 0; bar < bar_count; bar++) {
		uint32_t center =
			(uint32_t)(((uint64_t)(bar + 1) * status_width) /
				   (uint64_t)(bar_count + 1));
		uint32_t start = center > bar_width / 2
			? center - bar_width / 2 : 0;

		for (y = 0; y < status_height; y++) {
			for (x = start; x < start + bar_width &&
			     x < status_width; x++)
				status_pixels[y * status_pitch_pixels + x] =
					0xFFFFFFFF;
		}
	}
	msync(status_pixels, status_pixel_count * sizeof(uint32_t), MS_SYNC);
}

static void fill_status_rect(uint32_t x, uint32_t y,
			     uint32_t width, uint32_t height)
{
	uint32_t draw_x;
	uint32_t draw_y;

	for (draw_y = y; draw_y < y + height && draw_y < status_height;
	     draw_y++)
		for (draw_x = x; draw_x < x + width && draw_x < status_width;
		     draw_x++)
			status_pixels[draw_y * status_pitch_pixels + draw_x] =
				0xFFFFFFFF;
}

static void paint_digit(uint32_t x, uint32_t y, uint32_t width,
			uint32_t height, uint32_t thickness, int digit)
{
	static const unsigned char segments[10] = {
		0x3f, 0x06, 0x5b, 0x4f, 0x66,
		0x6d, 0x7d, 0x07, 0x7f, 0x6f
	};
	unsigned int mask;
	uint32_t half_height = height / 2;

	if (digit < 0 || digit > 9)
		return;
	mask = segments[digit];
	if (mask & 1)
		fill_status_rect(x + thickness, y, width - 2 * thickness,
				 thickness);
	if (mask & 2)
		fill_status_rect(x + width - thickness, y + thickness,
				 thickness, half_height - thickness);
	if (mask & 4)
		fill_status_rect(x + width - thickness,
				 y + half_height, thickness,
				 half_height - thickness);
	if (mask & 8)
		fill_status_rect(x + thickness, y + height - thickness,
				 width - 2 * thickness, thickness);
	if (mask & 16)
		fill_status_rect(x, y + half_height, thickness,
				 half_height - thickness);
	if (mask & 32)
		fill_status_rect(x, y + thickness, thickness,
				 half_height - thickness);
	if (mask & 64)
		fill_status_rect(x + thickness, y + half_height - thickness / 2,
				 width - 2 * thickness, thickness);
}

static void paint_decimal_number(int value)
{
	int digits[6];
	int count = 0;
	uint32_t draw_width;
	uint32_t gap;
	uint32_t thickness;
	uint32_t digit_width;
	uint32_t digit_height;
	uint32_t start_x;
	uint32_t start_y;
	uint32_t x;
	uint32_t i;

	for (i = 0; i < status_pixel_count; i++)
		status_pixels[i] = 0xFF000000;

	if (value == 0) {
		digits[count++] = 0;
	} else {
		while (value > 0 && count < 6) {
			digits[count++] = value % 10;
			value /= 10;
		}
	}

	thickness = status_width / 45;
	if (thickness < 8)
		thickness = 8;
	digit_width = status_width / 6;
	digit_height = status_height / 4;
	gap = status_width / 14;
	draw_width = (uint32_t)count * digit_width +
		(uint32_t)(count - 1) * gap;
	start_x = status_width > draw_width
		? (status_width - draw_width) / 2 : 0;
	start_y = (status_height - digit_height) / 2;
	x = start_x;
	for (i = 0; i < (uint32_t)count; i++) {
		paint_digit(x, start_y, digit_width, digit_height, thickness,
			    digits[count - 1 - (int)i]);
		x += digit_width + gap;
	}
	msync(status_pixels, status_pixel_count * sizeof(uint32_t), MS_SYNC);
}

static void status_signal(int signal_number, siginfo_t *info, void *context)
{
	(void)context;

	if (signal_number == SIGRTMIN) {
		requested_stage = info->si_value.sival_int;
		return;
	}

	switch (signal_number) {
	case SIGUSR1:
		requested_stage = 1;
		break;
	case SIGUSR2:
		requested_stage = 2;
		break;
	case SIGALRM:
		requested_stage = 3;
		break;
	case SIGWINCH:
		requested_stage = 4;
		break;
	case SIGHUP:
		requested_stage = 5;
		break;
	case SIGQUIT:
		requested_stage = 8;
		break;
	case SIGVTALRM:
		requested_stage = 6;
		break;
	case SIGXCPU:
		requested_stage = 7;
		break;
	default:
		break;
	}
}

static void install_status_signals(void)
{
	struct sigaction action;

	memset(&action, 0, sizeof(action));
	action.sa_sigaction = status_signal;
	action.sa_flags = SA_SIGINFO;
	sigemptyset(&action.sa_mask);
	sigaction(SIGUSR1, &action, NULL);
	sigaction(SIGUSR2, &action, NULL);
	sigaction(SIGALRM, &action, NULL);
	sigaction(SIGWINCH, &action, NULL);
	sigaction(SIGHUP, &action, NULL);
	sigaction(SIGQUIT, &action, NULL);
	sigaction(SIGVTALRM, &action, NULL);
	sigaction(SIGXCPU, &action, NULL);
	sigaction(SIGRTMIN, &action, NULL);
}

int main(int argc, char **argv)
{
	const char *dev = argc > 1 ? argv[1] : "/dev/dri/card0";
	uint32_t color = argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 0) :
				   0xFF00FF00;
	uint32_t width = 1088, height = 2400, bpp = 32;
	struct drm_mode_card_res res;
	struct drm_mode_get_connector conn;
	struct drm_mode_get_encoder enc;
	struct drm_mode_get_property prop;
	struct drm_mode_connector_set_property set_prop;
	struct drm_mode_modeinfo mode;
	struct drm_mode_create_dumb dumb;
	struct drm_mode_map_dumb mm;
	struct drm_mode_fb_cmd fb;
	struct drm_mode_crtc set;
	uint32_t *crtc_ids, *conn_ids, *enc_ids, *fb_ids;
	uint32_t *conn_enc_ids = NULL;
	uint32_t *conn_prop_ids = NULL;
	uint64_t *conn_prop_values = NULL;
	struct drm_mode_modeinfo *conn_modes = NULL;
	uint32_t conn_prop_count = 0;
	uint32_t conn_id = 0;
	uint32_t crtc_id = 0;
	uint32_t crtc_index = 0;
	void *map;
	int fd, i, j;

	fd = open(dev, O_RDWR);
	if (fd < 0) {
		perror("open");
		return 1;
	}

	if (xioctl(fd, DRM_IOCTL_SET_MASTER, NULL) < 0) {
		perror("SET_MASTER");
		fprintf(stderr, "stop SurfaceFlinger first\n");
		return 1;
	}

	memset(&res, 0, sizeof(res));
	if (xioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
		perror("GETRESOURCES count");
		return 1;
	}
	crtc_ids = calloc(res.count_crtcs, sizeof(uint32_t));
	conn_ids = calloc(res.count_connectors, sizeof(uint32_t));
	enc_ids = calloc(res.count_encoders, sizeof(uint32_t));
	fb_ids = calloc(res.count_fbs ? res.count_fbs : 1, sizeof(uint32_t));
	res.crtc_id_ptr = (uintptr_t)crtc_ids;
	res.connector_id_ptr = (uintptr_t)conn_ids;
	res.encoder_id_ptr = (uintptr_t)enc_ids;
	res.fb_id_ptr = (uintptr_t)fb_ids;
	if (xioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
		perror("GETRESOURCES ids");
		return 1;
	}
	printf("crtcs=%u connectors=%u encoders=%u fbs=%u\n",
	       res.count_crtcs, res.count_connectors, res.count_encoders,
	       res.count_fbs);

	for (i = 0; i < (int)res.count_connectors; i++) {
		memset(&conn, 0, sizeof(conn));
		conn.connector_id = conn_ids[i];
		if (xioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0)
			continue;
		printf("connector %u type=%u connection=%u encoder=%u modes=%u\n",
		       conn.connector_id, conn.connector_type, conn.connection,
		       conn.encoder_id, conn.count_modes);
		if (conn.connection != 1)
			continue;

		conn_modes = calloc(conn.count_modes ? conn.count_modes : 1,
				    sizeof(*conn_modes));
		conn_enc_ids = calloc(conn.count_encoders ? conn.count_encoders : 1,
				      sizeof(*conn_enc_ids));
		conn_prop_count = conn.count_props;
		conn_prop_ids = calloc(conn_prop_count ? conn_prop_count : 1,
				       sizeof(*conn_prop_ids));
		conn_prop_values = calloc(conn_prop_count ? conn_prop_count : 1,
					  sizeof(*conn_prop_values));
		if (!conn_modes || !conn_enc_ids ||
		    !conn_prop_ids || !conn_prop_values) {
			perror("calloc connector");
			return 1;
		}
		conn.modes_ptr = (uintptr_t)conn_modes;
		conn.encoders_ptr = (uintptr_t)conn_enc_ids;
		conn.props_ptr = (uintptr_t)conn_prop_ids;
		conn.prop_values_ptr = (uintptr_t)conn_prop_values;
		if (xioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0) {
			perror("GETCONNECTOR data");
			return 1;
		}
		if (!conn.count_modes || !conn.count_encoders)
			continue;

		conn_id = conn.connector_id;
		mode = conn_modes[0];
		width = mode.hdisplay;
		height = mode.vdisplay;
		printf("selected mode %s %ux%u clock=%u flags=0x%x\n",
		       mode.name, width, height, mode.clock, mode.type);

		for (j = 0; j < (int)conn.count_encoders && !crtc_id; j++) {
			memset(&enc, 0, sizeof(enc));
			enc.encoder_id = conn_enc_ids[j];
			if (xioctl(fd, DRM_IOCTL_MODE_GETENCODER, &enc) < 0)
				continue;
			printf("encoder %u crtc=%u possible_crtcs=0x%x\n",
			       enc.encoder_id, enc.crtc_id, enc.possible_crtcs);
			if (enc.crtc_id) {
				crtc_id = enc.crtc_id;
				for (crtc_index = 0;
				     crtc_index < res.count_crtcs; crtc_index++) {
					if (crtc_ids[crtc_index] == crtc_id)
						break;
				}
			} else {
				for (crtc_index = 0;
				     crtc_index < res.count_crtcs; crtc_index++) {
					if (enc.possible_crtcs & (1U << crtc_index)) {
						crtc_id = crtc_ids[crtc_index];
						break;
					}
				}
			}
		}
		if (crtc_id)
			break;
	}
	if (!conn_id || !crtc_id) {
		fprintf(stderr, "no usable connector/crtc\n");
		return 1;
	}
	printf("using connector %u crtc %u, mode %ux%u\n",
	       conn_id, crtc_id, width, height);

	memset(&dumb, 0, sizeof(dumb));
	dumb.width = width;
	dumb.height = height;
	dumb.bpp = bpp;
	if (xioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &dumb) < 0) {
		perror("CREATE_DUMB");
		return 1;
	}
	printf("dumb handle=%u pitch=%u size=%llu\n", dumb.handle, dumb.pitch,
	       (unsigned long long)dumb.size);

	memset(&mm, 0, sizeof(mm));
	mm.handle = dumb.handle;
	if (xioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mm) < 0) {
		perror("MAP_DUMB");
		return 1;
	}
	map = mmap(NULL, dumb.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
		   mm.offset);
	if (map == MAP_FAILED) {
		perror("mmap");
		return 1;
	}
	memset(map, 0, dumb.size);
	{
		uint32_t *p = map;
		unsigned long long n;

		for (n = 0; n + 4 <= dumb.size; n += 4)
			p[n / 4] = color;
	}
	msync(map, dumb.size, MS_SYNC);
	status_pixels = map;
	status_pixel_count = dumb.size / sizeof(uint32_t);
	status_width = dumb.width;
	status_height = dumb.height;
	status_pitch_pixels = dumb.pitch / sizeof(uint32_t);
	install_status_signals();

	memset(&fb, 0, sizeof(fb));
	fb.width = width;
	fb.height = height;
	fb.pitch = dumb.pitch;
	fb.bpp = bpp;
	fb.depth = 24;
	fb.handle = dumb.handle;
	if (xioctl(fd, DRM_IOCTL_MODE_ADDFB, &fb) < 0) {
		perror("ADDFB");
		return 1;
	}
	printf("fb_id=%u\n", fb.fb_id);

	memset(&set, 0, sizeof(set));
	set.crtc_id = crtc_id;
	set.fb_id = fb.fb_id;
	set.set_connectors_ptr = (uintptr_t)&conn_id;
	set.count_connectors = 1;
	set.x = 0;
	set.y = 0;
	set.mode_valid = 1;
	set.mode = mode;
	if (xioctl(fd, DRM_IOCTL_MODE_SETCRTC, &set) < 0) {
		perror("SETCRTC");
		return 1;
	}
	printf("SETCRTC ok\n");

	for (i = 0; i < (int)conn_prop_count; i++) {
		memset(&prop, 0, sizeof(prop));
		prop.prop_id = conn_prop_ids[i];
		if (xioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &prop) < 0)
			continue;
		printf("connector prop %u %s = %llu\n", prop.prop_id, prop.name,
		       (unsigned long long)conn_prop_values[i]);
		if (strcmp(prop.name, "DPMS") != 0)
			continue;
		if (conn_prop_values[i] == DRM_MODE_DPMS_ON)
			continue;
		memset(&set_prop, 0, sizeof(set_prop));
		set_prop.value = DRM_MODE_DPMS_ON;
		set_prop.prop_id = prop.prop_id;
		set_prop.connector_id = conn_id;
		if (xioctl(fd, DRM_IOCTL_MODE_SETPROPERTY, &set_prop) < 0) {
			perror("connector DPMS ON");
		} else {
			printf("connector DPMS forced on\n");
		}
	}

	printf("linear framebuffer active, staying alive\n");
	fflush(stdout);
	for (;;) {
		sig_atomic_t stage;

		pause();
		stage = requested_stage;
		if (stage != 0) {
			requested_stage = 0;
			if (stage >= 100000 && stage <= 104095)
				paint_decimal_number(stage - 100000);
			else if (stage >= 201 && stage <= 208)
				paint_failure_code(stage - 200);
			else if (stage >= 301 && stage <= 320)
				paint_failure_code(stage - 300);
			else
				paint_status_stage(stage);
			printf("status stage %d painted\n", (int)stage);
			fflush(stdout);
		}
	}
	return 0;
}
