/*
 * hardlink - consolidate duplicate files via hardlinks
 *
 * Copyright (C) 2018 Red Hat, Inc. All rights reserved.
 * Written by Jakub Jelinek <jakub@redhat.com>
 *
 * Copyright (C) 2019 Karel Zak <kzak@redhat.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it would be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */
#include <sys/types.h>
#include <stdlib.h>
#include <getopt.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <string.h>
#include <dirent.h>
#include <fcntl.h>
#include <errno.h>
#ifdef HAVE_PCRE
# define PCRE2_CODE_UNIT_WIDTH 8
# include <pcre2.h>
#endif

#include "c.h"
#include "xalloc.h"
#include "nls.h"
#include "closestream.h"

#define NHASH   (1<<17)  /* Must be a power of 2! */
#define NIOBUF  (1<<12)
#define NBUF    64

#ifdef HAVE_PCRE
pcre2_code *re;
PCRE2_SPTR exclude_pattern;
pcre2_match_data *match_data;
#endif

struct hardlink_file;

struct hardlink_hash {
	struct hardlink_hash *next;
	struct hardlink_file *chain;
	off_t size;
	time_t mtime;
};

struct hardlink_dir {
	struct hardlink_dir *next;
	char name[0];
};

struct hardlink_file {
	struct hardlink_file *next;
	ino_t ino;
	dev_t dev;
	unsigned int cksum;
	char name[0];
};

struct hardlink_dynstr {
	char *buf;
	size_t alloc;
};

static struct hardlink_dir *dirs;
static struct hardlink_hash *hps[NHASH];

static int no_link = 0;
static int verbose = 0;
static int content_only = 0;
static int force = 0;
static dev_t dev = 0;

/* summary counters */
static unsigned long long ndirs, nobjects, nregfiles, ncomp, nlinks, nsaved;

static unsigned int buf[NBUF];
static char iobuf1[NIOBUF], iobuf2[NIOBUF];

__attribute__ ((always_inline))
static inline unsigned int hash(off_t size, time_t mtime)
{
	return (size ^ mtime) & (NHASH - 1);
}

__attribute__ ((always_inline))
static inline int stcmp(struct stat *st1, struct stat *st2, int content_scope)
{
	if (content_scope)
		return st1->st_size != st2->st_size;

	return st1->st_mode != st2->st_mode
		|| st1->st_uid != st2->st_uid
		|| st1->st_gid != st2->st_gid
		|| st1->st_size != st2->st_size
		|| st1->st_mtime != st2->st_mtime;
}

static void print_summary(void)
{
	if (!verbose)
		return;

	if (verbose > 1 && nlinks)
		fputc('\n', stdout);

	printf(_("Directories:   %9lld\n"), ndirs);
	printf(_("Objects:       %9lld\n"), nobjects);
	printf(_("Regular files: %9lld\n"), nregfiles);
	printf(_("Comparisons:   %9lld\n"), ncomp);
	printf("%s%9lld\n", (no_link ?
	       _("Would link:    ") :
	       _("Linked:        ")), nlinks);
	printf("%s %9lld\n", (no_link ?
		_("Would save:   ") :
		_("Saved:        ")), nsaved);
}

static void __attribute__((__noreturn__)) usage(void)
{
	fputs(USAGE_HEADER, stdout);
	printf(_(" %s [options] directory...\n"), program_invocation_short_name);

	fputs(USAGE_SEPARATOR, stdout);
	puts(_("Consolidate duplicate files using hardlinks."));

	fputs(USAGE_OPTIONS, stdout);
	puts(_(" -c, --content          compare only contents, ignore permission, etc."));
	puts(_(" -n, --dry-run          don't actually link anything"));
	puts(_(" -v, --verbose          print summary after hardlinking"));
	puts(_(" -vv                    print every hardlinked file and summary"));
	puts(_(" -f, --force            force hardlinking across filesystems"));
	puts(_(" -x, --exclude <regex>  exclude files matching pattern"));

	fputs(USAGE_SEPARATOR, stdout);
	printf(USAGE_HELP_OPTIONS(16)); /* char offset to align option descriptions */
	printf(USAGE_MAN_TAIL("hardlink(1)"));
	exit(EXIT_SUCCESS);
}

__attribute__ ((always_inline))
static inline size_t add2(size_t a, size_t b)
{
	size_t sum = a + b;

	if (sum < a)
		errx(EXIT_FAILURE, _("integer overflow"));
	return sum;
}

__attribute__ ((always_inline))
static inline size_t add3(size_t a, size_t b, size_t c)
{
	return add2(add2(a, b), c);
}

static void growstr(struct hardlink_dynstr *str, size_t newlen)
{
	if (newlen < str->alloc)
		return;
	str->buf = xrealloc(str->buf, str->alloc = add2(newlen, 1));
}

static void process_path(const char *name)
{
	struct stat st, st2, st3;
	const size_t namelen = strlen(name);

	nobjects++;
	if (lstat(name, &st))
		return;

	if (st.st_dev != dev && !force) {
		if (dev)
			errx(EXIT_FAILURE,
			     _("%s is on different filesystem than the rest "
			       "(use -f option to override)."), name);
		dev = st.st_dev;
	}
	if (S_ISDIR(st.st_mode)) {
		struct hardlink_dir *dp = xmalloc(add3(sizeof(*dp), namelen, 1));
		memcpy(dp->name, name, namelen + 1);
		dp->next = dirs;
		dirs = dp;

	} else if (S_ISREG(st.st_mode)) {
		int fd, i;
		struct hardlink_file *fp, *fp2;
		struct hardlink_hash *hp;
		const char *n1, *n2;
		int cksumsize = sizeof(buf);
		unsigned int cksum;
		time_t mtime = content_only ? 0 : st.st_mtime;
		unsigned int hsh = hash(st.st_size, mtime);
		off_t fsize;

		nregfiles++;
		if (verbose > 1)
			printf("  %s", name);

		fd = open(name, O_RDONLY);
		if (fd < 0)
			return;

		if ((size_t)st.st_size < sizeof(buf)) {
			cksumsize = st.st_size;
			memset(((char *)buf) + cksumsize, 0,
			       (sizeof(buf) - cksumsize) % sizeof(buf[0]));
		}
		if (read(fd, buf, cksumsize) != cksumsize) {
			close(fd);
			if (verbose > 1 && namelen <= PATH_MAX)
				printf("\r%*s\r", (int)(namelen + 2), "");
			return;
		}
		cksumsize = (cksumsize + sizeof(buf[0]) - 1) / sizeof(buf[0]);
		for (i = 0, cksum = 0; i < cksumsize; i++) {
			if (cksum + buf[i] < cksum)
				cksum += buf[i] + 1;
			else
				cksum += buf[i];
		}
		for (hp = hps[hsh]; hp; hp = hp->next) {
			if (hp->size == st.st_size && hp->mtime == mtime)
				break;
		}
		if (!hp) {
			hp = xmalloc(sizeof(*hp));
			hp->size = st.st_size;
			hp->mtime = mtime;
			hp->chain = NULL;
			hp->next = hps[hsh];
			hps[hsh] = hp;
		}
		for (fp = hp->chain; fp; fp = fp->next) {
			if (fp->cksum == cksum)
				break;
		}
		for (fp2 = fp; fp2 && fp2->cksum == cksum; fp2 = fp2->next) {
			if (fp2->ino == st.st_ino && fp2->dev == st.st_dev) {
				close(fd);
				if (verbose > 1 && namelen <= PATH_MAX)
					printf("\r%*s\r", (int)(namelen + 2), "");
				return;
			}
		}
		for (fp2 = fp; fp2 && fp2->cksum == cksum; fp2 = fp2->next) {

			if (!lstat(fp2->name, &st2) && S_ISREG(st2.st_mode) &&
			    !stcmp(&st, &st2, content_only) &&
			    st2.st_ino != st.st_ino &&
			    st2.st_dev == st.st_dev) {

				int fd2 = open(fp2->name, O_RDONLY);
				if (fd2 < 0)
					continue;

				if (fstat(fd2, &st2) || !S_ISREG(st2.st_mode)
				    || st2.st_size == 0) {
					close(fd2);
					continue;
				}
				ncomp++;
				lseek(fd, 0, SEEK_SET);

				for (fsize = st.st_size; fsize > 0;
				     fsize -= NIOBUF) {
					ssize_t xsz;
					off_t rsize = fsize >= NIOBUF ? NIOBUF : fsize;

					if ((xsz = read(fd, iobuf1, rsize)) != rsize)
						warn(_("cannot read %s"), name);
					else if ((xsz = read(fd2, iobuf2, rsize)) != rsize)
						warn(_("cannot read %s"), fp2->name);

					if (xsz != rsize) {
						close(fd);
						close(fd2);
						return;
					}
					if (memcmp(iobuf1, iobuf2, rsize))
						break;
				}
				close(fd2);
				if (fsize > 0)
					continue;
				if (lstat(name, &st3)) {
					warn(_("cannot stat %s"), name);
					close(fd);
					return;
				}
				st3.st_atime = st.st_atime;
				if (stcmp(&st, &st3, 0)) {
					warnx(_("file %s changed underneath us"), name);
					close(fd);
					return;
				}
				n1 = fp2->name;
				n2 = name;

				if (!no_link) {
					const char *suffix =
					    ".$$$___cleanit___$$$";
					const size_t suffixlen = strlen(suffix);
					size_t n2len = strlen(n2);
					struct hardlink_dynstr nam2 = { NULL, 0 };

					growstr(&nam2, add2(n2len, suffixlen));
					memcpy(nam2.buf, n2, n2len);
					memcpy(&nam2.buf[n2len], suffix,
					       suffixlen + 1);
					/* First create a temporary link to n1 under a new name */
					if (link(n1, nam2.buf)) {
						warn(_("failed to hardlink %s to %s (create temporary link as %s failed)"),
							n1, n2, nam2.buf);
						free(nam2.buf);
						continue;
					}
					/* Then rename into place over the existing n2 */
					if (rename(nam2.buf, n2)) {
						warn(_("failed to hardlink %s to %s (rename temporary link to %s failed)"),
							n1, n2, n2);
						/* Something went wrong, try to remove the now redundant temporary link */
						if (unlink(nam2.buf))
							warn(_("failed to remove temporary link %s"), nam2.buf);
						free(nam2.buf);
						continue;
					}
					free(nam2.buf);
				}
				nlinks++;
				if (st3.st_nlink > 1) {
					/* We actually did not save anything this time, since the link second argument
					   had some other links as well.  */
					if (verbose > 1)
						printf(_("\r%*s\r%s %s to %s\n"),
							(int)(((namelen > PATH_MAX) ? 0 : namelen) + 2),
							"",
							(no_link ? _("Would link") : _("Linked")),
							n1, n2);
				} else {
					nsaved += ((st.st_size + 4095) / 4096) * 4096;
					if (verbose > 1)
						printf(_("\r%*s\r%s %s to %s, %s %jd\n"),
							(int)(((namelen > PATH_MAX) ? 0 : namelen) + 2),
							"",
							(no_link ? _("Would link") : _("Linked")),
							n1, n2,
							(no_link ? _("would save") : _("saved")),
							(intmax_t)st.st_size);
				}
				close(fd);
				return;
			}
		}
		fp2 = xmalloc(add3(sizeof(*fp2), namelen, 1));
		close(fd);
		fp2->ino = st.st_ino;
		fp2->dev = st.st_dev;
		fp2->cksum = cksum;
		memcpy(fp2->name, name, namelen + 1);

		if (fp) {
			fp2->next = fp->next;
			fp->next = fp2;
		} else {
			fp2->next = hp->chain;
			hp->chain = fp2;
		}
		if (verbose > 1 && namelen <= PATH_MAX)
			printf("\r%*s\r", (int)(namelen + 2), "");
		return;
	}
}

int main(int argc, char **argv)
{
	int ch;
	int i;
#ifdef HAVE_PCRE
	int errornumber;
	PCRE2_SIZE erroroffset;
#endif
	struct hardlink_dynstr nam1 = { NULL, 0 };

	static const struct option longopts[] = {
		{ "content",    no_argument, NULL, 'c' },
		{ "dry-run",    no_argument, NULL, 'n' },
		{ "exclude",    required_argument, NULL, 'x' },
		{ "force",      no_argument, NULL, 'f' },
		{ "help",       no_argument, NULL, 'h' },
		{ "verbose",    no_argument, NULL, 'v' },
		{ "version",    no_argument, NULL, 'V' },
		{ NULL, 0, NULL, 0 },
	};

	setlocale(LC_ALL, "");
	bindtextdomain(PACKAGE, LOCALEDIR);
	textdomain(PACKAGE);
	atexit(close_stdout);

	while ((ch = getopt_long(argc, argv, "cnvfx:Vh", longopts, NULL)) != -1) {
		switch (ch) {
		case 'n':
			no_link++;
			break;
		case 'v':
			verbose++;
			break;
		case 'c':
			content_only++;
			break;
		case 'f':
			force = 1;
			break;
		case 'x':
#ifdef HAVE_PCRE
			exclude_pattern = (PCRE2_SPTR) optarg;
#else
			errx(EXIT_FAILURE,
			     _("option --exclude not supported (built without pcre2)"));
#endif
			break;
		case 'V':
			printf(UTIL_LINUX_VERSION);
			return EXIT_SUCCESS;
		case 'h':
			usage();
		default:
			errtryhelp(EXIT_FAILURE);
		}
	}

	if (optind == argc) {
		warnx(_("no directory specified"));
		errtryhelp(EXIT_FAILURE);
	}

#ifdef HAVE_PCRE
	if (exclude_pattern) {
		re = pcre2_compile(exclude_pattern, /* the pattern */
				   PCRE2_ZERO_TERMINATED, /* indicates pattern is zero-terminate */
				   0, /* default options */
				   &errornumber, &erroroffset, NULL); /* use default compile context */
		if (!re) {
			PCRE2_UCHAR buffer[256];
			pcre2_get_error_message(errornumber, buffer,
						sizeof(buffer));
			errx(EXIT_FAILURE, _("pattern error at offset %d: %s"),
				(int)erroroffset, buffer);
		}
		match_data = pcre2_match_data_create_from_pattern(re, NULL);
	}
#endif
	atexit(print_summary);

	for (i = optind; i < argc; i++)
		process_path(argv[i]);

	while (dirs) {
		DIR *dh;
		struct dirent *di;
		struct hardlink_dir *dp = dirs;
		size_t nam1baselen = strlen(dp->name);

		dirs = dp->next;
		growstr(&nam1, add2(nam1baselen, 1));
		memcpy(nam1.buf, dp->name, nam1baselen);
		free(dp);
		nam1.buf[nam1baselen++] = '/';
		nam1.buf[nam1baselen] = 0;
		dh = opendir(nam1.buf);

		if (dh == NULL)
			continue;
		ndirs++;

		while ((di = readdir(dh)) != NULL) {
			if (!di->d_name[0])
				continue;
			if (di->d_name[0] == '.') {
				if (!di->d_name[1] || !strcmp(di->d_name, ".."))
					continue;
			}
#ifdef HAVE_PCRE
			if (re && pcre2_match(re, /* compiled regex */
					      (PCRE2_SPTR) di->d_name, strlen(di->d_name), 0, /* start at offset 0 */
					      0, /* default options */
					      match_data, /* block for storing the result */
					      NULL) /* use default match context */
			    >=0) {
				if (verbose) {
					nam1.buf[nam1baselen] = 0;
					printf(_("Skipping %s%s\n"), nam1.buf, di->d_name);
				}
				continue;
			}
#endif
			{
				size_t subdirlen;
				growstr(&nam1,
					add2(nam1baselen, subdirlen =
					     strlen(di->d_name)));
				memcpy(&nam1.buf[nam1baselen], di->d_name,
				       add2(subdirlen, 1));
			}
			process_path(nam1.buf);
		}
		closedir(dh);
	}

	return 0;
}
