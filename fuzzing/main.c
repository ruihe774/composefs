#define _GNU_SOURCE

#include "../kernel/lcfs-reader.h"

#include <stddef.h>
#include <fcntl.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include "../tools/read-file.h"

struct test_context_s
{
	struct lcfs_context_s *ctx;
	int recursion_left;
};

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
	return 0;
}

bool iter_cb(void *private, const char *name, int namelen, u64 ino, unsigned int dtype)
{
	struct test_context_s *test_ctx = private;
	struct lcfs_xattr_header_s *xattrs;
	struct lcfs_inode_s *s_ino;
	struct lcfs_inode_s buffer;
	struct lcfs_dir_s *dir;
	ssize_t xattrs_len;
	char names[512];
	loff_t out_size;
	char *out_path;
	char *payload;

	s_ino = lcfs_get_ino_index(test_ctx->ctx, ino, &buffer);
	if (IS_ERR(s_ino))
		return true;

	payload = lcfs_dup_payload_path(test_ctx->ctx, s_ino, 0);
	if (!IS_ERR(payload))
		free(payload);

	xattrs = lcfs_get_xattrs(test_ctx->ctx, s_ino);
	if (!IS_ERR(xattrs)) {
		char value[512];

		xattrs_len = lcfs_list_xattrs(xattrs, names, sizeof(names));
		if (xattrs_len < 0)
			return true;

		/* just retrieve the first name.  */
		lcfs_get_xattr(xattrs, names, value, sizeof(value));
	}

	dir = lcfs_get_dir(test_ctx->ctx, s_ino, 0);
	if (test_ctx->recursion_left > 0 && !IS_ERR(dir)) {
		test_ctx->recursion_left--;
		lcfs_iterate_dir(dir, 0, iter_cb, test_ctx);
		test_ctx->recursion_left++;
	}
	return true;
}

#define min(a,b) ((a)<(b)?(a):(b))

ssize_t safe_write(int fd, const void *buf, ssize_t count)
{
	ssize_t written = 0;
	if (count < 0) {
		errno = EINVAL;
		return -1;
	}
	while (written < count) {
		ssize_t w = write (fd, buf + written, count - written);
		if (w < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			return w;
		}
		written += w;
	}
	return written;
}

static struct lcfs_context_s *create_ctx(uint8_t *buf, size_t len)
{
	struct lcfs_context_s *ctx;
	char proc_path[64];
	int fd;

	fd = open(".", O_TMPFILE | O_RDWR, S_IRUSR | S_IWUSR);
	if (fd < -1)
		return NULL;

	if(safe_write(fd, buf, len) < 0) {
		close(fd);
		return NULL;
	}

	sprintf(proc_path, "/proc/self/fd/%d", fd);
	ctx = lcfs_create_ctx(proc_path);
	close(fd);
	if (IS_ERR(ctx)) {
		return NULL;
	}

	return ctx;
}

int LLVMFuzzerTestOneInput(uint8_t *buf, size_t len)
{
	const size_t max_recursion = 4;
	struct test_context_s test_ctx;
	struct lcfs_context_s *ctx;
	struct lcfs_inode_s ino_buf;
	struct lcfs_inode_s *ino;
	struct lcfs_dir_s *dir;
	char name[NAME_MAX];
	char value[256];
	lcfs_off_t index;
	lcfs_off_t off;
	int fd;

	ctx = create_ctx(buf, len);
	if (ctx == NULL)
		return 0;

	if (len >= sizeof (lcfs_off_t)) {
		off = *((lcfs_off_t *) buf);
		lcfs_get_ino_index(ctx, off, &ino_buf);
	}

	for (off = 0; off < 4; off++) {
		ino = lcfs_get_ino_index(ctx, off, &ino_buf);
		if (!IS_ERR(ino)) {
			struct lcfs_dir_s *dir;

			dir = lcfs_get_dir(ctx, ino, off);
			if (!IS_ERR(dir))
				free(dir);
		}
	}

	ino = lcfs_get_ino_index(ctx, LCFS_ROOT_INODE, &ino_buf);
	if (IS_ERR(ino))
		goto cleanup;

	test_ctx.ctx = ctx;
	test_ctx.recursion_left = max_recursion;

	dir = lcfs_get_dir(ctx, ino, LCFS_ROOT_INODE);
	if (IS_ERR(dir))
		goto cleanup;

	memcpy(name, buf, min(len, NAME_MAX - 1));
	name[min(len, NAME_MAX - 1)] = '\0';
	lcfs_lookup(dir, name, strlen(name), &index);

	lcfs_iterate_dir(dir, 0, iter_cb, &test_ctx);

cleanup:
	lcfs_destroy_ctx(ctx);

	return 0;
}

int main (int argc, char **argv)
{
#ifdef FUZZING_RUN_SINGLE
	size_t i;

	for (i = 1; i < argc; i++) {
		size_t len;
		char *content;

		content = read_file(argv[i], &len);
		if (content == NULL)
			continue;

		LLVMFuzzerTestOneInput(content, len);
		free(content);
	}
#else
	extern void HF_ITER(uint8_t** buf, size_t* len);
	for (;;) {
		size_t len;
		uint8_t *buf;

		HF_ITER(&buf, &len);

		LLVMFuzzerTestOneInput(buf, len);
	}
#endif
	return 0;
}
