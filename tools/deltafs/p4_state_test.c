// SPDX-License-Identifier: GPL-2.0
/*
 * DeltaFS P4 state builder test.
 *
 * A valid P4 request must build a complete target state, release it through
 * the shared free helper, and return -EOPNOTSUPP without changing the active
 * OverlayFS view.  This program exercises the build/free success path, the
 * feature/path/invariant error matrix, active-view stability, trap reuse from
 * the active layers, and the 1/8/64 lower-layer boundaries.
 *
 * Run as root in a QEMU/KVM guest:
 *
 *   ./p4_state_test MERGED SCRATCH ACT_UPPER ACT_WORK ACT_LOWER [ACT_LOWER...]
 *
 * MERGED     mounted, writable OverlayFS root (its layers must all sit on the
 *            same backing filesystem as SCRATCH and the ACT_* directories).
 * SCRATCH    writable, non-overlay directory on the same backing filesystem;
 *            each test creates its own fresh branch underneath it.
 * ACT_UPPER  upperdir of the active overlay.
 * ACT_WORK   workdir (work base) of the active overlay.
 * ACT_LOWER  one lowerdir of the active overlay (P4 checkpoint reuses these as
 *            the new view's lowers).
 *
 * Optional:
 *   DELTAFS_P4_WRONG_BACKING=PATH   directory on a *different* filesystem
 *                                   (e.g. a tmpfs) to exercise -EXDEV.
 *   DELTAFS_P4_ACTIVE_MARKER        file under MERGED whose contents must be
 *                                   unchanged after every build (default:
 *                                   deltafs-p4-active-marker).
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/deltafs.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAX_LOWER DELTAFS_V1_MAX_LOWERS
#define ACTIVE_MARKER_DEFAULT "deltafs-p4-active-marker"
#define ACTIVE_PAYLOAD "deltafs-p4-active-unchanged"

static int failures;

static void fail(const char *fmt, ...)
{
	va_list ap;

	failures++;
	va_start(ap, fmt);
	fprintf(stderr, "FAIL: ");
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

static void passf(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	fputs("PASS: ", stdout);
	vprintf(fmt, ap);
	fputc('\n', stdout);
	va_end(ap);
}

static void skipf(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	fputs("SKIP: ", stdout);
	vprintf(fmt, ap);
	fputc('\n', stdout);
	va_end(ap);
}

static void expect_errno(const char *label, int root_fd, unsigned long cmd,
			 const struct deltafs_ioc_switch_v1 *req, int expected)
{
	int ret;

	errno = 0;
	ret = ioctl(root_fd, cmd, req);
	if (ret == -1 && errno == expected) {
		passf("%s: %s", label, strerror(expected));
		return;
	}

	if (ret == -1)
		fail("%s: expected %s, got %s", label, strerror(expected),
		     strerror(errno));
	else
		fail("%s: expected %s, got success", label,
		     strerror(expected));
}

static void build_init(struct deltafs_ioc_switch_v1 *req, int upper_fd,
		       int work_fd, int nr_lower, const int *lower_fds,
		       __u64 expected_generation)
{
	int i;

	memset(req, 0, sizeof(*req));
	req->size = sizeof(*req);
	req->version = DELTAFS_ABI_VERSION;
	req->expected_generation = expected_generation;
	req->upper_fd = upper_fd;
	req->work_fd = work_fd;
	req->nr_lower = nr_lower;
	for (i = 0; i < MAX_LOWER; i++)
		req->lower_fds[i] = -1;
	for (i = 0; i < nr_lower && i < MAX_LOWER; i++)
		req->lower_fds[i] = lower_fds[i];
}

static int make_dir_at(int dirfd, const char *name)
{
	if (mkdirat(dirfd, name, 0700) < 0)
		return -errno;
	return 0;
}

static int open_dir_at(int dirfd, const char *name)
{
	int fd = openat(dirfd, name, O_PATH | O_DIRECTORY | O_CLOEXEC);

	if (fd < 0)
		return -errno;
	return fd;
}

/* A fresh DeltaFS branch: new upper/work and the requested number of new
 * lowers, all created under a unique subtree in SCRATCH. */
struct fresh {
	int sub_fd;
	int upper_fd;
	int work_fd;
	int lower_fd[MAX_LOWER];
	int nr_lower;
	bool populated_upper;
};

static void fresh_close(struct fresh *f)
{
	int i;

	if (!f)
		return;
	for (i = 0; i < MAX_LOWER; i++)
		if (f->lower_fd[i] >= 0)
			close(f->lower_fd[i]);
	if (f->work_fd >= 0)
		close(f->work_fd);
	if (f->upper_fd >= 0)
		close(f->upper_fd);
	if (f->sub_fd >= 0)
		close(f->sub_fd);
}

static unsigned int fresh_seq;

static int fresh_create(int scratch_fd, int nr_lower, bool populate_upper,
			struct fresh *f)
{
	char name[48];
	int err, fd, i;

	memset(f, 0, sizeof(*f));
	f->sub_fd = -1;
	f->upper_fd = -1;
	f->work_fd = -1;
	for (i = 0; i < MAX_LOWER; i++)
		f->lower_fd[i] = -1;

	/* Seed once per process so the linear scan for a free branch name starts
	 * well past any branch-* a previous invocation left in the reused scratch
	 * directory. */
	if (!fresh_seq)
		fresh_seq = (((unsigned int)getpid() << 8) ^
			     (unsigned int)time(NULL)) | 1u;

	/* The scratch dir may be reused across process invocations, so skip any
	 * branch-* name that a previous run left behind. */
	for (;;) {
		snprintf(name, sizeof(name), "branch-%u", fresh_seq++);
		err = make_dir_at(scratch_fd, name);
		if (err && err != -EEXIST)
			return err;
		if (!err)
			break;
	}
	f->sub_fd = open_dir_at(scratch_fd, name);
	if (f->sub_fd < 0) {
		err = f->sub_fd;
		goto fail;
	}

	err = make_dir_at(f->sub_fd, "upper");
	if (err)
		goto fail;
	f->upper_fd = open_dir_at(f->sub_fd, "upper");
	if (f->upper_fd < 0) {
		err = f->upper_fd;
		goto fail;
	}

	err = make_dir_at(f->sub_fd, "work");
	if (err)
		goto fail;
	f->work_fd = open_dir_at(f->sub_fd, "work");
	if (f->work_fd < 0) {
		err = f->work_fd;
		goto fail;
	}

	if (populate_upper) {
		fd = openat(f->upper_fd, "leftover", O_CREAT | O_WRONLY | O_CLOEXEC,
			    0644);
		if (fd < 0) {
			err = -errno;
			goto fail;
		}
		close(fd);
		f->populated_upper = true;
	}

	for (i = 0; i < nr_lower; i++) {
		char lname[32];

		snprintf(lname, sizeof(lname), "lower%d", i);
		err = make_dir_at(f->sub_fd, lname);
		if (err)
			goto fail;
		f->lower_fd[i] = open_dir_at(f->sub_fd, lname);
		if (f->lower_fd[i] < 0) {
			err = f->lower_fd[i];
			goto fail;
		}
	}
	f->nr_lower = nr_lower;

	return 0;

fail:
	fresh_close(f);
	return err;
}

/* Build with a fresh branch; on success the helper still owns nothing because
 * P4 releases the state in-kernel.  Returns a positive errno. */
static int build_once(int root_fd, unsigned long cmd, int scratch_fd,
		      int nr_lower, __u64 expected_generation)
{
	struct deltafs_ioc_switch_v1 req;
	struct fresh f;
	int err;

	err = fresh_create(scratch_fd, nr_lower, false, &f);
	if (err) {
		fail("fresh_create: %s", strerror(-err));
		return -err;
	}
	build_init(&req, f.upper_fd, f.work_fd, nr_lower, f.lower_fd,
		   expected_generation);
	errno = 0;
	ioctl(root_fd, cmd, &req);
	err = errno;
	fresh_close(&f);
	return err;
}

static bool active_marker_read(int merged_fd, const char *marker,
			       char *buf, size_t size)
{
	int fd;
	ssize_t n;

	fd = openat(merged_fd, marker, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		skipf("active marker %s not present", marker);
		return false;
	}
	n = read(fd, buf, size - 1);
	close(fd);
	if (n < 0)
		return false;
	buf[n] = '\0';
	return true;
}

int main(int argc, char **argv)
{
	const char *wrong_backing;
	const char *marker = getenv("DELTAFS_P4_ACTIVE_MARKER") ?: ACTIVE_MARKER_DEFAULT;
	struct deltafs_ioc_switch_v1 req;
	int active_lower[MAX_LOWER] = { [0 ... MAX_LOWER - 1] = -1 };
	char marker_before[256] = "";
	char marker_after[256] = "";
	int root_fd = -1, scratch_fd = -1, act_upper_fd = -1, act_work_fd = -1;
	int wb_fd = -1, wb_a_fd = -1, wb_b_fd = -1;
	int nr_active_lower, active_layer_count;
	int nonpath_fd = -1;
	bool have_marker = false;
	int err, i;

	if (argc < 6 || argc > 5 + MAX_LOWER) {
		fprintf(stderr,
			"Usage: %s MERGED SCRATCH ACT_UPPER ACT_WORK ACT_LOWER [ACT_LOWER...]\n",
			argv[0]);
		return EXIT_FAILURE;
	}

	nr_active_lower = argc - 5;
	active_layer_count = nr_active_lower + 1; /* includes active upper */

	root_fd = open(argv[1], O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (root_fd < 0) {
		fprintf(stderr, "open merged root %s: %s\n", argv[1], strerror(errno));
		return EXIT_FAILURE;
	}
	scratch_fd = open(argv[2], O_PATH | O_DIRECTORY | O_CLOEXEC);
	if (scratch_fd < 0) {
		fprintf(stderr, "open scratch %s: %s\n", argv[2], strerror(errno));
		return EXIT_FAILURE;
	}
	act_upper_fd = open(argv[3], O_PATH | O_DIRECTORY | O_CLOEXEC);
	act_work_fd = open(argv[4], O_PATH | O_DIRECTORY | O_CLOEXEC);
	if (act_upper_fd < 0 || act_work_fd < 0) {
		fprintf(stderr, "open active upper/work: %s\n", strerror(errno));
		return EXIT_FAILURE;
	}
	for (i = 0; i < nr_active_lower; i++) {
		active_lower[i] = open(argv[5 + i], O_PATH | O_DIRECTORY | O_CLOEXEC);
		if (active_lower[i] < 0) {
			fprintf(stderr, "open active lower %s: %s\n", argv[5 + i],
				strerror(errno));
			return EXIT_FAILURE;
		}
	}

	/*
	 * Fault-injection mode: drive a single restore build and report the errno
	 * without asserting it.  fail_function may inject an error at any build
	 * checkpoint, so any errno is acceptable here; the shell harness relies on
	 * kmemleak/dmesg (not this program) to catch leaks or corruption.  This
	 * keeps the strict assertion matrix above out of the injected path.
	 */
	if (getenv("DELTAFS_P4_FAULT_BUILD")) {
		err = build_once(root_fd, DELTAFS_IOC_RESTORE, scratch_fd, 1, 1);
		printf("FAULT_BUILD_ERRNO=%d (%s)\n", err, strerror(err));
		return EXIT_SUCCESS;
	}

	/* -- Successful build/free: valid requests return -EOPNOTSUPP. -- */

	/* Restore with fresh lowers reuses no active trap and must build+free. */
	err = build_once(root_fd, DELTAFS_IOC_RESTORE, scratch_fd, 1, 1);
	if (err == EOPNOTSUPP)
		passf("valid restore builds and frees (1 fresh lower)");
	else
		fail("valid restore: expected EOPNOTSUPP, got %s", strerror(err));

	/* Checkpoint reuses the active layer roots' traps as its lowers. */
	{
		int lower[MAX_LOWER] = { [0 ... MAX_LOWER - 1] = -1 };

		lower[0] = act_upper_fd;
		for (i = 0; i < nr_active_lower; i++)
			lower[i + 1] = active_lower[i];
		build_init(&req, /*upper*/ -1, /*work*/ -1, active_layer_count,
			   lower, 1);
	}
	{
		struct fresh f;

		err = fresh_create(scratch_fd, 0, false, &f);
		if (err) {
			fail("fresh_create: %s", strerror(-err));
		} else {
			req.upper_fd = f.upper_fd;
			req.work_fd = f.work_fd;
			expect_errno("valid checkpoint reuses active traps",
				     root_fd, DELTAFS_IOC_CHECKPOINT, &req,
				     EOPNOTSUPP);
			fresh_close(&f);
		}
	}

	/* -- Feature/path/invariant errors. -- */

	/* Non-O_PATH upper fd: fget_raw returns a non-FMODE_PATH file. */
	{
		struct fresh f;

		err = fresh_create(scratch_fd, 1, false, &f);
		if (err) {
			fail("fresh_create: %s", strerror(-err));
		} else {
			nonpath_fd = openat(f.sub_fd, "upper", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
			if (nonpath_fd < 0) {
				fail("open non-path upper: %s", strerror(errno));
			} else {
				build_init(&req, nonpath_fd, f.work_fd, 1, f.lower_fd, 1);
				expect_errno("non-O_PATH upper fd", root_fd,
					     DELTAFS_IOC_RESTORE, &req, EINVAL);
				close(nonpath_fd);
			}
			fresh_close(&f);
		}
	}

	/* Duplicate upper == work root. */
	{
		struct fresh f;

		err = fresh_create(scratch_fd, 1, false, &f);
		if (err) {
			fail("fresh_create: %s", strerror(-err));
		} else {
			build_init(&req, f.upper_fd, f.upper_fd, 1, f.lower_fd, 1);
			expect_errno("duplicate upper and work", root_fd,
				     DELTAFS_IOC_RESTORE, &req, EINVAL);
			fresh_close(&f);
		}
	}

	/* Overlap: upper is an ancestor of a lower. */
	{
		struct fresh f;
		int child_fd = -1;

		err = fresh_create(scratch_fd, 0, false, &f);
		if (err) {
			fail("fresh_create: %s", strerror(-err));
		} else {
			err = make_dir_at(f.upper_fd, "nested");
			if (err)
				fail("mkdir nested lower: %s", strerror(-err));
			else {
				child_fd = open_dir_at(f.upper_fd, "nested");
				if (child_fd < 0)
					fail("open nested lower: %s", strerror(-child_fd));
				else {
					int lowers[1] = { child_fd };

					build_init(&req, f.upper_fd, f.work_fd, 1, lowers, 1);
					expect_errno("overlapping upper/lower", root_fd,
						     DELTAFS_IOC_RESTORE, &req, EINVAL);
				}
			}
			if (child_fd >= 0)
				close(child_fd);
			fresh_close(&f);
		}
	}

	/* Non-empty fresh upper: controller did not clean the branch. */
	{
		struct fresh f;

		err = fresh_create(scratch_fd, 1, true, &f);
		if (err) {
			fail("fresh_create: %s", strerror(-err));
		} else {
			build_init(&req, f.upper_fd, f.work_fd, 1, f.lower_fd, 1);
			expect_errno("non-empty fresh upper", root_fd,
				     DELTAFS_IOC_RESTORE, &req, ENOTEMPTY);
			fresh_close(&f);
		}
	}

	/* Restore invariant: a lower must not equal the active upper. */
	{
		int lowers[1] = { act_upper_fd };

		build_init(&req, /*upper*/ -1, /*work*/ -1, 1, lowers, 1);
	}
	{
		struct fresh f;

		err = fresh_create(scratch_fd, 0, false, &f);
		if (err) {
			fail("fresh_create: %s", strerror(-err));
		} else {
			req.upper_fd = f.upper_fd;
			req.work_fd = f.work_fd;
			expect_errno("restore lower is active upper", root_fd,
				     DELTAFS_IOC_RESTORE, &req, EINVAL);
			fresh_close(&f);
		}
	}

	/* Checkpoint invariant: lower count must equal the active layer count. */
	{
		struct fresh f;

		err = fresh_create(scratch_fd, active_layer_count - 1, false, &f);
		if (err) {
			fail("fresh_create: %s", strerror(-err));
		} else {
			/* Fresh lowers that do not match the active layers. */
			int lowers[MAX_LOWER] = { [0 ... MAX_LOWER - 1] = -1 };

			for (i = 0; i < active_layer_count - 1; i++)
				lowers[i] = f.lower_fd[i];
			build_init(&req, f.upper_fd, f.work_fd,
				   active_layer_count - 1, lowers, 1);
			expect_errno("checkpoint wrong lower count", root_fd,
				     DELTAFS_IOC_CHECKPOINT, &req, EINVAL);
			fresh_close(&f);
		}
	}

	/* Checkpoint invariant: lowers must match the active layers in order. */
	{
		struct fresh f;

		err = fresh_create(scratch_fd, active_layer_count, false, &f);
		if (err) {
			fail("fresh_create: %s", strerror(-err));
		} else {
			build_init(&req, f.upper_fd, f.work_fd, active_layer_count,
				   f.lower_fd, 1);
			expect_errno("checkpoint mismatched lowers", root_fd,
				     DELTAFS_IOC_CHECKPOINT, &req, EINVAL);
			fresh_close(&f);
		}
	}

	/* Stale expected generation. */
	err = build_once(root_fd, DELTAFS_IOC_RESTORE, scratch_fd, 1, 2);
	if (err == ESTALE)
		passf("stale expected generation");
	else
		fail("stale generation: expected ESTALE, got %s", strerror(err));

	/* -- Active view stability and generation unchanged after a build. -- */
	have_marker = active_marker_read(root_fd, marker, marker_before,
					 sizeof(marker_before));

	err = build_once(root_fd, DELTAFS_IOC_RESTORE, scratch_fd, 1, 1);
	if (err != EOPNOTSUPP)
		fail("active-view probe build: expected EOPNOTSUPP, got %s", strerror(err));

	if (have_marker) {
		active_marker_read(root_fd, marker, marker_after,
				   sizeof(marker_after));
		if (strcmp(marker_before, marker_after) == 0)
			passf("active view unchanged after build");
		else
			fail("active view changed: '%s' -> '%s'", marker_before,
			     marker_after);
	}

	/* Generation must still accept the original value (P4 does not bump it)
	 * and still reject a wrong value. */
	err = build_once(root_fd, DELTAFS_IOC_RESTORE, scratch_fd, 1, 1);
	if (err == EOPNOTSUPP)
		passf("original generation still accepted after build");
	else
		fail("post-build retry: expected EOPNOTSUPP, got %s", strerror(err));
	err = build_once(root_fd, DELTAFS_IOC_RESTORE, scratch_fd, 1, 2);
	if (err == ESTALE)
		passf("wrong generation still rejected after build");
	else
		fail("post-build stale: expected ESTALE, got %s", strerror(err));

	/* -- Lower-layer boundaries 1 / 8 / 64 (restore, fresh lowers). -- */
	{
		int boundaries[3] = { 1, 8, MAX_LOWER };

		for (i = 0; i < 3; i++) {
			err = build_once(root_fd, DELTAFS_IOC_RESTORE, scratch_fd,
					 boundaries[i], 1);
			if (err == EOPNOTSUPP)
				passf("restore builds with %d lower(s)", boundaries[i]);
			else
				fail("restore %d lowers: expected EOPNOTSUPP, got %s",
				     boundaries[i], strerror(err));
		}
	}

	/* -- Repeated build/free lifecycle (trap reuse, no leak). -- */
	for (i = 0; i < 8; i++) {
		err = build_once(root_fd, DELTAFS_IOC_RESTORE, scratch_fd, 2, 1);
		if (err != EOPNOTSUPP) {
			fail("lifecycle iter %d: expected EOPNOTSUPP, got %s",
			     i, strerror(err));
			break;
		}
	}
	if (i == 8)
		passf("repeated build/free lifecycle (8 iterations)");

	/* -- Wrong backing filesystem (-EXDEV), if a second fs is provided. -- */
	wrong_backing = getenv("DELTAFS_P4_WRONG_BACKING");
	if (!wrong_backing || !*wrong_backing) {
		skipf("EXDEV test: set DELTAFS_P4_WRONG_BACKING to a dir on another fs");
	} else {
		wb_fd = open(wrong_backing, O_PATH | O_DIRECTORY | O_CLOEXEC);
		if (wb_fd < 0) {
			skipf("EXDEV test: cannot open %s: %s", wrong_backing,
			      strerror(errno));
		} else {
			/* EEXIST is fine: a previous run may have left the dirs. */
			err = make_dir_at(wb_fd, "p4a");
			if (err && err != -EEXIST) {
				skipf("EXDEV test: cannot mkdir p4a: %s", strerror(-err));
				err = 0;
			} else {
				err = make_dir_at(wb_fd, "p4b");
				if (err && err != -EEXIST) {
					skipf("EXDEV test: cannot mkdir p4b: %s", strerror(-err));
				} else {
					err = 0;
				}
			}
			if (!err) {
				wb_a_fd = open_dir_at(wb_fd, "p4a");
				wb_b_fd = open_dir_at(wb_fd, "p4b");
				if (wb_a_fd < 0 || wb_b_fd < 0) {
					skipf("EXDEV test: cannot open wrong-backing dirs");
				} else {
					/* Fresh lower on the real backing keeps nr_lower>=1. */
					struct fresh f;

					err = fresh_create(scratch_fd, 1, false, &f);
					if (err) {
						fail("fresh_create: %s", strerror(-err));
					} else {
						build_init(&req, wb_a_fd, wb_b_fd, 1, f.lower_fd, 1);
						expect_errno("upper on wrong backing fs", root_fd,
							     DELTAFS_IOC_RESTORE, &req, EXDEV);
						fresh_close(&f);
					}
				}
			}
		}
	}

	if (wb_b_fd >= 0)
		close(wb_b_fd);
	if (wb_a_fd >= 0)
		close(wb_a_fd);
	if (wb_fd >= 0)
		close(wb_fd);
	for (i = 0; i < MAX_LOWER; i++)
		if (active_lower[i] >= 0)
			close(active_lower[i]);
	if (act_work_fd >= 0)
		close(act_work_fd);
	if (act_upper_fd >= 0)
		close(act_upper_fd);
	if (scratch_fd >= 0)
		close(scratch_fd);
	if (root_fd >= 0)
		close(root_fd);

	if (failures) {
		fprintf(stderr, "%d test(s) failed\n", failures);
		return EXIT_FAILURE;
	}
	puts("All P4 state builder tests passed");
	return EXIT_SUCCESS;
}
