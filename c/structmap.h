#ifndef klox_structmap_h
#define klox_structmap_h

#include <assert.h>
#include <inttypes.h>
#include <stdint.h>

#include <cb.h>
#include <cb_region.h>

#define CB_NULL ((cb_offset_t)0)

extern __thread void *thread_ring_start;
extern __thread cb_mask_t thread_ring_mask;

static const int STRUCTMAP_FIRSTLEVEL_BITS = 0;
static const int STRUCTMAP_LEVEL_BITS = 5;

//NOTES:
// 1) Neither keys nor values are allowed to be 0, as this value is reserved
//    for NULL-like sentinels.  FIXME is this still true after the revision toward the Bagwell-like implementation?

typedef size_t (*structmap_value_size_t)(const struct cb *cb, uint64_t v);
typedef bool (*structmap_is_value_read_cutoff_t)(cb_offset_t read_cutoff, uint64_t v);

enum structmap_entry_type
{
  STRUCTMAP_ENTRY_EMPTY,
  STRUCTMAP_ENTRY_ITEM,
  STRUCTMAP_ENTRY_NODE
};

struct structmap_entry
{
  enum structmap_entry_type type;
  union
  {
    struct { uint64_t key; uint64_t value; } item;
    struct { uint64_t offset; } node;
  };
};

struct structmap
{
    uint64_t     lowest_inserted_key;
    uint64_t     highest_inserted_key;
    cb_offset_t  root_node_offset; //FIXME remove, no longer used
    unsigned int node_count;
    size_t       total_external_size;
    unsigned int layer_mark_node_count;
    size_t       layer_mark_external_size;
    structmap_value_size_t sizeof_value;
    structmap_is_value_read_cutoff_t is_value_read_cutoff;

    struct structmap_entry entries[1 << STRUCTMAP_FIRSTLEVEL_BITS];
};

struct structmap_node
{
    struct structmap_entry entries[1 << STRUCTMAP_LEVEL_BITS];
};


// The maximum amount structmap_nodes we may need for a modification (insertion)
// so that no CB resizes will happen.  This is ceil((64 - STRUCTMAP_FIRSTLEVEL_BITS) / STRUCTMAP_LEVEL_BITS).
static const int STRUCTMAP_MODIFICATION_MAX_NODES = ((64 - STRUCTMAP_FIRSTLEVEL_BITS) / STRUCTMAP_LEVEL_BITS) + (int)!!((64 - STRUCTMAP_FIRSTLEVEL_BITS) % STRUCTMAP_LEVEL_BITS);
static const size_t STRUCTMAP_MODIFICATION_SIZE = (STRUCTMAP_MODIFICATION_MAX_NODES * sizeof(struct structmap_node)) + alignof(struct structmap_node) - 1;  //Contiguous, so no need to include alignment.


void structmap_init(struct structmap *sm, structmap_value_size_t sizeof_value, structmap_is_value_read_cutoff_t is_value_read_cutoff);

int
structmap_insert(struct cb        **cb,
                 struct cb_region  *region,
                 cb_offset_t        read_cutoff,
                 cb_offset_t        write_cutoff,
                 struct structmap  *sm,
                 uint64_t           key,
                 uint64_t           value);

bool
structmap_lookup_slowpath(const struct cb        *cb,
                          cb_offset_t             read_cutoff,
                          const struct structmap *sm,
                          uint64_t                key,
                          uint64_t               *value);

extern inline bool
structmap_lookup(const struct cb        *cb,
                 cb_offset_t             read_cutoff,
                 const struct structmap *sm,
                 uint64_t                key,
                 uint64_t               *value)
{
  const struct structmap_entry *entry = &(sm->entries[key & ((1 << STRUCTMAP_FIRSTLEVEL_BITS) - 1)]);

  if (entry->type == STRUCTMAP_ENTRY_ITEM
      && key == entry->item.key
      && !sm->is_value_read_cutoff(read_cutoff, entry->item.value)) {
    *value = entry->item.value;
    return true;
  }

  return structmap_lookup_slowpath(cb, read_cutoff, sm, key, value);
}

extern inline bool
structmap_contains_key(const struct cb        *cb,
                       cb_offset_t             read_cutoff,
                       const struct structmap *sm,
                       uint64_t                key)
{
  uint64_t v;
  return structmap_lookup(cb, read_cutoff, sm, key, &v);
}

unsigned int
structmap_would_collide_node_count_slowpath(const struct cb        *cb,
                                            cb_offset_t             read_cutoff,
                                            const struct structmap *sm,
                                            uint64_t                key);

extern inline unsigned int
structmap_would_collide_node_count(const struct cb        *cb,
                                   cb_offset_t             read_cutoff,
                                   const struct structmap *sm,
                                   uint64_t                key)
{
  const struct structmap_entry *entry = &(sm->entries[key & ((1 << STRUCTMAP_FIRSTLEVEL_BITS) - 1)]);

  if (entry->type == STRUCTMAP_ENTRY_EMPTY
      || (entry->type == STRUCTMAP_ENTRY_ITEM
          && (entry->item.key == key || sm->is_value_read_cutoff(read_cutoff, entry->item.value)))) {
    return 0;
  }

  return structmap_would_collide_node_count_slowpath(cb, read_cutoff, sm, key);
}

typedef int (*structmap_traverse_func_t)(uint64_t key, uint64_t value, void *closure);

int
structmap_traverse(const struct cb           **cb,
                   cb_offset_t                 read_cutoff,
                   const struct structmap     *sm,
                   structmap_traverse_func_t   func,
                   void                       *closure);

extern inline void
structmap_set_layer_mark(struct structmap *sm)
{
  sm->layer_mark_external_size = sm->total_external_size;
  sm->layer_mark_node_count = sm->node_count;
}

extern inline size_t
structmap_internal_size(const struct structmap *sm)
{
  //NOTE: Because the nodes may not be contiguous but rather interleaved with
  // other, external structures, we have to account for as many alignments.
  return sm->node_count * (sizeof(struct structmap_node) + alignof(struct structmap_node) - 1);
}

extern inline size_t
structmap_external_size(const struct structmap *sm)
{
  return sm->total_external_size;
}

extern inline unsigned int
structmap_node_count(const struct structmap *sm)
{
  return sm->node_count;
}

//NOTE: This returns an ssize_t in case the marked layer has incorporated a
// reduction of internal nodes. (as of 2020-02-12 reduction of internal nodes is not yet possible)
extern inline ssize_t
structmap_layer_internal_size(const struct structmap *sm)
{
  return ((int)sm->node_count - (int)sm->layer_mark_node_count) * (ssize_t)(sizeof(struct structmap_node) + alignof(struct structmap_node) - 1);
}

//NOTE: This returns an ssize_t in case the marked layer has incorporated a
// reduction of external sizes (e.g. via replacement of a key's value with a
// value bearing a smaller external size).
extern inline ssize_t
structmap_layer_external_size(const struct structmap *sm)
{
  return (ssize_t)sm->total_external_size - (ssize_t)sm->layer_mark_external_size;
}

extern inline void
structmap_external_size_adjust(struct structmap *sm,
                               ssize_t           adjustment)
{
  assert(adjustment >= 0 || -adjustment < (ssize_t)sm->total_external_size);
  sm->total_external_size = (size_t)((ssize_t)sm->total_external_size + adjustment);
}

extern inline size_t
structmap_size(const struct structmap *sm)
{
  return structmap_internal_size(sm) + structmap_external_size(sm);
}

#endif  //klox_structmap_h
