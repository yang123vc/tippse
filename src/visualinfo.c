/* Tippse - Visual information store - Holds information about variable length or long visuals like comments/strings etc. */

#include "visualinfo.h"

// Reset structure to known state
void visual_info_clear(struct visual_info* visuals) {
  memset(visuals, 0, sizeof(struct visual_info));
  visuals->dirty = VISUAL_DIRTY_UPDATE|VISUAL_DIRTY_LEFT;
}

// Update page with informations about two other pages (usally its children)
void visual_info_combine(struct visual_info* visuals, const struct visual_info* left, const struct visual_info* right) {
  visuals->characters = left->characters+right->characters;
  visuals->lines = left->lines+right->lines;
  visuals->ys = left->ys+right->ys;
  if (right->ys!=0) {
    visuals->xs = right->xs;
  } else {
    visuals->xs = left->xs+right->xs;
  }

  if (right->lines!=0) {
    visuals->columns = right->columns;
    visuals->indentation = right->indentation;
    visuals->indentation_extra = right->indentation_extra;
  } else {
    visuals->columns = left->columns+right->columns;
    visuals->indentation = left->indentation+right->indentation;
    visuals->indentation_extra = left->indentation_extra+right->indentation_extra;
  }

  visuals->detail_before = left->detail_before;
  visuals->detail_after = ((left->detail_after&right->detail_after)&VISUAL_INFO_WHITESPACED_COMPLETE)|((left->detail_after|right->detail_after)&VISUAL_INFO_WHITESPACED_START);

  int dirty = (left->dirty|right->dirty)&VISUAL_DIRTY_UPDATE;
  dirty |= (left->dirty)&(VISUAL_DIRTY_SPLITTED|VISUAL_DIRTY_LEFT);

  if (!(left->dirty&VISUAL_DIRTY_UPDATE) && (right->dirty&VISUAL_DIRTY_LEFT) && !(dirty&VISUAL_DIRTY_SPLITTED)) {
    dirty |= VISUAL_DIRTY_LASTSPLIT|VISUAL_DIRTY_SPLITTED;
  }

  visuals->dirty = dirty;

  for (size_t n = 0; n<VISUAL_BRACKET_MAX; n++) {
    visuals->brackets[n].diff = right->brackets[n].diff+left->brackets[n].diff;

    visuals->brackets[n].max = right->brackets[n].max+left->brackets[n].diff;
    if (visuals->brackets[n].max<left->brackets[n].max) {
      visuals->brackets[n].max = left->brackets[n].max;
    }

    visuals->brackets[n].min = right->brackets[n].min-left->brackets[n].diff;
    if (visuals->brackets[n].min<left->brackets[n].min) {
      visuals->brackets[n].min = left->brackets[n].min;
    }
  }
}