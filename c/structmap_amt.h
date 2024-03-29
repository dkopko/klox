#ifndef klox_structmap_amt_h
#define klox_structmap_amt_h

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

extern __thread struct cb_at_immed_param_t thread_cb_at_immed_param;
extern __thread bool       on_main_thread;

//NOTES:
// 1) Neither keys nor values are allowed to be 0, as this value is reserved
//    for NULL-like sentinels.  FIXME is this still true after the revision toward the Bagwell-like implementation?

typedef size_t (*structmap_value_size_t)(const struct cb *cb, uint64_t v);
typedef int (*structmap_traverse_func_t)(uint64_t key, uint64_t value, void *closure);
typedef int (*structmap_value_cmp_func_t)(uint64_t lhsvalue, uint64_t rhsvalue);


enum structmap_amt_entry_type
{
  //In debug modes, require non-zero enum values to distinguish cases of uninitialized memory.
  //In release modes, use 0x0 for the node enum for micro-optimization of branches.
#ifndef NDEBUG
  STRUCTMAP_AMT_ENTRY_NODE  = 0x1,
  STRUCTMAP_AMT_ENTRY_EMPTY = 0x2,
  STRUCTMAP_AMT_ENTRY_ITEM  = 0x3
#else
  STRUCTMAP_AMT_ENTRY_NODE  = 0x0,
  STRUCTMAP_AMT_ENTRY_EMPTY = 0x1,
  STRUCTMAP_AMT_ENTRY_ITEM  = 0x2
#endif
};

static const unsigned int STRUCTMAP_AMT_TYPEMASK = 0x3;

struct structmap_amt_entry
{
  uint64_t key_offset_and_type;
  uint64_t value;
};

extern inline structmap_amt_entry_type
entrytypeof(const structmap_amt_entry *entry) {
  return static_cast<structmap_amt_entry_type>(entry->key_offset_and_type & STRUCTMAP_AMT_TYPEMASK);
}

extern inline uint64_t
entrykeyof(const structmap_amt_entry *entry) {
  return (entry->key_offset_and_type >> 2);
}

extern inline uint64_t
entryoffsetof(const structmap_amt_entry *entry) {
  // Rely on alignment to have 2 LSB 0 bits for offsets.
  return ((entry->key_offset_and_type >> 2) << 2);  //shift out the type tag
}


template<unsigned int FIRSTLEVEL_BITS, unsigned int LEVEL_BITS>
struct structmap_amt
{
  cb_offset_t            root_node_offset;
  unsigned int           node_count_;
  size_t                 total_external_size;
  structmap_value_size_t sizeof_value;
  struct structmap_amt_entry entries[1 << FIRSTLEVEL_BITS];

  struct node
  {
      struct structmap_amt_entry entries[1 << LEVEL_BITS];
  } __attribute__ ((aligned (64)));

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

  unsigned int
  would_collide_node_count_slowpath(const struct cb *cb,
                                    uint64_t         key) const;

  int
  traverse_node(const struct cb           **cb,
                structmap_traverse_func_t   func,
                void                       *closure,
                const node                 *node) const;

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

  size_t
  size() const
  {
    return this->internal_size() + this->external_size();
  }

  void
  validate() const
  {
    assert(sizeof_value);

    for (int i = 0; i < (1 << FIRSTLEVEL_BITS); i++) {
      const struct structmap_amt_entry *entry = &(this->entries[i]);
      (void)entry;
      assert(entrytypeof(entry) == STRUCTMAP_AMT_ENTRY_NODE || entrytypeof(entry) == STRUCTMAP_AMT_ENTRY_EMPTY || entrytypeof(entry) == STRUCTMAP_AMT_ENTRY_ITEM);
    }
  }

  bool
  lookup(const struct cb *cb,
         uint64_t         key,
         uint64_t        *value) const
  {
    const struct structmap_amt_entry *entry = &(this->entries[key & ((1 << FIRSTLEVEL_BITS) - 1)]);

    assert(entrytypeof(entry) == STRUCTMAP_AMT_ENTRY_NODE || entrytypeof(entry) == STRUCTMAP_AMT_ENTRY_EMPTY || entrytypeof(entry) == STRUCTMAP_AMT_ENTRY_ITEM);
    if (entry->key_offset_and_type == ((key << 2) | STRUCTMAP_AMT_ENTRY_ITEM)) {
      *value = entry->value;
      return true;
    }

    unsigned int key_route_base = FIRSTLEVEL_BITS;
    while (entrytypeof(entry) == STRUCTMAP_AMT_ENTRY_NODE) {
        const node *child_node = (node *)cb_at_immed(&thread_cb_at_immed_param, entryoffsetof(entry));
        unsigned int child_route = (key >> key_route_base) & ((1 << LEVEL_BITS) - 1);
        entry = &(child_node->entries[child_route]);
        assert(entrytypeof(entry) == STRUCTMAP_AMT_ENTRY_NODE || entrytypeof(entry) == STRUCTMAP_AMT_ENTRY_EMPTY || entrytypeof(entry) == STRUCTMAP_AMT_ENTRY_ITEM);
        key_route_base += LEVEL_BITS;
    }

    assert(entrytypeof(entry) == STRUCTMAP_AMT_ENTRY_EMPTY || entrytypeof(entry) == STRUCTMAP_AMT_ENTRY_ITEM);
    if (entry->key_offset_and_type == ((key << 2) | STRUCTMAP_AMT_ENTRY_ITEM)) {
      *value = entry->value;
      return true;
    }

    return false;
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
    const struct structmap_amt_entry *entry = &(this->entries[key & ((1 << FIRSTLEVEL_BITS) - 1)]);

    if (entrytypeof(entry) == STRUCTMAP_AMT_ENTRY_EMPTY
        || (entrytypeof(entry) == STRUCTMAP_AMT_ENTRY_ITEM && entrykeyof(entry) == key)) {
      return 0;
    }

    return this->would_collide_node_count_slowpath(cb, key);
  }

  static int
  compare_node(const structmap_amt<FIRSTLEVEL_BITS, LEVEL_BITS>::node *lhs,
               const structmap_amt<FIRSTLEVEL_BITS, LEVEL_BITS>::node *rhs,
               structmap_value_cmp_func_t                              value_cmp);
  int
  compare(const structmap_amt<FIRSTLEVEL_BITS, LEVEL_BITS> &rhs,
          structmap_value_cmp_func_t                        value_cmp) const;
};

template<unsigned int FIRSTLEVEL_BITS, unsigned int LEVEL_BITS>
void
structmap_amt<FIRSTLEVEL_BITS, LEVEL_BITS>::init(structmap_value_size_t sizeof_value)
{
  this->root_node_offset = CB_NULL;
  this->node_count_ = 0;
  this->total_external_size = 0;
  this->sizeof_value = sizeof_value;

  for (int i = 0; i < (1 << FIRSTLEVEL_BITS); ++i) {
    this->entries[i].key_offset_and_type = STRUCTMAP_AMT_ENTRY_EMPTY;
    this->entries[i].value = 0;
  }
}

template<unsigned int FIRSTLEVEL_BITS, unsigned int LEVEL_BITS>
int
structmap_amt<FIRSTLEVEL_BITS, LEVEL_BITS>::node_alloc(struct cb        **cb,
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
        sn->entries[i].key_offset_and_type = STRUCTMAP_AMT_ENTRY_EMPTY;
      }
    }

    ++(this->node_count_);

    *node_offset = new_node_offset;

    return 0;
}

template<unsigned int FIRSTLEVEL_BITS, unsigned int LEVEL_BITS>
void
structmap_amt<FIRSTLEVEL_BITS, LEVEL_BITS>::ensure_modification_size(struct cb        **cb,
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
int
structmap_amt<FIRSTLEVEL_BITS, LEVEL_BITS>::insert(struct cb        **cb,
                                                   struct cb_region  *region,
                                                   uint64_t           key,
                                                   uint64_t           value)
{
  DEBUG_ONLY(unsigned int pre_node_count = this->node_count_);
  int ret;

  (void)ret;

  assert(key > 0);

  // We do not want to have to re-sample pointers, so reserve the maximum amount
  // of space for the maximum amount of nodes we may need for this insertion so
  // that no CB resizes would happen.
  this->ensure_modification_size(cb, region);

  struct structmap_amt_entry *entry = &(this->entries[key & ((1 << FIRSTLEVEL_BITS) - 1)]);
  unsigned int key_route_base = FIRSTLEVEL_BITS;

  while (true) {
    switch (entrytypeof(entry)) {
      case STRUCTMAP_AMT_ENTRY_EMPTY:
        entry->key_offset_and_type = ((key << 2) | STRUCTMAP_AMT_ENTRY_ITEM);
        entry->value = value;
        this->external_size_adjust((ssize_t)this->sizeof_value(*cb, value));
        goto exit_loop;

      case STRUCTMAP_AMT_ENTRY_ITEM: {
        // Replace the value of the key, if the key is already present, or if
        // the mapping is considered below the read cutoff (having a value which
        // fulfills the 'is_value_read_cutoff' predicate.
        if (entrykeyof(entry) == key) {
          this->external_size_adjust((ssize_t)this->sizeof_value(*cb, value));
          entry->key_offset_and_type = ((key << 2) | STRUCTMAP_AMT_ENTRY_ITEM);
          entry->value = value;
          goto exit_loop;
        }

        // Otherwise, there is a collision in this slot at this level, create
        // a child node and add the old_key/old_value to it.
        cb_offset_t child_node_offset = CB_NULL; //FIXME shouldn't have to initialize
        DEBUG_ONLY(struct cb *old_cb = *cb);
        ret = this->node_alloc(cb, region, &child_node_offset);
        assert(ret == 0);
        DEBUG_ONLY(struct cb *new_cb = *cb);
        assert(old_cb == new_cb);
        node *child_node = (node *)cb_at(*cb, child_node_offset);
        unsigned int child_route = (entrykeyof(entry) >> key_route_base) & ((1 << LEVEL_BITS) - 1);
        struct structmap_amt_entry *child_entry = &(child_node->entries[child_route]);
        child_entry->key_offset_and_type = ((entrykeyof(entry) << 2) | STRUCTMAP_AMT_ENTRY_ITEM);
        child_entry->value = entry->value;

        // Make the old location of the key/value now point to the nested child node.
        entry->key_offset_and_type = (child_node_offset | STRUCTMAP_AMT_ENTRY_NODE);  // Rely on alignment to have 2 LSB 0 bits.
      }
      /* FALLTHROUGH to process addition of key/value to the new STRUCTMAP_AMT_ENTRY_NODE */
      /* fall through */

      case STRUCTMAP_AMT_ENTRY_NODE: {
        entry->key_offset_and_type = (entryoffsetof(entry) | STRUCTMAP_AMT_ENTRY_NODE);  // Rely on alignment to have 2 LSB 0 bits.
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
#ifndef NDEBUG
  {
    unsigned int post_node_count = this->node_count_;

    assert(post_node_count >= pre_node_count);
    assert(post_node_count - pre_node_count <= MODIFICATION_MAX_NODES);

    uint64_t test_v;
    bool lookup_success = this->lookup(*cb, key, &test_v);
    //printf("lookup_success? %d, same? %d:  #%ju -> @%ju\n", lookup_success, test_v == value, (uintmax_t)key, (uintmax_t)value);
    assert(lookup_success);
    assert(test_v == value);
  }
#endif

  return 0;
}

template<unsigned int FIRSTLEVEL_BITS, unsigned int LEVEL_BITS>
unsigned int
structmap_amt<FIRSTLEVEL_BITS, LEVEL_BITS>::would_collide_node_count_slowpath(const struct cb *cb,
                                                                              uint64_t         key) const
{
  //NOTE: The purpose of this function is to determine how many nodes would need
  // to additionally be created for the target structmap 'sm' if key 'key' were
  // to be inserted.  It is used when mutating the A layer to check for the
  // additional size needing to be allocated for future consolidation merge of
  // the A layer keys down with the keys of the B and C layers. For this to
  // work, all of the A, B, and C layers must use the same slot layouts.

  assert(key > 0);

  const struct structmap_amt_entry *entry = &(this->entries[key & ((1 << FIRSTLEVEL_BITS) - 1)]);
  unsigned int key_route_base = FIRSTLEVEL_BITS;

  while (entrytypeof(entry) == STRUCTMAP_AMT_ENTRY_NODE) {
      const node *child_node = (node *)cb_at(cb, entryoffsetof(entry));
      unsigned int child_route = (key >> key_route_base) & ((1 << LEVEL_BITS) - 1);
      entry = &(child_node->entries[child_route]);
      key_route_base += LEVEL_BITS;
  }
  assert(entrytypeof(entry) == STRUCTMAP_AMT_ENTRY_EMPTY || entrytypeof(entry) == STRUCTMAP_AMT_ENTRY_ITEM);

  if (entrytypeof(entry) == STRUCTMAP_AMT_ENTRY_EMPTY)
    return 0;

  assert(entrytypeof(entry) == STRUCTMAP_AMT_ENTRY_ITEM);
  if (entrykeyof(entry) == key) {
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

  assert(addl_nodes <= MODIFICATION_MAX_NODES);
  return addl_nodes;
}

template<unsigned int FIRSTLEVEL_BITS, unsigned int LEVEL_BITS>
int
structmap_amt<FIRSTLEVEL_BITS, LEVEL_BITS>::traverse_node(const struct cb           **cb,
                                                          structmap_traverse_func_t   func,
                                                          void                       *closure,
                                                          const node                 *n) const
{
  for (unsigned int i = 0; i < (1 << LEVEL_BITS); ++i) {
    const struct structmap_amt_entry *entry = &(n->entries[i]);
    switch (entrytypeof(entry)) {
      case STRUCTMAP_AMT_ENTRY_NODE: {
        const node *child_node = (node *)cb_at_immed(&thread_cb_at_immed_param, entryoffsetof(entry));
        traverse_node(cb, func, closure, child_node);
        break;
      }

      case STRUCTMAP_AMT_ENTRY_EMPTY:
        continue;

      case STRUCTMAP_AMT_ENTRY_ITEM:
        func(entrykeyof(entry), entry->value, closure);
        continue;

#ifndef NDEBUG
      default:
        printf("Bogus structmap entry type: %d\n", entrytypeof(entry));
        assert(false);
#endif
    }
  }

  return 0;
}

template<unsigned int FIRSTLEVEL_BITS, unsigned int LEVEL_BITS>
int
structmap_amt<FIRSTLEVEL_BITS, LEVEL_BITS>::traverse(const struct cb           **cb,
                                                     structmap_traverse_func_t   func,
                                                     void                       *closure) const
{
  for (unsigned int i = 0; i < (1 << FIRSTLEVEL_BITS); ++i) {
    const struct structmap_amt_entry *entry = &(this->entries[i]);
    switch (entrytypeof(entry)) {
      case STRUCTMAP_AMT_ENTRY_NODE: {
        const node *child_node = (node *)cb_at_immed(&thread_cb_at_immed_param, entryoffsetof(entry));
        traverse_node(cb, func, closure, child_node);
        break;
      }

      case STRUCTMAP_AMT_ENTRY_EMPTY:
        continue;

      case STRUCTMAP_AMT_ENTRY_ITEM:
        func(entrykeyof(entry), entry->value, closure);
        continue;

#ifndef NDEBUG
      default:
        printf("Bogus structmap entry type: %d\n", entrytypeof(entry));
        assert(false);
#endif
    }
  }

  return 0;
}

template<unsigned int FIRSTLEVEL_BITS, unsigned int LEVEL_BITS>
int
structmap_amt<FIRSTLEVEL_BITS, LEVEL_BITS>::compare_node(const structmap_amt<FIRSTLEVEL_BITS, LEVEL_BITS>::node *lhs,
                                                         const structmap_amt<FIRSTLEVEL_BITS, LEVEL_BITS>::node *rhs,
                                                         structmap_value_cmp_func_t                              value_cmp)
{
  for (unsigned int i = 0; i < (1 << LEVEL_BITS); ++i) {
    const struct structmap_amt_entry *lentry = &(lhs->entries[i]);
    const struct structmap_amt_entry *rentry = &(rhs->entries[i]);

    enum structmap_amt_entry_type ltype = entrytypeof(lentry);
    enum structmap_amt_entry_type rtype = entrytypeof(rentry);

    if (ltype < rtype) return -1;
    if (ltype > rtype) return 1;

    switch (ltype) {
      case STRUCTMAP_AMT_ENTRY_NODE: {
        const node *lnode = (node *)cb_at_immed(&thread_cb_at_immed_param, entryoffsetof(lentry));
        const node *rnode = (node *)cb_at_immed(&thread_cb_at_immed_param, entryoffsetof(rentry));

        int cmp = compare_node(lnode, rnode, value_cmp);
        if (cmp < 0) return -1;
        if (cmp > 0) return 1;

        continue;
      }

      case STRUCTMAP_AMT_ENTRY_EMPTY:
        continue;

      case STRUCTMAP_AMT_ENTRY_ITEM: {
        if (entrykeyof(lentry) < entrykeyof(rentry)) return -1;
        if (entrykeyof(lentry) > entrykeyof(rentry)) return 1;

        int cmp = value_cmp(lentry->value, rentry->value);
        if (cmp < 0) return -1;
        if (cmp > 0) return 1;

        continue;
      }

#ifndef NDEBUG
      default:
        printf("Bogus structmap entry type: %d\n", entrytypeof(lentry));
        assert(false);
#endif
    }
  }

  return 0;
}

template<unsigned int FIRSTLEVEL_BITS, unsigned int LEVEL_BITS>
int
structmap_amt<FIRSTLEVEL_BITS, LEVEL_BITS>::compare(const structmap_amt<FIRSTLEVEL_BITS, LEVEL_BITS> &rhs,
                                                    structmap_value_cmp_func_t                        value_cmp) const
{
  for (unsigned int i = 0; i < (1 << FIRSTLEVEL_BITS); ++i) {
    const struct structmap_amt_entry *lentry = &(this->entries[i]);
    const struct structmap_amt_entry *rentry = &(rhs.entries[i]);

    enum structmap_amt_entry_type ltype = entrytypeof(lentry);
    enum structmap_amt_entry_type rtype = entrytypeof(rentry);

    if (ltype < rtype) return -1;
    if (ltype > rtype) return 1;

    switch (ltype) {
      case STRUCTMAP_AMT_ENTRY_NODE: {
        const node *lnode = (node *)cb_at_immed(&thread_cb_at_immed_param, entryoffsetof(lentry));
        const node *rnode = (node *)cb_at_immed(&thread_cb_at_immed_param, entryoffsetof(rentry));

        int cmp = compare_node(lnode, rnode, value_cmp);
        if (cmp < 0) return -1;
        if (cmp > 0) return 1;

        continue;
      }

      case STRUCTMAP_AMT_ENTRY_EMPTY:
        continue;

      case STRUCTMAP_AMT_ENTRY_ITEM: {
        if (entrykeyof(lentry) < entrykeyof(rentry)) return -1;
        if (entrykeyof(lentry) > entrykeyof(rentry)) return 1;

        int cmp = value_cmp(lentry->value, rentry->value);
        if (cmp < 0) return -1;
        if (cmp > 0) return 1;

        continue;
      }

#ifndef NDEBUG
      default:
        printf("Bogus structmap entry type: %d\n", entrytypeof(lentry));
        assert(false);
#endif
    }
  }

  return 0;
}

#endif  //klox_structmap_amt_h
