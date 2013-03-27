/*
** Copyright (C) 2013 Red Hat, Inc. All rights reserved.
**
** This program is free software: you can redistribute it and/or modify
** it under the terms of the GNU Lesser General Public License as published by
** the Free Software Foundation, either version 2.1 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU Lesser General Public License for more details.
**
** You should have received a copy of the GNU Lesser General Public License
** along with This program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef __CPGLOCK_UTIL_H
#define __CPGLOCK_UTIL_H

void *wait_calloc(size_t len);
ssize_t read_timeout(int fd, void *buf, size_t buflen, int timeout_ms);
ssize_t write_timeout(int fd, void *buf, size_t buflen, int timeout_ms);
int sock_connect(const char *path, int timeout_ms);

#endif
