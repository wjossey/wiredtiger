/*-
 * Public Domain 2008-2013 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <assert.h>
#include <stdlib.h>
#include <time.h>

#include <wiredtiger.h>
#include <stdarg.h>

void check(const char *fmt, ...)
{
	char buf[200], *end, *p;
	va_list ap;
	size_t len;

	va_start(ap, fmt);
	len = wiredtiger_struct_sizev(fmt, ap);
	va_end(ap);

	assert(len < sizeof buf);

	va_start(ap, fmt);
	assert(wiredtiger_struct_packv(buf, sizeof buf, fmt, ap) == 0);
	va_end(ap);

	printf("%s ", fmt);
	for (p = buf, end = p + len; p < end; p++)
		printf("%02x", *p & 0xff);
	printf("\n");
}

int main() {
	check("iii", 0, 101, -99);
	check("3i", 0, 101, -99);
	check("iS", 42, "forty two");
#if 0
	/* TODO: need a WT_ITEM */
	check("u", r"\x42" * 20)
	check("uu", r"\x42" * 10, r"\x42" * 10)
#endif
	return (0);
}
