// Tippse - Document Text View - Cursor and display operations for 2d text mode

#include "document_text.h"

#include "clipboard.h"
#include "config.h"
#include "document.h"
#include "documentfile.h"
#include "documentundo.h"
#include "documentview.h"
#include "editor.h"
#include "encoding.h"
#include "encoding/utf8.h"
#include "filetype.h"
#include "fragment.h"
#include "misc.h"
#include "rangetree.h"
#include "screen.h"
#include "trie.h"
#include "splitter.h"
#include "unicode.h"

// Temporary debug visualisation
#define DEBUG_NONE 0
#define DEBUG_RERENDERDISPLAY 1
#define DEBUG_ALWAYSRERENDER 2
#define DEBUG_PAGERENDERDISPLAY 4
#define DEBUG_LINERENDERTIMING 8

int debug = DEBUG_NONE;
int debug_draw = 0;
int debug_relocates = 0;
int debug_pages_collect = 0;
int debug_pages_prerender = 0;
int debug_pages_render = 0;
int debug_chars = 0;
struct screen* debug_screen = NULL;
struct splitter* debug_splitter = NULL;

extern struct trie* unicode_transform_lower;
extern struct trie* unicode_transform_upper;
extern struct trie* unicode_transform_nfd_nfc;
extern struct trie* unicode_transform_nfc_nfd;

// Create document
struct document* document_text_create(void) {
  struct document_text* document = (struct document_text*)malloc(sizeof(struct document_text));
  document->vtbl.reset = document_text_reset;
  document->vtbl.draw = document_text_draw;
  document->vtbl.keypress = document_text_keypress;
  document->vtbl.incremental_update = document_text_incremental_update;

  return (struct document*)document;
}

// Destroy document
void document_text_destroy(struct document* base) {
  free(base);
}

// Called after a new document was assigned (correct to next character)
void document_text_reset(struct document* base, struct splitter* splitter) {
  struct document_file* file = splitter->file;
  struct document_view* view = splitter->view;

  if (file->buffer) {
    struct range_tree_node* node = range_tree_first(file->buffer);
    if (node) {
      range_tree_shrink(node);
    }
  }

  struct document_text_position in;
  struct document_text_position out;
  in.type = VISUAL_SEEK_OFFSET;
  in.offset = view->offset;
  in.clip = 0;

  view->offset = document_text_cursor_position(splitter, &in, &out, 0, 1);
}

// Goto specified location, apply cursor clipping/wrapping and render dirty pages as necessary
file_offset_t document_text_cursor_position_partial(struct document_text_render_info* render_info, struct splitter* splitter, struct document_text_position* in, struct document_text_position* out, int wrap, int cancel) {
  struct document_file* file = splitter->file;
  struct document_view* view = splitter->view;

  position_t* x;
  position_t* y;
  if (in->type==VISUAL_SEEK_X_Y) {
    x = &in->x;
    y = &in->y;
  } else {
    x = &in->column;
    y = &in->line;
  }

  while (1) {
    while (1) {
      document_text_render_seek(render_info, file->buffer, file->encoding, in);
      if (in->clip) {
        if (document_text_prerender_span(render_info, NULL, NULL, view, file, in, out, ~0, cancel)) {
          break;
        }
      } else {
        if (document_text_collect_span(render_info, view, file, in, out, ~0, cancel)) {
          break;
        }
      }
    }

    if (in->type==VISUAL_SEEK_OFFSET) {
      break;
    }

    if (!out->y_drawn) {
      if (*y<0) {
        *y = 0;
        *x = 0;
      } else if (*y>0) {
        if (in->type==VISUAL_SEEK_X_Y) {
          *y = file->buffer?file->buffer->visuals.ys:0;
        } else {
          *y = file->buffer?file->buffer->visuals.lines:0;
        }

        *x = POSITION_T_MAX;
      } else {
        break;
      }
    } else {
      if (*x>out->x_max) {
        if (wrap) {
          *y = (*y)+1;
          *x = 0;
          wrap = 0;
        } else {
          *x = out->x_max;
        }
      } else if (*x<out->x_min) {
        if (*y>0 && wrap) {
          *y = (*y)-1;
          *x = POSITION_T_MAX;
          wrap = 0;
        } else {
          *x = out->x_min;
        }
      } else {
        break;
      }
    }
  }

  return out->offset;
}

// Return best fitting line width
position_t document_text_line_width(struct splitter* splitter) {
  position_t max = splitter->client_width-splitter->view->address_width;
  position_t selected = splitter->file->defaults.line_width;
  if (selected==0 || max<selected) {
    return max;
  }

  return selected;
}

// Goto specified position
file_offset_t document_text_cursor_position(struct splitter* splitter, struct document_text_position* in, struct document_text_position* out, int wrap, int cancel) {
  struct document_text_render_info render_info;
  document_text_render_clear(&render_info, document_text_line_width(splitter), splitter->view->selection);
  file_offset_t offset = document_text_cursor_position_partial(&render_info, splitter, in, out, wrap, cancel);
  document_text_render_destroy(&render_info);
  return offset;
}

// Clear renderer state to ensure a restart at next seek
void document_text_render_clear(struct document_text_render_info* render_info, position_t width, struct range_tree_node* selection) {
  memset(render_info, 0, (uintptr_t)&render_info->stream-(uintptr_t)render_info);
  render_info->width = width;
  render_info->selection_root = selection;
  stream_from_plain(&render_info->stream, NULL, 0);
}

// Remove renderer temporaries
void document_text_render_destroy(struct document_text_render_info* render_info) {
  stream_destroy(&render_info->stream);
}

// Update renderer state to restart at the given position or to continue if possible
void document_text_render_seek(struct document_text_render_info* render_info, struct range_tree_node* buffer, struct encoding* encoding, const struct document_text_position* in) {
  file_offset_t offset_new = 0;
  position_t x_new = 0;
  position_t y_new = 0;
  position_t lines_new = 0;
  position_t columns_new = 0;
  int indentation_new = 0;
  int indentation_extra_new = 0;
  file_offset_t characters_new = 0;
  struct range_tree_node* buffer_new;

  int type = in->type;
  if (type==VISUAL_SEEK_BRACKET_NEXT || type==VISUAL_SEEK_BRACKET_PREV || type==VISUAL_SEEK_INDENTATION_LAST || type==VISUAL_SEEK_WORD_TRANSITION_NEXT || type==VISUAL_SEEK_WORD_TRANSITION_PREV) {
    type = VISUAL_SEEK_OFFSET;
  }

  buffer_new = range_tree_find_visual(buffer, type, in->offset, in->x, in->y, in->line, in->column, &offset_new, &x_new, &y_new, &lines_new, &columns_new, &indentation_new, &indentation_extra_new, &characters_new, 0, in->offset);

  // TODO: Combine into single statement (if correctness is confirmed)
  bool_t rerender = (debug&DEBUG_ALWAYSRERENDER)?1:0;

  if (render_info->append==0) {
    rerender = 1;
  }

  if (type==VISUAL_SEEK_OFFSET && render_info->offset>in->offset) {
    rerender = 1;
  }

  if (type==VISUAL_SEEK_X_Y && (render_info->y_view>in->y || (render_info->y_view==in->y && render_info->x>in->x && render_info->xs>0))) {
    rerender = 1;
  }

  if (type==VISUAL_SEEK_LINE_COLUMN && (render_info->line>in->line || (render_info->line==in->line && render_info->column>in->column))) {
    rerender = 1;
  }

  int dirty = 0;
  struct range_tree_node* next = range_tree_next(buffer_new);
  if (next) {
    dirty = range_tree_next(buffer_new)->visuals.dirty;
  }

  if (dirty && (!buffer_new || render_info->buffer!=next) && render_info->buffer!=buffer_new) {
    rerender = 1;
  } else if (!dirty && ((next && render_info->offset+range_tree_length(render_info->buffer)<range_tree_offset(next)) || (!next && render_info->offset+range_tree_length(render_info->buffer)<range_tree_length(buffer)) || !render_info->buffer || !buffer_new)) {
    rerender = 1;
  }

  if (rerender) {
    debug_draw++;
    debug_relocates++;
    render_info->append = 1;
    if (buffer_new) {
      render_info->visual_detail = buffer_new->visuals.detail_before;
      render_info->offset_sync = offset_new-buffer_new->visuals.rewind;
      render_info->displacement = buffer_new->visuals.displacement;
      render_info->offset = offset_new+buffer_new->visuals.displacement;
      if (offset_new==0) {
        render_info->visual_detail = VISUAL_DETAIL_NEWLINE;
      }

      render_info->indented = range_tree_find_indentation(buffer_new);

      render_info->visual_detail |= VISUAL_DETAIL_WHITESPACED_COMPLETE;
      render_info->visual_detail &= ~(VISUAL_DETAIL_WHITESPACED_START|VISUAL_DETAIL_STOPPED_INDENTATION);

      render_info->indentation_extra = indentation_extra_new;
      render_info->indentation = indentation_new;
      render_info->x = x_new+render_info->indentation+render_info->indentation_extra;
      render_info->y_view = y_new;
      render_info->line = lines_new;
      render_info->column = columns_new;
      render_info->character = characters_new;

      render_info->xs = 0;
      render_info->ys = 0;
      render_info->lines = 0;
      render_info->columns = 0;
      render_info->characters = 0;
      render_info->indentations = 0;
      render_info->indentations_extra = 0;
      render_info->buffer = buffer_new;
      render_info->keyword_color = buffer_new->visuals.keyword_color;
      render_info->keyword_length = buffer_new->visuals.keyword_length;
      render_info->selection = range_tree_find_offset(render_info->selection_root, render_info->offset, &render_info->selection_displacement);
      for (size_t n = 0; n<VISUAL_BRACKET_MAX; n++) {
        render_info->depth_new[n] = range_tree_find_bracket(buffer_new, n);
        render_info->depth_old[n] = render_info->depth_new[n];
        render_info->depth_line[n] = render_info->depth_new[n];
        render_info->brackets[n].diff = 0;
        render_info->brackets[n].min = 0;
        render_info->brackets[n].max = 0;
        render_info->brackets_line[n].diff = 0;
        render_info->brackets_line[n].min = 0;
        render_info->brackets_line[n].max = 0;
      }
    }

    stream_destroy(&render_info->stream);
    stream_from_page(&render_info->stream, render_info->buffer, render_info->displacement);
    unicode_sequencer_clear(&render_info->sequencer, encoding, &render_info->stream);
  }

  if (in->type==VISUAL_SEEK_X_Y) {
    render_info->y = in->y;
  } else {
    render_info->y = render_info->y_view;
  }
}

// Get word length from specified position
TIPPSE_INLINE int document_text_render_lookahead_word_wrap(struct unicode_sequencer* sequencer, int max, int width0) {
  struct unicode_sequencer alt_sequencer;
  struct stream alt_stream;
  int count = width0;
  size_t advance = 1;
  while (count<max) {
    struct unicode_sequence* sequence = unicode_sequencer_find(sequencer, advance);

    if (sequence->cp[0]<=' ') {
      break;
    }

    count += unicode_width(&sequence->cp[0], sequence->length);

    unicode_sequencer_alternate(&sequencer, advance, &alt_sequencer, &alt_stream);
    advance++;
  }

  unicode_sequencer_drop_alternate(sequencer, advance, &alt_sequencer, &alt_stream);

  if (count>=max) {
    count = width0;
  }

  return count;
}

// Scan for complete whitespace
bool_t document_text_render_whitespace_scan(struct document_text_render_info* render_info, codepoint_t newline_cp1, codepoint_t newline_cp2) {
  struct unicode_sequencer* sequencer = &render_info->sequencer;
  struct unicode_sequencer alt_sequencer;
  struct stream alt_stream;
  file_offset_t displacement = render_info->displacement;
  size_t advance = 0;
  bool_t whitespaced = 0;
  while (1) {
    if (displacement>=render_info->buffer->length) {
      struct range_tree_node* buffer_new = range_tree_next(render_info->buffer);
      whitespaced = buffer_new?range_tree_find_whitespaced(buffer_new):1;
      break;
    }

    struct unicode_sequence* sequence = unicode_sequencer_find(sequencer, advance);
    displacement += sequence->size;
    codepoint_t cp = sequence->cp[0];
    if ((cp==newline_cp1 || cp==0)) {
      whitespaced = 1;
      break;
    }

    if (cp!=' ' && cp!='\t' && cp!=newline_cp2) {
      whitespaced = 0;
      break;
    }

    unicode_sequencer_alternate(&sequencer, advance, &alt_sequencer, &alt_stream);
    advance++;
  }

  unicode_sequencer_drop_alternate(sequencer, advance, &alt_sequencer, &alt_stream);

  return whitespaced;
}

// Split huge pages into smaller ones ahead of rendering time
int document_text_split_buffer(struct range_tree_node* buffer, struct document_file* file) {
  if (!buffer || buffer->length<=TREE_BLOCK_LENGTH_MIN) {
    return 0;
  }

  struct range_tree_node* after = buffer;
  file->buffer = range_tree_split(file->buffer, &after, TREE_BLOCK_LENGTH_MIN, NULL);
  return 1;
}

// Get character size depending on various render states
TIPPSE_INLINE int document_text_fill_width(position_t x, bool_t show_invisibles, int tabstop_width, struct unicode_sequence* sequence, codepoint_t newline_cp1, codepoint_t newline_cp2, codepoint_t* show, codepoint_t* fill_code) {
  int fill;
  codepoint_t cp = sequence->cp[0];
  if (UNLIKELY(show_invisibles)) {
    if (cp=='\t') {
      fill = tabstop_width-(x%tabstop_width);
      *fill_code = ' ';
      *show = 0x00bb;
    } else {
      if (cp==newline_cp1) {
        *show = 0x00ac;
      } else if (cp==newline_cp2) {
        *show = 0x00ac;
      } else if (cp==' ') {
        *show = 0x22c5;
      } else if (cp==0x7f) {
        *show = 0xfffd;
      } else if (cp==UNICODE_CODEPOINT_BOM) {
        *show = 0x2433;
      } else if (cp>UNICODE_CODEPOINT_MAX) {
        *show = 0xfffd;
      } else {
        *show = UNICODE_CODEPOINT_BAD;
      }

      fill = unicode_width(&sequence->cp[0], sequence->length);
      if (*show!=UNICODE_CODEPOINT_BAD || fill<=0) {
        fill = 1;
      }

      *fill_code = UNICODE_CODEPOINT_BAD;
    }
  } else {
    if (cp=='\t') {
      fill = tabstop_width-(x%tabstop_width);
      *fill_code = ' ';
      *show = ' ';
    } else if (cp==newline_cp1) {
      fill = 1;
      *fill_code = UNICODE_CODEPOINT_BAD;
      *show = ' ';
    } else if (cp==newline_cp2) {
      fill = 0;
      *fill_code = UNICODE_CODEPOINT_BAD;
      *show = UNICODE_CODEPOINT_BAD;
    } else if (cp==0) {
      fill = 1;
      *fill_code = UNICODE_CODEPOINT_BAD;
      *show = ' ';
    } else if (cp>UNICODE_CODEPOINT_MAX) {
      fill = 1;
      *fill_code = UNICODE_CODEPOINT_BAD;
      *show = 0xfffd;
    } else {
      fill = unicode_width(&sequence->cp[0], sequence->length);
      *fill_code = UNICODE_CODEPOINT_BAD;
      *show = UNICODE_CODEPOINT_BAD;
    }
  }
  return fill;
}

// Get character size depending on various render states
TIPPSE_INLINE int document_text_fill_width_fillonly(position_t x, bool_t show_invisibles, int tabstop_width, struct unicode_sequence* sequence, codepoint_t newline_cp1, codepoint_t newline_cp2) {
  int fill;
  codepoint_t cp = sequence->cp[0];
  if (UNLIKELY(show_invisibles)) {
    if (cp=='\t') {
      fill = tabstop_width-(x%tabstop_width);
    } else {
      codepoint_t show = UNICODE_CODEPOINT_BAD;
      if (cp==newline_cp1) {
        show = 0x00ac;
      } else if (cp==newline_cp2) {
        show = 0x00ac;
      } else if (cp==' ') {
        show = 0x22c5;
      } else if (cp==0x7f) {
        show = 0xfffd;
      } else if (cp==UNICODE_CODEPOINT_BOM) {
        show = 0x2433;
      } else if (cp>UNICODE_CODEPOINT_MAX) {
        show = 0xfffd;
      } else {
        show = UNICODE_CODEPOINT_BAD;
      }

      fill = unicode_width(&sequence->cp[0], sequence->length);
      if (show!=UNICODE_CODEPOINT_BAD || fill<=0) {
        fill = 1;
      }
    }
  } else {
    if (cp=='\t') {
      fill = tabstop_width-(x%tabstop_width);
    } else if (cp==newline_cp1) {
      fill = 1;
    } else if (cp==newline_cp2) {
      fill = 0;
    } else if (cp==0) {
      fill = 1;
    } else if (cp>UNICODE_CODEPOINT_MAX) {
      fill = 1;
    } else {
      fill = unicode_width(&sequence->cp[0], sequence->length);
    }
  }
  return fill;
}

// Render some pages until the position is found or pages are no longer dirty
// TODO: find better visualization for debugging, find unnecessary render iterations and then optimize (as soon the code is "feature complete")
int document_text_collect_span_base(struct document_text_render_info* render_info, const struct document_view* view, struct document_file* file, const struct document_text_position* in, struct document_text_position* out, int dirty_pages, int cancel) {
  // TODO: Following initializations shouldn't be needed since the caller should check the type / verify
  if (out) {
    out->type = VISUAL_SEEK_NONE;
    out->offset = 0;
    out->x = 0;
    out->y = 0;
    out->x_min = 0;
    out->x_max = 0;
    out->y_drawn = 0;
    out->line = 0;
    out->column = 0;
    out->character = 0;
    out->buffer = NULL;
    out->displacement = 0;
    out->bracket_match = 0;
    for (size_t n = 0; n<VISUAL_BRACKET_MAX; n++) {
      out->depth[n] = 0;
      out->min_line[n] = 0;
    }
  }

  int rendered = 1;
  int stop = render_info->buffer?0:1;
  int page_count = 0;
  bool_t page_dirty = (render_info->buffer && render_info->buffer->visuals.dirty)?1:0;

  render_info->visual_detail |= (view->wrapping)?VISUAL_DETAIL_WRAPPING:0;
  render_info->visual_detail |= (view->show_invisibles)?VISUAL_DETAIL_SHOW_INVISIBLES:0;

  codepoint_t newline_cp1 = (file->newline==TIPPSE_NEWLINE_CR)?'\r':'\n';
  codepoint_t newline_cp2 = (file->newline==TIPPSE_NEWLINE_CRLF)?'\r':UNICODE_CODEPOINT_UNASSIGNED;
  codepoint_t newline_cp3 = (newline_cp2==UNICODE_CODEPOINT_UNASSIGNED)?UNICODE_CODEPOINT_UNASSIGNED:newline_cp1;
  debug_pages_collect++;

  void (*mark)(struct document_text_render_info* render_info) = file->type->mark;
  //int (*match)(const struct document_text_render_info* render_info) = file->type->bracket_match;
  render_info->file_type = file->type;

  if (document_text_split_buffer(render_info->buffer, file)) {
    stream_destroy(&render_info->stream);
    stream_from_page(&render_info->stream, render_info->buffer, render_info->displacement);
    unicode_sequencer_clear(&render_info->sequencer, file->encoding, &render_info->stream);
    page_dirty = 1;
  }

  editor_process_message(file->editor, "Locating...", render_info->offset, range_tree_length(file->buffer));

  bool_t bracketed_line = render_info->bracketed_line;
  bool_t wrapping = view->wrapping;
  bool_t show_invisibles = view->show_invisibles;
  bool_t indented = render_info->indented;
  int tabstop_width = file->tabstop_width;
  render_info->sequence_next = NULL;
  render_info->fill_next = -1;
  while (1) {
    bool_t boundary = 0;
    while (render_info->buffer && UNLIKELY(render_info->displacement>=render_info->buffer->length)) {
      boundary = 1;
      if (render_info->keyword_length<0) {
        render_info->keyword_length = 0;
      }

      bool_t dirty = (render_info->buffer->visuals.xs!=render_info->xs || render_info->buffer->visuals.ys!=render_info->ys || render_info->buffer->visuals.lines!=render_info->lines ||  render_info->buffer->visuals.columns!=render_info->columns || render_info->buffer->visuals.indentation!=render_info->indentations || render_info->buffer->visuals.indentation_extra!=render_info->indentations_extra || render_info->buffer->visuals.detail_after!=render_info->visual_detail || (render_info->ys==0 && render_info->buffer->visuals.dirty && wrapping))?1:0;
      for (size_t n = 0; n<VISUAL_BRACKET_MAX; n++) {
        if (render_info->buffer->visuals.brackets[n].diff!=render_info->brackets[n].diff || render_info->buffer->visuals.brackets[n].min!=render_info->brackets[n].min || render_info->buffer->visuals.brackets[n].max!=render_info->brackets[n].max || render_info->buffer->visuals.brackets_line[n].diff!=render_info->brackets_line[n].diff || render_info->buffer->visuals.brackets_line[n].min!=render_info->brackets_line[n].min || render_info->buffer->visuals.brackets_line[n].max!=render_info->brackets_line[n].max) {
          dirty = 1;
          break;
        }
      }

      if (render_info->buffer->visuals.dirty || dirty) {
        render_info->buffer->visuals.dirty = 0;
        render_info->buffer->visuals.xs = render_info->xs;
        render_info->buffer->visuals.ys = render_info->ys;
        render_info->buffer->visuals.lines = render_info->lines;
        render_info->buffer->visuals.columns = render_info->columns;
        render_info->buffer->visuals.characters = render_info->characters;
        render_info->buffer->visuals.indentation = render_info->indentations;
        render_info->buffer->visuals.indentation_extra = render_info->indentations_extra;
        render_info->buffer->visuals.detail_after = render_info->visual_detail;
        for (size_t n = 0; n<VISUAL_BRACKET_MAX; n++) {
          render_info->buffer->visuals.brackets[n] = render_info->brackets[n];
          render_info->buffer->visuals.brackets_line[n] = render_info->brackets_line[n];
        }

        range_tree_update_calc_all(render_info->buffer);
      }

      render_info->displacement -= render_info->buffer->length;
      file_offset_t rewind = render_info->offset-render_info->offset_sync-render_info->displacement;
      render_info->buffer = range_tree_next(render_info->buffer);
      if (document_text_split_buffer(render_info->buffer, file)) {
        dirty = 1;
      }

      if (render_info->buffer) {
        if (render_info->visual_detail!=render_info->buffer->visuals.detail_before || render_info->keyword_length!=render_info->buffer->visuals.keyword_length || (render_info->keyword_color!=render_info->buffer->visuals.keyword_color && render_info->keyword_length>0) || render_info->displacement!=render_info->buffer->visuals.displacement || rewind!=render_info->buffer->visuals.rewind || dirty) {
          render_info->buffer->visuals.keyword_length = render_info->keyword_length;
          render_info->buffer->visuals.keyword_color = render_info->keyword_color;
          render_info->buffer->visuals.detail_before = render_info->visual_detail;
          render_info->buffer->visuals.displacement = render_info->displacement;
          render_info->buffer->visuals.rewind = rewind;
          render_info->buffer->visuals.dirty |= VISUAL_DIRTY_UPDATE|VISUAL_DIRTY_LEFT;
          range_tree_update_calc_all(render_info->buffer);
        }

        if (render_info->buffer->visuals.dirty) {
          if (dirty_pages!=~0) {
            dirty_pages--;
            if (dirty_pages==0 && stop==0) {
              stop = 2;
            }
          }

          page_count = 0;
        } else {
          page_count++;
        }
      } else {
        if (stop==0) {
          stop = 2;
        }
      }

      render_info->indentations = 0;
      render_info->indentations_extra = 0;
      render_info->xs = 0;
      render_info->ys = 0;
      render_info->columns = 0;
      render_info->lines = 0;
      render_info->characters = 0;
      render_info->visual_detail |= VISUAL_DETAIL_WHITESPACED_COMPLETE;
      render_info->visual_detail &= ~(VISUAL_DETAIL_WHITESPACED_START|VISUAL_DETAIL_STOPPED_INDENTATION);
      for (size_t n = 0; n<VISUAL_BRACKET_MAX; n++) {
        render_info->brackets[n].diff = 0;
        render_info->brackets[n].min = 0;
        render_info->brackets[n].max = 0;
        render_info->brackets_line[n].diff = 0;
        render_info->brackets_line[n].min = 0;
        render_info->brackets_line[n].max = 0;
        render_info->depth_old[n] = render_info->depth_new[n];
        render_info->depth_line[n] = render_info->depth_new[n];
      }
      bracketed_line = 0;

      page_dirty = (render_info->buffer && render_info->buffer->visuals.dirty)?1:0;
      debug_pages_collect++;

      stream_destroy(&render_info->stream);
      stream_from_page(&render_info->stream, render_info->buffer, render_info->displacement);
      unicode_sequencer_clear(&render_info->sequencer, file->encoding, &render_info->stream);
      render_info->sequence_next = NULL;
      render_info->fill_next = -1;

/*      if (page_dirty && render_info->buffer) {
        size_t count = encoding_cache_range(&render_info->cache, &big_buffer[0], render_info->buffer->length+2048);
        stream_destroy(&render_info->stream);
        stream_from_plain(&render_info->stream, &big_buffer[0], count*sizeof(codepoint_t));
        encoding_cache_clear(&render_info->cache,  encoding_native_static(), &render_info->stream);
      }*/

      editor_process_message(file->editor, "Locating...", render_info->offset, range_tree_length(file->buffer));
    }

    render_info->sequence = render_info->sequence_next?render_info->sequence_next:unicode_sequencer_find(&render_info->sequencer, 0);

    codepoint_t cp = render_info->sequence->cp[0];

    if (cp==UNICODE_CODEPOINT_BOM) {
      render_info->visual_detail |= VISUAL_DETAIL_CONTROLCHARACTER;
    } else if (cp!=newline_cp3) {
      render_info->visual_detail &= ~VISUAL_DETAIL_CONTROLCHARACTER;
    }

    int visual_detail_before = render_info->visual_detail;

    if (render_info->keyword_length<=0) {
      // 100ms
      (*mark)(render_info);
    }

    int bracket_match = 0; //(*match)(render_info); inlined for the moment since all file types use the same matching
    if (UNLIKELY((render_info->visual_detail&(VISUAL_DETAIL_STRING0|VISUAL_DETAIL_STRING1|VISUAL_DETAIL_COMMENT0|VISUAL_DETAIL_COMMENT1))==0)) {
      if (cp=='{') {
        bracket_match = 0|VISUAL_BRACKET_OPEN;
      } else if (cp=='[') {
        bracket_match = 1|VISUAL_BRACKET_OPEN;
      } else if (cp=='(') {
        bracket_match = 2|VISUAL_BRACKET_OPEN;
      } else if (cp=='}') {
        bracket_match = 0|VISUAL_BRACKET_CLOSE;
      } else if (cp==']') {
        bracket_match = 1|VISUAL_BRACKET_CLOSE;
      } else if (cp==')') {
        bracket_match = 2|VISUAL_BRACKET_CLOSE;
      }
    }

    // Character bounds / location based stops
    // bibber *brr*
    // values 0 = below; 1 = on line (below); 3 = on line (above); 4 = above
    // bitset 0 = on line; 1 = above (line); 2 = above; 3 = set

    if (!(render_info->visual_detail&VISUAL_DETAIL_CONTROLCHARACTER) || show_invisibles) {
      int drawn = 0;
      if (in->type==VISUAL_SEEK_X_Y) {
        if (render_info->y_view==in->y) {
          drawn = (render_info->x>=in->x)?(2|1):1;
        } else if (render_info->y_view>in->y) {
          drawn = 4;
        }
      } else if (in->type==VISUAL_SEEK_LINE_COLUMN) {
        if (render_info->line==in->line) {
          drawn = (render_info->column>=in->column)?(2|1):1;
        } else if (render_info->line>in->line) {
          drawn = 4;
        }
      } else if (in->type==VISUAL_SEEK_OFFSET) {
        drawn = (render_info->offset>=in->offset)?(4|2|1):1;
      } else if (in->type==VISUAL_SEEK_BRACKET_NEXT) {
        bool_t bracket_correction = ((bracket_match&VISUAL_BRACKET_CLOSE) && ((bracket_match&VISUAL_BRACKET_MASK)==in->bracket))?1:0;
        drawn = (render_info->offset>=in->offset && render_info->depth_new[in->bracket]-bracket_correction==in->bracket_search)?(4|2|1):0;
        rendered = (render_info->offset>=in->offset && (out->type!=VISUAL_SEEK_NONE || (drawn&4)))?1:-1;
      } else if (in->type==VISUAL_SEEK_BRACKET_PREV) {
        bool_t bracket_correction = ((bracket_match&VISUAL_BRACKET_CLOSE) && ((bracket_match&VISUAL_BRACKET_MASK)==in->bracket))?1:0;
        drawn = (render_info->depth_new[in->bracket]-bracket_correction==in->bracket_search && render_info->offset<=in->offset)?(0|1):0;
        if (render_info->offset>=in->offset) {
          rendered = (out->type!=VISUAL_SEEK_NONE || drawn)?1:-1;
        } else {
          rendered = 0;
        }
      } else if (in->type==VISUAL_SEEK_INDENTATION_LAST) {
        if (render_info->line==in->line) {
          drawn = ((render_info->visual_detail&VISUAL_DETAIL_NEWLINE) || (indented))?(0|1):0;
          if (!(render_info->visual_detail&VISUAL_DETAIL_INDENTATION)) {
            drawn |= 1|2;
          }
        } else if (render_info->line>in->line) {
          drawn = 4;
        }
      } else if (in->type==VISUAL_SEEK_WORD_TRANSITION_NEXT) {
        drawn = (render_info->offset>=in->offset && ((render_info->visual_detail^visual_detail_before)&VISUAL_DETAIL_WORD))?(4|2|1):0;
        rendered = (render_info->offset>=in->offset && (out->type!=VISUAL_SEEK_NONE || (drawn&4)))?1:-1;
      } else if (in->type==VISUAL_SEEK_WORD_TRANSITION_PREV) {
        drawn = (render_info->offset<=in->offset && ((render_info->visual_detail^visual_detail_before)&VISUAL_DETAIL_WORD))?(0|1):0;
        if (render_info->offset>=in->offset) {
          rendered = (out->type!=VISUAL_SEEK_NONE || drawn)?1:-1;
        } else {
          rendered = 0;
        }
      }

      if (drawn&1 && out && stop!=1) {
        out->y_drawn |= 1;
        if (in->type==VISUAL_SEEK_X_Y) {
          out->x_max = render_info->x;
          out->x_min = (render_info->visual_detail&VISUAL_DETAIL_WRAPPED)?render_info->indentation+render_info->indentation_extra:0;
        } else if (in->type==VISUAL_SEEK_LINE_COLUMN) {
          out->x_max = render_info->column;
          out->x_min = 0;
        }

        bool_t set = 1;
        if (drawn&2) {
          if (cancel && out->type!=VISUAL_SEEK_NONE) {
            if ((in->type==VISUAL_SEEK_OFFSET && render_info->offset>in->offset) || (in->type==VISUAL_SEEK_X_Y && render_info->x>in->x) || (in->type==VISUAL_SEEK_LINE_COLUMN && render_info->column>in->column)) {
              set = 0;
            }
          }

          stop = 1;
        }

        if (set) {
          out->type = in->type;
          out->x = render_info->x;
          out->y = render_info->y_view;
          out->offset = render_info->offset;
          out->line = render_info->line;
          out->column = render_info->column;
          out->buffer = render_info->buffer;
          out->displacement = render_info->displacement;
          out->character = render_info->character;
          out->bracket_match = bracket_match;
          out->visual_detail = render_info->visual_detail;
          out->lines = render_info->lines;
          out->indented = indented;
          for (size_t n = 0; n<VISUAL_BRACKET_MAX; n++) {
            out->depth[n] = render_info->depth_new[n];
            out->min_line[n] = render_info->brackets_line[n].min;
          }
        }
      }

      if (drawn>=2) {
        stop = 1;
      } else if (boundary && page_count>1 && !stop) {
        if (in->type!=VISUAL_SEEK_BRACKET_NEXT && in->type!=VISUAL_SEEK_BRACKET_PREV && in->type!=VISUAL_SEEK_INDENTATION_LAST && in->type!=VISUAL_SEEK_WORD_TRANSITION_NEXT && in->type!=VISUAL_SEEK_WORD_TRANSITION_PREV) {
          rendered = 0;
        }

        stop = 1;
      }
    }

    if (cp==newline_cp1) {
      render_info->visual_detail &= ~VISUAL_DETAIL_CONTROLCHARACTER;
    } else if (cp==newline_cp2) {
      render_info->visual_detail |= VISUAL_DETAIL_CONTROLCHARACTER;
    }

    if (stop && (boundary || !page_dirty)) {
      break;
    }

    int fill = render_info->fill_next>=0?render_info->fill_next:document_text_fill_width_fillonly(render_info->x, show_invisibles, tabstop_width, render_info->sequence, newline_cp1, newline_cp2);

    if (render_info->visual_detail&VISUAL_DETAIL_NEWLINE) {
      render_info->visual_detail &= ~VISUAL_DETAIL_NEWLINE;
      indented = 1;
    }

    if (!(render_info->visual_detail&VISUAL_DETAIL_INDENTATION)) {
      render_info->visual_detail |= VISUAL_DETAIL_STOPPED_INDENTATION;
      indented = 0;
    }

    if (indented && render_info->indentation<render_info->width/2) {
      render_info->indentation += fill;
      render_info->indentations += fill;
    } else {
      render_info->xs += fill;
    }

    render_info->x += fill;

    render_info->displacement += render_info->sequence->size;
    render_info->offset += render_info->sequence->size;
    render_info->selection_displacement += render_info->sequence->size;

    render_info->sequence_next = unicode_sequencer_find(&render_info->sequencer, 1);
    unicode_sequencer_advance(&render_info->sequencer, 1);

    render_info->keyword_length--;

    if (!(render_info->visual_detail&VISUAL_DETAIL_CONTROLCHARACTER)) {
      render_info->column++;
      render_info->columns++;
      render_info->character++;
      render_info->characters++;

      if (render_info->keyword_length<=0) {
        render_info->offset_sync = render_info->offset;
      }
    }

    int width0 = render_info->fill_next = document_text_fill_width_fillonly(render_info->x, show_invisibles, tabstop_width, render_info->sequence_next, newline_cp1, newline_cp2);
    if (wrapping && cp<=' ' && render_info->sequence_next->cp[0]>' ') {
      position_t row_width = render_info->width-render_info->indentation-tabstop_width;
      width0 = document_text_render_lookahead_word_wrap(&render_info->sequencer, row_width+1, render_info->fill_next);
    }

    if (cp==newline_cp1 || (wrapping && render_info->x+(width0?width0:1)>render_info->width)) {
      if (cp==newline_cp1) {
        render_info->indentations = 0;
        render_info->indentations_extra = 0;
        render_info->indentation = 0;
        render_info->indentation_extra = 0;

        if (render_info->visual_detail&VISUAL_DETAIL_WHITESPACED_COMPLETE) {
          render_info->visual_detail |= VISUAL_DETAIL_WHITESPACED_START;
        }

        render_info->visual_detail |= VISUAL_DETAIL_NEWLINE;
        render_info->visual_detail &= ~(VISUAL_DETAIL_WRAPPED|VISUAL_DETAIL_STOPPED_INDENTATION);

        indented = 0;

        render_info->line++;
        render_info->lines++;
        render_info->column = 0;
        render_info->columns = 0;

        if (bracketed_line) {
          bracketed_line = 0;
          for (size_t n = 0; n<VISUAL_BRACKET_MAX; n++) {
            render_info->brackets_line[n].diff = 0;
            render_info->brackets_line[n].min = 0;
            render_info->brackets_line[n].max = 0;
            render_info->depth_line[n] = render_info->depth_new[n];
          }
        }
      } else {
        if (render_info->indentation_extra==0) {
          render_info->indentations_extra = tabstop_width;
          render_info->indentation_extra = tabstop_width;
        }

        render_info->visual_detail |= VISUAL_DETAIL_WRAPPED;
      }

      render_info->ys++;
      render_info->y_view++;
      render_info->x = render_info->indentation+render_info->indentation_extra;
      render_info->xs = 0;
      if (render_info->sequence_next && render_info->sequence_next->cp[0]=='\t') {
        render_info->fill_next = tabstop_width;
      }
    }

    if (cp!='\t' && cp!=' ' && cp!=newline_cp2) {
      render_info->visual_detail &= ~VISUAL_DETAIL_WHITESPACED_COMPLETE;
    }

    if (bracket_match) {
      bracketed_line = bracket_match;
      if (bracket_match&VISUAL_BRACKET_CLOSE) {
        size_t bracket = bracket_match&VISUAL_BRACKET_MASK;
        render_info->depth_new[bracket]--;
        int min = render_info->depth_old[bracket]-render_info->depth_new[bracket];
        if (min>render_info->brackets[bracket].min) {
          render_info->brackets[bracket].min = min;
        }

        render_info->brackets[bracket].diff--;

        int min_line = render_info->depth_line[bracket]-render_info->depth_new[bracket];
        if (min_line>render_info->brackets_line[bracket].min) {
          render_info->brackets_line[bracket].min = min_line;
        }

        render_info->brackets_line[bracket].diff--;
      } else {
        size_t bracket = bracket_match&VISUAL_BRACKET_MASK;
        render_info->depth_new[bracket]++;
        int max = render_info->depth_new[bracket]-render_info->depth_old[bracket];
        if (max>render_info->brackets[bracket].max) {
          render_info->brackets[bracket].max = max;
        }

        render_info->brackets[bracket].diff++;

        int max_line = render_info->depth_new[bracket]-render_info->depth_line[bracket];
        if (max_line>render_info->brackets_line[bracket].max) {
          render_info->brackets_line[bracket].max = max_line;
        }

        render_info->brackets_line[bracket].diff++;
      }
    }
  }

  render_info->indented = indented;
  render_info->bracketed_line = bracketed_line;
  return rendered;
}

int document_text_collect_span(struct document_text_render_info* render_info, const struct document_view* view, struct document_file* file, const struct document_text_position* in, struct document_text_position* out, int dirty_pages, int cancel) {
  // Trampoline for special cases
  /*if (view->wrapping && !view->show_invisibles && in->type==VISUAL_SEEK_X_Y) {
    return document_text_collect_span_w_x_y(render_info, view, file, in, out, dirty_pages, cancel);
  }*/

  return document_text_collect_span_base(render_info, view, file, in, out, dirty_pages, cancel);
}

int document_text_prerender_span(struct document_text_render_info* render_info, struct screen* screen, struct splitter* splitter, const struct document_view* view, struct document_file* file, const struct document_text_position* in, struct document_text_position* out, int dirty_pages, int cancel) {
  // TODO: Following initializations shouldn't be needed since the caller should check the type / verify
  if (out) {
    out->type = VISUAL_SEEK_NONE;
    out->offset = 0;
    out->x = 0;
    out->y = 0;
    out->line = 0;
    out->visual_detail = 0;
  }

  int rendered = 1;
  int stop = render_info->buffer?0:1;

  codepoint_t newline_cp1 = (file->newline==TIPPSE_NEWLINE_CR)?'\r':'\n';
  codepoint_t newline_cp2 = (file->newline==TIPPSE_NEWLINE_CRLF)?'\r':UNICODE_CODEPOINT_UNASSIGNED;
  debug_pages_prerender++;

  void (*mark)(struct document_text_render_info* render_info) = file->type->mark;
  render_info->file_type = file->type;

  render_info->sequence_next = NULL;
  while (1) {
    while (render_info->buffer && render_info->displacement>=render_info->buffer->length) {
      render_info->displacement -= render_info->buffer->length;
      render_info->buffer = range_tree_next(render_info->buffer);
      if (render_info->keyword_length<0) {
        render_info->keyword_length = 0;
      }

      if (!render_info->buffer) {
        stop = 2;
      }

      debug_pages_prerender++;
    }

    if (render_info->y_view==in->y) {
      out->type = in->type;
      out->offset = render_info->offset;
      out->x = render_info->x;
      out->y = render_info->y_view;
      out->line = render_info->line;
      out->visual_detail = render_info->visual_detail;
      stop = 1;
    }

    if (stop) {
      break;
    }

    render_info->sequence = render_info->sequence_next?render_info->sequence_next:unicode_sequencer_find(&render_info->sequencer, 0);

    codepoint_t cp = render_info->sequence->cp[0];

    if (render_info->keyword_length<=0) {
      (*mark)(render_info);
    }

    int fill = render_info->sequence_next?render_info->fill_next:document_text_fill_width_fillonly(render_info->x, view->show_invisibles, file->tabstop_width, render_info->sequence, newline_cp1, newline_cp2);

    render_info->x += fill;
    if (render_info->visual_detail&VISUAL_DETAIL_NEWLINE) {
      render_info->visual_detail &= ~VISUAL_DETAIL_NEWLINE;
      render_info->indented = 1;
    }

    if (!(render_info->visual_detail&VISUAL_DETAIL_INDENTATION)) {
      render_info->visual_detail |= VISUAL_DETAIL_STOPPED_INDENTATION;
      render_info->indented = 0;
    }

    if (render_info->indented && render_info->indentation<render_info->width/2) {
      render_info->indentation += fill;
    }

    render_info->displacement += render_info->sequence->size;
    render_info->offset += render_info->sequence->size;
    render_info->selection_displacement += render_info->sequence->size;

    render_info->sequence_next = unicode_sequencer_find(&render_info->sequencer, 1);
    unicode_sequencer_advance(&render_info->sequencer, 1);

    render_info->keyword_length--;

    int width0 = render_info->fill_next = document_text_fill_width_fillonly(render_info->x, view->show_invisibles, file->tabstop_width, render_info->sequence_next, newline_cp1, newline_cp2);
    if (view->wrapping && cp<=' ' && render_info->sequence_next->cp[0]>' ') {
      position_t row_width = render_info->width-render_info->indentation-file->tabstop_width;
      width0 = document_text_render_lookahead_word_wrap(&render_info->sequencer, row_width+1, render_info->fill_next);
    }

    if (cp==newline_cp1 || (view->wrapping && render_info->x+(width0?width0:1)>render_info->width)) {
      if (cp==newline_cp1) {
        render_info->indentation = 0;
        render_info->indentation_extra = 0;

        render_info->visual_detail |= VISUAL_DETAIL_NEWLINE;
        render_info->visual_detail &= ~(VISUAL_DETAIL_WRAPPED|VISUAL_DETAIL_STOPPED_INDENTATION);

        render_info->indented = 0;

        render_info->line++;
      } else {
        if (render_info->indentation_extra==0) {
          render_info->indentation_extra = file->tabstop_width;
        }

        render_info->visual_detail |= VISUAL_DETAIL_WRAPPED;
      }

      render_info->y_view++;
      render_info->x = render_info->indentation+render_info->indentation_extra;
      render_info->sequence_next = NULL;
    }
  }

  return rendered;
}

int document_text_render_span(struct document_text_render_info* render_info, struct screen* screen, struct splitter* splitter, const struct document_view* view, struct document_file* file, const struct document_text_position* in, struct document_text_position* out, int dirty_pages, int cancel) {
  // TODO: Following initializations shouldn't be needed since the caller should check the type / verify
  int rendered = 1;
  int stop = render_info->buffer?0:1;

  codepoint_t newline_cp1 = (file->newline==TIPPSE_NEWLINE_CR)?'\r':'\n';
  codepoint_t newline_cp2 = (file->newline==TIPPSE_NEWLINE_CRLF)?'\r':UNICODE_CODEPOINT_UNASSIGNED;
  debug_pages_render++;

  void (*mark)(struct document_text_render_info* render_info) = file->type->mark;
  render_info->file_type = file->type;

  position_t whitespace_x = -1;
  position_t whitespace_max = 0;
  position_t whitespace_y = 0;

  codepoint_t show;
  codepoint_t fill_code;
  render_info->sequence_next = NULL;

  while (1) {
    while (render_info->buffer && render_info->displacement>=render_info->buffer->length) {
      render_info->visual_detail &= ~(VISUAL_DETAIL_STOPPED_INDENTATION);

      render_info->displacement -= render_info->buffer->length;
      render_info->buffer = range_tree_next(render_info->buffer);

      if (render_info->keyword_length<0) {
        render_info->keyword_length = 0;
      }

      if (!render_info->buffer) {
        stop = 1;
      }

      debug_pages_render++;
    }

    if (stop) {
      break;
    }

    render_info->sequence = render_info->sequence_next?render_info->sequence_next:unicode_sequencer_find(&render_info->sequencer, 0);

    codepoint_t cp = render_info->sequence->cp[0];

    if (render_info->keyword_length<=0) {
      (*mark)(render_info);
    }

    int fill;
    if (render_info->sequence_next) {
      fill = render_info->fill_next;
      show = render_info->show_next;
      fill_code = render_info->fill_code_next;
    } else {
      fill = document_text_fill_width(render_info->x, view->show_invisibles, file->tabstop_width, render_info->sequence, newline_cp1, newline_cp2, &show, &fill_code);
    }

    if (fill>0) {
      position_t x = render_info->x-view->scroll_x+view->address_width;
      if (x>=view->address_width) {
        int color = file->defaults.colors[VISUAL_FLAG_COLOR_TEXT];
        int background = file->defaults.colors[VISUAL_FLAG_COLOR_BACKGROUND];

        if (render_info->buffer) {
          if (render_info->buffer->inserter&TIPPSE_INSERTER_HIGHLIGHT) {
            color = file->defaults.colors[render_info->buffer->inserter>>TIPPSE_INSERTER_HIGHLIGHT_COLOR_SHIFT];
          }

          if ((debug&DEBUG_PAGERENDERDISPLAY)) {
            color = (intptr_t)render_info->buffer&0xff;
          }

          if ((debug&DEBUG_RERENDERDISPLAY)) {
            background = (debug_draw%22)*6+21;
          }
        }

        while (render_info->selection && render_info->selection_displacement>=render_info->selection->length) {
          render_info->selection_displacement -= render_info->selection->length;
          render_info->selection = render_info->selection->next;
        }

        if (render_info->selection && (render_info->selection->inserter&TIPPSE_INSERTER_MARK)) {
          background = file->defaults.colors[VISUAL_FLAG_COLOR_SELECTION];
        }

        if (render_info->keyword_length>0) {
          color = file->defaults.colors[render_info->keyword_color];
        }

        position_t y = render_info->y-view->scroll_y;

        if (cp==' ' || cp=='\t' || cp==newline_cp2) {
          if (whitespace_x==-1) {
            whitespace_x = x;
          }
        } else {
          if (cp!=newline_cp1) {
            whitespace_x = -1;
          }
        }

        whitespace_max = x+((cp!=newline_cp1)?fill:0);
        whitespace_y = y;

        if (show!=UNICODE_CODEPOINT_BAD) {
          splitter_drawchar(splitter, screen, (int)x, (int)y, &show, 1, color, background);
        } else {
          struct unicode_sequence visuals;
          encoding_visuals(file->encoding, render_info->sequence, &visuals);

          splitter_drawunicode(splitter, screen, (int)x, (int)y, &visuals, color, background);
        }

        for (int pos = 1; pos<fill; pos++) {
          splitter_drawchar(splitter, screen, (int)x+pos, (int)y, &fill_code, 1, color, background);
        }
      }
    }

    render_info->x += fill;
    if (render_info->visual_detail&VISUAL_DETAIL_NEWLINE) {
      render_info->visual_detail &= ~VISUAL_DETAIL_NEWLINE;
      render_info->indented = 1;
    }

    if (!(render_info->visual_detail&VISUAL_DETAIL_INDENTATION)) {
      render_info->visual_detail |= VISUAL_DETAIL_STOPPED_INDENTATION;
      render_info->indented = 0;
    }

    if (render_info->indented && render_info->indentation<render_info->width/2) {
      render_info->indentation += fill;
    }

    render_info->displacement += render_info->sequence->size;
    render_info->offset += render_info->sequence->size;
    render_info->selection_displacement += render_info->sequence->size;

    render_info->sequence_next = unicode_sequencer_find(&render_info->sequencer, 1);
    unicode_sequencer_advance(&render_info->sequencer, 1);

    render_info->keyword_length--;

    int width0 = render_info->fill_next = document_text_fill_width(render_info->x, view->show_invisibles, file->tabstop_width, render_info->sequence_next, newline_cp1, newline_cp2, &render_info->show_next, &render_info->fill_code_next);
    if (view->wrapping && cp<=' ' && render_info->sequence_next->cp[0]>' ') {
      position_t row_width = render_info->width-render_info->indentation-file->tabstop_width;
      width0 = document_text_render_lookahead_word_wrap(&render_info->sequencer, row_width+1, render_info->fill_next);
    }

    if (cp==newline_cp1 || (view->wrapping && render_info->x+(width0?width0:1)>render_info->width)) {
      if (cp==newline_cp1) {
        render_info->indentation = 0;
        render_info->indentation_extra = 0;

        render_info->visual_detail |= VISUAL_DETAIL_NEWLINE;
        render_info->visual_detail &= ~(VISUAL_DETAIL_WRAPPED|VISUAL_DETAIL_STOPPED_INDENTATION);

        render_info->indented = 0;

        render_info->line++;
      } else {
        if (render_info->indentation_extra==0) {
          render_info->indentations_extra = file->tabstop_width;
          render_info->indentation_extra = file->tabstop_width;
        }

        render_info->visual_detail |= VISUAL_DETAIL_WRAPPED;
      }

      render_info->y_view++;
      render_info->x = render_info->indentation+render_info->indentation_extra;
      render_info->sequence_next = NULL;
      stop = 1;
    }

    if (render_info->x-view->scroll_x>render_info->width) {
      stop = 1;
    }
  }

  if (whitespace_x>=0) {
    if (!render_info->buffer || (render_info->visual_detail&VISUAL_DETAIL_NEWLINE) || document_text_render_whitespace_scan(render_info, newline_cp1, newline_cp2)) {
      int background = 1;
      int color = file->defaults.colors[VISUAL_FLAG_COLOR_BACKGROUND];
      for (position_t x = whitespace_x; x<whitespace_max; x++) {
        splitter_exchange_color(splitter, screen, (int)x, (int)whitespace_y, color, background);
      }
    }
  }

  return rendered;
}

// Find next dirty pages and rerender them (background task)
int document_text_incremental_update(struct document* base, struct splitter* splitter) {
  struct document_file* file = splitter->file;
  struct document_view* view = splitter->view;

  if (file->buffer && file->buffer->visuals.dirty) {
    struct document_text_position in;
    in.type = VISUAL_SEEK_OFFSET;
    in.offset = range_tree_length(file->buffer);
    in.clip = 0;

    struct document_text_render_info render_info;
    document_text_render_clear(&render_info, document_text_line_width(splitter), view->selection);
    document_text_render_seek(&render_info, file->buffer, file->encoding, &in);
    document_text_collect_span(&render_info, view, file, &in, NULL, 16, 1);
    document_text_render_destroy(&render_info);
  }

  file_offset_t autocomplete_length = range_tree_length(file->buffer);
  if (autocomplete_length>TIPPSE_AUTOCOMPLETE_MAX) {
    autocomplete_length = TIPPSE_AUTOCOMPLETE_MAX;
  }

  if (file->buffer && file->autocomplete_offset<autocomplete_length) {
    file_offset_t displacement;
    struct range_tree_node* buffer = range_tree_find_offset(file->buffer, file->autocomplete_offset, &displacement);
    struct stream stream;
    stream_from_page(&stream, buffer, displacement);
    struct unicode_sequencer sequencer;
    unicode_sequencer_clear(&sequencer, file->encoding, &stream);
    size_t count = 0;

    if (!file->autocomplete_build) {
      file->autocomplete_build = trie_create(0);
    }

    if (!file->autocomplete_last) {
      file->autocomplete_last = trie_create(0);
    }

    struct trie_node* parent_build = NULL;
    struct trie_node* parent_last = NULL;
    while (1) {
      codepoint_t cp = unicode_sequencer_find(&sequencer, 0)->cp[0];
      if (cp>UNICODE_CODEPOINT_MAX) {
        break;
      }

      int literal = unicode_word(cp);
      if (count>10000 && (!literal || (!parent_last && !parent_build))) {
        break;
      }

      if (!literal) {
        if (parent_last) {
          parent_last->end = 1;
          parent_last = NULL;
        }
        if (parent_build) {
          parent_build->end = 1;
          parent_build = NULL;
        }
      } else {
        // TODO: codepoint!=sequence
        parent_last = trie_append_codepoint(file->autocomplete_last, parent_last, cp, 0);
        parent_build = trie_append_codepoint(file->autocomplete_build, parent_build, cp, 0);
      }
      unicode_sequencer_advance(&sequencer, 1);
      count++;
    }
    file->autocomplete_offset = stream_offset(&stream);
    if (file->autocomplete_offset>=autocomplete_length) {
      trie_destroy(file->autocomplete_last);
      file->autocomplete_last = file->autocomplete_build;
      file->autocomplete_build = NULL;
    }
    stream_destroy(&stream);
  }

  if (file->autocomplete_offset>=range_tree_length(file->buffer) && file->autocomplete_rescan>1) {
    file->autocomplete_rescan = 0;
    file->autocomplete_offset = 0;
  }

  return file->buffer?file->buffer->visuals.dirty:0;
}

// Draw entire visible screen space
void document_text_draw(struct document* base, struct screen* screen, struct splitter* splitter) {

  debug_screen = screen;
  debug_splitter = splitter;
  struct document_file* file = splitter->file;
  struct document_view* view = splitter->view;

  if (file->save && file->buffer && file->buffer->length!=view->selection->length) {
    printf("\r\nSelection area and file length differ %d<>%d\r\n", (int)file->buffer->length, (int)view->selection->length);
    exit(0);
  }

  view->address_width = 6;

  struct document_text_position cursor;
  struct document_text_position in;

  int prerender = 0;
  in.type = VISUAL_SEEK_OFFSET;
  in.clip = 0;
  in.offset = view->offset;

  // TODO: cursor position is already known by seek flag in keypress function? Test me.
  document_text_cursor_position(splitter, &in, &cursor, 0, 1);

  while (1) {
    position_t scroll_x = view->scroll_x;
    position_t scroll_y = view->scroll_y;
    if (cursor.x<view->scroll_x) {
      view->scroll_x = cursor.x;
    }
    if (cursor.x>=view->scroll_x+splitter->client_width-1-view->address_width) {
      view->scroll_x = cursor.x-(splitter->client_width-1-view->address_width);
    }
    if (cursor.y<view->scroll_y) {
      view->scroll_y = cursor.y;
    }
    if (cursor.y>=view->scroll_y+splitter->client_height-1) {
      view->scroll_y = cursor.y-(splitter->client_height-1);
    }
    if (view->scroll_y+splitter->client_height>(file->buffer?file->buffer->visuals.ys+1:0) && (file->buffer?file->buffer->visuals.dirty:0)==0) {
      view->scroll_y = (file->buffer?file->buffer->visuals.ys+1:0)-(splitter->client_height);
    }
    if (view->scroll_y<0) {
      view->scroll_y = 0;
    }
    if (view->scroll_x<0) {
      view->scroll_x = 0;
    }

    if (scroll_x==view->scroll_x && scroll_y==view->scroll_y && prerender>0) {
      break;
    }

    prerender++;

    struct document_text_render_info render_info;
    document_text_render_clear(&render_info, document_text_line_width(splitter), view->selection);
    in.type = VISUAL_SEEK_X_Y;
    in.clip = 0;
    in.x = view->scroll_x+splitter->client_width;
    in.y = view->scroll_y+splitter->client_height;

    while (1) {
      document_text_render_seek(&render_info, file->buffer, file->encoding, &in);
      if (document_text_collect_span(&render_info, view, file, &in, NULL, ~0, 1)) {
        break;
      }
    }
    document_text_render_destroy(&render_info);
  }

  if (view->scroll_x_old!=view->scroll_x || view->scroll_y_old!=view->scroll_y) {
    view->scroll_x_old = view->scroll_x;
    view->scroll_y_old = view->scroll_y;
    view->show_scrollbar = 1;
  }

  struct document_text_render_info render_info;
  document_text_render_clear(&render_info, document_text_line_width(splitter), view->selection);
  in.type = VISUAL_SEEK_X_Y;
  in.clip = 1;
  in.x = view->scroll_x;
  position_t last_line = -1;
  for (position_t y = 0; y<splitter->client_height+1; y++) {
    debug_relocates = 0;
    debug_pages_collect = 0;
    debug_pages_prerender = 0;
    debug_pages_render = 0;
    debug_chars = 0;
    in.y = y+view->scroll_y;
    int64_t t1 = tick_count();

    // Get render start offset (for bookmark detection)
    struct document_text_position out;

    //document_text_cursor_position_partial(&render_info, splitter, &in_x_y, &out, 0, 1);
    document_text_render_seek(&render_info, file->buffer, file->encoding, &in);
    document_text_prerender_span(&render_info, screen, splitter, view, file, &in, &out, ~0, 1);

    int debug_seek_chars = debug_chars;

    int start_x = (int)render_info.x;

    // Line wrap indicator
    if ((out.visual_detail&VISUAL_DETAIL_WRAPPED) && view->show_invisibles && out.y==in.y) {
      position_t x = out.x-file->tabstop_width-view->scroll_x+view->address_width;
      // TODO: cp should be 0x21aa
      splitter_drawtext(splitter, screen, x, (int)y, "*", 1, file->defaults.colors[VISUAL_FLAG_COLOR_LINENUMBER], file->defaults.colors[VISUAL_FLAG_COLOR_BACKGROUND]);
    }

    int64_t t2 = tick_count();

    // Actual rendering
    document_text_render_span(&render_info, screen, splitter, view, file, &in, NULL, ~0, 1);

    int64_t t3 = tick_count();

    int end_x = (int)render_info.x;

    // Bookmark detection
    int marked = range_tree_marked(file->bookmarks, out.offset, render_info.offset-out.offset, TIPPSE_INSERTER_MARK);

    if (in.y<=(file->buffer?file->buffer->visuals.ys:0)) {
      char line[1024];
      int size = 0;
      if (out.line!=last_line) {
        last_line = out.line;
        size = sprintf(line, "%lld", (long long int)(out.line+1));
      }

      if (view->address_width>0) {
        int start = size-(view->address_width-1);
        int x = 0;
        if (start<=0) {
          start = 0;
          x = (view->address_width-1)-size;
        } else {
          size = view->address_width+1;
          start--;
        }

        splitter_drawtext(splitter, screen, x, (int)y, line+start, (size_t)size, file->defaults.colors[marked?VISUAL_FLAG_COLOR_BOOKMARK:VISUAL_FLAG_COLOR_LINENUMBER], file->defaults.colors[VISUAL_FLAG_COLOR_BACKGROUND]);
      }
    }

    int64_t te = tick_count();
    if (debug&DEBUG_LINERENDERTIMING) {
      fprintf(stderr, "%d: %d | %d/%d/%d | %d-%d (%d, %d) @ %d + %d\r\n", (int)y, (int)debug_relocates, (int)debug_pages_collect, (int)debug_pages_prerender, (int)debug_pages_render, (int)debug_chars, (int)debug_seek_chars, start_x, end_x, (int)(te-t1), (int)(t3-t2));
    }
  }

  document_text_render_destroy(&render_info);

  if (!screen) {
    return;
  }

  size_t name_length = strlen(file->filename);
  int modified = document_undo_modified(file);
  char* title = (char*)malloc((name_length+(size_t)modified*2+1)*sizeof(char));
  memcpy(title, file->filename, name_length);
  if (modified) {
    memcpy(title+name_length, " *\0", 3);
  } else {
    title[name_length] = '\0';
  }
  splitter_name(splitter, title);
  free(title);

  if (document_view_select_active(view)) {
    splitter_cursor(splitter, screen, -1, -1);
  } else {
    splitter_cursor(splitter, screen, (int)(cursor.x-view->scroll_x+view->address_width), (int)(cursor.y-view->scroll_y));
  }

  int mark1 = document_text_mark_brackets(base, screen, splitter, &cursor);
  if (mark1<=0) {
    int mark2 = -1;
    struct document_text_position left;
    if (cursor.column>0 && mark1==-1) {
      in.type = VISUAL_SEEK_LINE_COLUMN;
      in.clip = 0;
      in.line = cursor.line;
      in.column = cursor.column-1;

      document_text_cursor_position(splitter, &in, &left, 0, 1);
      mark2 = document_text_mark_brackets(base, screen, splitter, &left);
    }

    if (mark1==0) {
      splitter_hilight(splitter, screen, (int)(cursor.x-view->scroll_x+view->address_width), (int)(cursor.y-view->scroll_y), file->defaults.colors[VISUAL_FLAG_COLOR_BRACKETERROR]);
    } else if (mark2==0) {
      splitter_hilight(splitter, screen, (int)(left.x-view->scroll_x+view->address_width), (int)(left.y-view->scroll_y), file->defaults.colors[VISUAL_FLAG_COLOR_BRACKETERROR]);
    }
  }

  // Auto complete hint
  if (file->autocomplete_last && range_tree_length(file->buffer)>0 && !document_view_select_active(view)) {
    char text[1024];
    size_t length = 0;

    file_offset_t displacement;
    struct range_tree_node* buffer = range_tree_find_offset(file->buffer, view->offset, &displacement);
    struct stream stream;
    stream_from_page(&stream, buffer, displacement);
    struct unicode_sequencer sequencer;
    unicode_sequencer_clear(&sequencer, file->encoding, &stream);
    codepoint_t cp = unicode_sequencer_find(&sequencer, 0)->cp[0];
    if (!unicode_word(cp)) {
      stream_destroy(&stream);

      stream_from_page(&stream, buffer, displacement);
      while (!stream_start(&stream)) {
        stream_reverse(&stream, 1); // TODO: this might not work with UTF16 for example ... introduce a smallest element per encoding (we go with 1 here)
        struct stream copy;
        stream_clone(&copy, &stream);
        unicode_sequencer_clear(&sequencer, file->encoding, &copy);
        // TODO: codepoint!=sequence
        codepoint_t cp = unicode_sequencer_find(&sequencer, 0)->cp[0];
        stream_destroy(&copy);
        if (cp<UNICODE_CODEPOINT_MAX && !unicode_word(cp)) {
          stream_forward(&stream, 1);
          break;
        }
      }

      file_offset_t offset = stream_offset(&stream);
      unicode_sequencer_clear(&sequencer, file->encoding, &stream);
      int prefix = 0;
      struct trie_node* parent = NULL;
      while (offset<view->offset) {
        // TODO: codepoint!=sequence
        struct unicode_sequence* sequence = unicode_sequencer_find(&sequencer, 0);
        codepoint_t cp = sequence->cp[0];
        length += encoding_utf8_encode(NULL, cp, (uint8_t*)&text[length], sizeof(text)-length);
        parent = trie_find_codepoint(file->autocomplete_last, parent, cp);
        if (!parent) {
          break;
        }

        offset += sequence->size;
        unicode_sequencer_advance(&sequencer, 1);
        prefix++;
      }

      if (parent && trie_find_codepoint_single(file->autocomplete_last, parent)!=UNICODE_CODEPOINT_BAD) {
        int offset_y = -1;
        if (cursor.y-view->scroll_y<=0) {
          offset_y = 1;
        }

        splitter_drawtext(splitter, screen, (int)(cursor.x-view->scroll_x+view->address_width)-prefix, (int)(cursor.y-view->scroll_y)+offset_y, &text[0], length, file->defaults.colors[VISUAL_FLAG_COLOR_TEXT], file->defaults.colors[VISUAL_FLAG_COLOR_BRACKETERROR]);

        length = 0;
        while (parent) {
          codepoint_t cp = trie_find_codepoint_single(file->autocomplete_last, parent);
          if (cp==UNICODE_CODEPOINT_UNASSIGNED && (length==0 || !parent->end)) {
            text[length++] = '[';
            codepoint_t min = 0;
            while (1) {
              if (!trie_find_codepoint_min(file->autocomplete_last, parent, min, &cp)) {
                break;
              }
              min = cp;
              length += encoding_utf8_encode(NULL, cp, (uint8_t*)&text[length], sizeof(text)-length-2);
            }
            text[length++] = ']';
            break;
          }
          length += encoding_utf8_encode(NULL, cp, (uint8_t*)&text[length], sizeof(text)-length-2);
          parent = trie_find_codepoint(file->autocomplete_last, parent, cp);
          if (!parent || parent->end) {
            break;
          }
        }
        splitter_drawtext(splitter, screen, (int)(cursor.x-view->scroll_x+view->address_width), (int)(cursor.y-view->scroll_y)+offset_y, &text[0], length, file->defaults.colors[VISUAL_FLAG_COLOR_TEXT], file->defaults.colors[VISUAL_FLAG_COLOR_BRACKET]);
      }
    }
    stream_destroy(&stream);
  }

  const char* newline[TIPPSE_NEWLINE_MAX] = {"Auto", "Lf", "Cr", "CrLf"};
  const char* tabstop[TIPPSE_TABSTOP_MAX] = {"Auto", "Tab", "Space"};

  char status[1024];
  sprintf(&status[0], "%s%s%lld/%lld:%lld - %lld/%lld byte - %s*%d %s %s/%s %s", (file->buffer?file->buffer->visuals.dirty:0)?"? ":"", (file->buffer?(file->buffer->inserter&TIPPSE_INSERTER_FILE):0)?"File ":"", (long long int)(file->buffer?file->buffer->visuals.lines+1:0), (long long int)(cursor.line+1), (long long int)(cursor.column+1), (long long int)view->offset, (long long int)range_tree_length(file->buffer), tabstop[file->tabstop], file->tabstop_width, newline[file->newline], (*file->type->name)(), (*file->type->type)(file->type), (*file->encoding->name)());
  splitter_status(splitter, &status[0]);

  view->scroll_y_max = file->buffer?file->buffer->visuals.ys:0;
  splitter_scrollbar(splitter, screen);
}

// Handle key press
void document_text_keypress(struct document* base, struct splitter* splitter, int command, struct config_command* arguments, int key, codepoint_t cp, int button, int button_old, int x, int y) {
  struct document_file* file = splitter->file;
  struct document_view* view = splitter->view;

  if (view->line_select) {
    document_text_keypress_line_select(base, splitter, command, arguments, key, cp, button, button_old, x, y);
    return;
  }

  struct document_text_position out;
  struct document_text_position in_offset;
  in_offset.type = VISUAL_SEEK_OFFSET;
  in_offset.clip = 0;

  struct document_text_position in_x_y;
  in_x_y.type = VISUAL_SEEK_X_Y;
  in_x_y.clip = 0;

  struct document_text_position in_line_column;
  in_line_column.type = VISUAL_SEEK_LINE_COLUMN;
  in_line_column.clip = 0;

  file_offset_t file_size = range_tree_length(file->buffer);
  file_offset_t offset_old = view->offset;
  int seek = 0;
  int selection_keep = 0;
  file_offset_t selection_low;
  file_offset_t selection_high;
  document_view_select_next(view, 0, &selection_low, &selection_high);

  if ((command!=TIPPSE_CMD_UP && command!=TIPPSE_CMD_DOWN && command!=TIPPSE_CMD_PAGEDOWN && command!=TIPPSE_CMD_PAGEUP && command!=TIPPSE_CMD_SELECT_UP && command!=TIPPSE_CMD_SELECT_DOWN && command!=TIPPSE_CMD_SELECT_PAGEDOWN && command!=TIPPSE_CMD_SELECT_PAGEUP) || view->offset!=view->offset_calculated) {
    in_offset.offset = view->offset;
    document_text_cursor_position(splitter, &in_offset, &out, 1, 1);
    view->cursor_x = out.x;
    view->cursor_y = out.y;
  }

  if (command!=TIPPSE_CMD_CUTLINE) {
    view->line_cut = 0;
  }

  in_x_y.x = view->cursor_x;
  in_x_y.y = view->cursor_y;

  if (command!=TIPPSE_CMD_CHARACTER) {
    view->bracket_indentation = 0;
  }

  if (command==TIPPSE_CMD_UP || command==TIPPSE_CMD_SELECT_UP) {
    in_x_y.y--;
    view->offset = document_text_cursor_position(splitter, &in_x_y, &out, 0, 1);
    view->cursor_y = out.y;
    view->show_scrollbar = 1;
  } else if (command==TIPPSE_CMD_DOWN || command==TIPPSE_CMD_SELECT_DOWN) {
    in_x_y.y++;
    view->offset = document_text_cursor_position(splitter, &in_x_y, &out, 0, 1);
    view->cursor_y = out.y;
    view->show_scrollbar = 1;
  } else if (command==TIPPSE_CMD_LEFT || command==TIPPSE_CMD_SELECT_LEFT) {
    in_x_y.x--;
    view->offset = document_text_cursor_position(splitter, &in_x_y, &out, 1, 1);
    view->cursor_x = out.x;
    view->cursor_y = out.y;
    seek = 1;
  } else if (command==TIPPSE_CMD_RIGHT || command==TIPPSE_CMD_SELECT_RIGHT) {
    in_x_y.x++;
    view->offset = document_text_cursor_position(splitter, &in_x_y, &out, 1, 0);
    view->cursor_x = out.x;
    view->cursor_y = out.y;
    seek = 1;
  } else if (command==TIPPSE_CMD_PAGEDOWN || command==TIPPSE_CMD_SELECT_PAGEDOWN) {
    int page = splitter->client_height;
    in_x_y.y += page;
    view->scroll_y += page;
    view->offset = document_text_cursor_position(splitter, &in_x_y, &out, 0, 1);
    view->cursor_y = out.y;
    view->show_scrollbar = 1;
  } else if (command==TIPPSE_CMD_PAGEUP || command==TIPPSE_CMD_SELECT_PAGEUP) {
    int page = splitter->client_height;
    in_x_y.y -= page;
    view->scroll_y -= page;
    view->offset = document_text_cursor_position(splitter, &in_x_y, &out, 0, 1);
    view->cursor_y = out.y;
    view->show_scrollbar = 1;
  } else if (command==TIPPSE_CMD_BACKSPACE) {
    if (!document_select_delete(splitter)) {
      in_x_y.x--;
      file_offset_t start = document_text_cursor_position(splitter, &in_x_y, &out, 1, 1);
      document_file_delete(file, start, view->offset-start);
    }
    seek = 1;
  } else if (command==TIPPSE_CMD_DELETE) {
    if (!document_select_delete(splitter)) {
      in_x_y.x++;
      file_offset_t end = document_text_cursor_position(splitter, &in_x_y, &out, 1, 0);
      document_file_delete(file, view->offset, end-view->offset);
    }
    seek = 1;
  } else if (command==TIPPSE_CMD_FIRST || command==TIPPSE_CMD_SELECT_FIRST) {
    struct range_tree_node* first = range_tree_find_indentation_last(out.buffer, out.lines, out.buffer?out.buffer:range_tree_last(file->buffer));
    bool_t seek_first = 1;

    if (first) {
      struct document_text_position in_last;
      in_last.type = VISUAL_SEEK_INDENTATION_LAST;
      in_last.clip = 0;
      in_last.offset = range_tree_offset(first);
      in_last.line = out.line;

      struct document_text_position out_first;
      document_text_cursor_position(splitter, &in_last, &out_first, 0, 1);

      if (out_first.type!=VISUAL_SEEK_NONE) {
        view->cursor_x = out_first.x;
        view->cursor_y = out_first.y;
        view->offset = out_first.offset;
        seek_first = (out_first.offset==out.offset)?1:0;
      }
    }

    if (seek_first) {
      in_line_column.line = out.line;
      in_line_column.column = 0;
      view->offset = document_text_cursor_position(splitter, &in_line_column, &out, 0, 1);
      view->cursor_x = out.x;
      view->cursor_y = out.y;
    }
  } else if (command==TIPPSE_CMD_LAST || command==TIPPSE_CMD_SELECT_LAST) {
    in_line_column.line = out.line;
    in_line_column.column = POSITION_T_MAX;
    view->offset = document_text_cursor_position(splitter, &in_line_column, &out, 0, 1);
    view->cursor_x = out.x;
    view->cursor_y = out.y;
  } else if (command==TIPPSE_CMD_HOME || command==TIPPSE_CMD_SELECT_HOME) {
    in_x_y.x = 0;
    in_x_y.y = 0;
    view->offset = document_text_cursor_position(splitter, &in_x_y, &out, 1, 1);
    view->cursor_x = out.x;
    view->cursor_y = out.y;
    view->show_scrollbar = 1;
  } else if (command==TIPPSE_CMD_END || command==TIPPSE_CMD_SELECT_END) {
    in_x_y.x = POSITION_T_MAX;
    in_x_y.y = POSITION_T_MAX;
    view->offset = document_text_cursor_position(splitter, &in_x_y, &out, 0, 1);
    view->cursor_x = out.x;
    view->cursor_y = out.y;
    view->show_scrollbar = 1;
  } else if (command==TIPPSE_CMD_UNDO) {
    document_undo_execute_chain(file, view, file->undos, file->redos, 0);
    return;
  } else if (command==TIPPSE_CMD_REDO) {
    document_undo_execute_chain(file, view, file->redos, file->undos, 1);
    return;
  } else if (command==TIPPSE_CMD_BOOKMARK) {
    if (document_view_select_active(view)) {
      document_bookmark_toggle_selection(splitter);
      document_view_select_nothing(view, file);
    } else {
      document_text_toggle_bookmark(base, splitter, view->offset);
    }
  } else if (command==TIPPSE_CMD_BOOKMARK_NEXT) {
    document_bookmark_next(splitter);
    seek = 1;
  } else if (command==TIPPSE_CMD_BOOKMARK_PREV) {
    document_bookmark_prev(splitter);
    seek = 1;
  } else if (command==TIPPSE_CMD_MOUSE) {
    if (button&TIPPSE_MOUSE_LBUTTON) {
      in_x_y.x = x+view->scroll_x-view->address_width;
      in_x_y.y = y+view->scroll_y;
      view->offset = document_text_cursor_position(splitter, &in_x_y, &out, 0, 1);
      view->cursor_x = out.x;
      view->cursor_y = out.y;
      if (view->selection_start==FILE_OFFSET_T_MAX) {
        view->selection_reset = 1;
        view->selection_start = view->offset;
      }
      view->selection_end = view->offset;
      view->line_cut = 0;
    } else if (button&TIPPSE_MOUSE_WHEEL_DOWN) {
      int page = ((splitter->height-2)/3)+1;
      in_x_y.y += page;
      view->scroll_y += page;
      view->offset = document_text_cursor_position(splitter, &in_x_y, &out, 1, 1);
      view->cursor_x = out.x;
      view->cursor_y = out.y;
      view->show_scrollbar = 1;
      view->line_cut = 0;
    } else if (button&TIPPSE_MOUSE_WHEEL_UP) {
      int page = ((splitter->height-2)/3)+1;
      in_x_y.y -= page;
      view->scroll_y -= page;
      view->offset = document_text_cursor_position(splitter, &in_x_y, &out, 1, 1);
      view->cursor_x = out.x;
      view->cursor_y = out.y;
      view->show_scrollbar = 1;
      view->line_cut = 0;
    }
  } else if (command==TIPPSE_CMD_UPPER) {
    if (selection_low!=FILE_OFFSET_T_MAX) {
      document_text_transform(base, unicode_transform_upper, file, selection_low, selection_high);
    } else {
      document_text_transform(base, unicode_transform_upper, file, 0, range_tree_length(file->buffer));
    }
  } else if (command==TIPPSE_CMD_LOWER) {
    if (selection_low!=FILE_OFFSET_T_MAX) {
      document_text_transform(base, unicode_transform_lower, file, selection_low, selection_high);
    } else {
      document_text_transform(base, unicode_transform_lower, file, 0, range_tree_length(file->buffer));
    }
  } else if (command==TIPPSE_CMD_NFC_NFD) {
    if (selection_low!=FILE_OFFSET_T_MAX) {
      document_text_transform(base, unicode_transform_nfc_nfd, file, selection_low, selection_high);
    } else {
      document_text_transform(base, unicode_transform_nfc_nfd, file, 0, range_tree_length(file->buffer));
    }
  } else if (command==TIPPSE_CMD_NFD_NFC) {
    if (selection_low!=FILE_OFFSET_T_MAX) {
      document_text_transform(base, unicode_transform_nfd_nfc, file, selection_low, selection_high);
    } else {
      document_text_transform(base, unicode_transform_nfd_nfc, file, 0, range_tree_length(file->buffer));
    }
  } else if (command==TIPPSE_CMD_TAB) {
    document_undo_chain(file, file->undos);
    if (selection_low==FILE_OFFSET_T_MAX) {
      char utf8[TIPPSE_TAB_MAX];
      file_offset_t size;
      if (file->tabstop==TIPPSE_TABSTOP_SPACE) {
        size = (file_offset_t)(file->tabstop_width-(view->cursor_x%file->tabstop_width));
        if (size>TIPPSE_TAB_MAX) {
          size = TIPPSE_TAB_MAX;
        }
        file_offset_t spaces;
        for (spaces = 0; spaces<size; spaces++) {
          utf8[spaces] = ' ';
        }
      } else {
        size = 1;
        utf8[0] = '\t';
      }

      document_file_insert_utf8(file, view->offset, &utf8[0], size, 0);
    } else {
      document_text_raise_indentation(base, splitter, selection_low, selection_high-1, 1);
    }
    document_undo_chain(file, file->undos);
    seek = 1;
    selection_keep = 1;
  } else if (command==TIPPSE_CMD_UNTAB) {
    document_undo_chain(file, file->undos);
    if (selection_low!=FILE_OFFSET_T_MAX) {
      document_text_lower_indentation(base, splitter, selection_low, selection_high-1);
    } else {
      document_text_lower_indentation(base, splitter, view->offset, view->offset);
    }
    document_undo_chain(file, file->undos);
    seek = 1;
    selection_keep = 1;
  } else if (command==TIPPSE_CMD_BLOCK_UP || command==TIPPSE_CMD_BLOCK_DOWN) {
    document_undo_chain(file, file->undos);
    if (selection_low!=FILE_OFFSET_T_MAX) {
      document_text_move_block(base, splitter, selection_low, selection_high>selection_low?selection_high-1:selection_high, (command==TIPPSE_CMD_BLOCK_UP)?1:0);
    } else {
      document_text_move_block(base, splitter, view->offset, view->offset, (command==TIPPSE_CMD_BLOCK_UP)?1:0);
    }
    document_undo_chain(file, file->undos);
    seek = 1;
    selection_keep = 1;
  } else if (command==TIPPSE_CMD_WORD_NEXT || command==TIPPSE_CMD_SELECT_WORD_NEXT) {
    view->offset = document_text_word_transition_next(base, splitter, view->offset);
    if (selection_low==FILE_OFFSET_T_MAX && command==TIPPSE_CMD_SELECT_WORD_NEXT) {
      offset_old = document_text_word_transition_prev(base, splitter, view->offset);
    }
    seek = 1;
  } else if (command==TIPPSE_CMD_WORD_PREV || command==TIPPSE_CMD_SELECT_WORD_PREV) {
    view->offset = document_text_word_transition_prev(base, splitter, view->offset);
    if (selection_low==FILE_OFFSET_T_MAX && command==TIPPSE_CMD_SELECT_WORD_PREV) {
      offset_old = document_text_word_transition_next(base, splitter, view->offset);
    }
    seek = 1;
  } else if (command==TIPPSE_CMD_DELETE_WORD_NEXT) {
    file_offset_t end = document_text_word_transition_next(base, splitter, view->offset);
    document_file_delete(file, view->offset, end-view->offset);
    seek = 1;
    selection_keep = 1;
  } else if (command==TIPPSE_CMD_DELETE_WORD_PREV) {
    file_offset_t start = document_text_word_transition_prev(base, splitter, view->offset);
    document_file_delete(file, start, view->offset-start);
    seek = 1;
    selection_keep = 1;
  } else if (command==TIPPSE_CMD_SELECT_ALL) {
    offset_old = view->selection_start = 0;
    view->offset = view->selection_end = file_size;
  } else if (command==TIPPSE_CMD_SELECT_LINE) {
    document_text_select_line(base, splitter);
    selection_keep = 1;
  } else if (command==TIPPSE_CMD_CUTLINE) {
    document_undo_chain(file, file->undos);

    position_t line = out.line;
    in_line_column.line = line;
    in_line_column.column = 0;
    file_offset_t from = document_text_cursor_position(splitter, &in_line_column, &out, 0, 1);
    in_line_column.column = POSITION_T_MAX;
    file_offset_t to = document_text_cursor_position(splitter, &in_line_column, &out, 1, 1);

    struct range_tree_node* clipboard = view->line_cut?clipboard_get():NULL;
    view->line_cut = 1;
    struct range_tree_node* rootold = range_tree_copy(clipboard, 0, range_tree_length(clipboard));
    struct range_tree_node* rootnew = range_tree_copy(file->buffer, from, to-from);
    rootold = range_tree_paste(rootold, rootnew, range_tree_length(rootold), NULL);
    range_tree_destroy(rootnew, NULL);

    clipboard_set(rootold, file->binary);
    document_file_delete(file, from, to-from);

    selection_keep = 1;
    document_undo_chain(file, file->undos);
    seek = 1;
  } else if (command==TIPPSE_CMD_COPY || command==TIPPSE_CMD_CUT) {
    document_undo_chain(file, file->undos);
    if (selection_low!=FILE_OFFSET_T_MAX) {
      document_clipboard_copy(splitter);
      if (command==TIPPSE_CMD_CUT) {
        document_select_delete(splitter);
      } else {
        selection_keep = 1;
      }
    }
    document_undo_chain(file, file->undos);
    seek = 1;
  } else if (command==TIPPSE_CMD_PASTE) {
    document_undo_chain(file, file->undos);
    document_select_delete(splitter);
    document_clipboard_paste(splitter);
    document_undo_chain(file, file->undos);
    seek = 1;
  } else if (command==TIPPSE_CMD_SHOW_INVISIBLES) {
    view->show_invisibles ^= 1;
    (*base->reset)(base, splitter);
  } else if (command==TIPPSE_CMD_WORDWRAP) {
    view->wrapping ^= 1;
    (*base->reset)(base, splitter);
  } else if (command==TIPPSE_CMD_RETURN) {
    document_select_delete(splitter);

    struct document_text_position out_begin;
    in_offset.offset = view->offset;
    document_text_cursor_position(splitter, &in_offset, &out_begin, 1, 1);

    if (out_begin.lines==0 && range_tree_first(file->buffer)!=out_begin.buffer) {
      range_tree_find_bracket_lowest(out_begin.buffer, out_begin.min_line, out_begin.buffer?out_begin.buffer:range_tree_last(file->buffer));
    }

    struct document_text_position out_indentation_copy;
    struct document_text_position out_indentation_last;
    if (out_begin.column!=0) {
      in_line_column.line = out_begin.line;
      in_line_column.column = 0;
      document_text_cursor_position(splitter, &in_line_column, &out_indentation_copy, 0, 1);

      struct document_text_position in_last;
      in_last.type = VISUAL_SEEK_INDENTATION_LAST;
      in_last.clip = 0;
      in_last.offset = out_indentation_copy.offset;
      in_last.line = out_begin.line;

      document_text_cursor_position(splitter, &in_last, &out_indentation_last, 0, 1);
      if (out_indentation_last.offset>view->offset) {
        out_indentation_last.offset = view->offset;
      }
    } else {
      out_indentation_copy = out_begin;
      out_indentation_last = out_begin;
    }

    if (file->newline==TIPPSE_NEWLINE_CRLF) {
      document_file_insert_utf8(file, view->offset, "\r\n", 2, 0);
    } else if (file->newline==TIPPSE_NEWLINE_CR) {
      document_file_insert_utf8(file, view->offset, "\r", 1, 0);
    } else {
      document_file_insert_utf8(file, view->offset, "\n", 1, 0);
    }

    // Build a binary copy of the previous indentation (one could insert the default indentation style as alternative... to discuss)
    if (out_indentation_copy.offset<out_indentation_last.offset) {
      file_offset_t length = out_indentation_last.offset-out_indentation_copy.offset;
      struct range_tree_node* copy = range_tree_copy(file->buffer, out_indentation_copy.offset, length);
      document_file_insert_buffer(file, view->offset, copy);
      range_tree_destroy(copy, NULL);
    }

    int diff = 0;
    for (size_t bracket = 0; bracket<VISUAL_BRACKET_MAX; bracket++) {
      int add = out_begin.depth[bracket]-out_indentation_copy.depth[bracket];
      if (add>0) {
        diff += add;
      }

      if (add==0 && out_begin.min_line[bracket]>0) {
        diff++;
      }
    }

    if (diff>0) {
      document_text_raise_indentation(base, splitter, view->offset, view->offset, 0);
    }

    view->bracket_indentation = 1;

    seek = 1;
  } else if (command==TIPPSE_CMD_CHARACTER) {
    document_select_delete(splitter);
    uint8_t coded[8];
    size_t size = file->encoding->encode(file->encoding, cp, &coded[0], 8);
    document_file_insert(file, view->offset, &coded[0], size, 0);

    struct document_text_position out_bracket;
    in_line_column.line = out.line;
    in_line_column.column = out.column;
    document_text_cursor_position(splitter, &in_line_column, &out_bracket, 0, 1);

    if ((out_bracket.bracket_match&VISUAL_BRACKET_CLOSE) && out_bracket.indented && splitter->view->bracket_indentation) {
      document_text_lower_indentation(base, splitter, view->offset, view->offset);
    }

    view->bracket_indentation = 0;

    seek = 1;
  }

  if (seek) {
    in_offset.offset = view->offset;
    document_text_cursor_position(splitter, &in_offset, &out, 1, 1);
    view->offset = out.offset;
    view->cursor_x = out.x;
    if (view->cursor_y!=out.y && file->autocomplete_rescan==1) {
      file->autocomplete_rescan = 2;
    }
    view->cursor_y = out.y;
  }

  int selection_reset = 0;
  if (command==TIPPSE_CMD_MOUSE) {
    if (button&TIPPSE_MOUSE_LBUTTON) {
      if (!(button_old&TIPPSE_MOUSE_LBUTTON) && !(key&TIPPSE_KEY_MOD_SHIFT)) view->selection_start = FILE_OFFSET_T_MAX;
      if (view->selection_start==FILE_OFFSET_T_MAX) {
        view->selection_reset = 1;
        view->selection_start = view->offset;
      }
      view->selection_end = view->offset;
    }
  } else {
    if (command==TIPPSE_CMD_SELECT_UP || command==TIPPSE_CMD_SELECT_DOWN || command==TIPPSE_CMD_SELECT_LEFT || command==TIPPSE_CMD_SELECT_RIGHT || command==TIPPSE_CMD_SELECT_PAGEUP || command==TIPPSE_CMD_SELECT_PAGEDOWN || command==TIPPSE_CMD_SELECT_FIRST || command==TIPPSE_CMD_SELECT_LAST || command==TIPPSE_CMD_SELECT_HOME || command==TIPPSE_CMD_SELECT_END || command==TIPPSE_CMD_SELECT_ALL || command==TIPPSE_CMD_SELECT_WORD_NEXT || command==TIPPSE_CMD_SELECT_WORD_PREV) {
      if (view->selection_start==FILE_OFFSET_T_MAX) {
        view->selection_reset = 1;
        view->selection_start = offset_old;
      }
      view->selection_end = view->offset;
    } else {
      selection_reset = selection_keep?0:1;
    }
  }
  if (!selection_keep) {
    if (selection_reset) {
      view->selection_reset = 1;
      view->selection_start = FILE_OFFSET_T_MAX;
      view->selection_end = FILE_OFFSET_T_MAX;
    }

    document_view_select_nothing(view, file);
    document_view_select_range(view, view->selection_start, view->selection_end, TIPPSE_INSERTER_MARK|TIPPSE_INSERTER_NOFUSE);
  }
  view->offset_calculated = view->offset;
}

// In line select mode the keyboard is used in a different way since the display shows a list of options one can't edit
void document_text_keypress_line_select(struct document* base, struct splitter* splitter, int command, struct config_command* arguments, int key, codepoint_t cp, int button, int button_old, int x, int y) {
  struct document_file* file = splitter->file;
  struct document_view* view = splitter->view;

  struct document_text_position out;
  struct document_text_position in_offset;
  in_offset.type = VISUAL_SEEK_OFFSET;
  in_offset.clip = 0;

  struct document_text_position in_x_y;
  in_x_y.type = VISUAL_SEEK_X_Y;
  in_x_y.clip = 0;

  struct document_text_position in_line_column;
  in_line_column.type = VISUAL_SEEK_LINE_COLUMN;
  in_line_column.clip = 0;

  in_offset.offset = view->offset;
  document_text_cursor_position(splitter, &in_offset, &out, 1, 1);
  in_line_column.line = out.line;
  in_line_column.column = 0;
  in_x_y.x = 0;
  in_x_y.y = out.y;

  file_offset_t selection_low;
  file_offset_t selection_high;
  document_view_select_next(view, 0, &selection_low, &selection_high);

  if (command==TIPPSE_CMD_UP) {
    in_line_column.line--;
    view->offset = document_text_cursor_position(splitter, &in_line_column, &out, 0, 1);
    view->show_scrollbar = 1;
  } else if (command==TIPPSE_CMD_DOWN) {
    in_line_column.line++;
    view->offset = document_text_cursor_position(splitter, &in_line_column, &out, 0, 1);
    view->show_scrollbar = 1;
  } else if (command==TIPPSE_CMD_PAGEDOWN) {
    int page = splitter->client_height;
    in_x_y.y += page;
    view->offset = document_text_cursor_position(splitter, &in_x_y, &out, 0, 1);
    if (out.line==in_line_column.line) {
      in_line_column.line++;
      view->offset = document_text_cursor_position(splitter, &in_line_column, &out, 0, 1);
    }
    view->show_scrollbar = 1;
  } else if (command==TIPPSE_CMD_PAGEUP) {
    int page = splitter->client_height;
    in_x_y.y -= page;
    view->offset = document_text_cursor_position(splitter, &in_x_y, &out, 0, 1);
    if (out.line==in_line_column.line) {
      in_line_column.line--;
      view->offset = document_text_cursor_position(splitter, &in_line_column, &out, 0, 1);
    }
    view->show_scrollbar = 1;
  } else if (command==TIPPSE_CMD_HOME) {
    in_line_column.line = 0;
    view->offset = document_text_cursor_position(splitter, &in_line_column, &out, 1, 1);
    view->cursor_x = out.x;
    view->cursor_y = out.y;
    view->show_scrollbar = 1;
  } else if (command==TIPPSE_CMD_END) {
    in_line_column.line = POSITION_T_MAX;
    view->offset = document_text_cursor_position(splitter, &in_line_column, &out, 0, 1);
    view->show_scrollbar = 1;
  } else if (command==TIPPSE_CMD_MOUSE) {
    if (button&TIPPSE_MOUSE_LBUTTON) {
      in_x_y.x = x+view->scroll_x-view->address_width;
      in_x_y.y = y+view->scroll_y;
      view->offset = document_text_cursor_position(splitter, &in_x_y, &out, 0, 1);
    } else if (button&TIPPSE_MOUSE_WHEEL_DOWN) {
      int page = ((splitter->height-2)/3)+1;
      in_x_y.y += page;
      view->offset = document_text_cursor_position(splitter, &in_x_y, &out, 1, 1);
      if (out.line==in_line_column.line) {
        in_line_column.line++;
        view->offset = document_text_cursor_position(splitter, &in_line_column, &out, 0, 1);
      }
      view->show_scrollbar = 1;
    } else if (button&TIPPSE_MOUSE_WHEEL_UP) {
      int page = ((splitter->height-2)/3)+1;
      in_x_y.y -= page;
      view->offset = document_text_cursor_position(splitter, &in_x_y, &out, 1, 1);
      if (out.line==in_line_column.line) {
        in_line_column.line--;
        view->offset = document_text_cursor_position(splitter, &in_line_column, &out, 0, 1);
      }
      view->show_scrollbar = 1;
    }
  } else if (command==TIPPSE_CMD_RETURN) {
  }

  in_offset.offset = view->offset;
  document_text_cursor_position(splitter, &in_offset, &out, 1, 1);
  view->offset = out.offset;
  view->cursor_x = 0;
  view->cursor_y = out.y;

  if (view->line_select) {
    in_offset.offset = view->offset;
    in_offset.clip = 0;

    in_offset.offset = view->offset;
    document_text_cursor_position(splitter, &in_offset, &out, 0, 1);

    in_line_column.line = out.line;
    in_line_column.column = 0;
    document_text_cursor_position(splitter, &in_line_column, &out, 0, 1);
    view->selection_start = out.offset;
    view->offset = out.offset;
    in_line_column.column = POSITION_T_MAX;
    document_text_cursor_position(splitter, &in_line_column, &out, 0, 1);
    view->selection_end = out.offset;
  }

  document_view_select_nothing(view, file);
  document_view_select_range(view, view->selection_start, view->selection_end, TIPPSE_INSERTER_MARK|TIPPSE_INSERTER_NOFUSE);
}

// Set or clear bookmark range
void document_text_toggle_bookmark(struct document* base, struct splitter* splitter, file_offset_t offset) {
  struct document_view* view = splitter->view;

  struct document_text_position out;
  struct document_text_position in_offset;
  in_offset.type = VISUAL_SEEK_OFFSET;
  in_offset.clip = 0;

  struct document_text_position in_line_column;
  in_line_column.type = VISUAL_SEEK_LINE_COLUMN;
  in_line_column.clip = 0;

  in_offset.offset = view->offset;
  document_text_cursor_position(splitter, &in_offset, &out, 0, 1);

  in_line_column.line = out.line;
  in_line_column.column = 0;
  document_text_cursor_position(splitter, &in_line_column, &out, 0, 1);
  file_offset_t start = out.offset;
  in_line_column.column = POSITION_T_MAX;
  document_text_cursor_position(splitter, &in_line_column, &out, 1, 1);
  file_offset_t end = out.offset;

  document_bookmark_toggle_range(splitter, start, end);
}

// Lower indentation for selected range (if possible)
void document_text_lower_indentation(struct document* base, struct splitter* splitter, file_offset_t low, file_offset_t high) {
  struct document_file* file = splitter->file;

  struct document_text_position in_offset;
  in_offset.type = VISUAL_SEEK_OFFSET;
  in_offset.clip = 0;

  struct document_text_position in_line_column;
  in_line_column.type = VISUAL_SEEK_LINE_COLUMN;
  in_line_column.clip = 0;

  struct document_text_position out;
  struct document_text_position out_start;
  struct document_text_position out_end;

  in_offset.offset = low;
  document_text_cursor_position(splitter, &in_offset, &out_start, 0, 1);
  in_offset.offset = high;
  document_text_cursor_position(splitter, &in_offset, &out_end, 0, 1);

  while (out_end.line>=out_start.line) {
    in_line_column.column = 0;
    in_line_column.line = out_end.line;
    document_text_cursor_position(splitter, &in_line_column, &out, 0, 1);

    struct stream stream;
    stream_from_page(&stream, out.buffer, out.displacement);
    file_offset_t length = 0;
    while (!stream_end(&stream)) {
      uint8_t text = stream_read_forward(&stream);
      if (text!='\t' && text!=' ') {
        break;
      }

      length++;
      if (text=='\t' || (int)length>=file->tabstop_width) {
        break;
      }
    }
    stream_destroy(&stream);

    if (length>0) {
      document_file_delete(splitter->file, out.offset, length);
    }

    out_end.line--;
  }
}

// Raise indentation for selected range
void document_text_raise_indentation(struct document* base, struct splitter* splitter, file_offset_t low, file_offset_t high, int empty_lines) {
  struct document_file* file = splitter->file;

  struct document_text_position in_offset;
  in_offset.type = VISUAL_SEEK_OFFSET;
  in_offset.clip = 0;

  struct document_text_position in_line_column;
  in_line_column.type = VISUAL_SEEK_LINE_COLUMN;
  in_line_column.clip = 0;

  struct document_text_position out_start;
  struct document_text_position out_end;
  struct document_text_position out_line_start;
  struct document_text_position out_line_end;

  in_offset.offset = low;
  document_text_cursor_position(splitter, &in_offset, &out_start, 0, 1);
  in_offset.offset = high;
  document_text_cursor_position(splitter, &in_offset, &out_end, 0, 1);

  while (out_end.line>=out_start.line) {
    in_line_column.column = 0;
    in_line_column.line = out_end.line;
    document_text_cursor_position(splitter, &in_line_column, &out_line_start, 0, 1);

    in_line_column.column = POSITION_T_MAX;
    in_line_column.line = out_end.line;
    document_text_cursor_position(splitter, &in_line_column, &out_line_end, 0, 1);

    char utf8[TIPPSE_TAB_MAX];
    file_offset_t size;
    if (file->tabstop==TIPPSE_TABSTOP_SPACE) {
      size = (file_offset_t)file->tabstop_width;
      if (size>TIPPSE_TAB_MAX) {
        size = TIPPSE_TAB_MAX;
      }
      for (file_offset_t spaces = 0; spaces<size; spaces++) {
        utf8[spaces] = ' ';
      }
    } else {
      size = 1;
      utf8[0] = '\t';
    }

    if (!empty_lines || out_line_start.offset!=out_line_end.offset) {
      document_file_insert_utf8(splitter->file, out_line_start.offset, &utf8[0], size, 0);
    }

    out_end.line--;
  }
}

// Move given block up or down (TODO: not multiselection aware / caller)
void document_text_move_block(struct document* base, struct splitter* splitter, file_offset_t low, file_offset_t high, int up) {
  struct document_file* file = splitter->file;

  struct document_text_position in_offset;
  in_offset.type = VISUAL_SEEK_OFFSET;
  in_offset.clip = 0;

  struct document_text_position in_line_column;
  in_line_column.type = VISUAL_SEEK_LINE_COLUMN;
  in_line_column.clip = 0;

  struct document_text_position out_start;
  struct document_text_position out_end;

  in_offset.offset = low;
  document_text_cursor_position(splitter, &in_offset, &out_start, 0, 1);
  in_offset.offset = high;
  document_text_cursor_position(splitter, &in_offset, &out_end, 0, 1);

  struct document_text_position out_line_start;
  struct document_text_position out_line_end;
  struct document_text_position out_line_return_start;
  struct document_text_position out_line_return_end;
  struct document_text_position out_insert;

  in_line_column.column = 0;
  in_line_column.line = out_start.line;
  document_text_cursor_position(splitter, &in_line_column, &out_line_start, 0, 1);

  in_line_column.column = POSITION_T_MAX;
  in_line_column.line = out_end.line;
  document_text_cursor_position(splitter, &in_line_column, &out_line_end, 0, 1);

  if (out_line_end.offset<out_line_start.offset) {
    return;
  }

  in_line_column.column = 0;
  in_line_column.line = up?out_line_start.line-1:out_line_end.line+2;
  document_text_cursor_position(splitter, &in_line_column, &out_insert, 0, 1);

  if (out_line_end.offset!=range_tree_length(file->buffer)) {
    in_line_column.column = 0;
    in_line_column.line = out_line_end.line+1;
    document_text_cursor_position(splitter, &in_line_column, &out_line_return_end, 0, 1);
    out_line_return_start.offset = out_line_end.offset;
  } else {
    in_line_column.column = POSITION_T_MAX;
    in_line_column.line = out_line_start.line-1;
    document_text_cursor_position(splitter, &in_line_column, &out_line_return_start, 0, 1);
    out_line_return_end.offset = out_line_start.offset;
  }

  bool_t exchange = (out_insert.offset==range_tree_length(file->buffer))?1:0;

  if (out_insert.offset<out_line_start.offset || out_insert.offset>out_line_end.offset) {
    file_offset_t length_text = out_line_end.offset-out_line_start.offset;
    file_offset_t length_return = out_line_return_end.offset-out_line_return_start.offset;
    file_offset_t offset_text = out_line_start.offset;
    file_offset_t offset_return = out_line_return_start.offset;
    file_offset_t offset_insert = out_insert.offset;
    file_offset_t offset_insert_base = out_insert.offset;
    if (!exchange) {
      document_file_move(file, offset_return, offset_insert, length_return);
      if (offset_insert>offset_return) {
        offset_insert -= length_return;
      }
      document_file_relocate(&offset_text, offset_return, offset_insert_base, length_return);
      document_file_move(file, offset_text, offset_insert, length_text);
    } else {
      document_file_move(file, offset_text, offset_insert, length_text);
      if (offset_insert>offset_text) {
        offset_insert -= length_text;
      }
      document_file_relocate(&offset_return, offset_text, offset_insert_base, length_text);
      document_file_move(file, offset_return, offset_insert, length_return);
    }
  }
}

// Extend selection to whole line
void document_text_select_line(struct document* base, struct splitter* splitter) {
  struct document_view* view = splitter->view;

  struct document_text_position in_offset;
  in_offset.type = VISUAL_SEEK_OFFSET;
  in_offset.clip = 0;

  struct document_text_position in_line_column;
  in_line_column.type = VISUAL_SEEK_LINE_COLUMN;
  in_line_column.clip = 0;

  int64_t lines = 0;
  file_offset_t low;
  file_offset_t high = 0;
  while (1) {
    int found = document_view_select_next(view, high, &low, &high);
    if (found==0) {
      if (lines!=0) {
        break;
      }
      low = view->offset;
      high = low;
    }

    lines++;
    struct document_text_position out_start;
    struct document_text_position out_end;
    in_offset.offset = low;
    document_text_cursor_position(splitter, &in_offset, &out_start, 0, 1);
    in_offset.offset = high;
    document_text_cursor_position(splitter, &in_offset, &out_end, 0, 1);

    struct document_text_position out_line_start;
    struct document_text_position out_line_end;
    in_line_column.column = 0;
    in_line_column.line = out_start.line;
    document_text_cursor_position(splitter, &in_line_column, &out_line_start, 0, 1);

    in_line_column.column = 0;
    in_line_column.line = out_end.line+1;
    document_text_cursor_position(splitter, &in_line_column, &out_line_end, 0, 1);

    document_view_select_range(view, out_line_start.offset, out_line_end.offset, TIPPSE_INSERTER_MARK);
    high = out_line_end.offset;
  }
}

// Mark the bracket and its partner below the cursor position and return whether something was marked
int document_text_mark_brackets(struct document* base, struct screen* screen, struct splitter* splitter, struct document_text_position* cursor) {
  if (!(cursor->bracket_match&(VISUAL_BRACKET_OPEN|VISUAL_BRACKET_CLOSE))) {
    return -1;
  }

  struct document_file* file = splitter->file;
  struct document_view* view = splitter->view;

  struct document_text_position in;
  struct document_text_position out;
  struct document_text_render_info render_info;

  in.clip = 0;
  in.bracket = cursor->bracket_match&VISUAL_BRACKET_MASK;
  in.bracket_search = cursor->depth[in.bracket];
  if (cursor->bracket_match&VISUAL_BRACKET_OPEN) {
    in.type = VISUAL_SEEK_BRACKET_NEXT;
    in.offset = cursor->offset+1;
  } else {
    in.type = VISUAL_SEEK_BRACKET_PREV;
    in.offset = (cursor->offset>0)?cursor->offset-1:0;
    size_t bracket = cursor->bracket_match&VISUAL_BRACKET_MASK;
    if (bracket==in.bracket) {
      in.bracket_search--;
    }
  }

  document_text_render_clear(&render_info, document_text_line_width(splitter), view->selection);
  while (1) {
    document_text_render_seek(&render_info, file->buffer, file->encoding, &in);
    int rendered = document_text_collect_span(&render_info, view, file, &in, &out, ~0, 1);
    if (rendered==1) {
      break;
    }

    if (rendered==-1) {
      document_text_render_destroy(&render_info);
      document_text_render_clear(&render_info, document_text_line_width(splitter), view->selection);
      document_text_render_seek(&render_info, file->buffer, file->encoding, &in);
      if (cursor->bracket_match&VISUAL_BRACKET_OPEN) {
        struct range_tree_node* node = range_tree_find_bracket_forward(render_info.buffer, in.bracket, in.bracket_search);
        if (!node) {
          node = range_tree_last(file->buffer);
          if (render_info.buffer==node) {
            break;
          }
        }

        in.offset = range_tree_offset(node)+node->visuals.displacement;
      } else {
        struct range_tree_node* node = range_tree_find_bracket_backward(render_info.buffer, in.bracket, in.bracket_search);
        if (!node) {
          node = range_tree_first(file->buffer);
        }

        if (render_info.buffer==range_tree_first(file->buffer)) {
          break;
        }

        in.offset = range_tree_offset(node)+node->length-1;
      }
    }
  }

  document_text_render_destroy(&render_info);

  if (((cursor->bracket_match&VISUAL_BRACKET_OPEN) && out.offset>cursor->offset) || ((cursor->bracket_match&VISUAL_BRACKET_CLOSE) && out.offset<cursor->offset && out.type!=VISUAL_SEEK_NONE)) {
    splitter_hilight(splitter, screen, (int)(cursor->x-view->scroll_x+view->address_width), (int)(cursor->y-view->scroll_y), file->defaults.colors[VISUAL_FLAG_COLOR_BRACKET]);
    splitter_hilight(splitter, screen, (int)(out.x-view->scroll_x+view->address_width), (int)(out.y-view->scroll_y), file->defaults.colors[VISUAL_FLAG_COLOR_BRACKET]);
    return 1;
  }

  return 0;
}

// Jump to specified line
void document_text_goto(struct document* base, struct splitter* splitter, position_t line, position_t column) {
  struct document_view* view = splitter->view;

  struct document_text_position in_line_column;
  in_line_column.type = VISUAL_SEEK_LINE_COLUMN;
  in_line_column.clip = 0;
  in_line_column.column = column;
  in_line_column.line = line;

  struct document_text_position out;
  view->offset = document_text_cursor_position(splitter, &in_line_column, &out, 0, 1);
  view->show_scrollbar = 1;
}

// Return line start offset of current cursor position
file_offset_t document_text_line_start_offset(struct document* base, struct splitter* splitter) {
  struct document_view* view = splitter->view;

  struct document_text_position in_offset;
  in_offset.type = VISUAL_SEEK_OFFSET;
  in_offset.clip = 0;
  in_offset.offset = view->offset;

  struct document_text_position out;
  document_text_cursor_position(splitter, &in_offset, &out, 0, 1);

  struct document_text_position in_line_column;
  in_line_column.type = VISUAL_SEEK_LINE_COLUMN;
  in_line_column.clip = 0;
  in_line_column.column = 0;
  in_line_column.line = out.line;

  return document_text_cursor_position(splitter, &in_line_column, &out, 0, 1);
}

// Move to next word
file_offset_t document_text_word_transition_next(struct document* base, struct splitter* splitter, file_offset_t offset) {
  struct document_file* file = splitter->file;
  struct document_view* view = splitter->view;

  if (offset<range_tree_length(file->buffer)) {
    offset++;
  }

  while (offset<range_tree_length(file->buffer)) {
    struct document_text_position out;
    struct document_text_position in;
    in.type = VISUAL_SEEK_WORD_TRANSITION_NEXT;
    in.clip = 0;
    in.offset = offset;

    struct document_text_render_info render_info;
    document_text_render_clear(&render_info, document_text_line_width(splitter), view->selection);
    document_text_render_seek(&render_info, file->buffer, file->encoding, &in);
    int rendered = document_text_collect_span(&render_info, view, file, &in, &out, ~0, 1);
    offset = render_info.offset;
    document_text_render_destroy(&render_info);
    if (rendered==1) {
      return out.offset;
    }
  }

  return offset;
}

// Move to previous word
file_offset_t document_text_word_transition_prev(struct document* base, struct splitter* splitter, file_offset_t offset) {
  struct document_file* file = splitter->file;
  struct document_view* view = splitter->view;

  if (offset>0) {
    offset--;
  }

  while (offset>0) {
    struct document_text_position out;
    struct document_text_position in;
    in.type = VISUAL_SEEK_WORD_TRANSITION_PREV;
    in.clip = 0;
    in.offset = offset;

    struct document_text_render_info render_info;
    document_text_render_clear(&render_info, document_text_line_width(splitter), view->selection);
    document_text_render_seek(&render_info, file->buffer, file->encoding, &in);
    int rendered = document_text_collect_span(&render_info, view, file, &in, &out, ~0, 1);

    if (rendered==1) {
      document_text_render_destroy(&render_info);
      return out.offset;
    } else if (rendered==0) {
      offset = render_info.offset;
      document_text_render_destroy(&render_info);
    } else if (rendered==-1) {
      document_text_render_destroy(&render_info);
      file_offset_t diff;
      struct range_tree_node* node = range_tree_find_offset(file->buffer, offset, &diff);
      if (!node) {
        return 0;
      }

      node = range_tree_prev(node);
      if (!node) {
        return 0;
      }

      offset = range_tree_offset(node)+node->length-1;
    }
  }

  return offset;
}

// sequence text // TODO: move to encoding.c
void document_text_transform(struct document* base, struct trie* transformation, struct document_file* file, file_offset_t from, file_offset_t to) {
  int seek = 1;
  size_t offset = 0;
  struct stream stream;
  stream_from_plain(&stream, NULL, 0);
  struct unicode_sequencer sequencer;

  uint8_t recoded[1024];
  size_t recode = 0;
  file_offset_t recode_from = 0;
  while (from<to) {
    if (seek) {
      seek = 0;
      offset = 0;
      file_offset_t displacement;
      struct range_tree_node* buffer = range_tree_find_offset(file->buffer, from, &displacement);
      if (!buffer) {
        return;
      }

      stream_destroy(&stream);
      stream_from_page(&stream, buffer, displacement);
      unicode_sequencer_clear(&sequencer, file->encoding, &stream);
    }

    size_t advance = 0;
    size_t length = 0;
    struct unicode_sequence* sequence = unicode_transform(transformation, &sequencer, offset, &advance, &length);
    if (recode>512 || (!sequence && recode>256)) {
      document_file_delete(file, recode_from, from-recode_from);
      document_file_reduce(&to, recode_from, from-recode_from);
      document_file_insert(file, recode_from, &recoded[0], recode, 0);
      document_file_expand(&to, recode_from, recode);
      recode = 0;
      seek = 1;
    }

    if (!sequence) {
      struct unicode_sequence* sequence = unicode_sequencer_find(&sequencer, offset);
      if (recode>0) {
        // TODO: this might change the original encoded characters due to decode and reencode (just copy the old things if possible)
        for (size_t n = 0; n<sequence->length; n++) {
          recode += file->encoding->encode(file->encoding, sequence->cp[n], &recoded[recode], 8);
        }
      }

      from += sequence->size;
      offset++;
    } else {
      if (recode==0) {
        recode_from = from;
      }
      from += length;
      offset += advance;

      for (size_t n = 0; n<sequence->length; n++) {
        recode += file->encoding->encode(file->encoding, sequence->cp[n], &recoded[recode], 8);
      }
    }
  }

  if (recode>0) {
    document_file_delete(file, recode_from, from-recode_from);
    document_file_insert(file, recode_from, &recoded[0], recode, 0);
  }
}
