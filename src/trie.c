// Tippse - Trie - Faster string lookups

#include "trie.h"

#include "list.h"

// Return pointer to new trie instance
struct trie* trie_create(size_t node_size) {
  struct trie* base = (struct trie*)malloc(sizeof(struct trie));
  base->node_size = node_size;
  base->buckets = list_create((sizeof(struct trie_node)+node_size)*TRIE_NODES_PER_BUCKET);
  trie_clear(base);
  return base;
}

// Free trie memory
void trie_destroy(struct trie* base) {
  trie_clear(base);
  list_destroy(base->buckets);
  free(base);
}

// Remove all nodes from trie
void trie_clear(struct trie* base) {
  while (base->buckets->first) {
    list_remove(base->buckets, base->buckets->first);
  }

  base->fill = TRIE_NODES_PER_BUCKET;
  base->root = NULL;
}

// Create node in bucket and create new bucket if no node is left
struct trie_node* trie_create_node(struct trie* base) {
  if (base->fill==TRIE_NODES_PER_BUCKET) {
    base->fill = 0;
    list_object(list_insert_empty(base->buckets, NULL));
  }

  struct trie_node* node = (struct trie_node*)(((uint8_t*)list_object(base->buckets->first))+(sizeof(struct trie_node)+base->node_size)*base->fill);
  node->parent = NULL;
  node->end = 0;
  for (int side = 0; side<16; side++) {
    node->side[side] = NULL;
  }

  base->fill++;
  return node;
}

// Append specific codepoint to current node (or return result node if it already existed)
struct trie_node* trie_append_codepoint(struct trie* base, struct trie_node* parent, codepoint_t cp, int end) {
  if (!parent) {
    parent = base->root;
    if (!parent) {
      base->root = trie_create_node(base);
      parent = base->root;
    }
  }

  for (int bit = TRIE_CODEPOINT_BIT-4; bit>=0; bit-=4) {
    int set = (cp>>bit)&15;
    struct trie_node* node = parent->side[set];
    if (!node) {
      node = trie_create_node(base);
      parent->side[set] = node;
      node->parent = parent;
    }

    node->end = (bit==0 && end!=0)?end:node->end;

    parent = node;
  }

  return parent;
}

// Find specific codepoint
struct trie_node* trie_find_codepoint(struct trie* base, struct trie_node* parent, codepoint_t cp) {
  if (!parent) {
    parent = base->root;
    if (!parent) {
      return NULL;
    }
  }

  for (int bit = TRIE_CODEPOINT_BIT-4; bit>=0; bit-=4) {
    int set = (cp>>bit)&15;
    struct trie_node* node = parent->side[set];
    if (!node) {
      return NULL;
    }

    parent = node;
  }

  return parent;
}

// Find next codepoint (higher than the one before)
struct trie_node* trie_find_codepoint_min(struct trie* base, struct trie_node* parent, codepoint_t cp, codepoint_t* out) {
  if (!parent) {
    parent = base->root;
    if (!parent) {
      return NULL;
    }
  }

  return trie_find_codepoint_recursive(base, parent, cp, out, 0, TRIE_CODEPOINT_BIT-4);
}

// Find next codepoint (higher than the one before)
struct trie_node* trie_find_codepoint_recursive(struct trie* base, struct trie_node* parent, codepoint_t cp, codepoint_t* out, codepoint_t build, int bit) {

  struct trie_node* node = NULL;
  for (int set = 0; set<16; set++) {
    if (parent->side[set]) {
      codepoint_t update = build|((codepoint_t)set<<bit);
      if (update+(codepoint_t)((1<<bit)-1)>cp) {
        if (bit==0) {
          *out = update;
          return parent->side[set];
        } else {
          node = trie_find_codepoint_recursive(base, parent->side[set], cp, out, update, bit-4);
          if (node) {
            break;
          }
        }
      }
    }
  }

  return node;
}

// Find next single codepoint
codepoint_t trie_find_codepoint_single(struct trie* base, struct trie_node* parent) {
  if (!parent) {
    parent = base->root;
    if (!parent) {
      return UNICODE_CODEPOINT_BAD;
    }
  }

  codepoint_t cp = 0;
  for (int bit = TRIE_CODEPOINT_BIT-4; bit>=0; bit-=4) {
    size_t found = 16;
    for (size_t set = 0; set<16; set++) {
      if (parent->side[set]) {
        if (found!=16) {
          return UNICODE_CODEPOINT_UNASSIGNED;
        }
        found = set;
      }
    }

    if (found==16) {
      return UNICODE_CODEPOINT_BAD;
    }

    struct trie_node* node = parent->side[found];
    if (!node) {
      return UNICODE_CODEPOINT_BAD;
    }

    parent = node;
    cp <<= 4;
    cp |= (codepoint_t)found;
  }

  return cp;
}
