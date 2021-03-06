#ifndef TIPPSE_ENCODING_ASCII_H
#define TIPPSE_ENCODING_ASCII_H

#include <stdlib.h>
#include "../encoding.h"

struct encoding_ascii {
  struct encoding vtbl;
};

struct encoding* encoding_ascii_create(void);
void encoding_ascii_destroy(struct encoding* base);

const char* encoding_ascii_name(void);
size_t encoding_ascii_character_length(struct encoding* base);
codepoint_t encoding_ascii_visual(struct encoding* base, codepoint_t cp);
codepoint_t encoding_ascii_decode(struct encoding* base, struct stream* stream, size_t* used);
size_t encoding_ascii_encode(struct encoding* base, codepoint_t cp, uint8_t* text, size_t size);
size_t encoding_ascii_next(struct encoding* base, struct stream* stream);

#endif  /* #ifndef TIPPSE_ENCODING_ASCII_H */
