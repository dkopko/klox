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

//NOTES:
// 1) Neither keys nor values are allowed to be 0, as this value is reserved
//    for NULL-like sentinels.  FIXME is this still true after the revision toward the Bagwell-like implementation?

typedef size_t (*structmap_value_size_t)(const struct cb *cb, uint64_t v);
typedef bool (*structmap_is_value_read_cutoff_t)(cb_offset_t read_cutoff, uint64_t v);
typedef int (*structmap_traverse_func_t)(uint64_t key, uint64_t value, void *closure);


#if 0
//BACKED OUT SLOW IMPLEMENTATION
enum structmap_entry_type
{
  STRUCTMAP_ENTRY_EMPTY = 0x0,
  STRUCTMAP_ENTRY_ITEM  = 0x1,
  STRUCTMAP_ENTRY_NODE  = 0x2
};

static const unsigned int STRUCTMAP_TYPEMASK = 0x3;

struct structmap_entry
{
  uint64_t key_offset_and_type;
  uint64_t value;
};

extern inline structmap_entry_type
entrytypeof(const structmap_entry *entry) {
  return static_cast<structmap_entry_type>(entry->key_offset_and_type & STRUCTMAP_TYPEMASK);
}

extern inline uint64_t
entrykeyof(const structmap_entry *entry) {
  return (entry->key_offset_and_type >> 2);
}

extern inline uint64_t
entryoffsetof(const structmap_entry *entry) {
  // Rely on alignment to have 2 LSB 0 bits for offsets.
  return ((entry->key_offset_and_type >> 2) << 2);  //shift out the type tag
}
#endif


template<unsigned int FIRSTLEVEL_BITS, unsigned int LEVEL_BITS, structmap_is_value_read_cutoff_t CUTOFF>
struct structmap
{
  struct firstlevel_entry
  {
    uint64_t     enclosed_mask;
    int          shl;
    unsigned int height;
    uint64_t     child;
  };

  //uint64_t               enclosed_mask;
  //int                    shl;
  uint64_t               lowest_inserted_key;
  uint64_t               highest_inserted_key;
  cb_offset_t            root_node_offset;
  unsigned int           node_count_;
  size_t                 total_external_size;
  //unsigned int           height;
  unsigned int           layer_mark_node_count;
  size_t                 layer_mark_external_size;
  structmap_value_size_t sizeof_value;
  //struct structmap_entry entries[1 << FIRSTLEVEL_BITS];
  firstlevel_entry       children[1 << FIRSTLEVEL_BITS];

  struct node
  {
      //struct structmap_entry entries[1 << LEVEL_BITS];
      uint64_t children[1 << LEVEL_BITS];
  };

  // The maximum amount structmap_nodes we may need for a modification (insertion)
  // This is ceil((64 - FIRSTLEVEL_BITS) / LEVEL_BITS).
  // On modification, this will be preallocated to ensure no CB resizes happen.
  static const unsigned int MODIFICATION_MAX_NODES = ((64 - FIRSTLEVEL_BITS) / LEVEL_BITS) + (int)!!((64 - FIRSTLEVEL_BITS) % LEVEL_BITS);
  static const size_t MODIFICATION_MAX_SIZE = MODIFICATION_MAX_NODES * sizeof(node) + alignof(node) - 1;

  void init(structmap_value_size_t sizeof_value);

  int
  node_alloc(struct cb        **cb,
             struct cb_region  *region,
             cb_offset_t       *node_offset);

  static void
  ensure_modification_size(struct cb        **cb,
                           struct cb_region  *region);

  int
  select_modifiable_node(struct cb        **cb,
                         struct cb_region  *region,
                         cb_offset_t        read_cutoff,
                         cb_offset_t        write_cutoff,
                         cb_offset_t       *node_offset);

  void
  heighten(struct cb        **cb,
           struct cb_region  *region,
           firstlevel_entry  *entry);

#if 0
  bool
  lookup_slowpath(const struct cb *cb,
                  cb_offset_t      read_cutoff,
                  uint64_t         key,
                  uint64_t        *value) const;

  unsigned int
  would_collide_node_count_slowpath(const struct cb *cb,
                                    cb_offset_t      read_cutoff,
                                    uint64_t         key) const;
#endif

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

    for (int shl = entry->shl; shl; shl -= LEVEL_BITS) {
      n = (node *)cb_at_immed(thread_ring_start, thread_ring_mask, child);
      path = (key >> shl) & (((uint64_t)1 << LEVEL_BITS) - 1);
      child = n->children[path];
      if (child == 1) { return false; }
    }
    n = (node *)cb_at_immed(thread_ring_start, thread_ring_mask, child);
    path = key & (((uint64_t)1 << LEVEL_BITS) - 1);
    uint64_t tmpval = n->children[path];
    if (tmpval == 1) { return false; }

    *value = tmpval;

    return true;

#if 0
    //OLD PERFORMANT IMPLEMENTATION
    if ((key & sm->enclosed_mask) != key)
      return false;

    struct structmap_node *n;
    uint64_t child = sm->root_node_offset;
    int path;

    for (int shl = sm->shl; shl; shl -= STRUCTMAP_LEVEL_BITS) {
      n = (struct structmap_node *)cb_at_immed(thread_ring_start, thread_ring_mask, child);
      path = (key >> shl) & (((uint64_t)1 << STRUCTMAP_LEVEL_BITS) - 1);
      child = n->children[path];
      if (child == 1) { return false; }
    }
    n = (struct structmap_node *)cb_at_immed(thread_ring_start, thread_ring_mask, child);
    path = key & (((uint64_t)1 << STRUCTMAP_LEVEL_BITS) - 1);
    uint64_t tmpval = n->children[path];
    if (tmpval == 1) { return false; }

    *value = tmpval;

    return true;
#endif

#if 0
    //BACKED OUT NEW IMPLEMENTATION
    const struct structmap_entry *entry = &(this->entries[key & ((1 << FIRSTLEVEL_BITS) - 1)]);

    if (entrytypeof(entry) == STRUCTMAP_ENTRY_ITEM
        && key == entrykeyof(entry)
        && !CUTOFF(read_cutoff, entry->value)) {
      *value = entry->value;
      return true;
    }

    return this->lookup_slowpath(cb, read_cutoff, key, value);
#endif
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

#if 0
  unsigned int
  would_collide_node_count(const struct cb        *cb,
                           cb_offset_t             read_cutoff,
                           uint64_t                key) const
  {
    const struct structmap_entry *entry = &(this->entries[key & ((1 << FIRSTLEVEL_BITS) - 1)]);

    if (entrytypeof(entry) == STRUCTMAP_ENTRY_EMPTY
        || (entrytypeof(entry) == STRUCTMAP_ENTRY_ITEM
            && (entrykeyof(entry) == key || CUTOFF(read_cutoff, entry->value)))) {
      return 0;
    }

    return this->would_collide_node_count_slowpath(cb, read_cutoff, key);
  }
#endif

};

template<unsigned int FIRSTLEVEL_BITS, unsigned int LEVEL_BITS, structmap_is_value_read_cutoff_t CUTOFF>
void
structmap<FIRSTLEVEL_BITS, LEVEL_BITS, CUTOFF>::init(structmap_value_size_t sizeof_value)
{
  //this->enclosed_mask = 0;
  //this->shl = 0;
  this->lowest_inserted_key = 0;
  this->highest_inserted_key = 0;
  this->root_node_offset = CB_NULL;
  this->node_count_ = 0;
  this->total_external_size = 0;
  //this->height = 0;
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

template<unsigned int FIRSTLEVEL_BITS, unsigned int LEVEL_BITS, structmap_is_value_read_cutoff_t CUTOFF>
int
structmap<FIRSTLEVEL_BITS, LEVEL_BITS, CUTOFF>::node_alloc(struct cb        **cb,
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

template<unsigned int FIRSTLEVEL_BITS, unsigned int LEVEL_BITS, structmap_is_value_read_cutoff_t CUTOFF>
void
structmap<FIRSTLEVEL_BITS, LEVEL_BITS, CUTOFF>::ensure_modification_size(struct cb        **cb,
                                                                         struct cb_region  *region)
{
  int     nodes_left = MODIFICATION_MAX_NODES;
  ssize_t size_left_in_region = (ssize_t)(cb_region_end(region) - cb_region_cursor(region) - (alignof(node) - 1));
  int ret;

  (void)ret;

  //First, check whether we have enough size in this region alone.
  if (size_left_in_region > 0) {
    nodes_left -= size_left_in_region / sizeof(node);
    if (nodes_left <= 0)
      return; //Success, have enough space in remainder of this region.
  }

  //Otherwise, simulate an allocation of the size we may need.
  cb_offset_t      cursor     = cb_cursor(*cb);
  struct cb_region region_tmp = *region;
  cb_offset_t      offset_tmp;

  ret = cb_region_memalign(cb, &region_tmp, &offset_tmp, alignof(node), nodes_left * sizeof(node));
  assert(ret == CB_SUCCESS);
  //if (ret != CB_SUCCESS) { printf("DANDEBUGwtf5\n"); abort(); }
  cb_rewind_to(*cb, cursor);
}

template<unsigned int FIRSTLEVEL_BITS, unsigned int LEVEL_BITS, structmap_is_value_read_cutoff_t CUTOFF>
int
structmap<FIRSTLEVEL_BITS, LEVEL_BITS, CUTOFF>::select_modifiable_node(struct cb        **cb,
                                                                       struct cb_region  *region,
                                                                       cb_offset_t        read_cutoff,
                                                                       cb_offset_t        write_cutoff,
                                                                       cb_offset_t       *node_offset)
{
  int ret;

  (void)ret;

  if (cb_offset_cmp(write_cutoff, *node_offset) <= 0)
    return 0;

  if (CUTOFF(read_cutoff, *node_offset)) {
    ret = this->node_alloc(cb, region, node_offset);
    assert(ret == 0);
    return 0;
  }

  node *old_node = (node *)cb_at(*cb, *node_offset);

  ret = this->node_alloc(cb, region, node_offset);
  assert(ret == 0);

  node *new_node = (node *)cb_at(*cb, *node_offset);
  memcpy(new_node, old_node, sizeof(*old_node));
  return 0;  //FIXME just return new_node?
}


template<unsigned int FIRSTLEVEL_BITS, unsigned int LEVEL_BITS, structmap_is_value_read_cutoff_t CUTOFF>
void
structmap<FIRSTLEVEL_BITS, LEVEL_BITS, CUTOFF>::heighten(struct cb        **cb,
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

template<unsigned int FIRSTLEVEL_BITS, unsigned int LEVEL_BITS, structmap_is_value_read_cutoff_t CUTOFF>
int
structmap<FIRSTLEVEL_BITS, LEVEL_BITS, CUTOFF>::insert(struct cb        **cb,
                                                       struct cb_region  *region,
                                                       uint64_t           key,
                                                       uint64_t           value)
{
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
      ret = node_alloc(cb, region, child_offset);
      assert(ret == 0);
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
    uint64_t test_v;
    bool lookup_success = lookup(*cb, orig_key, &test_v);
    assert(lookup_success);
    assert(test_v == value);
  }
#endif

  return 0;


#if 0
  //BACKED OUT SLOW IMPLEMENTATION
  int ret;

  (void)ret;

  assert(cb_offset_cmp(read_cutoff, write_cutoff) <= 0);
  assert(key > 0);

  // We do not want to have to re-sample pointers, so reserve the maximum amount
  // of space for the maximum amount of nodes we may need for this insertion so
  // that no CB resizes would happen.
  this->ensure_modification_size(cb, region);

  struct structmap_entry *entry = &(this->entries[key & ((1 << FIRSTLEVEL_BITS) - 1)]);
  unsigned int key_route_base = FIRSTLEVEL_BITS;

  while (true) {
    switch (entrytypeof(entry)) {
      case STRUCTMAP_ENTRY_EMPTY:
        entry->key_offset_and_type = ((key << 2) | STRUCTMAP_ENTRY_ITEM);
        entry->value = value;
        this->external_size_adjust((ssize_t)this->sizeof_value(*cb, value));
        goto exit_loop;

      case STRUCTMAP_ENTRY_ITEM: {
        // Replace the value of the key, if the key is already present, or if
        // the mapping is considered below the read cutoff (having a value which
        // fulfills the 'is_value_read_cutoff' predicate.
        if (entrykeyof(entry) == key || CUTOFF(read_cutoff, entry->value)) {
          this->external_size_adjust((ssize_t)this->sizeof_value(*cb, value));
          entry->key_offset_and_type = ((key << 2) | STRUCTMAP_ENTRY_ITEM);
          entry->value = value;
          goto exit_loop;
        }

        // Otherwise, there is a collision in this slot at this level, create
        // a child node and add the old_key/old_value to it.
        cb_offset_t child_node_offset = CB_NULL; //FIXME shouldn't have to initialize
        ret = this->node_alloc(cb, region, &child_node_offset);
        assert(ret == 0);
        node *child_node = (node *)cb_at(*cb, child_node_offset);
        unsigned int child_route = (entrykeyof(entry) >> key_route_base) & ((1 << LEVEL_BITS) - 1);
        struct structmap_entry *child_entry = &(child_node->entries[child_route]);
        child_entry->key_offset_and_type = ((entrykeyof(entry) << 2) | STRUCTMAP_ENTRY_ITEM);
        child_entry->value = entry->value;

        // Make the old location of the key/value now point to the nested child node.
        entry->key_offset_and_type = (child_node_offset | STRUCTMAP_ENTRY_NODE);  // Rely on alignment to have 2 LSB 0 bits.
      }
      /* FALLTHROUGH to process addition of key/value to the new STRUCTMAP_ENTRY_NODE */
      /* fall through */

      case STRUCTMAP_ENTRY_NODE: {
        //Pretend entries which are actually below the read_cutoff do not exist.
        if (cb_offset_cmp(entryoffsetof(entry), read_cutoff) == -1) {
          entry->key_offset_and_type = STRUCTMAP_ENTRY_EMPTY;
          continue;
        }
        cb_offset_t modifiable_node_offset = entryoffsetof(entry);
        this->select_modifiable_node(cb, region, write_cutoff, &modifiable_node_offset);
        entry->key_offset_and_type = (modifiable_node_offset | STRUCTMAP_ENTRY_NODE);  // Rely on alignment to have 2 LSB 0 bits.
        node *child_node = (node *)cb_at(*cb, entryoffsetof(entry));
        unsigned int child_route = (key >> key_route_base) & ((1 << LEVEL_BITS) - 1);
        entry = &(child_node->entries[child_route]);
        key_route_base += LEVEL_BITS;
      }
      break;

#ifndef NDEBUG
      default:
        printf("Bogus structmap entry type: %d\n", entrytypeof(entry));
        assert(false);
        goto exit_loop;
#endif
    }
  }

exit_loop:
  if (this->lowest_inserted_key == 0 || key < this->lowest_inserted_key)
    this->lowest_inserted_key = key;

  if (key > this->highest_inserted_key)
    this->highest_inserted_key = key;

#ifndef NDEBUG
  {
    uint64_t test_v;
    bool lookup_success = this->lookup(*cb, write_cutoff, key, &test_v);
    //printf("lookup_success? %d, same? %d:  #%ju -> @%ju\n", lookup_success, test_v == value, (uintmax_t)key, (uintmax_t)value);
    assert(lookup_success);
    assert(test_v == value);
  }
#endif

  return 0;
#endif
}

#if 0
template<unsigned int FIRSTLEVEL_BITS, unsigned int LEVEL_BITS, structmap_is_value_read_cutoff_t CUTOFF>
bool
structmap<FIRSTLEVEL_BITS, LEVEL_BITS, CUTOFF>::lookup_slowpath(const struct cb *cb,
                                                                cb_offset_t      read_cutoff,
                                                                uint64_t         key,
                                                                uint64_t        *value) const
{
  const struct structmap_entry *entry = &(this->entries[key & ((1 << FIRSTLEVEL_BITS) - 1)]);
  unsigned int key_route_base = FIRSTLEVEL_BITS;

  //FIXME consider cb_at_immed().

  while (entrytypeof(entry) == STRUCTMAP_ENTRY_NODE) {
      if (entryoffsetof(entry) != CB_NULL && cb_offset_cmp(entryoffsetof(entry), read_cutoff) < 0) { return false; }
      const node *child_node = (node *)cb_at(cb, entryoffsetof(entry));
      unsigned int child_route = (key >> key_route_base) & ((1 << LEVEL_BITS) - 1);
      entry = &(child_node->entries[child_route]);
      key_route_base += LEVEL_BITS;
  }

  if (entrytypeof(entry) == STRUCTMAP_ENTRY_ITEM
      && key == entrykeyof(entry)
      && !CUTOFF(read_cutoff, entry->value)) {
    *value = entry->value;
    return true;
  }

  return false;
}


template<unsigned int FIRSTLEVEL_BITS, unsigned int LEVEL_BITS, structmap_is_value_read_cutoff_t CUTOFF>
unsigned int
structmap<FIRSTLEVEL_BITS, LEVEL_BITS, CUTOFF>::would_collide_node_count_slowpath(const struct cb *cb,
                                                                                  cb_offset_t      read_cutoff,
                                                                                  uint64_t         key) const
{
  //NOTE: The purpose of this function is to determine how many nodes would need
  // to additionally be created for the target structmap 'sm' if key 'key' were
  // to be inserted.  It is used when mutating the A layer to check for the
  // additional size needing to be allocated for future consolidation merge of
  // the A layer keys down with the keys of the B and C layers.
  // For this to work, all of the A, B, and C layers must use the same slot
  // layouts (once this becomes dynamic in the future).

  assert(key > 0);

  const struct structmap_entry *entry = &(this->entries[key & ((1 << FIRSTLEVEL_BITS) - 1)]);
  unsigned int key_route_base = FIRSTLEVEL_BITS;

  while (entrytypeof(entry) == STRUCTMAP_ENTRY_NODE) {
      if (entryoffsetof(entry) != CB_NULL && cb_offset_cmp(entryoffsetof(entry), read_cutoff) < 0) { return 0; }
      const node *child_node = (node *)cb_at(cb, entryoffsetof(entry));
      unsigned int child_route = (key >> key_route_base) & ((1 << LEVEL_BITS) - 1);
      entry = &(child_node->entries[child_route]);
      key_route_base += LEVEL_BITS;
  }
  assert(entrytypeof(entry) == STRUCTMAP_ENTRY_EMPTY || entrytypeof(entry) == STRUCTMAP_ENTRY_ITEM);

  if (entrytypeof(entry) == STRUCTMAP_ENTRY_EMPTY)
    return 0;

  assert(entrytypeof(entry) == STRUCTMAP_ENTRY_ITEM);
  if (entrykeyof(entry) == key || CUTOFF(read_cutoff, entry->value)) {
    // The key is already present, or the mapping is considered below the
    // read_cutoff (having a value which fulfills the 'is_value_read_cutoff'
    // predicate.  This is equivalent to a STRUCTMAP_ENTRY_EMPTY empty slot.
    return 0;
  }

  // Otherwise, there is a collision in this slot at this level.  Figure
  // out how many nodes would need to be created to reach the point of
  // slot independence for the existing key and the key being evaluated.
  unsigned int addl_nodes = 1;
  unsigned int existing_key_child_slot = (entrykeyof(entry) >> key_route_base) & ((1 << LEVEL_BITS) - 1);
  unsigned int key_child_slot = (key >> key_route_base) & ((1 << LEVEL_BITS) - 1);

  while (key_child_slot == existing_key_child_slot) {
    key_route_base += LEVEL_BITS;
    ++addl_nodes;
    existing_key_child_slot = (entrykeyof(entry) >> key_route_base) & ((1 << LEVEL_BITS) - 1);
    key_child_slot = (key >> key_route_base) & ((1 << LEVEL_BITS) - 1);
  }

  return addl_nodes;
}
#endif

template<unsigned int FIRSTLEVEL_BITS, unsigned int LEVEL_BITS, structmap_is_value_read_cutoff_t CUTOFF>
int
structmap<FIRSTLEVEL_BITS, LEVEL_BITS, CUTOFF>::traverse(const struct cb           **cb,
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

#endif  //klox_structmap_h
