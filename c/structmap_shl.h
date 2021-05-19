#ifndef klox_structmap_shl_h
#define klox_structmap_shl_h

#include <assert.h>
#include <inttypes.h>
#include <stdint.h>

#include <cb.h>
#include <cb_region.h>

#if NDEBUG
#define DEBUG_ONLY(x)
#else
#define DEBUG_ONLY(x) x
#endif

#define CB_NULL ((cb_offset_t)0)

extern __thread void      *thread_ring_start;
extern __thread cb_mask_t  thread_ring_mask;
extern __thread bool       on_main_thread;

//NOTES:
// 1) Neither keys nor values are allowed to be 0, as this value is reserved
//    for NULL-like sentinels.  FIXME is this still true after the revision toward the Bagwell-like implementation?

typedef size_t (*structmap_value_size_t)(const struct cb *cb, uint64_t v);
typedef int (*structmap_traverse_func_t)(uint64_t key, uint64_t value, void *closure);

template<unsigned int FIRSTLEVEL_BITS, unsigned int LEVEL_BITS>
struct structmap_shl
{
  struct firstlevel_entry
  {
    uint64_t     enclosed_mask;
    int          shl;
    unsigned int height;
    uint64_t     child;
  };

  uint64_t               lowest_inserted_key;
  uint64_t               highest_inserted_key;
  cb_offset_t            root_node_offset;
  unsigned int           node_count_;
  size_t                 total_external_size;
  unsigned int           layer_mark_node_count;
  size_t                 layer_mark_external_size;
  structmap_value_size_t sizeof_value;
  firstlevel_entry       children[1 << FIRSTLEVEL_BITS];

  struct node
  {
      uint64_t children[1 << LEVEL_BITS];
  };

  // The maximum amount structmap_nodes we may need for a modification (insertion)
  // This is ceil((64 - FIRSTLEVEL_BITS) / LEVEL_BITS).
  // On modification, this will be preallocated to ensure no CB resizes happen.
  static const unsigned int MODIFICATION_MAX_NODES = 2 * (((64 - FIRSTLEVEL_BITS) / LEVEL_BITS) + (int)!!((64 - FIRSTLEVEL_BITS) % LEVEL_BITS)) - 1;
  static const size_t MODIFICATION_MAX_SIZE = MODIFICATION_MAX_NODES * sizeof(node) + alignof(node) - 1;

  void init(structmap_value_size_t sizeof_value);

  int
  node_alloc(struct cb        **cb,
             struct cb_region  *region,
             cb_offset_t       *node_offset);

  static void
  ensure_modification_size(struct cb        **cb,
                           struct cb_region  *region);

  void
  heighten(struct cb        **cb,
           struct cb_region  *region,
           firstlevel_entry  *entry);

  int
  traverse(const struct cb           **cb,
           structmap_traverse_func_t   func,
           void                       *closure) const;


  unsigned int
  node_count() const
  {
    return this->node_count_;
  }

  size_t
  internal_size() const
  {
    //NOTE: Because the nodes may not be contiguous but rather interleaved with
    // other, external structures, we have to account for as many alignments.
    return node_count() * (sizeof(node) + alignof(node) - 1);
  }

  size_t
  external_size() const
  {
    return this->total_external_size;
  }

  void
  external_size_adjust(ssize_t adjustment) {
    assert(adjustment >= 0 || -adjustment < (ssize_t)this->total_external_size);
    this->total_external_size = (size_t)((ssize_t)this->total_external_size + adjustment);
  }

  void
  set_layer_mark()
  {
    this->layer_mark_external_size = this->total_external_size;
    this->layer_mark_node_count = this->node_count_;
  }

  //NOTE: This returns an ssize_t in case the marked layer has incorporated a
  // reduction of internal nodes. (as of 2020-02-12 reduction of internal nodes is not yet possible)
  ssize_t
  layer_internal_size() const
  {
    return ((int)node_count() - (int)this->layer_mark_node_count) * (ssize_t)(sizeof(node) + alignof(node) - 1);
  }

  //NOTE: This returns an ssize_t in case the marked layer has incorporated a
  // reduction of external sizes (e.g. via replacement of a key's value with a
  // value bearing a smaller external size).
  ssize_t
  layer_external_size() const
  {
    return (ssize_t)this->total_external_size - (ssize_t)this->layer_mark_external_size;
  }

  size_t
  size() const
  {
    return this->internal_size() + this->external_size();
  }


  bool
  lookup(const struct cb *cb,
         uint64_t         key,
         uint64_t        *value) const
  {
    const firstlevel_entry *entry = &(this->children[key & ((1 << FIRSTLEVEL_BITS) - 1)]);
    key >>= FIRSTLEVEL_BITS;

    if ((key & entry->enclosed_mask) != key || !entry->enclosed_mask)
      return false;

    node *n;
    uint64_t child = entry->child;
    int path;

    int shl = entry->shl;
    do {
      n = (node *)cb_at_immed(thread_ring_start, thread_ring_mask, child);
      path = (key >> shl) & (((uint64_t)1 << LEVEL_BITS) - 1);
      child = n->children[path];
      if (child == 1) { return false; }
      shl -= LEVEL_BITS;
    } while (shl >= 0);

    *value = child;
    return true;
  }

  int
  insert(struct cb        **cb,
         struct cb_region  *region,
         uint64_t           key,
         uint64_t           value);

  bool
  contains_key(const struct cb *cb,
               uint64_t         key) const
  {
    uint64_t v;
    return lookup(cb, key, &v);
  }

  unsigned int
  would_collide_node_count(const struct cb        *cb,
                           uint64_t                key) const
  {
    // The types of collisions this method is designed to report do not exist
    // for the SHL implementation.
    return 0;
  }
};

template<unsigned int FIRSTLEVEL_BITS, unsigned int LEVEL_BITS>
void
structmap_shl<FIRSTLEVEL_BITS, LEVEL_BITS>::init(structmap_value_size_t sizeof_value)
{
  this->lowest_inserted_key = 0;
  this->highest_inserted_key = 0;
  this->root_node_offset = CB_NULL;
  this->node_count_ = 0;
  this->total_external_size = 0;
  this->layer_mark_node_count = 0;
  this->layer_mark_external_size = 0;
  this->sizeof_value = sizeof_value;

  for (int i = 0; i < (1 << FIRSTLEVEL_BITS); ++i) {
    this->children[i].enclosed_mask = 0;
    this->children[i].shl = 0;
    this->children[i].height = 0;
    this->children[i].child = 1;
  }
}

template<unsigned int FIRSTLEVEL_BITS, unsigned int LEVEL_BITS>
int
structmap_shl<FIRSTLEVEL_BITS, LEVEL_BITS>::node_alloc(struct cb        **cb,
                                                       struct cb_region  *region,
                                                       cb_offset_t       *node_offset)
{
    cb_offset_t new_node_offset;
    int ret;

    ret = cb_region_memalign(cb,
                             region,
                             &new_node_offset,
                             cb_alignof(node),
                             sizeof(node));
    //if (ret != CB_SUCCESS) { printf("DANDEBUGwtf4\n"); abort(); }
    assert(ret == CB_SUCCESS);
    if (ret != CB_SUCCESS)
        return ret;

    // Initialize.
    {
      node *sn = (node *)cb_at(*cb, new_node_offset);
      for (int i = 0; i < (1 << LEVEL_BITS); ++i) {
        sn->children[i] = 1;
      }
    }

    ++(this->node_count_);

    *node_offset = new_node_offset;

    return 0;
}

template<unsigned int FIRSTLEVEL_BITS, unsigned int LEVEL_BITS>
void
structmap_shl<FIRSTLEVEL_BITS, LEVEL_BITS>::ensure_modification_size(struct cb        **cb,
                                                                     struct cb_region  *region)
{
  int ret;

  (void)ret;

  //Simulate an allocation of the size we may need.
  cb_offset_t      cursor     = cb_cursor(*cb);
  struct cb_region region_tmp = *region;
  cb_offset_t      offset_tmp;

  ret = cb_region_memalign(cb, &region_tmp, &offset_tmp, alignof(node), MODIFICATION_MAX_NODES * sizeof(node));
  assert(ret == CB_SUCCESS);
  //if (ret != CB_SUCCESS) { printf("DANDEBUGwtf5\n"); abort(); }
  if (region_tmp.start != region->start) {
    //NOTE: This is only appropriate on the main thread.  The GC thread should
    // never overwrite the cursor, as it is owned by and in use by the main thread.
    // But the region in use by the GC thread should be pre-sized and therefore
    // tmp.start == region->start.
    assert(on_main_thread);
    cb_rewind_to(*cb, cursor);
  }
}

template<unsigned int FIRSTLEVEL_BITS, unsigned int LEVEL_BITS>
void
structmap_shl<FIRSTLEVEL_BITS, LEVEL_BITS>::heighten(struct cb        **cb,
                                                     struct cb_region  *region,
                                                     firstlevel_entry  *entry)
{
  cb_offset_t new_node_offset;
  int ret;

  (void) ret;

  ret = node_alloc(cb, region, &new_node_offset);
  assert(ret == 0);

  node *new_root_node = (node *)cb_at(*cb, new_node_offset);
  new_root_node->children[0] = entry->child;

  entry->child = new_node_offset;
  if (entry->enclosed_mask) { entry->shl += LEVEL_BITS; }
  entry->enclosed_mask = (entry->enclosed_mask << LEVEL_BITS) | (((uint64_t)1 << LEVEL_BITS) - 1);
  ++(entry->height);
  //KLOX_TRACE("DANDEBUG heightened structmap %p to new height %u, new enclosed_mask: %jx\n", sm, sm->height, (uintmax_t)sm->enclosed_mask);
}


template<unsigned int FIRSTLEVEL_BITS, unsigned int LEVEL_BITS>
int
structmap_shl<FIRSTLEVEL_BITS, LEVEL_BITS>::insert(struct cb        **cb,
                                                   struct cb_region  *region,
                                                   uint64_t           key,
                                                   uint64_t           value)
{
  DEBUG_ONLY(unsigned int pre_node_count = this->node_count_);
  uint64_t orig_key = key;
  int ret;

  (void)ret;

  assert(key > 0);

  // We do not want to have to re-sample pointers, so reserve the maximum amount
  // of space for the maximum amount of nodes we may need for this insertion so
  // that no CB resizes would happen.
  ensure_modification_size(cb, region);

  // Heighten structmap until it encloses this key.
  firstlevel_entry *entry = &(this->children[key & ((1 << FIRSTLEVEL_BITS) - 1)]);
  key >>= FIRSTLEVEL_BITS;

  while ((key & entry->enclosed_mask) != key || !entry->enclosed_mask) {
    heighten(cb, region, entry);
  }

  node *n = (node *)cb_at(*cb, entry->child);
  for (int shl = entry->shl; shl; shl -= LEVEL_BITS) {
    int path = (key >> shl) & (((uint64_t)1 << LEVEL_BITS) - 1);
    cb_offset_t *child_offset = &(n->children[path]);

    assert(*child_offset != 0);

    if (*child_offset == 1) {
      DEBUG_ONLY(struct cb *cb0 = *cb);
      ret = node_alloc(cb, region, child_offset);
      assert(ret == 0);
      DEBUG_ONLY(struct cb *cb1 = *cb);
      assert(cb0 == cb1);
    }

    n = (node *)cb_at(*cb, *child_offset);
  }
  int finalpath = key & (((uint64_t)1 << LEVEL_BITS) - 1);
  uint64_t old_value = n->children[finalpath];
  n->children[finalpath] = value;

  external_size_adjust((ssize_t)this->sizeof_value(*cb, value) - (old_value == 1 ? 0 : (ssize_t)this->sizeof_value(*cb, old_value)));

  if (this->lowest_inserted_key == 0 || orig_key < this->lowest_inserted_key)
    this->lowest_inserted_key = orig_key;

  if (orig_key > this->highest_inserted_key)
    this->highest_inserted_key = orig_key;

#ifndef NDEBUG
  {
    unsigned int post_node_count = this->node_count_;

    assert(post_node_count >= pre_node_count);
    assert(post_node_count - pre_node_count <= MODIFICATION_MAX_NODES);

    uint64_t test_v;
    bool lookup_success = lookup(*cb, orig_key, &test_v);
    assert(lookup_success);
    assert(test_v == value);
  }
#endif

  return 0;
}

template<unsigned int FIRSTLEVEL_BITS, unsigned int LEVEL_BITS>
int
structmap_shl<FIRSTLEVEL_BITS, LEVEL_BITS>::traverse(const struct cb           **cb,
                                                     structmap_traverse_func_t   func,
                                                     void                       *closure) const
{
  uint64_t v;

  //FIXME Improve this traversal, it is only sufficient for a demo, but will
  // be pretty craptacular when used on a sparse range of keys.
  for (uint64_t k = this->lowest_inserted_key, e = this->highest_inserted_key;
       k > 0 && k <= e;
       ++k)
  {
    bool lookup_success = this->lookup(*cb, k, &v);
    if (lookup_success) {
      func(k, v, closure);
    }
  }

  return 0;
}

#endif  //klox_structmap_shl_h
