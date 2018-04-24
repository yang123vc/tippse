// Tippse - Encoding - Base for different encoding schemes (characters <-> bytes)

#include "encoding.h"

// Reset code point cache
void encoding_cache_clear(struct encoding_cache* base, struct encoding* encoding, struct stream* stream) {
  base->start = 0;
  base->end = 0;
  base->encoding = encoding;
  base->stream = stream;
}

// Invert lookup table for unicode and codepage translation
uint16_t* encoding_reverse_table(uint16_t* table, size_t length, size_t max) {
  uint16_t* output = (uint16_t*)malloc(sizeof(uint16_t)*max);
  for (size_t n = 0; n<max; n++) {
    output[n] = (uint16_t)~0u;
  }

  for (size_t n = 0; n<length; n++) {
    size_t set = (size_t)table[n];
    if (set<max) {
      output[set] = (uint16_t)n;
    }
  }

  return output;
}

// Transform stream to different encoding
struct range_tree_node* encoding_transform_stream(struct stream* stream, struct encoding* from, struct encoding* to) {
  uint8_t recoded[1024];
  size_t recode = 0;

  struct encoding_cache cache;
  encoding_cache_clear(&cache, from, stream);
  struct range_tree_node* root = NULL;
  while (1) {
    if (stream_end(stream) || recode>512) {
      root = range_tree_insert_split(root, range_tree_length(root), &recoded[0], recode, 0);
      recode = 0;
      if (stream_end(stream)) {
        break;
      }
    }

    codepoint_t cp = encoding_cache_find_codepoint(&cache, 0);
    encoding_cache_advance(&cache, 1);
    if (cp==-1) {
      continue;
    }

    recode += to->encode(to, cp, &recoded[recode], 8);
  }

  return root;
}

// Transform plain text to different encoding
uint8_t* encoding_transform_plain(const uint8_t* buffer, size_t length, struct encoding* from, struct encoding* to) {
  struct stream stream;
  stream_from_plain(&stream, buffer, length);
  struct range_tree_node* root = encoding_transform_stream(&stream, from, to);
  uint8_t* raw = range_tree_raw(root, 0, root?root->length:0);
  range_tree_destroy(root, NULL);
  return raw;
}

// Check string length (within maximum)
size_t encoding_strnlen(struct encoding* base, struct stream* stream, size_t size) {
  return encoding_strnlen_based(base, base->next, stream, size);
}

size_t encoding_strnlen_based(struct encoding* base, size_t (*next)(struct encoding*, struct stream*), struct stream* stream, size_t size) {
  size_t length = 0;
  while (size!=0) {
    size_t skip = (*next)(base, stream);
    if (skip>size) {
      size = 0;
    } else {
      size -= skip;
    }

    length++;
  }

  return length;
}

// Check string length (to NUL terminator)
size_t encoding_strlen(struct encoding* base, struct stream* stream) {
  return encoding_strlen_based(base, base->decode, stream);
}

size_t encoding_strlen_based(struct encoding* base, codepoint_t (*decode)(struct encoding*, struct stream*, size_t*), struct stream* stream) {
  size_t length = 0;
  while (1) {
    size_t skip;
    codepoint_t cp = encoding_utf8_decode(base, stream, &skip);
    if (cp==0) {
      break;
    }

    length++;
  }

  return length;
}

// Skip to character position
size_t encoding_seek(struct encoding* base, struct stream* stream, size_t pos) {
  return encoding_seek_based(base, base->next, stream, pos);
}

size_t encoding_seek_based(struct encoding* base, size_t (*next)(struct encoding*, struct stream*), struct stream* stream, size_t pos) {
  size_t current = 0;
  while (pos!=0) {
    size_t skip = encoding_utf8_next(base, stream);
    current += skip;
    pos--;
  }

  return current;
}
