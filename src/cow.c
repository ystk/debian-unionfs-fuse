/*
*  C Implementation: cow
*
* Copy-on-write functions
*
* License: BSD-style license
* Copyright: Radek Podgorny <radek@podgorny.cz>,
*            Bernd Schubert <bernd-schubert@gmx.de>
*/

#include <sys/types.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <locale.h>
#include <errno.h>
#include <stdio.h>
#include <dirent.h>

#include "opts.h"
#include "findbranch.h"
#include "general.h"
#include "cow.h"
#include "cow_utils.h"
#include "string.h"
#include "debug.h"


/**
 * Actually create the directory here.
 */
static int do_create(const char *path, int nbranch_ro, int nbranch_rw) {
	DBG_IN();

	char dirp[PATHLEN_MAX]; // dir path to create
	sprintf(dirp, "%s%s", uopt.branches[nbranch_rw].path, path);

	struct stat buf;
	int res = stat(dirp, &buf);
	if (res != -1) return 0; // already exists

	if (nbranch_ro == nbranch_rw) {
		// special case nbranch_ro = nbranch_rw, this is if we a create
		// unionfs meta directories, so not directly on cow operations
		buf.st_mode = S_IRWXU | S_IRWXG;
	} else {
		// data from the ro-branch
		char o_dirp[PATHLEN_MAX]; // the pathname we want to copy
		sprintf(o_dirp, "%s%s", uopt.branches[nbranch_ro].path, path);
		res = stat(o_dirp, &buf);
		if (res == -1) return 1; // lower level branch removed in the mean time?
	}

	res = mkdir(dirp, buf.st_mode);
	if (res == -1) {
		usyslog(LOG_DAEMON, "Creating %s failed: \n", dirp);
		return 1;
	}

	if (nbranch_ro == nbranch_rw) return 0; // the special case again

	if (setfile(dirp, &buf))  return 1; // directory already removed by another process?

	// TODO: time, but its values are modified by the next dir/file creation steps?

	return 0;
}

/**
 * l_nbranch (lower nbranch than nbranch) is write protected, create the dir path on
 * nbranch for an other COW operation.
 */
int path_create(const char *path, int nbranch_ro, int nbranch_rw) {
	DBG_IN();

	if (!uopt.cow_enabled) return 0;

	if (strlen(path) + strlen(uopt.branches[nbranch_rw].path) > PATHLEN_MAX
	|| strlen(path) + strlen(uopt.branches[nbranch_ro].path) > PATHLEN_MAX) {
		// TODO: how to handle that?
		return 1;
	}

	char p[PATHLEN_MAX];
	snprintf(p, PATHLEN_MAX, "%s%s", uopt.branches[nbranch_rw].path, path);

	struct stat st;
	if (!stat(p, &st)) {
		// path does already exists, no need to create it
		return 0;
	}

	char *walk = (char *)path;

	// first slashes, e.g. we have path = /dir1/dir2/, will set walk = dir1/dir2/
	while (*walk != '\0' && *walk == '/') walk++;

	do {
		// walk over the directory name, walk will now be /dir2
		while (*walk != '\0' && *walk != '/') walk++;

		// +1 due to \0, which gets added automatically
		snprintf(p, (walk - path) + 1, "%s", path); // walk - path = strlen(/dir1)
		int res = do_create(p, nbranch_ro, nbranch_rw);
		if (res) return res; // creating the directory failed

		// as above the do loop, walk over the next slashes, walk = dir2/
		while (*walk != '\0' && *walk == '/') walk++;
	} while (*walk != '\0');

	return 0;
}

/**
 * Same as  path_create(), but ignore the last segment in path,
 * i.e. it might be a filename.
 */
int path_create_cutlast(const char *path, int nbranch_ro, int nbranch_rw) {
	DBG_IN();

	char *dname = u_dirname(path);
	if (dname == NULL)
		return -ENOMEM;
	int ret = path_create(dname, nbranch_ro, nbranch_rw);
	free(dname);

	return ret;
}

/**
 * initiate the cow-copy action
 */
int cow_cp(const char *path, int branch_ro, int branch_rw) {
	DBG_IN();

	// create the path to the file
	path_create_cutlast(path, branch_ro, branch_rw);

	char from[PATHLEN_MAX], to[PATHLEN_MAX];
	snprintf(from, PATHLEN_MAX, "%s%s", uopt.branches[branch_ro].path, path);
	snprintf(to, PATHLEN_MAX, "%s%s", uopt.branches[branch_rw].path, path);

	setlocale(LC_ALL, "");

	struct cow cow;

	cow.uid = getuid();

	// Copy the umask for explicit mode setting.
	cow.umask = umask(0);
	umask(cow.umask);

	cow.from_path = from;
	cow.to_path = to;

	struct stat buf;
	lstat(cow.from_path, &buf);
	cow.stat = &buf;

	int res;
	switch (buf.st_mode & S_IFMT) {
		case S_IFLNK:
			res = copy_link(&cow);
			break;
		case S_IFDIR:
			res = copy_directory(path, branch_ro, branch_rw);
			break;
		case S_IFBLK:
		case S_IFCHR:
			res = copy_special(&cow);
			break;
		case S_IFIFO:
			res = copy_fifo(&cow);
			break;
		case S_IFSOCK:
			usyslog(LOG_WARNING, "COW of sockets not supported: %s\n", cow.from_path);
			return 1;
		default:
			res = copy_file(&cow);
	}

	return res;
}

/**
 * copy a directory between branches (includes all contents of the directory)
 */
int copy_directory(const char *path, int branch_ro, int branch_rw) {
	DBG_IN();

	/* create the directory on the destination branch */
	int res = path_create(path, branch_ro, branch_rw);
	if (res != 0) {
		return res;
	}

	/* determine path to source directory on read-only branch */
	char from[PATHLEN_MAX];
	if (BUILD_PATH(from, uopt.branches[branch_ro].path, path)) return 1;

	DIR *dp = opendir(from);
	if (dp == NULL) return 1;

	struct dirent *de;
	while ((de = readdir(dp)) != NULL) {
		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;

		char member[PATHLEN_MAX];
		if (BUILD_PATH(member, path, de->d_name)) return 1;
		res = cow_cp(member, branch_ro, branch_rw);
		if (res != 0) return res;
	}

	closedir(dp);
	return 0;
}

