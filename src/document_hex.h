#ifndef TIPPSE_DOCUMENT_HEX_H
#define TIPPSE_DOCUMENT_HEX_H

#include <stdlib.h>
#include "types.h"

#include "document.h"

#define DOCUMENT_HEX_MODE_BYTE 0
#define DOCUMENT_HEX_MODE_CHARACTER 1
#define DOCUMENT_HEX_MODE_MAX 2

struct document_hex {
  struct document vtbl;     // virtual table of document
  codepoint_t cp_first;     // first pressed key
  int mode;                 // current editor mode
};

struct document* document_hex_create(void);
void document_hex_destroy(struct document* base);

void document_hex_reset(struct document* base, struct splitter* splitter);
int document_hex_incremental_update(struct document* base, struct splitter* splitter);
void document_hex_draw(struct document* base, struct screen* screen, struct splitter* splitter);
void document_hex_keypress(struct document* base, struct splitter* splitter, int command, struct config_command* arguments, int key, codepoint_t cp, int button, int button_old, int x, int y);
void document_hex_cursor_from_point(struct document* base, struct splitter* splitter, int x, int y, file_offset_t* offset);
uint8_t document_hex_value(codepoint_t cp);
uint8_t document_hex_value_from_string(const char* text, size_t length);
void document_hex_convert(codepoint_t* codepoints, size_t* length, codepoint_t* visuals, int show_invisibles, codepoint_t cp_default);

#endif /* #ifndef TIPPSE_DOCUMENT_HEX_H */
