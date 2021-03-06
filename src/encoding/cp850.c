// Tippse - Encoding CP850 - Encode/Decode of code page 850 bytes to/from unicode code points

#include "cp850.h"

// source de.wikipedia.org/wiki/Codepage_850
uint16_t translate_cp850_unicode[256] = {
  0x2400, 0x263a, 0x263b, 0x2665, 0x2666, 0x2663, 0x2660, 0x2022, 0x25d8, 0x25cb, 0x25d9, 0x2642, 0x2640, 0x266a, 0x266b, 0x263c,
  0x25ba, 0x25c4, 0x2195, 0x203c, 0x00b6, 0x00a7, 0x25ac, 0x21a8, 0x2191, 0x2193, 0x2192, 0x2190, 0x221f, 0x2194, 0x25b2, 0x25bc,
  0x0020, 0x0021, 0x0022, 0x0023, 0x0024, 0x0025, 0x0026, 0x0027, 0x0028, 0x0029, 0x002a, 0x002b, 0x002c, 0x002d, 0x002e, 0x002f,
  0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036, 0x0037, 0x0038, 0x0039, 0x003a, 0x003b, 0x003c, 0x003d, 0x003e, 0x003f,
  0x0040, 0x0041, 0x0042, 0x0043, 0x0044, 0x0045, 0x0046, 0x0047, 0x0048, 0x0049, 0x004a, 0x004b, 0x004c, 0x004d, 0x004e, 0x004f,
  0x0050, 0x0051, 0x0052, 0x0053, 0x0054, 0x0055, 0x0056, 0x0057, 0x0058, 0x0059, 0x005a, 0x005b, 0x005c, 0x005d, 0x005e, 0x005f,
  0x0060, 0x0061, 0x0062, 0x0063, 0x0064, 0x0065, 0x0066, 0x0067, 0x0068, 0x0069, 0x006a, 0x006b, 0x006c, 0x006d, 0x006e, 0x006f,
  0x0070, 0x0071, 0x0072, 0x0073, 0x0074, 0x0075, 0x0076, 0x0077, 0x0078, 0x0079, 0x007a, 0x007b, 0x007c, 0x007d, 0x007e, 0x2302,
  0x00c7, 0x00fc, 0x00e9, 0x00e2, 0x00e4, 0x00e0, 0x00e5, 0x00e7, 0x00ea, 0x00eb, 0x00e8, 0x00ef, 0x00ee, 0x00eb, 0x00c4, 0x00c5,
  0x00c9, 0x00e6, 0x00c6, 0x00f4, 0x00f6, 0x00f2, 0x00fb, 0x00f9, 0x00ff, 0x00d6, 0x00dc, 0x00f8, 0x00a3, 0x00d8, 0x00d7, 0x0192,
  0x00e1, 0x00ed, 0x00f3, 0x00fa, 0x00f1, 0x00d1, 0x00aa, 0x00ba, 0x00bf, 0x00ae, 0x00ac, 0x00bd, 0x00bc, 0x00a1, 0x00ab, 0x00bb,
  0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x00c1, 0x00c2, 0x00c0, 0x00a9, 0x2563, 0x2551, 0x2557, 0x255d, 0x00a2, 0x00a5, 0x2510,
  0x2514, 0x2534, 0x252c, 0x251c, 0x2500, 0x253c, 0x00e3, 0x00c3, 0x255a, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256c, 0x00a4,
  0x00f0, 0x00d0, 0x00ca, 0x00cb, 0x00c8, 0x0131, 0x00cd, 0x00ce, 0x00cf, 0x2518, 0x250c, 0x2588, 0x2584, 0x00a6, 0x00cc, 0x2580,
  0x00d3, 0x00df, 0x00d4, 0x00d2, 0x00f5, 0x00d5, 0x00b5, 0x00fe, 0x00de, 0x00da, 0x00db, 0x00d9, 0x00fd, 0x00dd, 0x00af, 0x00b4,
  0x00ad, 0x00b1, 0x2017, 0x00be, 0x00b6, 0x00a7, 0x00f7, 0x00b8, 0x00b0, 0x00a8, 0x00b7, 0x00b9, 0x00b3, 0x00b2, 0x25a0, 0x00a0
};

#define CODEPOINT_MAX_CP850 0x3000

uint16_t* translate_unicode_cp850 = NULL;

struct encoding* encoding_cp850_create(void) {
  struct encoding_cp850* self = (struct encoding_cp850*)malloc(sizeof(struct encoding_cp850));
  self->vtbl.create = encoding_cp850_create;
  self->vtbl.destroy = encoding_cp850_destroy;
  self->vtbl.name = encoding_cp850_name;
  self->vtbl.character_length = encoding_cp850_character_length;
  self->vtbl.encode = encoding_cp850_encode;
  self->vtbl.decode = encoding_cp850_decode;
  self->vtbl.visual = encoding_cp850_visual;
  self->vtbl.next = encoding_cp850_next;

  if (!translate_unicode_cp850) {
    // TODO: self isn't freed at the moment
    translate_unicode_cp850 = encoding_reverse_table(&translate_cp850_unicode[0], 256, CODEPOINT_MAX_CP850);
  }

  return (struct encoding*)self;
}

void encoding_cp850_destroy(struct encoding* base) {
  struct encoding_cp850* self = (struct encoding_cp850*)base;
  free(self);
}

const char* encoding_cp850_name(void) {
  return "CP850";
}

size_t encoding_cp850_character_length(struct encoding* base) {
  return 1;
}

codepoint_t encoding_cp850_visual(struct encoding* base, codepoint_t cp) {
  if (cp>=0x20 && cp<UNICODE_CODEPOINT_MAX) {
    return cp;
  }

  return (codepoint_t)translate_cp850_unicode[cp];
}

codepoint_t encoding_cp850_decode(struct encoding* base, struct stream* stream, size_t* used) {
  uint8_t c = stream_read_forward(stream);
  *used = 1;
  if (c>=0x20) {
    return (codepoint_t)translate_cp850_unicode[c];
  } else {
    return c;
  }
}

size_t encoding_cp850_encode(struct encoding* base, codepoint_t cp, uint8_t* text, size_t size) {
  if (cp<CODEPOINT_MAX_CP850) {
    uint16_t converted = translate_unicode_cp850[cp];
    if (converted!=(uint16_t)~0u) {
      *text = (uint8_t)translate_unicode_cp850[cp];
      return 1;
    }
  }

  return 0;
}

size_t encoding_cp850_next(struct encoding* base, struct stream* stream) {
  return 1;
}
