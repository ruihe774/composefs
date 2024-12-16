/* lcfs
   Copyright (C) 2023 Alexander Larsson <alexl@redhat.com>

   SPDX-License-Identifier: GPL-2.0-or-later OR Apache-2.0
*/
#define _GNU_SOURCE

#include "config.h"

#include "lcfs-writer.h"
#include "lcfs-mount.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <linux/limits.h>
#include <linux/loop.h>
#include <linux/fsverity.h>

#include <sys/syscall.h>
#include <sys/mount.h>
#ifdef HAVE_FSCONFIG_CMD_CREATE_LINUX_MOUNT_H
#include <linux/mount.h>
#endif
#if defined HAVE_FSCONFIG_CMD_CREATE_LINUX_MOUNT_H ||                          \
	defined HAVE_FSCONFIG_CMD_CREATE_SYS_MOUNT_H
#define HAVE_NEW_MOUNT_API
#endif

#include "lcfs-erofs.h"
#include "lcfs-utils.h"
#include "lcfs-internal.h"

// The "source" field for overlayfs isn't strictly meaningful,
// by default; but it's useful to identify at least the software that
// created the mount.
#define CFS_MOUNT_SOURCE "composefs"

#ifndef LOOP_CONFIGURE
/* Snippet from util-linux/include/loopdev.h */
/*
 * Since Linux v5.8-rc1 (commit 3448914e8cc550ba792d4ccc74471d1ca4293aae)
 */
#define LOOP_CONFIGURE 0x4C0A
struct loop_config {
	uint32_t fd;
	uint32_t block_size;
	struct loop_info64 info;
	uint64_t __reserved[8];
};
#endif

static int syscall_fsopen(const char *fs_name, unsigned int flags)
{
#if defined __NR_fsopen
	return (int)syscall(__NR_fsopen, fs_name, flags);
#else
	(void)fs_name;
	(void)flags;
	errno = ENOSYS;
	return -1;
#endif
}

static int syscall_fsmount(int fsfd, unsigned int flags, unsigned int attr_flags)
{
#if defined __NR_fsmount
	return (int)syscall(__NR_fsmount, fsfd, flags, attr_flags);
#else
	(void)fsfd;
	(void)flags;
	(void)attr_flags;
	errno = ENOSYS;
	return -1;
#endif
}

static int syscall_fsconfig(int fsfd, unsigned int cmd, const char *key,
			    const void *val, int aux)
{
#if defined __NR_fsconfig
	return (int)syscall(__NR_fsconfig, fsfd, cmd, key, val, aux);
#else
	(void)fsfd;
	(void)cmd;
	(void)key;
	(void)val;
	(void)aux;
	errno = ENOSYS;
	return -1;
#endif
}

static int syscall_move_mount(int from_dfd, const char *from_pathname, int to_dfd,
			      const char *to_pathname, unsigned int flags)

{
#if defined __NR_move_mount
	return (int)syscall(__NR_move_mount, from_dfd, from_pathname, to_dfd,
			    to_pathname, flags);
#else
	(void)from_dfd;
	(void)from_pathname;
	(void)to_dfd;
	(void)to_pathname;
	(void)flags;
	errno = ENOSYS;
	return -1;
#endif
}

#ifdef HAVE_MOUNT_ATTR_IDMAP
static int syscall_mount_setattr(int dfd, const char *path, unsigned int flags,
				 struct mount_attr *attr, size_t usize)
{
#ifdef __NR_mount_setattr
	return (int)syscall(__NR_mount_setattr, dfd, path, flags, attr, usize);
#else
	(void)dfd;
	(void)path;
	(void)flags;
	(void)attr;
	errno = ENOSYS;
	return -1;
#endif
}
#endif

struct lcfs_mount_state_s {
	const char *image_path;
	const char *mountpoint;
	struct lcfs_mount_options_s *options;
	int fd;
	uint8_t expected_digest[MAX_DIGEST_SIZE];
	int expected_digest_len;
};

static void escape_mount_option_to(const char *str, char *dest)
{
	const char *s;
	char *d;

	d = dest + strlen(dest);
	for (s = str; *s != 0; s++) {
		if (*s == ',')
			*d++ = '\\';
		*d++ = *s;
	}
	*d++ = 0;
}

static char *escape_mount_option(const char *str)
{
	const char *s;
	char *res;
	int n_escapes = 0;

	for (s = str; *s != 0; s++) {
		if (*s == ',')
			n_escapes++;
	}

	res = malloc(strlen(str) + n_escapes + 1);
	if (res == NULL)
		return NULL;

	*res = 0;

	escape_mount_option_to(str, res);

	return res;
}

static errint_t lcfs_validate_mount_options(struct lcfs_mount_state_s *state)
{
	struct lcfs_mount_options_s *options = state->options;

	if ((options->flags & ~LCFS_MOUNT_FLAGS_MASK) != 0) {
		return -EINVAL;
	}

	if (options->n_objdirs == 0)
		return -EINVAL;

	if ((options->upperdir && !options->workdir) ||
	    (!options->upperdir && options->workdir))
		return -EINVAL;

	if (options->expected_fsverity_digest) {
		int raw_len = digest_to_raw(options->expected_fsverity_digest,
					    state->expected_digest, MAX_DIGEST_SIZE);
		if (raw_len < 0)
			return -EINVAL;
		state->expected_digest_len = raw_len;
	}

	if ((options->flags & LCFS_MOUNT_FLAGS_IDMAP) != 0 && options->idmap_fd < 0) {
		return -EINVAL;
	}

	return 0;
}

static errint_t lcfs_validate_verity_fd(struct lcfs_mount_state_s *state)
{
	int res;

	if (state->expected_digest_len != 0) {
		uint8_t found_digest[LCFS_DIGEST_SIZE];
		res = lcfs_fd_measure_fsverity(found_digest, state->fd);
		if (res < 0) {
			return res;
		}
		if (memcmp(state->expected_digest, found_digest, LCFS_DIGEST_SIZE) != 0)
			return -EWRONGVERITY;
	}

	return 0;
}

static errint_t setup_loopback(int fd, const char *image_path, char *loopname)
{
	struct loop_config loopconfig = { 0 };
	int loopctlfd, loopfd;
	long devnr;
	int errsv;

	loopctlfd = open("/dev/loop-control", O_RDWR | O_CLOEXEC);
	if (loopctlfd < 0)
		return -errno;

	devnr = ioctl(loopctlfd, LOOP_CTL_GET_FREE);
	errsv = errno;
	close(loopctlfd);
	if (devnr == -1) {
		return -errsv;
	}

	sprintf(loopname, "/dev/loop%ld", devnr);
	loopfd = open(loopname, O_RDWR | O_CLOEXEC);
	if (loopfd < 0)
		return -errno;

	loopconfig.fd = fd;
	loopconfig.block_size =
		4096; /* This is what we use for the erofs block size, so probably good */
	loopconfig.info.lo_flags =
		LO_FLAGS_READ_ONLY | LO_FLAGS_DIRECT_IO | LO_FLAGS_AUTOCLEAR;
	if (image_path)
		strncat((char *)loopconfig.info.lo_file_name, image_path,
			LO_NAME_SIZE - 1);

	if (ioctl(loopfd, LOOP_CONFIGURE, &loopconfig) < 0) {
		errsv = errno;
		close(loopfd);
		return -errsv;
	}

	return loopfd;
}

static char *compute_lower(const char *imagemount,
			   struct lcfs_mount_state_s *state, bool with_datalower)
{
	size_t size;
	char *lower;
	size_t i;

	/* Compute the total max size (including escapes) */
	size = 2 * strlen(imagemount);
	for (i = 0; i < state->options->n_objdirs; i++)
		size += 2 + 2 * strlen(state->options->objdirs[i]);

	lower = malloc(size + 1);
	if (lower == NULL)
		return NULL;
	*lower = 0;

	escape_mount_option_to(imagemount, lower);

	for (i = 0; i < state->options->n_objdirs; i++) {
		if (with_datalower)
			strcat(lower, "::");
		else
			strcat(lower, ":");
		escape_mount_option_to(state->options->objdirs[i], lower);
	}

	return lower;
}

static errint_t lcfs_mount_ovl_legacy(struct lcfs_mount_state_s *state, char *imagemount)
{
	struct lcfs_mount_options_s *options = state->options;

	/* Note: We ignore the TRY_VERITY option the VOLATILE option for legacy mounts,
	   as it is hard to check if the option is supported. */

	bool require_verity =
		(options->flags & LCFS_MOUNT_FLAGS_REQUIRE_VERITY) != 0;
	bool readonly = (options->flags & LCFS_MOUNT_FLAGS_READONLY) != 0;

	/* First try new version with :: separating datadirs. */
	cleanup_free char *lowerdir_1 = compute_lower(imagemount, state, true);
	if (lowerdir_1 == NULL) {
		return -ENOMEM;
	}
	/* Can point to lowerdir_1 or _2 */
	const char *lowerdir_target = lowerdir_1;

	/* Then fall back. */
	cleanup_free char *lowerdir_2 = compute_lower(imagemount, state, false);
	if (lowerdir_2 == NULL) {
		return -ENOMEM;
	}

	cleanup_free char *upperdir = NULL;
	if (options->upperdir) {
		upperdir = escape_mount_option(options->upperdir);
		if (upperdir == NULL) {
			return -ENOMEM;
		}
	}
	cleanup_free char *workdir = NULL;
	if (options->workdir) {
		workdir = escape_mount_option(options->workdir);
		if (workdir == NULL)
			return -ENOMEM;
	}

	int res;
	cleanup_free char *overlay_options = NULL;
retry:
	free(steal_pointer(&overlay_options));
	res = asprintf(&overlay_options,
		       "metacopy=on,redirect_dir=on,lowerdir=%s%s%s%s%s%s",
		       lowerdir_target, upperdir ? ",upperdir=" : "",
		       upperdir ? upperdir : "", workdir ? ",workdir=" : "",
		       workdir ? workdir : "",
		       require_verity ? ",verity=require" : "");
	if (res < 0)
		return -ENOMEM;

	int mount_flags = 0;
	if (readonly)
		mount_flags |= MS_RDONLY;
	if (lowerdir_target == lowerdir_1)
		mount_flags |= MS_SILENT;

	errint_t err = 0;
	res = mount(CFS_MOUNT_SOURCE, state->mountpoint, "overlay", mount_flags,
		    overlay_options);
	if (res != 0) {
		err = -errno;
	}

	if (err == -EINVAL && lowerdir_target == lowerdir_1) {
		lowerdir_target = lowerdir_2;
		goto retry;
	}

	return err;
}

static errint_t lcfs_mount_ovl(struct lcfs_mount_state_s *state, char *imagemount)
{
#ifdef HAVE_NEW_MOUNT_API
	struct lcfs_mount_options_s *options = state->options;

	bool require_verity =
		(options->flags & LCFS_MOUNT_FLAGS_REQUIRE_VERITY) != 0;
	bool try_verity = (options->flags & LCFS_MOUNT_FLAGS_TRY_VERITY) != 0;
	bool readonly = (options->flags & LCFS_MOUNT_FLAGS_READONLY) != 0;
	bool try_volatile = (options->flags & LCFS_MOUNT_FLAGS_VOLATILE) != 0;

	cleanup_fd int fd_fs = syscall_fsopen("overlay", FSOPEN_CLOEXEC);
	if (fd_fs < 0)
		return -errno;

	/* Ensure overlayfs is fully supporting the new mount api, not just
	   via the legacy mechanism that doesn't validate options */
	int res = syscall_fsconfig(fd_fs, FSCONFIG_SET_STRING, "unsupported",
				   "unsupported", 0);
	if (res == 0)
		return -ENOSYS;

	res = syscall_fsconfig(fd_fs, FSCONFIG_SET_STRING, "source",
			       CFS_MOUNT_SOURCE, 0);
	if (res < 0)
		return -errno;

	res = syscall_fsconfig(fd_fs, FSCONFIG_SET_STRING, "metacopy", "on", 0);
	if (res < 0)
		return -errno;

	res = syscall_fsconfig(fd_fs, FSCONFIG_SET_STRING, "redirect_dir", "on", 0);
	if (res < 0)
		return -errno;

	if (require_verity || try_verity) {
		res = syscall_fsconfig(fd_fs, FSCONFIG_SET_STRING, "verity",
				       "require", 0);
		if (res < 0 && require_verity)
			return -errno;
	}

	if (try_volatile) {
		res = syscall_fsconfig(fd_fs, FSCONFIG_SET_FLAG, "volatile", NULL, 0);
		if (res < 0) {
			// It's okay to ignore it if the option is not supported,
			// giving it's just an optimization.
		}
	}

	/* Here we're using the new mechanism to append to lowerdir that was added in
	 * 6.7 (24e16e385f227), because that is the only way to handle escaping
	 * of commas (i.e. we don't need to in this case) with the new mount api.
	 * Also, since 6.7 has data-only lowerdir support we can just always use it.
	 *
	 * For older kernels a lack of append support will make the mount fail with EINVAL,
	 * and a lack of comma in the options will cause the fsconfig to fail with EINVAL. If any of these happen we fall back to
	 * the legacy implementation (via ENOSYS).
	 */
	res = syscall_fsconfig(fd_fs, FSCONFIG_SET_STRING, "lowerdir+",
			       imagemount, 0);
	if (res < 0) {
		/* EINVAL lack of support for appending as per above, fallback */
		if (errno == EINVAL)
			return -ENOSYS;
		return -errno;
	}

	for (size_t i = 0; i < state->options->n_objdirs; i++) {
		const char *objdir = state->options->objdirs[i];
		res = syscall_fsconfig(fd_fs, FSCONFIG_SET_STRING, "datadir+",
				       objdir, 0);
		if (res < 0) {
			/* EINVAL lack of support for appending as per above, fallback */
			if (errno == EINVAL)
				return -ENOSYS;
			return -errno;
		}
	}

	if (options->upperdir) {
		res = syscall_fsconfig(fd_fs, FSCONFIG_SET_STRING, "upperdir",
				       options->upperdir, 0);
		if (res < 0) {
			/* EINVAL lack of support for appending as per above, fallback */
			if (errno == EINVAL)
				return -ENOSYS;
			return -errno;
		}
	}
	if (options->workdir) {
		res = syscall_fsconfig(fd_fs, FSCONFIG_SET_STRING, "workdir",
				       options->workdir, 0);
		if (res < 0) {
			/* EINVAL probably the lack of support for commas in options as per above, fallback */
			if (errno == EINVAL)
				return -ENOSYS;
			return -errno;
		}
	}

	res = syscall_fsconfig(fd_fs, FSCONFIG_CMD_CREATE, NULL, NULL, 0);
	if (res < 0) {
		/* EINVAL probably the lack of support for dataonly dirs as per above, fallback */
		if (errno == EINVAL)
			return -ENOSYS;
		return -errno;
	}

	int mount_flags = 0;
	if (readonly)
		mount_flags |= MS_RDONLY;

	cleanup_fd int fd_mnt = syscall_fsmount(fd_fs, FSMOUNT_CLOEXEC, mount_flags);
	if (fd_mnt < 0)
		return -errno;

	res = syscall_move_mount(fd_mnt, "", AT_FDCWD, state->mountpoint,
				 MOVE_MOUNT_F_EMPTY_PATH);
	if (res < 0)
		return -errno;

	return 0;
#else
	return -ENOSYS;
#endif
}

static errint_t lcfs_mount_erofs(const char *source, const char *target,
				 uint32_t image_flags,
				 struct lcfs_mount_state_s *state)
{
	bool image_has_acls = (image_flags & LCFS_EROFS_FLAGS_HAS_ACL) != 0;
	bool use_idmap = (state->options->flags & LCFS_MOUNT_FLAGS_IDMAP) != 0;
	int res;

#ifdef HAVE_NEW_MOUNT_API
	/* We have new mount API is in header */
	cleanup_fd int fd_fs = -1;
	cleanup_fd int fd_mnt = -1;

	fd_fs = syscall_fsopen("erofs", FSOPEN_CLOEXEC);
	if (fd_fs < 0) {
		if (errno == ENOSYS)
			goto fallback;
		return -errno;
	}

	res = syscall_fsconfig(fd_fs, FSCONFIG_SET_STRING, "source", source, 0);
	if (res < 0)
		return -errno;

	res = syscall_fsconfig(fd_fs, FSCONFIG_SET_FLAG, "ro", NULL, 0);
	if (res < 0)
		return -errno;

	if (!image_has_acls) {
		res = syscall_fsconfig(fd_fs, FSCONFIG_SET_FLAG, "noacl", NULL, 0);
		if (res < 0)
			return -errno;
	}

	res = syscall_fsconfig(fd_fs, FSCONFIG_CMD_CREATE, NULL, NULL, 0);
	if (res < 0)
		return -errno;

	fd_mnt = syscall_fsmount(fd_fs, FSMOUNT_CLOEXEC, MS_RDONLY);
	if (fd_mnt < 0)
		return -errno;

	if (use_idmap) {
#ifdef HAVE_MOUNT_ATTR_IDMAP
		struct mount_attr attr = {
			.attr_set = MOUNT_ATTR_IDMAP,
			.userns_fd = state->options->idmap_fd,
		};

		res = syscall_mount_setattr(fd_mnt, "", AT_EMPTY_PATH, &attr,
					    sizeof(struct mount_attr));
		if (res < 0)
			return -errno;
#else
		return -ENOTSUP;
#endif
	}

	res = syscall_move_mount(fd_mnt, "", AT_FDCWD, target,
				 MOVE_MOUNT_F_EMPTY_PATH);
	if (res < 0)
		return -errno;

	return 0;

fallback:
#endif

	/* We need new mount api for idmapped mounts */
	if (use_idmap)
		return -ENOTSUP;

	res = mount(source, target, "erofs", MS_RDONLY,
		    image_has_acls ? NULL : "noacl");
	if (res < 0)
		return -errno;

	return 0;
}

#define HEADER_SIZE sizeof(struct lcfs_erofs_header_s)

static errint_t lcfs_mount_erofs_ovl(struct lcfs_mount_state_s *state,
				     struct lcfs_erofs_header_s *header)
{
	struct lcfs_mount_options_s *options = state->options;
	uint32_t image_flags;
	char imagemountbuf[] = "/tmp/.composefs.XXXXXX";
	char *imagemount;
	bool created_tmpdir = false;
	char loopname[PATH_MAX];
	int errsv;
	errint_t err;
	int loopfd;

	image_flags = lcfs_u32_from_file(header->flags);

	loopfd = setup_loopback(state->fd, state->image_path, loopname);
	if (loopfd < 0)
		return loopfd;

	if (options->image_mountdir) {
		imagemount = (char *)options->image_mountdir;
	} else {
		imagemount = mkdtemp(imagemountbuf);
		if (imagemount == NULL) {
			errsv = errno;
			close(loopfd);
			return -errsv;
		}
		created_tmpdir = true;
	}

	err = lcfs_mount_erofs(loopname, imagemount, image_flags, state);
	close(loopfd);
	if (err < 0) {
		rmdir(imagemount);
		return err;
	}

	/* We use the legacy API to mount overlayfs, because the new API doesn't allow use
	 * to pass in escaped directory names
	 */
	err = lcfs_mount_ovl(state, imagemount);
	if (err == -ENOSYS)
		err = lcfs_mount_ovl_legacy(state, imagemount);

	umount2(imagemount, MNT_DETACH);
	if (created_tmpdir) {
		rmdir(imagemount);
	}

	return err;
}

static errint_t lcfs_mount(struct lcfs_mount_state_s *state)
{
	uint8_t header_data[HEADER_SIZE];
	struct lcfs_erofs_header_s *erofs_header;
	int err;
	int res;

	err = lcfs_validate_verity_fd(state);
	if (err < 0)
		return err;

	res = pread(state->fd, &header_data, HEADER_SIZE, 0);
	if (res < 0)
		return -errno;
	else if (res != HEADER_SIZE)
		return -EINVAL;

	erofs_header = (struct lcfs_erofs_header_s *)header_data;
	if (lcfs_u32_from_file(erofs_header->magic) == LCFS_EROFS_MAGIC)
		return lcfs_mount_erofs_ovl(state, erofs_header);

	return -EINVAL;
}

int lcfs_mount_fd(int fd, const char *mountpoint, struct lcfs_mount_options_s *options)
{
	struct lcfs_mount_state_s state = { .mountpoint = mountpoint,
					    .options = options,
					    .fd = fd };
	errint_t err;
	int res;

	err = lcfs_validate_mount_options(&state);
	if (err < 0) {
		errno = -err;
		return -1;
	}

	res = lcfs_mount(&state);
	if (res < 0) {
		errno = -res;
		return -1;
	}
	return 0;
}

int lcfs_mount_image(const char *path, const char *mountpoint,
		     struct lcfs_mount_options_s *options)
{
	struct lcfs_mount_state_s state = { .image_path = path,
					    .mountpoint = mountpoint,
					    .options = options,
					    .fd = -1 };
	errint_t err;
	int fd, res;

	err = lcfs_validate_mount_options(&state);
	if (err < 0) {
		errno = -err;
		return -1;
	}

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		return -1;
	}
	state.fd = fd;

	res = lcfs_mount(&state);
	close(fd);
	if (res < 0) {
		errno = -res;
		return -1;
	}

	return 0;
}
