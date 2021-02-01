#include "structmap.h"

#include "trace.h"


void
structmap_init(struct structmap *sm, structmap_value_size_t sizeof_value)
{
  sm->lowest_inserted_key = 0;
  sm->highest_inserted_key = 0;
  sm->root_node_offset = CB_NULL;
  sm->node_count = 0;
  sm->total_external_size = 0;
  sm->sizeof_value = sizeof_value;

  for (int i = 0; i < (1 << STRUCTMAP_FIRSTLEVEL_BITS); ++i) {
    sm->entries[i].type = STRUCTMAP_ENTRY_EMPTY;
  }

}

static int
structmap_node_alloc(struct cb        **cb,
                     struct cb_region  *region,
                     cb_offset_t       *node_offset)
{
    cb_offset_t new_node_offset;
    int ret;

    ret = cb_region_memalign(cb,
                             region,
                             &new_node_offset,
                             cb_alignof(struct structmap_node),
                             sizeof(struct structmap_node));
    if (ret != CB_SUCCESS)
        return ret;

    {
      struct structmap_node *sn = (struct structmap_node *)cb_at(*cb, new_node_offset);
      for (int i = 0; i < (1 << STRUCTMAP_LEVEL_BITS); ++i) {
        sn->entries[i].type = STRUCTMAP_ENTRY_EMPTY;
      }
    }

    *node_offset = new_node_offset;

    return 0;
}

static void
ensure_structmap_modification_size(struct cb        **cb,
                                   struct cb_region  *region)
{
  int     nodes_left = STRUCTMAP_MODIFICATION_MAX_NODES;
  ssize_t size_left_in_region = (ssize_t)(cb_region_end(region) - cb_region_cursor(region) - (alignof(struct structmap_node) - 1));

  //First, check whether we have enough size in this region alone.
  if (size_left_in_region > 0) {
    nodes_left -= size_left_in_region / sizeof(struct structmap_node);
    if (nodes_left <= 0)
      return; //Success, have enough space in remainder of this region.
  }

  //Otherwise, simulate an allocation of the size we may need.
  cb_offset_t      cursor     = cb_cursor(*cb);
  struct cb_region region_tmp = *region;
  cb_offset_t      offset_tmp;

  cb_region_memalign(cb, &region_tmp, &offset_tmp, alignof(struct structmap_node), nodes_left * sizeof(struct structmap_node));
  cb_rewind_to(*cb, cursor);
}

static int
structmap_select_modifiable_node(struct cb        **cb,
                                 struct cb_region  *region,
                                 cb_offset_t        write_cutoff,
                                 cb_offset_t       *node_offset)
{
  int ret;

  (void)ret;

  if (cb_offset_cmp(write_cutoff, *node_offset) <= 0)
    return 0;

  struct structmap_node *old_node = (struct structmap_node *)cb_at(*cb, *node_offset);

  ret = structmap_node_alloc(cb, region, node_offset);
  assert(ret == 0);

  struct structmap_node *new_node = (struct structmap_node *)cb_at(*cb, *node_offset);
  memcpy(new_node, old_node, sizeof(*old_node));
  return 0;
}

int
structmap_insert(struct cb        **cb,
                 struct cb_region  *region,
                 cb_offset_t        read_cutoff,
                 cb_offset_t        write_cutoff,
                 struct structmap  *sm,
                 uint64_t           key,
                 uint64_t           value)
{
  //FIXME use read_cutoff/write_cutoff
  int ret;

  (void)ret;

  assert(cb_offset_cmp(read_cutoff, write_cutoff) <= 0);
  assert(key > 0);

  // We do not want to have to re-sample pointers, so reserve the maximum amount
  // of space for the maximum amount of nodes we may need for this insertion so
  // that no CB resizes would happen.
  ensure_structmap_modification_size(cb, region);

  struct structmap_entry *entry = &(sm->entries[key & ((1 << STRUCTMAP_FIRSTLEVEL_BITS) - 1)]);
  unsigned int key_route_base = STRUCTMAP_FIRSTLEVEL_BITS;

  while (true) {
    switch (entry->type) {
      case STRUCTMAP_ENTRY_EMPTY:
        entry->type = STRUCTMAP_ENTRY_ITEM;
        entry->item.key = key;
        entry->item.value = value;
        structmap_external_size_adjust(sm, (ssize_t)sm->sizeof_value(*cb, value));
        goto exit_loop;

      case STRUCTMAP_ENTRY_ITEM: {
        // Replace the value of the key, if the key is already present.
        if (entry->item.key == key) {
          structmap_external_size_adjust(sm, (ssize_t)sm->sizeof_value(*cb, value) - (ssize_t)sm->sizeof_value(*cb, entry->item.value));
          entry->item.value = value;
          goto exit_loop;
        }

        // Otherwise, there is a collision in this slot at this level, create
        // a child node and add the old_key/old_value to it.
        cb_offset_t child_node_offset = CB_NULL; //FIXME shouldn't have to initialize
        ret = structmap_node_alloc(cb, region, &child_node_offset);
        assert(ret == 0);
        ++(sm->node_count);
        struct structmap_node *child_node = (struct structmap_node *)cb_at(*cb, child_node_offset);
        unsigned int child_route = (entry->item.key >> key_route_base) & ((1 << STRUCTMAP_LEVEL_BITS) - 1);
        struct structmap_entry *child_entry = &(child_node->entries[child_route]);
        child_entry->type = STRUCTMAP_ENTRY_ITEM;
        child_entry->item.key = entry->item.key;
        child_entry->item.value = entry->item.value;

        // Make the old location of the key/value now point to the nested child node.
        entry->type = STRUCTMAP_ENTRY_NODE;
        entry->node.offset = child_node_offset;
      }
      /* FALLTHROUGH to process addition of key/value to the new STRUCTMAP_ENTRY_NODE */
      /* fall through */

      case STRUCTMAP_ENTRY_NODE: {
        structmap_select_modifiable_node(cb, region, write_cutoff, &(entry->node.offset));
        struct structmap_node *child_node = (struct structmap_node *)cb_at(*cb, entry->node.offset);
        unsigned int child_route = (key >> key_route_base) & ((1 << STRUCTMAP_LEVEL_BITS) - 1);
        entry = &(child_node->entries[child_route]);
        key_route_base += STRUCTMAP_LEVEL_BITS;
      }
      break;

#ifndef NDEBUG
      default:
        printf("Bogus structmap entry type: %d\n", entry->type);
        assert(false);
        goto exit_loop;
#endif
    }
  }

exit_loop:
  if (sm->lowest_inserted_key == 0 || key < sm->lowest_inserted_key)
    sm->lowest_inserted_key = key;

  if (key > sm->highest_inserted_key)
    sm->highest_inserted_key = key;

#ifndef NDEBUG
  {
    uint64_t test_v;
    bool lookup_success = structmap_lookup(*cb, write_cutoff, sm, key, &test_v);
    //printf("lookup_success? %d, same? %d:  #%ju -> @%ju\n", lookup_success, test_v == value, (uintmax_t)key, (uintmax_t)value);
    assert(lookup_success);
    assert(test_v == value);
  }
#endif

  return 0;
}

int
structmap_traverse(const struct cb           **cb,
                   cb_offset_t                 read_cutoff,
                   const struct structmap     *sm,
                   structmap_traverse_func_t   func,
                   void                       *closure)
{
  uint64_t v;

  //FIXME Improve this traversal, it is only sufficient for a demo, but will
  // be pretty craptacular when used on a sparse range of keys.
  for (uint64_t k = sm->lowest_inserted_key, e = sm->highest_inserted_key;
       k > 0 && k <= e;
       ++k)
  {
    bool lookup_success = structmap_lookup(*cb, read_cutoff, sm, k, &v);
    if (lookup_success) {
      func(k, v, closure);
    }
  }

  return 0;
}

