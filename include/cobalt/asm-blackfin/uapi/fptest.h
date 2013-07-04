/*
 * Copyright (C) 2006 Gilles Chanteperdrix <gilles.chanteperdrix@xenomai.org>.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.

 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA.
 */
#ifndef _COBALT_ASM_BLACKFIN_UAPI_FPTEST_H
#define _COBALT_ASM_BLACKFIN_UAPI_FPTEST_H

static inline void fp_regs_set(int features, unsigned int val)
{
}

static inline unsigned int fp_regs_check(int features, unsigned int val,
					 int (*report)(const char *fmt, ...))
{
	return val;
}

#endif /* !_COBALT_ASM_BLACKFIN_UAPI_FPTEST_H */