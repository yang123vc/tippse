/* Tippse - Document undo - Manages undo and redo lists */

#include "documentundo.h"

void document_undo_add(struct document_file* file, struct document_view* view, file_offset_t offset, file_offset_t length, int type) {
  if (length==0) {
    return;
  }

  struct document_undo* undo = (struct document_undo*)malloc(sizeof(struct document_undo));
  undo->offset = offset;
  undo->length = length;
  undo->type = type;
  undo->buffer = range_tree_copy(file->buffer, offset, length);
  undo->cursor_delete = offset;
  undo->cursor_insert = offset+length;
/*  undo->selection_start = view->selection_start;
  undo->selection_end = view->selection_end;
  undo->selection_low = view->selection_low;
  undo->selection_high = view->selection_high;
  undo->scroll_x = view->scroll_x;
  undo->scroll_y = view->scroll_y;*/

  list_insert(file->undos, NULL, undo);
}

int document_undo_modified(struct document_file* file) {
   return (file->undo_save_point!=file->undos->count)?1:0;
}

void document_undo_mark_save_point(struct document_file* file) {
  document_undo_chain(file, file->undos);
  file->undo_save_point = file->undos->count;
}

void document_undo_check_save_point(struct document_file* file) {
  if (file->undos->count+file->redos->count<file->undo_save_point) {
    file->undo_save_point = -1;
  }
}

void document_undo_chain(struct document_file* file, struct list* list) {
  struct list_node* node = list->first;
  if (!node) {
    return;
  }

  struct document_undo* prev = (struct document_undo*)node->object;
  if (prev->type!=TIPPSE_UNDO_TYPE_CHAIN) {
    struct document_undo* undo = (struct document_undo*)malloc(sizeof(struct document_undo));
    undo->type = TIPPSE_UNDO_TYPE_CHAIN;
    undo->buffer = NULL;

    list_insert(list, NULL, undo);
  }
}

void document_undo_empty(struct document_file* file, struct list* list) {
  while (1) {
    struct list_node* node = list->first;
    if (!node) {
      break;
    }

    struct document_undo* undo = (struct document_undo*)node->object;
    if (undo->buffer) {
      range_tree_destroy(undo->buffer);
    }

    free(undo);
    list_remove(list, node);
  }

  document_undo_check_save_point(file);
}

void document_undo_execute_chain(struct document_file* file, struct document_view* view, struct list* from, struct list* to, int reverse) {
  document_undo_execute(file, view, from, to, 1);
  while (document_undo_execute(file, view, from, to, reverse)) {}
}

int document_undo_execute(struct document_file* file, struct document_view* view, struct list* from, struct list* to, int override) {
  int chain = 0;
  struct list_node* node = from->first;
  if (!node) {
    return chain;
  }

  file_offset_t offset = 0;
  struct document_undo* undo = (struct document_undo*)node->object;
  if (undo->type==TIPPSE_UNDO_TYPE_INSERT) {
    file->buffer = range_tree_delete(file->buffer, undo->offset, undo->length, 0);
    offset = undo->cursor_delete;

    struct list_node* views = file->views->first;
    while (views) {
      struct document_view* view = (struct document_view*)views->object;
      view->selection = range_tree_reduce(view->selection, undo->offset, undo->length);
      file->bookmarks = range_tree_reduce(file->bookmarks, undo->offset, undo->length);
      document_file_reduce(&view->selection_end, undo->offset, undo->length);
      document_file_reduce(&view->selection_start, undo->offset, undo->length);
      document_file_reduce(&view->selection_low, undo->offset, undo->length);
      document_file_reduce(&view->selection_high, undo->offset, undo->length);
      document_file_reduce(&view->offset, undo->offset, undo->length);

      views = views->next;
    }

    undo->type = TIPPSE_UNDO_TYPE_DELETE;
    view->offset = offset;
    chain = 1;
  } else if (undo->type==TIPPSE_UNDO_TYPE_DELETE) {
    file->buffer = range_tree_paste(file->buffer, undo->buffer, undo->offset);
    offset = undo->cursor_insert;

    struct list_node* views = file->views->first;
    while (views) {
      struct document_view* view = (struct document_view*)views->object;
      view->selection = range_tree_expand(view->selection, undo->offset, undo->length);
      file->bookmarks = range_tree_expand(file->bookmarks, undo->offset, undo->length);
      document_file_expand(&view->selection_end, undo->offset, undo->length);
      document_file_expand(&view->selection_start, undo->offset, undo->length);
      document_file_expand(&view->selection_low, undo->offset, undo->length);
      document_file_expand(&view->selection_high, undo->offset, undo->length);
      document_file_expand(&view->offset, undo->offset, undo->length);

      views = views->next;
    }

    undo->type = TIPPSE_UNDO_TYPE_INSERT;
    chain = 1;
  }

  if (chain==1) {
    view->offset = offset;

/*  view->selection_start = undo->selection_start;
    view->selection_end = undo->selection_end;
    view->selection_low = undo->selection_low;
    view->selection_high = undo->selection_high;
    view->scroll_x = undo->scroll_x;
    view->scroll_y = undo->scroll_y;*/
  }

  if (undo->type!=TIPPSE_UNDO_TYPE_CHAIN || override) {
    list_insert(to, NULL, undo);
    list_remove(from, node);
  }

  return chain;
}
