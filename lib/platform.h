/*
** Copyright (C) 2004-2013 Red Hat, Inc. All rights reserved.
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/


/** @file
 * Defines for byte-swapping
 */
#ifndef __PLATFORM_H
#define __PLATFORM_H

#include <endian.h>
#include <sys/param.h>
#include <byteswap.h>
#include <bits/wordsize.h>

/* No swapping on little-endian machines */
#if __BYTE_ORDER == __LITTLE_ENDIAN
#define le_swap16(x) (x)
#define le_swap32(x) (x)
#define le_swap64(x) (x)
#else
#define le_swap16(x) bswap_16(x)
#define le_swap32(x) bswap_32(x)
#define le_swap64(x) bswap_64(x)
#endif

/* No swapping on big-endian machines */
#if __BYTE_ORDER == __LITTLE_ENDIAN
#define be_swap16(x) bswap_16(x)
#define be_swap32(x) bswap_32(x)
#define be_swap64(x) bswap_64(x)
#else
#define be_swap16(x) (x)
#define be_swap32(x) (x)
#define be_swap64(x) (x)
#endif


#define swab16(x) x=be_swap16(x)
#define swab32(x) x=be_swap32(x)
#define swab64(x) x=be_swap64(x)


#endif /* __PLATFORM_H */
