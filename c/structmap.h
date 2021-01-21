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

static const int STRUCTMAP_FIRSTLEVEL_BITS = 10;
static const int STRUCTMAP_LEVEL_BITS = 5;

// The maximum amount structmap_nodes we may need for a modification (insertion)
// so that no CB resizes will happen.  This is ceil((64 - STRUCTMAP_FIRSTLEVEL_BITS) / STRUCTMAP_LEVEL_BITS).
static const int STRUCTMAP_MODIFICATION_MAX_NODES = ((64 - STRUCTMAP_FIRSTLEVEL_BITS) / STRUCTMAP_LEVEL_BITS) + (int)!!((64 - STRUCTMAP_FIRSTLEVEL_BITS) % STRUCTMAP_LEVEL_BITS);

//NOTES:
// 1) Neither keys nor values are allowed to be 0, as this value is reserved
//    for NULL-like sentinels.  FIXME is this still true after the revision toward the Bagwell-like implementation?

typedef size_t (*structmap_value_size_t)(const struct cb *cb, uint64_t v);

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
    structmap_value_size_t sizeof_value;

    struct structmap_entry entries[1 << STRUCTMAP_FIRSTLEVEL_BITS];
};

struct structmap_node
{
    struct structmap_entry entries[1 << STRUCTMAP_LEVEL_BITS];
};


void structmap_init(struct structmap *sm, structmap_value_size_t sizeof_value);

int
structmap_insert(struct cb        **cb,
                 struct cb_region  *region,
                 struct structmap  *sm,
                 uint64_t           key,
                 uint64_t           value);

extern inline bool
structmap_lookup(const struct cb        *cb,
                 const struct structmap *sm,
                 uint64_t                key,
                 uint64_t               *value)
{

  const struct structmap_entry *entry = &(sm->entries[key & ((1 << STRUCTMAP_FIRSTLEVEL_BITS) - 1)]);
  unsigned int key_route_base = STRUCTMAP_FIRSTLEVEL_BITS;

  while (true) {
    switch (entry->type) {
      case STRUCTMAP_ENTRY_EMPTY:
        return false;

      case STRUCTMAP_ENTRY_ITEM:
        if (key == entry->item.key) {
          *value = entry->item.value;
          return true;
        } else {
          return false;
        }

      case STRUCTMAP_ENTRY_NODE: {
        const struct structmap_node *child_node = (struct structmap_node *)cb_at(cb, entry->node.offset);
        unsigned int child_route = (key >> key_route_base) & ((1 << STRUCTMAP_LEVEL_BITS) - 1);
        entry = &(child_node->entries[child_route]);
        key_route_base += STRUCTMAP_LEVEL_BITS;
      }
      break;

#ifndef NDEBUG
      default:
        printf("Bogus structmap entry type: %d\n", entry->type);
        assert(false);
        break;
#endif
    }
  }

  return false;
}

extern inline bool
structmap_contains_key(const struct cb        *cb,
                       const struct structmap *sm,
                       uint64_t                key)
{
  uint64_t v;
  return (structmap_lookup(cb, sm, key, &v) == true);
}

typedef int (*structmap_traverse_func_t)(uint64_t key, uint64_t value, void *closure);

int
structmap_traverse(const struct cb           **cb,
                   const struct structmap     *sm,
                   structmap_traverse_func_t   func,
                   void                       *closure);

extern inline size_t
structmap_modification_size(void) //FIXME switch to constant?
{
  // The maximum amount of space for the maximum amount of nodes we may need for
  // a modification, plus alignment.

  return STRUCTMAP_MODIFICATION_MAX_NODES * sizeof(struct structmap_node) + alignof(struct structmap_node) - 1;
}

extern inline size_t
structmap_internal_size(const struct structmap *sm)
{
  return sm->node_count * sizeof(struct structmap_node) + alignof(struct structmap_node) - 1;
}

extern inline size_t
structmap_external_size(const struct structmap *sm)
{
  return sm->total_external_size;
}

extern inline void
structmap_external_size_adjust(struct structmap *sm,
                               ssize_t           adjustment)
{
  sm->total_external_size = (size_t)((ssize_t)sm->total_external_size + adjustment);
}

extern inline size_t
structmap_size(const struct structmap *sm)
{
  return structmap_internal_size(sm) + structmap_external_size(sm) + structmap_modification_size();
}

#endif  //klox_structmap_h
