// SPDX-License-Identifier: GPL-2.0-only
/*
 * Minimal PID 1 for the camellia pre-Android UEFI handoff.
 *
 * This program runs directly from initramfs. It prepares only the kernel
 * interfaces needed by drm_linear and uefi_jump, then hands control to UEFI.
 * It does not mount Android, start Magisk, or open an ADB connection.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef SYS_finit_module
#define SYS_finit_module 273
#endif

#ifndef SYS_delete_module
#define SYS_delete_module 106
#endif

#define DRM_DEV_PATH "/sys/class/drm/card0/dev"
#define DRM_NODE     "/dev/dri/card0"
#define PAYLOAD_DEV_PATH "/sys/class/block/sdc31/dev"
#define PAYLOAD_NODE     "/dev/block/sdc31"
#define PAYLOAD_BYTES    0x4000000UL
#define MODULE_PATH  "/uefi_jump.ko"
#define FD_PATH      "/uefi.fd"
#define JUMP_ARGS \
	"fd_path=" FD_PATH " payload_path=" PAYLOAD_NODE \
	" payload_bytes=67108864 jump=1 contig_fb=0 hb=0"
#define MAPDUMP_ARGS \
	"fd_path=" FD_PATH " jump=0 contig_fb=0 hb=0 mapdump=1"

static int log_fd = -1;
static pid_t drm_pid = -1;
static int module_errno;

static size_t text_len(const char *text)
{
	size_t length = 0;

	while (text[length] != '\0')
		length++;
	return length;
}

static void write_fd(int fd, const char *text)
{
	ssize_t ignored;

	if (fd < 0)
		return;

	ignored = write(fd, text, text_len(text));
	(void)ignored;
}

static void write_hex(int fd, uint64_t value)
{
	char buffer[20];
	static const char digits[] = "0123456789abcdef";
	size_t index = sizeof(buffer) - 1;
	int started = 0;

	buffer[index] = '\0';
	do {
		unsigned int digit = (unsigned int)(value & 0xfU);

		buffer[--index] = digits[digit];
		value >>= 4;
		started = 1;
	} while (value != 0 || !started);

	buffer[--index] = 'x';
	buffer[--index] = '0';
	write_fd(fd, &buffer[index]);
	write_fd(fd, "\n");
}

static void log_text(const char *text)
{
	write_fd(log_fd, text);
}

static void log_value(const char *name, int64_t value)
{
	write_fd(log_fd, name);
	if (value < 0) {
		write_fd(log_fd, "-");
		write_hex(log_fd, (uint64_t)(-value));
	} else {
		write_hex(log_fd, (uint64_t)value);
	}
}

static void sleep_ms(unsigned int milliseconds)
{
	struct timespec delay;

	delay.tv_sec = milliseconds / 1000U;
	delay.tv_nsec = (long)(milliseconds % 1000U) * 1000000L;
	while (nanosleep(&delay, &delay) < 0 && errno == EINTR)
		;
}

static int make_dir(const char *path)
{
	if (mkdir(path, 0755) == 0 || errno == EEXIST)
		return 0;
	return -1;
}

static int make_node(const char *path, mode_t mode, unsigned int major,
		     unsigned int minor)
{
	dev_t device = makedev(major, minor);

	unlink(path);
	return mknod(path, mode, device);
}

static int mount_early_filesystems(void)
{
	make_dir("/proc");
	make_dir("/sys");
	make_dir("/dev");
	make_dir("/dev/dri");
	make_dir("/dev/block");
	make_dir("/tmp");

	if (mount("proc", "/proc", "proc", 0, NULL) < 0 && errno != EBUSY)
		return -1;
	if (mount("sysfs", "/sys", "sysfs", 0, NULL) < 0 && errno != EBUSY)
		return -1;
	if (mount("tmpfs", "/dev", "tmpfs", 0, "mode=0755") < 0 &&
	    errno != EBUSY)
		return -1;

	make_dir("/dev/dri");
	make_dir("/dev/block");
	if (make_node("/dev/console", S_IFCHR | 0600, 5, 1) < 0)
		return -1;
	if (make_node("/dev/null", S_IFCHR | 0666, 1, 3) < 0)
		return -1;
	if (make_node("/dev/kmsg", S_IFCHR | 0600, 1, 11) < 0)
		return -1;

	return 0;
}

static int read_device_numbers(const char *path, unsigned int *major_out,
			       unsigned int *minor_out)
{
	char buffer[64];
	char *separator;
	char *end;
	ssize_t length;
	long major_value;
	long minor_value;
	int fd;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;

	length = read(fd, buffer, sizeof(buffer) - 1);
	close(fd);
	if (length <= 0)
		return -1;
	buffer[length] = '\0';

	separator = strchr(buffer, ':');
	if (separator == NULL)
		return -1;
	*separator = '\0';

	errno = 0;
	major_value = strtol(buffer, &end, 10);
	if (errno != 0 || end == buffer || major_value < 0)
		return -1;

	errno = 0;
	minor_value = strtol(separator + 1, &end, 10);
	if (errno != 0 || end == separator + 1 || minor_value < 0)
		return -1;

	*major_out = (unsigned int)major_value;
	*minor_out = (unsigned int)minor_value;
	return 0;
}

static int wait_for_drm(unsigned int timeout_ms)
{
	unsigned int waited = 0;
	unsigned int major;
	unsigned int minor;

	while (waited < timeout_ms) {
		if (read_device_numbers(DRM_DEV_PATH, &major, &minor) == 0) {
			if (make_node(DRM_NODE, S_IFCHR | 0666, major, minor) == 0) {
				log_text("preboot: created /dev/dri/card0\n");
				return 0;
			}
			log_value("preboot: mknod card0 errno=", errno);
		}
		sleep_ms(100);
		waited += 100;
	}

	log_text("preboot: timeout waiting for DRM card0\n");
	return -1;
}

static int wait_for_payload_block(unsigned int timeout_ms)
{
	unsigned int waited = 0;
	unsigned int major;
	unsigned int minor;

	while (waited < timeout_ms) {
		if (read_device_numbers(PAYLOAD_DEV_PATH, &major, &minor) == 0) {
			if (make_node(PAYLOAD_NODE, S_IFBLK | 0600,
				      major, minor) == 0) {
				log_text("preboot: created /dev/block/sdc31\n");
				return 0;
			}
			log_value("preboot: mknod payload errno=", errno);
		}
		sleep_ms(100);
		waited += 100;
	}

	log_text("preboot: timeout waiting for payload partition\n");
	return -1;
}

static int start_linear_framebuffer(void)
{
	pid_t child;
	int status = 0;
	int tries;

	child = fork();
	if (child < 0) {
		log_value("preboot: fork drm_linear errno=", errno);
		return -1;
	}
	if (child == 0) {
		char *const argv[] = {
			"/drm_linear",
			"/dev/dri/card0",
			"0xFFFF0000",
			NULL
		};

		if (log_fd >= 0) {
			dup2(log_fd, STDOUT_FILENO);
			dup2(log_fd, STDERR_FILENO);
		}
		execv(argv[0], argv);
		_exit(127);
	}

	drm_pid = child;
	for (tries = 0; tries < 30; tries++) {
		sleep_ms(100);
		if (waitpid(child, &status, WNOHANG) == child) {
			log_value("preboot: drm_linear exited status=", status);
			return -1;
		}
	}

	log_text("preboot: drm_linear is active\n");
	return 0;
}

static int set_status_color(int signal_number)
{
	if (drm_pid <= 0)
		return -1;
	if (kill(drm_pid, signal_number) < 0)
		return -1;
	sleep_ms(200);
	return 0;
}

static int set_status_code(int code)
{
	union sigval value;

	if (drm_pid <= 0)
		return -1;
	value.sival_int = code;
	if (sigqueue(drm_pid, SIGRTMIN, value) < 0)
		return -1;
	sleep_ms(200);
	return 0;
}

static int load_uefi_jump(const char *arguments, int expect_return)
{
	int fd;
	int result;

	fd = open(MODULE_PATH, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		log_value("preboot: open uefi_jump.ko errno=", errno);
		return -1;
	}

	log_text("preboot: calling finit_module\n");
	result = (int)syscall(SYS_finit_module, fd, arguments, 0);
	if (result < 0) {
		module_errno = errno;
		log_value("preboot: finit_module errno=", module_errno);
		close(fd);
		return -1;
	}

	close(fd);
	if (expect_return) {
		log_text("preboot: finit_module returned successfully\n");
		return 0;
	}
	log_text("preboot: finit_module returned unexpectedly\n");
	return -1;
}

static int unload_uefi_jump(void)
{
	int result;

	result = (int)syscall(SYS_delete_module, "uefi_jump", O_NONBLOCK);
	if (result < 0) {
		log_value("preboot: delete_module errno=", errno);
		return -1;
	}
	log_text("preboot: mapdump module unloaded\n");
	return 0;
}

static void halt_forever(void)
{
	log_text("preboot: halted; hold VolDown+Power to enter fastboot\n");
	for (;;)
		pause();
}

int main(void)
{
	int console;

	if (mount_early_filesystems() < 0) {
		halt_forever();
	}

	if (setsid() < 0 && errno != EPERM)
		return 1;

	console = open("/dev/console", O_RDWR | O_CLOEXEC);
	if (console >= 0) {
		dup2(console, STDIN_FILENO);
		dup2(console, STDOUT_FILENO);
		dup2(console, STDERR_FILENO);
	}

	log_fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
	log_text("preboot: camellia PID 1 started\n");

	if (access(FD_PATH, R_OK) < 0) {
		log_value("preboot: UEFI FD missing errno=", errno);
		halt_forever();
	}
	if (access(MODULE_PATH, R_OK) < 0) {
		log_value("preboot: uefi_jump.ko missing errno=", errno);
		halt_forever();
	}

	if (wait_for_drm(30000U) < 0)
		halt_forever();
	if (wait_for_payload_block(30000U) < 0)
		halt_forever();
	if (start_linear_framebuffer() < 0)
		halt_forever();

	sleep_ms(1200);
	if (set_status_color(SIGUSR1) < 0) {
		log_text("preboot: cannot paint green status\n");
		halt_forever();
	}
	if (load_uefi_jump(MAPDUMP_ARGS, 1) < 0) {
		if (module_errno >= 1000 && module_errno <= 4094)
			set_status_code(100000 + (module_errno - 1000));
		else if (module_errno >= 201 && module_errno <= 208)
			set_status_code(module_errno);
		else
			set_status_color(SIGQUIT);
		halt_forever();
	}
	if (set_status_color(SIGALRM) < 0) {
		log_text("preboot: cannot paint cyan status\n");
		halt_forever();
	}
	sleep_ms(1200);
	if (unload_uefi_jump() < 0) {
		set_status_color(SIGQUIT);
		halt_forever();
	}

	if (load_uefi_jump(JUMP_ARGS, 0) < 0) {
		if (module_errno >= 1000 && module_errno <= 4094)
			set_status_code(100000 + (module_errno - 1000));
		else if (module_errno >= 201 && module_errno <= 208)
			set_status_code(module_errno);
		else
			set_status_color(SIGQUIT);
		halt_forever();
	}

	halt_forever();
}
