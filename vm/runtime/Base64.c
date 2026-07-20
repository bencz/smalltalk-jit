// C runtime for packages/Core/src/Base64.st — RFC 4648 encode/decode over the
// standard alphabet. GC discipline (the string-primitive pattern): compute
// sizes and VALIDATE from the raw pointer while nothing allocates, allocate
// the output exactly once (which may move the input), then re-read the input
// pointer through its handle before the copy loop, which itself never
// allocates.
#include "runtime/Base64.h"
#include "core/Handle.h"

static const char base64Alphabet[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";


// The 0..63 value of a base-64 byte, or -1 when it is not one.
static inline int base64Value(uint8_t c)
{
	if (c >= 'A' && c <= 'Z') {
		return c - 'A';
	}
	if (c >= 'a' && c <= 'z') {
		return c - 'a' + 26;
	}
	if (c >= '0' && c <= '9') {
		return c - '0' + 52;
	}
	if (c == '+') {
		return 62;
	}
	if (c == '/') {
		return 63;
	}
	return -1;
}


_Bool base64Encode(Object *input, String **result)
{
	size_t n = rawObjectSize(input->raw);
	String *out = newString(((n + 2) / 3) * 4);   // may GC and move the input
	const uint8_t *src = getRawObjectIndexedVars(input->raw); // re-read AFTER
	uint8_t *dst = (uint8_t *) out->raw->contents;

	size_t i = 0;
	size_t o = 0;
	while (i + 3 <= n) {
		uint32_t b0 = src[i];
		uint32_t b1 = src[i + 1];
		uint32_t b2 = src[i + 2];
		dst[o++] = (uint8_t) base64Alphabet[b0 >> 2];
		dst[o++] = (uint8_t) base64Alphabet[((b0 & 3) << 4) | (b1 >> 4)];
		dst[o++] = (uint8_t) base64Alphabet[((b1 & 15) << 2) | (b2 >> 6)];
		dst[o++] = (uint8_t) base64Alphabet[b2 & 63];
		i += 3;
	}
	if (i < n) {
		uint32_t b0 = src[i];
		dst[o++] = (uint8_t) base64Alphabet[b0 >> 2];
		if (i + 1 < n) {
			uint32_t b1 = src[i + 1];
			dst[o++] = (uint8_t) base64Alphabet[((b0 & 3) << 4) | (b1 >> 4)];
			dst[o++] = (uint8_t) base64Alphabet[(b1 & 15) << 2];
		} else {
			dst[o++] = (uint8_t) base64Alphabet[(b0 & 3) << 4];
			dst[o++] = '=';
		}
		dst[o++] = '=';
	}
	*result = out;
	return 1;
}


_Bool base64Decode(String *input, Class *outputClass, Object **result)
{
	size_t n = input->raw->size;
	const uint8_t *s = (const uint8_t *) input->raw->contents;

	if (n % 4 != 0) {
		return 0;
	}
	size_t pad = 0;
	if (n > 0 && s[n - 1] == '=') {
		pad = s[n - 2] == '=' ? 2 : 1;
	}
	// Alphabet check up to the padding tail; base64Value('=') is -1, so a '='
	// anywhere before the tail fails here too.
	for (size_t i = 0; i < n - pad; i++) {
		if (base64Value(s[i]) < 0) {
			return 0;
		}
	}

	Object *out = newObject(outputClass, n / 4 * 3 - pad); // may move the input
	s = getRawObjectIndexedVars((RawObject *) input->raw);  // re-read AFTER
	uint8_t *d = getRawObjectIndexedVars(out->raw);

	uint32_t accum = 0;
	int bits = 0;
	size_t o = 0;
	for (size_t i = 0; i < n - pad; i++) {
		accum = (accum << 6) | (uint32_t) base64Value(s[i]);
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			d[o++] = (uint8_t) ((accum >> bits) & 255);
		}
	}
	*result = out;
	return 1;
}
