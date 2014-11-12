/*
 *  Phusion Passenger - https://www.phusionpassenger.com/
 *  Copyright (c) 2014 Phusion
 *
 *  "Phusion Passenger" is a trademark of Hongli Lai & Ninh Bui.
 *
 *  See LICENSE file for license information.
 */
#ifndef _PASSENGER_DATA_STRUCTURES_LSTRING_H_
#define _PASSENGER_DATA_STRUCTURES_LSTRING_H_

#include <boost/cstdint.hpp>
#include <cstring>
#include <cassert>
#include <algorithm>
#include <MemoryKit/palloc.h>
#include <MemoryKit/mbuf.h>
#include <StaticString.h>
#include <Utils/StrIntUtils.h>
#include <Utils/Hasher.h>

namespace Passenger {

using namespace std;


/**
 * A string data structure that consists of singly-linked parts. Its purpose
 * is to allow us to parse and store HTTP headers in a zero-copy manner.
 * Instead of copying parsed HTTP headers in order to make them contiguous,
 * we just store the headers non-contiguously using LString. Each LString
 * references the mbuf_block that the HTTP header data comes from.
 *
 * The empty string is repesented by `size == 0 && start == NULL && end == NULL`.
 * Parts may never contain the empty string.
 *
 * This struct must be a POD so that we can allocate it with psg_pool_t.
 */
struct LString {
	struct Part {
		Part *next;
		struct MemoryKit::mbuf_block *mbuf_block;
		/** May never be the empty string. */
		const char *data;
		unsigned int size;
	};

	Part *start;
	Part *end;
	unsigned int size;
};


namespace {
	OXT_FORCE_INLINE char
	psg_lstr_first_byte(const LString *str) {
		return str->start->data[0];
	}

	OXT_FORCE_INLINE char
	psg_lstr_last_byte(const LString *str) {
		return str->end->data[str->end->size - 1];
	}
}


inline void psg_lstr_append(LString *str, psg_pool_t *pool, const char *data,
	unsigned int size);


inline void
psg_lstr_init(LString *str) {
	str->start = NULL;
	str->end   = NULL;
	str->size  = 0;
}

inline LString *
psg_lstr_create(psg_pool_t *pool, const char *data, unsigned int size) {
	LString *result = (LString *) psg_palloc(pool, sizeof(LString));
	psg_lstr_init(result);
	psg_lstr_append(result, pool, data, size);
	return result;
}

inline LString *
psg_lstr_create(psg_pool_t *pool, const StaticString &str) {
	return psg_lstr_create(pool, str.data(), str.size());
}

inline void
psg_lstr_append_part(LString *str, LString::Part *part) {
	if (str->end == NULL) {
		str->start = part;
		str->end = part;
	} else {
		str->end->next = part;
		str->end = part;
	}
	str->size += part->size;
	part->next = NULL;
}

inline void
psg_lstr_append(LString *str, psg_pool_t *pool, const MemoryKit::mbuf &buffer,
	const char *data, unsigned int size)
{
	if (size == 0) {
		return;
	}
	LString::Part *part = (LString::Part *) psg_palloc(pool, sizeof(LString::Part));
	part->mbuf_block = buffer.mbuf_block;
	part->data = data;
	part->size = size;
	mbuf_block_ref(buffer.mbuf_block);
	psg_lstr_append_part(str, part);
}

inline void
psg_lstr_append(LString *str, psg_pool_t *pool, const MemoryKit::mbuf &buffer) {
	psg_lstr_append(str, pool, buffer, buffer.start, buffer.size());
}

inline void
psg_lstr_append(LString *str, psg_pool_t *pool, const char *data, unsigned int size) {
	if (size == 0) {
		return;
	}
	LString::Part *part = (LString::Part *) psg_palloc(pool, sizeof(LString::Part));
	part->next = NULL;
	part->mbuf_block = NULL;
	part->data = data;
	part->size = size;
	psg_lstr_append_part(str, part);
}

inline void
psg_lstr_append(LString *str, psg_pool_t *pool, const char *data) {
	psg_lstr_append(str, pool, data, strlen(data));
}

inline LString *
psg_lstr_null_terminate(const LString *str, psg_pool_t *pool) {
	LString *newstr;
	LString::Part *part;
	char *data, *pos;

	data = (char *) psg_pnalloc(pool, str->size + 1);
	pos = data;
	part = str->start;
	while (part != NULL) {
		memcpy(pos, part->data, part->size);
		pos += part->size;
		part = part->next;
	}
	*pos = '\0';

	newstr = (LString *) psg_palloc(pool, sizeof(LString));
	psg_lstr_init(newstr);
	psg_lstr_append(newstr, pool, data, str->size);
	return newstr;
}

inline LString *
psg_lstr_make_contiguous(LString *str, psg_pool_t *pool) {
	if (str->size == 0 || str->start == str->end) {
		return str;
	} else {
		return psg_lstr_null_terminate(str, pool);
	}
}

inline const LString *
psg_lstr_make_contiguous(const LString *str, psg_pool_t *pool) {
	if (str->size == 0 || str->start == str->end) {
		return str;
	} else {
		return psg_lstr_null_terminate(str, pool);
	}
}

inline bool
psg_lstr_cmp(const LString *str, const StaticString &other) {
	const LString::Part *part;
	const char *b;

	// Fast check: check length match
	if (str->size != other.size()) {
		return false;
	}
	// Fast check: check first and last bytes match
	if (str->size > 0
	 && (psg_lstr_first_byte(str) != other[0]
	  || psg_lstr_last_byte(str) != other[other.size() - 1]))
	{
		return false;
	}

	part = str->start;
	b = other.data();
	while (part != NULL) {
		if (memcmp(part->data, b, part->size) != 0) {
			return false;
		}
		b += part->size;
		part = part->next;
	}
	return true;
}

// Check whether the first `size` bytes of both `str` and `other` are equal.
inline bool
psg_lstr_cmp(const LString *str, const StaticString &other, unsigned int size) {
	const LString::Part *part;
	const char *b;
	unsigned int checked;

	if (size > str->size && size > other.size()) {
		size = std::max<size_t>(str->size, other.size());
	}

	// Fast check: check lengths
	if (size == 0) {
		return true;
	}
	if (str->size < size || other.size() < size) {
		return false;
	}
	assert(str->size > 0 && other.size() > 0);

	// Fast check: check first bytes of both strings match
	if (psg_lstr_first_byte(str) != other[0]) {
		return false;
	}
	// Fast check: in the common case where the LString only has 1 part,
	// check whether the last bytes of both strings match
	if (str->start == str->end
	 && str->start->data[size - 1] != other[size - 1])
	{
		return false;
	}

	checked = 0;
	part = str->start;
	b = other.data();
	while (part != NULL && checked < size) {
		unsigned int localSize = std::min(size - checked, part->size);
		if (memcmp(part->data, b, localSize) != 0) {
			return false;
		}
		b += localSize;
		checked += localSize;
		part = part->next;
	}
	return true;
}

inline bool
psg_lstr_cmp(const LString *str, const LString *other) {
	const LString::Part *a_part, *b_part;
	const char *a_start, *a_end;
	const char *b_start, *b_end;
	unsigned int chunklen;

	// Fast check: check length match
	if (str->size != other->size) {
		return false;
	}
	// Fast check: check both strings empty
	if (str->size == 0) {
		return true;
	}
	// Fast check: check first and last bytes match
	if (str->size > 0
	 && (psg_lstr_first_byte(str) != psg_lstr_first_byte(other)
	  || psg_lstr_last_byte(str) != psg_lstr_last_byte(other)))
	{
		return false;
	}

	a_part   = str->start;
	b_part   = other->start;
	a_start  = a_part->data;
	b_start  = b_part->data;
	chunklen = std::min(a_part->size, b_part->size);
	a_end    = a_start + chunklen;
	b_end    = b_start + chunklen;

	while (true) {
		if (memcmp(a_start, b_start, chunklen) != 0) {
			return false;
		}

		// End of part A or B reached?
		if (a_end == a_part->data + a_part->size || b_end == b_part->data + b_part->size) {
			if (((a_end == a_part->data + a_part->size) && a_part->next == NULL)
			 || ((b_end == b_part->data + b_part->size) && b_part->next == NULL))
			{
				// End of entire LString reached.
				assert(a_end == a_part->data + a_part->size);
				assert(b_end == b_part->data + b_part->size);
				assert(a_part->next == NULL);
				assert(b_part->next == NULL);
				return true;
			}

			unsigned int a_len, b_len;

			if (a_end == a_part->data + a_part->size) {
				// End of part A reached.
				a_part  = a_part->next;
				a_len   = a_part->size;
				a_start = a_part->data;
				a_end   = a_start + a_len;
			} else {
				a_len   = a_part->size - (a_end - a_part->data);
				a_start = a_end;
				a_end   = a_start + a_len;
			}

			if (b_end == b_part->data + b_part->size) {
				// End of part B reached.
				b_part  = b_part->next;
				b_len   = b_part->size;
				b_start = b_part->data;
				b_end   = b_start + b_len;
			} else {
				b_len   = b_part->size - (b_end - b_part->data);
				b_start = b_end;
				b_end   = b_start + b_len;
			}

			chunklen = std::min(a_len, b_len);
			a_end = a_start + chunklen;
			b_end = b_start + chunklen;
		}
	}

	return true; // Never reached.
}

inline boost::uint32_t
psg_lstr_hash(const LString *str) {
	const LString::Part *part = str->start;
	Hasher h;

	while (part != NULL) {
		h.update(part->data, part->size);
		part = part->next;
	}
	return h.finalize();
}

inline void
psg_lstr_deinit(LString *str) {
	LString::Part *part;

	for (part = str->start; part != NULL; part = part->next) {
		if (part->mbuf_block != NULL) {
			mbuf_block_unref(part->mbuf_block);
		}
	}

	psg_lstr_init(str);
}

inline char *
appendData(char *pos, const char *end, const LString *str) {
	const LString::Part *part = str->start;
	while (part != NULL) {
		pos = appendData(pos, end, part->data, part->size);
		part = part->next;
	}
	return pos;
}


} // namespace Passenger

#endif /* _PASSENGER_DATA_STRUCTURES_LSTRING_H_ */