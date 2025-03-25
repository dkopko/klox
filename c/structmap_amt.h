// This is a forward-include wrapper to the implementation now housed in cb
#ifndef klox_structmap_amt_h
#define klox_structmap_amt_h

#include <cb_structmap_amt.h>

// Keep backward compatibility for klox codebase
using structmap_amt_entry_type = cb_structmap_amt_entry_type;
using structmap_value_size_t = cb_structmap_amt_value_size_t;
using structmap_traverse_func_t = cb_structmap_amt_traverse_func_t;
using structmap_value_cmp_func_t = cb_structmap_amt_value_cmp_func_t;

// Define constants for backward compatibility
static const auto STRUCTMAP_AMT_ENTRY_NODE = CB_STRUCTMAP_AMT_ENTRY_NODE;
static const auto STRUCTMAP_AMT_ENTRY_EMPTY = CB_STRUCTMAP_AMT_ENTRY_EMPTY;
static const auto STRUCTMAP_AMT_ENTRY_ITEM = CB_STRUCTMAP_AMT_ENTRY_ITEM;

// Forward the structmap_amt_entry type
using structmap_amt_entry = cb_structmap_amt_entry;

// Forward the inline helper functions
inline structmap_amt_entry_type
entrytypeof(const structmap_amt_entry *entry) {
  return cb_entrytypeof(entry);
}

inline uint64_t
entrykeyof(const structmap_amt_entry *entry) {
  return cb_entrykeyof(entry);
}

inline uint64_t
entryoffsetof(const structmap_amt_entry *entry) {
  return cb_entryoffsetof(entry);
}

// Forward the structmap_amt template
template<unsigned int FIRSTLEVEL_BITS, unsigned int LEVEL_BITS>
using structmap_amt = cb_structmap_amt<FIRSTLEVEL_BITS, LEVEL_BITS>;

#endif  //klox_structmap_amt_h
