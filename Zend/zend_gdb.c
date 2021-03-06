/*
   +----------------------------------------------------------------------+
   | Zend Engine                                                          |
   +----------------------------------------------------------------------+
   | Copyright (c) 1998-2017 Zend Technologies Ltd. (http://www.zend.com) |
   +----------------------------------------------------------------------+
   | This source file is subject to version 2.00 of the Zend license,     |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.zend.com/license/2_00.txt.                                |
   | If you did not receive a copy of the Zend license and are unable to  |
   | obtain it through the world-wide-web, please send a note to          |
   | license@zend.com so we can mail you a copy immediately.              |
   +----------------------------------------------------------------------+
   | Authors: Dmitry Stogov <dmitry@zend.com>                             |
   |          Xinchen Hui <xinchen.h@zend.com>                            |
   +----------------------------------------------------------------------+
*/

#include "zend.h"
#include "zend_gdb.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

enum {
	ZEND_GDBJIT_NOACTION,
	ZEND_GDBJIT_REGISTER,
	ZEND_GDBJIT_UNREGISTER
};

typedef struct _zend_gdbjit_code_entry {
	struct _zend_gdbjit_code_entry *next_entry;
	struct _zend_gdbjit_code_entry *prev_entry;
	const char                     *symfile_addr;
	uint64_t                        symfile_size;
} zend_gdbjit_code_entry;

typedef struct _zend_gdbjit_descriptor {
	uint32_t                         version;
	uint32_t                         action_flag;
	struct _zend_gdbjit_code_entry *relevant_entry;
	struct _zend_gdbjit_code_entry *first_entry;
} zend_gdbjit_descriptor;

ZEND_API zend_gdbjit_descriptor __jit_debug_descriptor = {
	1, ZEND_GDBJIT_NOACTION, NULL, NULL
};

ZEND_API zend_never_inline void __jit_debug_register_code()
{
	__asm__ __volatile__("");
}

ZEND_API int zend_gdb_register_code(const void *object, size_t size)
{
	zend_gdbjit_code_entry *entry;

	entry = malloc(sizeof(zend_gdbjit_code_entry) + size);
	if (entry == NULL) {
		return 0;
	}

	entry->symfile_addr = ((char*)entry) + sizeof(zend_gdbjit_code_entry);
	entry->symfile_size = size;

	memcpy((char *)entry->symfile_addr, object, size);

	entry->prev_entry = NULL;
	entry->next_entry = __jit_debug_descriptor.first_entry;

	if (entry->next_entry) {
		entry->next_entry->prev_entry = entry;
	}
	__jit_debug_descriptor.first_entry = entry;

	/* Notify GDB */
	__jit_debug_descriptor.relevant_entry = entry;
	__jit_debug_descriptor.action_flag = ZEND_GDBJIT_REGISTER;
	__jit_debug_register_code();

	return 1;
}

ZEND_API void zend_gdb_unregister_all(void)
{
	zend_gdbjit_code_entry *entry;

	__jit_debug_descriptor.action_flag = ZEND_GDBJIT_UNREGISTER;
	while ((entry = __jit_debug_descriptor.first_entry)) {
		__jit_debug_descriptor.first_entry = entry->next_entry;
		if (entry->next_entry) {
			entry->next_entry->prev_entry = NULL;
		}
		/* Notify GDB */
		__jit_debug_descriptor.relevant_entry = entry;
		__jit_debug_register_code();

		free(entry);
	}
}

ZEND_API int zend_gdb_present(void)
{
	int ret = 0;
	int fd = open("/proc/self/status", O_RDONLY);

	if (fd > 0) {
		char buf[1024];
		ssize_t n = read(fd, buf, sizeof(buf) - 1);
		char *s;
		pid_t pid;

		if (n > 0) {
			buf[n] = 0;
			s = strstr(buf, "TracerPid:");
			if (s) {
				s += sizeof("TracerPid:") - 1;
				while (*s == ' ' || *s == '\t') {
					s++;
				}
				pid = atoi(s);
				if (pid) {
					sprintf(buf, "/proc/%d/exe", (int)pid);
					if (readlink(buf, buf, sizeof(buf) - 1) > 0) {
						if (strstr(buf, "gdb")) {
							ret = 1;
						}
					}
				}
			}
		}

		close(fd);
	}

	return ret;
}

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * indent-tabs-mode: t
 * End:
 */
