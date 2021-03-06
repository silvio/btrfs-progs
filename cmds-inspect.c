/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <getopt.h>
#include <limits.h>

#include "kerncompat.h"
#include "ioctl.h"
#include "utils.h"
#include "ctree.h"
#include "send-utils.h"
#include "disk-io.h"
#include "commands.h"
#include "btrfs-list.h"

static const char * const inspect_cmd_group_usage[] = {
	"btrfs inspect-internal <command> <args>",
	NULL
};

static int __ino_to_path_fd(u64 inum, int fd, int verbose, const char *prepend)
{
	int ret;
	int i;
	struct btrfs_ioctl_ino_path_args ipa;
	struct btrfs_data_container *fspath;

	fspath = malloc(4096);
	if (!fspath)
		return -ENOMEM;

	memset(fspath, 0, sizeof(*fspath));
	ipa.inum = inum;
	ipa.size = 4096;
	ipa.fspath = ptr_to_u64(fspath);

	ret = ioctl(fd, BTRFS_IOC_INO_PATHS, &ipa);
	if (ret) {
		printf("ioctl ret=%d, error: %s\n", ret, strerror(errno));
		goto out;
	}

	if (verbose)
		printf("ioctl ret=%d, bytes_left=%lu, bytes_missing=%lu, "
			"cnt=%d, missed=%d\n", ret,
			(unsigned long)fspath->bytes_left,
			(unsigned long)fspath->bytes_missing,
			fspath->elem_cnt, fspath->elem_missed);

	for (i = 0; i < fspath->elem_cnt; ++i) {
		u64 ptr;
		char *str;
		ptr = (u64)(unsigned long)fspath->val;
		ptr += fspath->val[i];
		str = (char *)(unsigned long)ptr;
		if (prepend)
			printf("%s/%s\n", prepend, str);
		else
			printf("%s\n", str);
	}

out:
	free(fspath);
	return !!ret;
}

static const char * const cmd_inspect_inode_resolve_usage[] = {
	"btrfs inspect-internal inode-resolve [-v] <inode> <path>",
	"Get file system paths for the given inode",
	"",
	"-v   verbose mode",
	NULL
};

static int cmd_inspect_inode_resolve(int argc, char **argv)
{
	int fd;
	int verbose = 0;
	int ret;
	DIR *dirstream = NULL;

	optind = 1;
	while (1) {
		int c = getopt(argc, argv, "v");
		if (c < 0)
			break;

		switch (c) {
		case 'v':
			verbose = 1;
			break;
		default:
			usage(cmd_inspect_inode_resolve_usage);
		}
	}

	if (check_argc_exact(argc - optind, 2))
		usage(cmd_inspect_inode_resolve_usage);

	fd = open_file_or_dir(argv[optind+1], &dirstream);
	if (fd < 0) {
		fprintf(stderr, "ERROR: can't access '%s'\n", argv[optind+1]);
		return 1;
	}

	ret = __ino_to_path_fd(arg_strtou64(argv[optind]), fd, verbose,
			       argv[optind+1]);
	close_file_or_dir(fd, dirstream);
	return !!ret;

}

static const char * const cmd_inspect_logical_resolve_usage[] = {
	"btrfs inspect-internal logical-resolve [-Pv] [-s bufsize] <logical> <path>",
	"Get file system paths for the given logical address",
	"-P          skip the path resolving and print the inodes instead",
	"-v          verbose mode",
	"-s bufsize  set inode container's size. This is used to increase inode",
	"            container's size in case it is not enough to read all the ",
	"            resolved results. The max value one can set is 64k",
	NULL
};

static int cmd_inspect_logical_resolve(int argc, char **argv)
{
	int ret;
	int fd;
	int i;
	int verbose = 0;
	int getpath = 1;
	int bytes_left;
	struct btrfs_ioctl_logical_ino_args loi;
	struct btrfs_data_container *inodes;
	u64 size = 4096;
	char full_path[4096];
	char *path_ptr;
	DIR *dirstream = NULL;

	optind = 1;
	while (1) {
		int c = getopt(argc, argv, "Pvs:");
		if (c < 0)
			break;

		switch (c) {
		case 'P':
			getpath = 0;
			break;
		case 'v':
			verbose = 1;
			break;
		case 's':
			size = arg_strtou64(optarg);
			break;
		default:
			usage(cmd_inspect_logical_resolve_usage);
		}
	}

	if (check_argc_exact(argc - optind, 2))
		usage(cmd_inspect_logical_resolve_usage);

	size = min(size, (u64)64 * 1024);
	inodes = malloc(size);
	if (!inodes)
		return 1;

	memset(inodes, 0, sizeof(*inodes));
	loi.logical = arg_strtou64(argv[optind]);
	loi.size = size;
	loi.inodes = ptr_to_u64(inodes);

	fd = open_file_or_dir(argv[optind+1], &dirstream);
	if (fd < 0) {
		fprintf(stderr, "ERROR: can't access '%s'\n", argv[optind+1]);
		ret = 12;
		goto out;
	}

	ret = ioctl(fd, BTRFS_IOC_LOGICAL_INO, &loi);
	if (ret) {
		printf("ioctl ret=%d, error: %s\n", ret, strerror(errno));
		goto out;
	}

	if (verbose)
		printf("ioctl ret=%d, total_size=%llu, bytes_left=%lu, "
			"bytes_missing=%lu, cnt=%d, missed=%d\n",
			ret, size,
			(unsigned long)inodes->bytes_left,
			(unsigned long)inodes->bytes_missing,
			inodes->elem_cnt, inodes->elem_missed);

	bytes_left = sizeof(full_path);
	ret = snprintf(full_path, bytes_left, "%s/", argv[optind+1]);
	path_ptr = full_path + ret;
	bytes_left -= ret + 1;
	BUG_ON(bytes_left < 0);

	for (i = 0; i < inodes->elem_cnt; i += 3) {
		u64 inum = inodes->val[i];
		u64 offset = inodes->val[i+1];
		u64 root = inodes->val[i+2];
		int path_fd;
		char *name;
		DIR *dirs = NULL;

		if (getpath) {
			name = btrfs_list_path_for_root(fd, root);
			if (IS_ERR(name)) {
				ret = PTR_ERR(name);
				goto out;
			}
			if (!name) {
				path_ptr[-1] = '\0';
				path_fd = fd;
			} else {
				path_ptr[-1] = '/';
				ret = snprintf(path_ptr, bytes_left, "%s",
						name);
				BUG_ON(ret >= bytes_left);
				free(name);
				path_fd = open_file_or_dir(full_path, &dirs);
				if (path_fd < 0) {
					fprintf(stderr, "ERROR: can't access "
						"'%s'\n", full_path);
					goto out;
				}
			}
			__ino_to_path_fd(inum, path_fd, verbose, full_path);
			if (path_fd != fd)
				close_file_or_dir(path_fd, dirs);
		} else {
			printf("inode %llu offset %llu root %llu\n", inum,
				offset, root);
		}
	}

out:
	close_file_or_dir(fd, dirstream);
	free(inodes);
	return !!ret;
}

static const char * const cmd_inspect_subvolid_resolve_usage[] = {
	"btrfs inspect-internal subvolid-resolve <subvolid> <path>",
	"Get file system paths for the given subvolume ID.",
	NULL
};

static int cmd_inspect_subvolid_resolve(int argc, char **argv)
{
	int ret;
	int fd = -1;
	u64 subvol_id;
	char path[PATH_MAX];
	DIR *dirstream = NULL;

	if (check_argc_exact(argc, 3))
		usage(cmd_inspect_subvolid_resolve_usage);

	fd = open_file_or_dir(argv[2], &dirstream);
	if (fd < 0) {
		fprintf(stderr, "ERROR: can't access '%s'\n", argv[2]);
		ret = -ENOENT;
		goto out;
	}

	subvol_id = arg_strtou64(argv[1]);
	ret = btrfs_subvolid_resolve(fd, path, sizeof(path), subvol_id);

	if (ret) {
		fprintf(stderr,
			"%s: btrfs_subvolid_resolve(subvol_id %llu) failed with ret=%d\n",
			argv[0], (unsigned long long)subvol_id, ret);
		goto out;
	}

	path[PATH_MAX - 1] = '\0';
	printf("%s\n", path);

out:
	close_file_or_dir(fd, dirstream);
	return ret ? 1 : 0;
}

static const char* const cmd_inspect_rootid_usage[] = {
	"btrfs inspect-internal rootid <path>",
	"Get tree ID of the containing subvolume of path.",
	NULL
};

static int cmd_inspect_rootid(int argc, char **argv)
{
	int ret;
	int fd = -1;
	u64 rootid;
	DIR *dirstream = NULL;

	if (check_argc_exact(argc, 2))
		usage(cmd_inspect_rootid_usage);

	fd = open_file_or_dir(argv[1], &dirstream);
	if (fd < 0) {
		fprintf(stderr, "ERROR: can't access '%s'\n", argv[1]);
		ret = -ENOENT;
		goto out;
	}

	ret = lookup_ino_rootid(fd, &rootid);
	if (ret) {
		fprintf(stderr, "%s: rootid failed with ret=%d\n",
			argv[0], ret);
		goto out;
	}

	printf("%llu\n", (unsigned long long)rootid);
out:
	close_file_or_dir(fd, dirstream);

	return !!ret;
}

struct dev_extent_elem {
	u64 start;
	/* inclusive end */
	u64 end;
	struct list_head list;
};

static int add_dev_extent(struct list_head *list,
			  const u64 start, const u64 end,
			  const int append)
{
	struct dev_extent_elem *e;

	e = malloc(sizeof(*e));
	if (!e)
		return -ENOMEM;

	e->start = start;
	e->end = end;

	if (append)
		list_add_tail(&e->list, list);
	else
		list_add(&e->list, list);

	return 0;
}

static void free_dev_extent_list(struct list_head *list)
{
	while (!list_empty(list)) {
		struct dev_extent_elem *e;

		e = list_first_entry(list, struct dev_extent_elem, list);
		list_del(&e->list);
		free(e);
	}
}

static int hole_includes_sb_mirror(const u64 start, const u64 end)
{
	int i;
	int ret = 0;

	for (i = 0; i < BTRFS_SUPER_MIRROR_MAX; i++) {
		u64 bytenr = btrfs_sb_offset(i);

		if (bytenr >= start && bytenr <= end) {
			ret = 1;
			break;
		}
	}

	return ret;
}

static void adjust_dev_min_size(struct list_head *extents,
				struct list_head *holes,
				u64 *min_size)
{
	/*
	 * If relocation of the block group of a device extent must happen (see
	 * below) scratch space is used for the relocation. So track here the
	 * size of the largest device extent that has to be relocated. We track
	 * only the largest and not the sum of the sizes of all relocated block
	 * groups because after each block group is relocated the running
	 * transaction is committed so that pinned space is released.
	 */
	u64 scratch_space = 0;

	/*
	 * List of device extents is sorted by descending order of the extent's
	 * end offset. If some extent goes beyond the computed minimum size,
	 * which initially matches the sum of the lenghts of all extents,
	 * we need to check if the extent can be relocated to an hole in the
	 * device between [0, *min_size[ (which is what the resize ioctl does).
	 */
	while (!list_empty(extents)) {
		struct dev_extent_elem *e;
		struct dev_extent_elem *h;
		int found = 0;
		u64 extent_len;
		u64 hole_len = 0;

		e = list_first_entry(extents, struct dev_extent_elem, list);
		if (e->end <= *min_size)
			break;

		/*
		 * Our extent goes beyond the computed *min_size. See if we can
		 * find a hole large enough to relocate it to. If not we must stop
		 * and set *min_size to the end of the extent.
		 */
		extent_len = e->end - e->start + 1;
		list_for_each_entry(h, holes, list) {
			hole_len = h->end - h->start + 1;
			if (hole_len >= extent_len) {
				found = 1;
				break;
			}
		}

		if (!found) {
			*min_size = e->end + 1;
			break;
		}

		/*
		 * If the hole found contains the location for a superblock
		 * mirror, we are pessimistic and require allocating one
		 * more extent of the same size. This is because the block
		 * group could be in the worst case used by a single extent
		 * with a size >= (block_group.length - superblock.size).
		 */
		if (hole_includes_sb_mirror(h->start,
					    h->start + extent_len - 1))
			*min_size += extent_len;

		if (hole_len > extent_len) {
			h->start += extent_len;
		} else {
			list_del(&h->list);
			free(h);
		}

		list_del(&e->list);
		free(e);

		if (extent_len > scratch_space)
			scratch_space = extent_len;
	}

	if (scratch_space) {
		*min_size += scratch_space;
		/*
		 * Chunk allocation requires inserting/updating items in the
		 * chunk tree, so often this can lead to the need of allocating
		 * a new system chunk too, which has a maximum size of 32Mb.
		 */
		*min_size += 32 * 1024 * 1024;
	}
}

static int print_min_dev_size(int fd, u64 devid)
{
	int ret = 1;
	/*
	 * Device allocations starts at 1Mb or at the value passed through the
	 * mount option alloc_start if it's bigger than 1Mb. The alloc_start
	 * option is used for debugging and testing only, and recently the
	 * possibility of deprecating/removing it has been discussed, so we
	 * ignore it here.
	 */
	u64 min_size = 1 * 1024 * 1024ull;
	struct btrfs_ioctl_search_args args;
	struct btrfs_ioctl_search_key *sk = &args.key;
	u64 last_pos = (u64)-1;
	LIST_HEAD(extents);
	LIST_HEAD(holes);

	memset(&args, 0, sizeof(args));
	sk->tree_id = BTRFS_DEV_TREE_OBJECTID;
	sk->min_objectid = devid;
	sk->max_objectid = devid;
	sk->max_type = BTRFS_DEV_EXTENT_KEY;
	sk->min_type = BTRFS_DEV_EXTENT_KEY;
	sk->min_offset = 0;
	sk->max_offset = (u64)-1;
	sk->min_transid = 0;
	sk->max_transid = (u64)-1;
	sk->nr_items = 4096;

	while (1) {
		int i;
		struct btrfs_ioctl_search_header *sh;
		unsigned long off = 0;

		ret = ioctl(fd, BTRFS_IOC_TREE_SEARCH, &args);
		if (ret < 0) {
			fprintf(stderr,
				"Error invoking tree search ioctl: %s\n",
				strerror(errno));
			ret = 1;
			goto out;
		}

		if (sk->nr_items == 0)
			break;

		for (i = 0; i < sk->nr_items; i++) {
			struct btrfs_dev_extent *extent;
			u64 len;

			sh = (struct btrfs_ioctl_search_header *)(args.buf +
								  off);
			off += sizeof(*sh);
			extent = (struct btrfs_dev_extent *)(args.buf + off);
			off += sh->len;

			sk->min_objectid = sh->objectid;
			sk->min_type = sh->type;
			sk->min_offset = sh->offset + 1;

			if (sh->objectid != devid ||
			    sh->type != BTRFS_DEV_EXTENT_KEY)
				continue;

			len = btrfs_stack_dev_extent_length(extent);
			min_size += len;
			ret = add_dev_extent(&extents, sh->offset,
					     sh->offset + len - 1, 0);

			if (!ret && last_pos != (u64)-1 &&
			    last_pos != sh->offset)
				ret = add_dev_extent(&holes, last_pos,
						     sh->offset - 1, 1);
			if (ret) {
				fprintf(stderr, "Error: %s\n", strerror(-ret));
				ret = 1;
				goto out;
			}

			last_pos = sh->offset + len;
		}

		if (sk->min_type != BTRFS_DEV_EXTENT_KEY ||
		    sk->min_objectid != devid)
			break;
	}

	adjust_dev_min_size(&extents, &holes, &min_size);
	printf("%llu bytes (%s)\n", min_size, pretty_size(min_size));
	ret = 0;
out:
	free_dev_extent_list(&extents);
	free_dev_extent_list(&holes);

	return ret;
}

static const char* const cmd_inspect_min_dev_size_usage[] = {
	"btrfs inspect-internal min-dev-size [options] <path>",
	"Get the minimum size the device can be shrunk to. The",
	"device id 1 is used by default.",
	"--id DEVID   specify the device id to query",
	NULL
};

static int cmd_inspect_min_dev_size(int argc, char **argv)
{
	int ret;
	int fd = -1;
	DIR *dirstream = NULL;
	u64 devid = 1;

	while (1) {
		int c;
		enum { GETOPT_VAL_DEVID = 256 };
		static const struct option long_options[] = {
			{ "id", required_argument, NULL, GETOPT_VAL_DEVID },
			{NULL, 0, NULL, 0}
		};

		c = getopt_long(argc, argv, "", long_options, NULL);
		if (c < 0)
			break;

		switch (c) {
		case GETOPT_VAL_DEVID:
			devid = arg_strtou64(optarg);
			break;
		default:
			usage(cmd_inspect_min_dev_size_usage);
		}
	}
	if (check_argc_exact(argc - optind, 1))
		usage(cmd_inspect_min_dev_size_usage);

	fd = open_file_or_dir(argv[optind], &dirstream);
	if (fd < 0) {
		fprintf(stderr, "ERROR: can't access '%s'\n", argv[optind]);
		ret = -ENOENT;
		goto out;
	}

	ret = print_min_dev_size(fd, devid);
out:
	close_file_or_dir(fd, dirstream);

	return !!ret;
}

static const char inspect_cmd_group_info[] =
"query various internal information";

const struct cmd_group inspect_cmd_group = {
	inspect_cmd_group_usage, inspect_cmd_group_info, {
		{ "inode-resolve", cmd_inspect_inode_resolve,
			cmd_inspect_inode_resolve_usage, NULL, 0 },
		{ "logical-resolve", cmd_inspect_logical_resolve,
			cmd_inspect_logical_resolve_usage, NULL, 0 },
		{ "subvolid-resolve", cmd_inspect_subvolid_resolve,
			cmd_inspect_subvolid_resolve_usage, NULL, 0 },
		{ "rootid", cmd_inspect_rootid, cmd_inspect_rootid_usage, NULL,
			0 },
		{ "min-dev-size", cmd_inspect_min_dev_size,
			cmd_inspect_min_dev_size_usage, NULL, 0 },
		NULL_CMD_STRUCT
	}
};

int cmd_inspect(int argc, char **argv)
{
	return handle_command_group(&inspect_cmd_group, argc, argv);
}
