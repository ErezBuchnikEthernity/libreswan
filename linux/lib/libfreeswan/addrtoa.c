/*
 * addresses to ASCII
 * Copyright (C) 1998, 1999  Henry Spencer.
 * 
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Library General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <http://www.fsf.org/copyleft/lgpl.txt>.
 * 
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Library General Public
 * License for more details.
 *
 * RCSID $Id: addrtoa.c,v 1.7.36.1 2004/03/21 05:23:31 mcr Exp $
 */
#include "internal.h"
#include "openswan.h"

#define	NBYTES	4		/* bytes in an address */
#define	PERBYTE	4		/* three digits plus a dot or NUL */
#define	BUFLEN	(NBYTES*PERBYTE)

#if BUFLEN != ADDRTOA_BUF
#error	"ADDRTOA_BUF in openswan.h inconsistent with addrtoa() code"
#endif

/*
 - addrtoa - convert binary address to ASCII dotted decimal
 */
size_t				/* space needed for full conversion */
addrtoa(addr, format, dst, dstlen)
struct in_addr addr;
int format;			/* character */
char *dst;			/* need not be valid if dstlen is 0 */
size_t dstlen;
{
	unsigned long a = ntohl(addr.s_addr);
	int i;
	size_t n;
	unsigned long byte;
	char buf[BUFLEN];
	char *p;

	switch (format) {
	case 0:
		break;
	default:
		return 0;
		break;
	}

	p = buf;
	for (i = NBYTES-1; i >= 0; i--) {
		byte = (a >> (i*8)) & 0xff;
		p += ultoa(byte, 10, p, PERBYTE);
		if (i != 0)
			*(p-1) = '.';
	}
	n = p - buf;

	if (dstlen > 0) {
		if (n > dstlen)
			buf[dstlen - 1] = '\0';
		strcpy(dst, buf);
	}
	return n;
}
