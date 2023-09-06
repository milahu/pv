/*
 * Functions for opening and closing files, and calculating their size.
 *
 * Copyright 2002-2008, 2010, 2012-2015, 2017, 2021, 2023 Andrew Wood
 *
 * Distributed under the Artistic License v2.0; see `docs/COPYING'.
 */

#include "config.h"
#include "pv.h"
#include "pv-internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>

/*@-type@*/
/* splint has trouble with off_t and mode_t throughout this file. */

/*
 * Calculate the total number of bytes to be transferred by adding up the
 * sizes of all input files.  If any of the input files are of indeterminate
 * size (such as if they are a pipe), the total size is set to zero.
 *
 * Any files that cannot be stat()ed or that access() says we can't read
 * will be skipped, and the total size will be set to zero.
 *
 * Returns the total size, or 0 if it is unknown.
 */
static unsigned long long pv_calc_total_bytes(pvstate_t state)
{
	unsigned long long total;
	struct stat sb;
	unsigned int file_idx;

	total = 0;
	memset(&sb, 0, sizeof(sb));

	/*
	 * No files specified - check stdin.
	 */
	if (state->input_file_count < 1) {
		if (0 == fstat(STDIN_FILENO, &sb))
			total = (unsigned long long) (sb.st_size);
		return total;
	}

	for (file_idx = 0; file_idx < state->input_file_count; file_idx++) {
		int rc;

		if (0 == strcmp(state->input_files[file_idx], "-")) {
			rc = fstat(STDIN_FILENO, &sb);
			if (rc != 0) {
				total = 0;
				return total;
			}
		} else {
			rc = stat(state->input_files[file_idx], &sb);
			if (0 == rc)
				rc = access(state->input_files[file_idx], R_OK);
		}

		if (rc != 0) {
			debug("%s: %s", state->input_files[file_idx], strerror(errno));
			total = 0;
			return total;
		}

		if (S_ISBLK(sb.st_mode)) {
			int fd;

			/*
			 * Get the size of block devices by opening
			 * them and seeking to the end.
			 */
			if (0 == strcmp(state->input_files[file_idx], "-")) {
				fd = open("/dev/stdin", O_RDONLY);
			} else {
				fd = open(state->input_files[file_idx], O_RDONLY);
			}
			if (fd >= 0) {
				off_t end_position;
				end_position = lseek(fd, 0, SEEK_END);
				if (end_position > 0) {
					total += (unsigned long long) end_position;
				}
				(void) close(fd);
			} else {
				total = 0;
				return total;
			}
		} else if (S_ISREG(sb.st_mode)) {
			total += (unsigned long long) (sb.st_size);
		} else {
			total = 0;
		}
	}

	/*
	 * Patch from Peter Samuelson: if we cannot work out the size of the
	 * input, but we are writing to a block device, then use the size of
	 * the output block device.
	 *
	 * Further modified to check that stdout is not in append-only mode
	 * and that we can seek back to the start after getting the size.
	 */
	if (total < 1) {
		int rc;

		rc = fstat(STDOUT_FILENO, &sb);

		if ((0 == rc) && S_ISBLK(sb.st_mode)
		    && (0 == (fcntl(STDOUT_FILENO, F_GETFL) & O_APPEND))) {
			off_t end_position;
			end_position = lseek(STDOUT_FILENO, 0, SEEK_END);
			total = 0;
			if (end_position > 0) {
				total = (unsigned long long) end_position;
			}
			if (lseek(STDOUT_FILENO, 0, SEEK_SET) != 0) {
				pv_error(state, "%s: %s: %s", "(stdout)",
					 _("failed to seek to start of output"), strerror(errno));
				state->exit_status |= 2;
			}
			/*
			 * If we worked out a size, then set the
			 * stop-at-size flag to prevent a "no space left on
			 * device" error when we reach the end of the output
			 * device.
			 */
			if (total > 0) {
				state->stop_at_size = true;
			}
		}
	}

	return total;
}


/*
 * Count the total number of lines to be transferred by reading through all
 * input files.  If any of the inputs are not regular files (such as if they
 * are a pipe or a block device), the total size is set to zero.
 *
 * Any files that cannot be stat()ed or that access() says we can't read
 * will be skipped, and the total size will be set to zero.
 *
 * Returns the total size, or 0 if it is unknown.
 */
static unsigned long long pv_calc_total_lines(pvstate_t state)
{
	unsigned long long total;
	struct stat sb;
	unsigned int file_idx;

	total = 0;

	for (file_idx = 0; file_idx < state->input_file_count; file_idx++) {
		int fd = -1;
		int rc = 0;

		if (0 == strcmp(state->input_files[file_idx], "-")) {
			rc = fstat(STDIN_FILENO, &sb);
			if ((rc != 0) || (!S_ISREG(sb.st_mode))) {
				total = 0;
				return total;
			}
			fd = dup(STDIN_FILENO);
		} else {
			rc = stat(state->input_files[file_idx], &sb);
			if ((rc != 0) || (!S_ISREG(sb.st_mode))) {
				total = 0;
				return total;
			}
			fd = open(state->input_files[file_idx], O_RDONLY);
		}

		if (fd < 0) {
			debug("%s: %s", state->input_files[file_idx], strerror(errno));
			total = 0;
			return total;
		}
#if HAVE_POSIX_FADVISE
		/* Advise the OS that we will only be reading sequentially. */
		(void) posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif

		while (true) {
			char scanbuf[1024];
			ssize_t numread, buf_idx;

			numread = read(fd, scanbuf, sizeof(scanbuf));
			if (numread < 0) {
				pv_error(state, "%s: %s", state->input_files[file_idx], strerror(errno));
				state->exit_status |= 2;
				break;
			} else if (0 == numread) {
				break;
			}
			for (buf_idx = 0; buf_idx < numread; buf_idx++) {
				if (state->null_terminated_lines) {
					if ('\0' == scanbuf[buf_idx])
						total++;
				} else {
					if ('\n' == scanbuf[buf_idx])
						total++;
				}
			}
		}

		if (0 != lseek(fd, 0, SEEK_SET)) {
			pv_error(state, "%s: %s", state->input_files[file_idx], strerror(errno));
			state->exit_status |= 2;
		}

		(void) close(fd);
	}

	return total;
}


/*
 * Work out the total size of all data by adding up the sizes of all input
 * files, using either pv_calc_total_bytes() or pv_calc_total_lines()
 * depending on whether state->linemode is true.
 *
 * Returns the total size, or 0 if it is unknown.
 */
unsigned long long pv_calc_total_size(pvstate_t state)
{
	if (state->linemode) {
		return pv_calc_total_lines(state);
	} else {
		return pv_calc_total_bytes(state);
	}
}


/*
 * Close the given file descriptor and open the next one, whose number in
 * the list is "filenum", returning the new file descriptor (or negative on
 * error). It is an error if the next input file is the same as the file
 * stdout is pointing to.
 *
 * Updates state->current_file in the process.
 */
int pv_next_file(pvstate_t state, unsigned int filenum, int oldfd)
{
	struct stat isb;
	struct stat osb;
	int fd;
	bool input_file_is_stdout;

	if (oldfd >= 0) {
		if (0 != close(oldfd)) {
			pv_error(state, "%s: %s", _("failed to close file"), strerror(errno));
			state->exit_status |= 8;
			return -1;
		}
	}

	if (filenum >= state->input_file_count) {
		debug("%s: %d >= %d", "filenum too large", filenum, state->input_file_count);
		state->exit_status |= 8;
		return -1;
	}

	if (0 == strcmp(state->input_files[filenum], "-")) {
		fd = STDIN_FILENO;
	} else {
		fd = open(state->input_files[filenum], O_RDONLY);
		if (fd < 0) {
			pv_error(state, "%s: %s: %s",
				 _("failed to read file"), state->input_files[filenum], strerror(errno));
			state->exit_status |= 2;
			return -1;
		}
	}

	if (0 != fstat(fd, &isb)) {
		pv_error(state, "%s: %s: %s", _("failed to stat file"), state->input_files[filenum], strerror(errno));
		(void) close(fd);
		state->exit_status |= 2;
		return -1;
	}

	if (0 != fstat(STDOUT_FILENO, &osb)) {
		pv_error(state, "%s: %s", _("failed to stat output file"), strerror(errno));
		(void) close(fd);
		state->exit_status |= 2;
		return -1;
	}

	/*
	 * Check that this new input file is not the same as stdout's
	 * destination. This restriction is ignored for anything other
	 * than a regular file or block device.
	 */
	input_file_is_stdout = true;
	if (isb.st_dev != osb.st_dev)
		input_file_is_stdout = false;
	if (isb.st_ino != osb.st_ino)
		input_file_is_stdout = false;
	if (0 != isatty(fd))
		input_file_is_stdout = false;
	if ((!S_ISREG(isb.st_mode)) && (!S_ISBLK(isb.st_mode)))
		input_file_is_stdout = false;

	if (input_file_is_stdout) {
		pv_error(state, "%s: %s", _("input file is output file"), state->input_files[filenum]);
		(void) close(fd);
		state->exit_status |= 4;
		return -1;
	}

	state->current_file = state->input_files[filenum];
	if (0 == strcmp(state->input_files[filenum], "-")) {
		state->current_file = "(stdin)";
	}
#ifdef O_DIRECT
	/*
	 * Set or clear O_DIRECT on the file descriptor.
	 */
	if (0 != fcntl(fd, F_SETFL, (state->direct_io ? O_DIRECT : 0) | fcntl(fd, F_GETFL))) {
		debug("%s: %s: %s", state->current_file, "fcntl", strerror(errno));
	}
	/*
	 * We don't clear direct_io_changed here, to avoid race conditions
	 * that could cause the input and output settings to differ.
	 */
#endif				/* O_DIRECT */

	debug("%s: %d: %s: fd=%d", "next file opened", filenum, state->current_file, fd);

	return fd;
}

/* EOF */
