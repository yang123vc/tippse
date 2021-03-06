#ifndef TIPPSE_DOCUMENT_H
#define TIPPSE_DOCUMENT_H

#include <stdlib.h>
#include <string.h>
#include "types.h"

struct document {
  void (*reset)(struct document* base, struct splitter* splitter);
  void (*draw)(struct document* base, struct screen* screen, struct splitter* splitter);
  void (*keypress)(struct document* base, struct splitter* splitter, int command, struct config_command* arguments, int key, codepoint_t cp, int button, int button_old, int x, int y);
  int (*incremental_update)(struct document* base, struct splitter* splitter);
};

int document_search(struct splitter* splitter, struct range_tree_node* search_text, struct encoding* search_encoding, struct range_tree_node* replace_text, struct encoding* replace_encoding, int reverse, int ignore_case, int regex, int all, int replace);
void document_search_directory(const char* path, struct range_tree_node* search_text, struct encoding* search_encoding, struct range_tree_node* replace_text, struct encoding* replace_encoding, int ignore_case, int regex, int replace, const char* pattern_text, struct encoding* pattern_encoding, int binary);
void document_directory(struct document_file* file, struct stream* filter_stream, struct encoding* filter_encoding, const char* predefined);
void document_insert_search(struct document_file* file, struct search* search, const char* output, size_t length, int inserter);
void document_select_all(struct splitter* splitter, int update_offset);
void document_select_nothing(struct splitter* splitter);
int document_select_delete(struct splitter* splitter);
void document_clipboard_copy(struct splitter* splitter);
void document_clipboard_paste(struct splitter* splitter);

void document_bookmark_toggle_range(struct splitter* splitter, file_offset_t low, file_offset_t high);
void document_bookmark_range(struct splitter* splitter, file_offset_t low, file_offset_t high, int marked);
void document_bookmark_selection(struct splitter* splitter, int marked);
void document_bookmark_toggle_selection(struct splitter* splitter);
void document_bookmark_next(struct splitter* splitter);
void document_bookmark_prev(struct splitter* splitter);

#endif /* #ifndef TIPPSE_DOCUMENT_H */
