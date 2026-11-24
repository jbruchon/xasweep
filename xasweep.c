/* XFS excess allocation sweep - strips pre-over-allocation to reclaim space
 * Copyright (C) 2026 by Jody Lee Bruchon <jody@jodybruchon.com>
 * Distributed under the GNU General Public License v2 */

#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>


/* Default minimum excess allocation threshold 2 4K blocks */
#define DEFAULT_MIN_EXCESS (2 * 4096)
#define STAT_BLOCK_SIZE 512


static int dry_run = 0;
static int verbose = 0;
static int64_t min_excess = DEFAULT_MIN_EXCESS;

static uint64_t total_files = 0;
static uint64_t total_cleared = 0;
static int prog_counter = 0;
static int exit_status = 0;

static void raise_fd_limit(void) {
	struct rlimit rl;
	if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < rl.rlim_max) {
		rl.rlim_cur = rl.rlim_max;
		setrlimit(RLIMIT_NOFILE, &rl);
	}
}

static void update_progress(void) {
	prog_counter++;
	if (prog_counter >= 64) {
		prog_counter = 0;
		printf("Processed %" PRIu64 " files\r", total_files);
		fflush(stdout);
	}
}

static void process_file(int dirfd, const char *name, const char *display_path,
						 const struct stat *sb) {
	total_files++;
	if (!verbose) update_progress();

	/* Skip files smaller than min_excess (mirrors `find -size +${MIN}c`) */
	if ((int64_t)sb->st_size <= min_excess) return;

	int64_t alloc_bytes = (int64_t)sb->st_blocks * STAT_BLOCK_SIZE;
	int64_t diff = alloc_bytes - (int64_t)sb->st_size;

	if (verbose) {
		printf("debug: %" PRId64 " * %d = %" PRId64 " - %" PRId64 " = %" PRId64 " (file '%s')\n",
			   (int64_t)sb->st_blocks, STAT_BLOCK_SIZE,
			   alloc_bytes, (int64_t)sb->st_size,
			   diff, display_path);
	}

	if (diff < min_excess) return;

	if (!verbose) printf("\r\033[K");
	printf("%" PRId64 " %s\n", diff, display_path);

	if (dry_run) {
		printf("truncate -cr \"%s\" \"%s\"\n", display_path, display_path);
		total_cleared += (uint64_t)diff;
		return;
	}

	/* Open via dirfd without following symlinks and without updating atime */
	int fd = openat(dirfd, name, O_WRONLY | O_NOFOLLOW | O_NOATIME);
	if (fd < 0 && errno == EPERM) fd = openat(dirfd, name, O_WRONLY | O_NOFOLLOW);
	if (fd < 0) {
		fprintf(stderr, "Error opening %s for truncate: %s\n", display_path, strerror(errno));
		exit_status = 1;
		return;
	}

	if (ftruncate(fd, sb->st_size) != 0) {
		fprintf(stderr, "Error truncating %s: %s\n", display_path, strerror(errno));
		close(fd);
		exit_status = 1;
		return;
	}

	/* Restore nanosecond access and modification timestamps on the open fd */
	struct timespec times[2] = { sb->st_atim, sb->st_mtim };
	if (futimens(fd, times) != 0) {
		fprintf(stderr, "Warning: failed to restore timestamps for %s: %s\n",
				display_path, strerror(errno));
		exit_status = 1;
	}

	close(fd);
	total_cleared += (uint64_t)diff;
}


static void walk_dir(int dirfd, const char *display_prefix, dev_t root_dev) {
	DIR *dp = fdopendir(dirfd);
	if (!dp) {
		fprintf(stderr, "Error reading directory %s: %s\n", display_prefix, strerror(errno));
		close(dirfd);
		exit_status = 1;
		return;
	}

	size_t prefix_len = strlen(display_prefix);
	int needs_sep = (prefix_len > 0 && display_prefix[prefix_len - 1] != '/');

	struct dirent *entry;
	for (;;) {
		errno = 0;
		entry = readdir(dp);
		if (!entry) {
			if (errno != 0) {
				fprintf(stderr, "Error reading directory %s: %s\n", display_prefix, strerror(errno));
				exit_status = 1;
			}
			break;
		}

		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

		/* Fast-path skip obvious non-regular/non-dir types when d_type is populated.
		 * DT_UNKNOWN (e.g. XFS v4) falls through to fstatat() below. */
		if (entry->d_type != DT_UNKNOWN &&
			entry->d_type != DT_REG &&
			entry->d_type != DT_DIR) {
			continue;
		}

		struct stat sb;
		if (fstatat(dirfd, entry->d_name, &sb, AT_SYMLINK_NOFOLLOW) != 0) {
			fprintf(stderr, "Error stating %s/%s: %s\n",
					display_prefix, entry->d_name, strerror(errno));
			exit_status = 1;
			continue;
		}

		/* Never follow symlinks; ignore special files */
		if (!S_ISDIR(sb.st_mode) && !S_ISREG(sb.st_mode)) continue;

		size_t name_len = strlen(entry->d_name);
		char *child_path = (char *)malloc(prefix_len + name_len + 2);
		if (!child_path) {
			fprintf(stderr, "Out of memory\n");
			exit_status = 1;
			continue;
		}

		memcpy(child_path, display_prefix, prefix_len);
		if (needs_sep) {
			child_path[prefix_len] = '/';
			memcpy(child_path + prefix_len + 1, entry->d_name, name_len + 1);
		} else {
			memcpy(child_path + prefix_len, entry->d_name, name_len + 1);
		}

		if (S_ISDIR(sb.st_mode)) {
			/* Enforce -xdev (stay on the same filesystem) */
			if (sb.st_dev == root_dev) {
				int subfd = openat(dirfd, entry->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_NOATIME);
				if (subfd < 0 && errno == EPERM) subfd = openat(dirfd, entry->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
				if (subfd < 0) {
					fprintf(stderr, "Error opening directory %s: %s\n", child_path, strerror(errno));
					exit_status = 1;
				} else {
					walk_dir(subfd, child_path, root_dev);
				}
			}
		} else if (S_ISREG(sb.st_mode)) {
			process_file(dirfd, entry->d_name, child_path, &sb);
		}

		free(child_path);
	}

	/* closedir() also closes the underlying dirfd */
	closedir(dp);
}

static void sweep_root(const char *root_path) {
	/* Open strictly as a real directory; O_NOFOLLOW rejects symlinks with ELOOP */
	int rootfd = openat(AT_FDCWD, root_path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_NOATIME);
	if (rootfd < 0 && errno == EPERM) rootfd = openat(AT_FDCWD, root_path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	if (rootfd < 0) {
		fprintf(stderr, "Failed to open directory '%s': %s\n", root_path, strerror(errno));
		exit_status = 1;
		return;
	}

	struct stat sb;
	if (fstat(rootfd, &sb) != 0) {
		fprintf(stderr, "Failed to stat directory '%s': %s\n", root_path, strerror(errno));
		close(rootfd);
		exit_status = 1;
		return;
	}

	walk_dir(rootfd, root_path, sb.st_dev);
}

static int parse_int64(const char *str, int64_t *out) {
	if (!str || !*str) return 0;
	char *endptr = NULL;
	errno = 0;
	long long val = strtoll(str, &endptr, 10);
	if (errno != 0 || *endptr != '\0' || val < 0) return 0;
	*out = (int64_t)val;
	return 1;
}

static void print_usage(const char *progname) {
	fprintf(stderr,
			"Usage: %s [-n] [-v] [-m MIN_BYTES] [DIR ...]\n"
			"  -n            No-action (dry-run) mode\n"
			"  -v            Verbose/debug output\n"
			"  -m MIN_BYTES  Minimum excess bytes to clear (default: %d, or $MIN)\n"
			"  DIR           Directories to sweep (default: current directory '.')\n",
			progname, DEFAULT_MIN_EXCESS);
}

int main(int argc, char *argv[]) {
	raise_fd_limit();

	const char *env_min = getenv("MIN");
	if (env_min) {
		int64_t val = 0;
		if (parse_int64(env_min, &val)) min_excess = val;
	}

	const char *env_v = getenv("V");
	if (env_v && strcmp(env_v, "1") == 0) verbose = 1;

	int opt;
	while ((opt = getopt(argc, argv, "nvm:h")) != -1) {
		switch (opt) {
			case 'n':
				dry_run = 1;
				break;
			case 'v':
				verbose = 1;
				break;
			case 'm':
				if (!parse_int64(optarg, &min_excess)) {
					fprintf(stderr, "Invalid minimum size: %s\n", optarg);
					return EXIT_FAILURE;
				}
				break;
			case 'h':
			default:
				print_usage(argv[0]);
				return (opt == 'h') ? EXIT_SUCCESS : EXIT_FAILURE;
		}
	}

	if (dry_run) {
		printf("No-action mode enabled\n");
	}

	if (optind >= argc) sweep_root(".");
	else for (int i = optind; i < argc; i++) sweep_root(argv[i]);

	if (!verbose) printf("\n");
	printf("Cleared %" PRIu64 " excess bytes (%" PRIu64 " MB)\n", total_cleared, total_cleared / 1048576);

	return exit_status;
}
