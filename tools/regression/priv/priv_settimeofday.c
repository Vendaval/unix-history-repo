/*-
 * Copyright (c) 2006 nCircle Network Security, Inc.
 * All rights reserved.
 *
 * This software was developed by Robert N. M. Watson for the TrustedBSD
 * Project under contract to nCircle Network Security, Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR, NCIRCLE NETWORK SECURITY,
 * INC., OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 * TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * $FreeBSD$
 */

/*
 * Confirm that privilege is required to invoke settimeofday().  So as not
 * to mess up the clock too much, first query the time, then immediately set
 * it.
 */

#include <sys/time.h>

#include <err.h>
#include <errno.h>
#include <unistd.h>

#include "main.h"

void
priv_settimeofday(void)
{
	struct timeval tv;
	int error;

	assert_root();

	/*
	 * Query time.
	 */
	if (gettimeofday(&tv, NULL) < 0)
		err(-1, "gettimeofday");

	/*
	 * Set with privilege.
	 */
	if (settimeofday(&tv, NULL) < 0)
		err(-1, "settimeofday as root");

	/*
	 * Set without privilege.
	 */
	set_euid(UID_OTHER);

	error = settimeofday(&tv, NULL);
	if (error == 0)
		errx(-1, "settimeofday succeeded as !root");
	if (errno != EPERM)
		errx(-1, "settimeofday wrong errno %d as !root", errno);
}
