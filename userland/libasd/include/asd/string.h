/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, ASD Project Contributors
 */

#ifndef LIBASD_STRING_H
#define LIBASD_STRING_H

#include <stddef.h>
#include <stdint.h>

void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int v, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strcpy(char *d, const char *s);
char *strncpy(char *d, const char *s, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
int atoi(const char *s);
void bzero(void *d, size_t n);
void *memmove(void *dest, const void *src, size_t n);

#endif /* LIBASD_STRING_H */
