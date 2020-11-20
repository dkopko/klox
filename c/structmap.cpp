#include "structmap.h"

#include "trace.h"


void
structmap_init(struct structmap *sm, structmap_value_size_t sizeof_value)
{
  sm->enclosed_mask = 0;
  sm->shl = 0;
  sm->lowest_inserted_key = 0;
  sm->highest_inserted_key = 0;
  sm->root_node_offset = CB_NULL;
  sm->node_count = 0;
  sm->total_external_size = 0;
  sm->height = 0;
  sm->sizeof_value = sizeof_value;
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
        //printf("DANDEBUG setting child offset %p to 1\n", &(sn->children[i]));
        sn->children[i] = 1;
      }
    }

    *node_offset = new_node_offset;

    return 0;
}

static void
structmap_heighten(struct cb        **cb,
                   struct cb_region  *region,
                   struct structmap  *sm)
{
  cb_offset_t new_root_node_offset;
  int ret;

  (void) ret;

  ret = structmap_node_alloc(cb, region, &new_root_node_offset);
  assert(ret == 0);
  ++(sm->node_count);

  struct structmap_node *new_root_node = (struct structmap_node *)cb_at(*cb, new_root_node_offset);
  new_root_node->children[0] = sm->root_node_offset;
  //KLOX_TRACE("DANDEBUG setting new_root_node->children[0] = sm->root_node_offset%ju\n", (uintmax_t)sm->root_node_offset);

  sm->root_node_offset = new_root_node_offset;
  if (sm->enclosed_mask) { sm->shl += STRUCTMAP_LEVEL_BITS; }
  sm->enclosed_mask = (sm->enclosed_mask << STRUCTMAP_LEVEL_BITS) | (((uint64_t)1 << STRUCTMAP_LEVEL_BITS) - 1);
  ++(sm->height);
  //KLOX_TRACE("DANDEBUG heightened structmap %p to new height %u, new enclosed_mask: %jx\n", sm, sm->height, (uintmax_t)sm->enclosed_mask);
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

int
structmap_insert(struct cb        **cb,
                 struct cb_region  *region,
                 struct structmap  *sm,
                 uint64_t           key,
                 uint64_t           value)
{
  int ret;

  (void)ret;

  assert(key > 0);

  // We do not want to have to re-sample pointers, so reserve the maximum amount
  // of space for the maximum amount of nodes we may need for this insertion so
  // that no CB resizes would happen.
  ensure_structmap_modification_size(cb, region);

  // Heighten structmap until it encloses this key.
  while ((key & sm->enclosed_mask) != key) {
    structmap_heighten(cb, region, sm);
  }

  struct structmap_node *n = (struct structmap_node *)cb_at(*cb, sm->root_node_offset);
  for (int shl = sm->shl; shl; shl -= STRUCTMAP_LEVEL_BITS) {
    int path = (key >> shl) & (((uint64_t)1 << STRUCTMAP_LEVEL_BITS) - 1);
    cb_offset_t *child_offset = &(n->children[path]);
    assert(*child_offset != 0);

    if (*child_offset == 1) {
      ret = structmap_node_alloc(cb, region, child_offset);
      assert(ret == 0);
      ++(sm->node_count);
    }

    n = (struct structmap_node *)cb_at(*cb, *child_offset);
  }
  int finalpath = key & (((uint64_t)1 << STRUCTMAP_LEVEL_BITS) - 1);
  uint64_t old_value = n->children[finalpath];
  n->children[finalpath] = value;

  structmap_external_size_adjust(sm, (ssize_t)sm->sizeof_value(*cb, value) - (old_value == 1 ? 0 : (ssize_t)sm->sizeof_value(*cb, old_value)));

  if (sm->lowest_inserted_key == 0 || key < sm->lowest_inserted_key)
    sm->lowest_inserted_key = key;

  if (key > sm->highest_inserted_key)
    sm->highest_inserted_key = key;

#ifndef NDEBUG
  {
    uint64_t test_v;
    bool lookup_success = structmap_lookup(*cb, sm, key, &test_v);
    assert(lookup_success);
    assert(test_v == value);
  }
#endif

  return 0;
}

int
structmap_traverse(const struct cb           **cb,
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
    bool lookup_success = structmap_lookup(*cb, sm, k, &v);
    if (lookup_success) {
      func(k, v, closure);
    }
  }

  return 0;
}

