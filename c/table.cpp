#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "memory.h"
#include "object.h"
#include "table.h"
#include "value.h"
#include "cb_bst.h"

void
initTable(Table                *table,
          cb_term_comparator_t  term_cmp,
          cb_term_render_t      term_render)
{
  int ret;

  (void)ret;

  ret = cb_bst_init(&thread_cb,
                    &thread_region,
                    &(table->root_a),
                    term_cmp,
                    term_cmp,
                    term_render,
                    term_render,
                    &klox_no_external_size,
                    &klox_no_external_size);
  assert(ret == 0);

  ret = cb_bst_init(&thread_cb,
                    &thread_region,
                    &(table->root_b),
                    term_cmp,
                    term_cmp,
                    term_render,
                    term_render,
                    &klox_no_external_size,
                    &klox_no_external_size);
  assert(ret == 0);

  ret = cb_bst_init(&thread_cb,
                    &thread_region,
                    &(table->root_c),
                    term_cmp,
                    term_cmp,
                    term_render,
                    term_render,
                    &klox_no_external_size,
                    &klox_no_external_size);
  assert(ret == 0);

}

void
freeTable(Table* table)
{
  //CBINT Redundant
  (void)table;
}

bool
tableGet(const Table *table,
         Value        key,
         Value       *value)
{
  struct cb_term key_term;
  struct cb_term value_term;
  int ret;

  cb_term_set_dbl(&key_term, valueToNum(key));

  ret = cb_bst_lookup(thread_cb, table->root_a, &key_term, &value_term);
  if (ret == 0) goto done;
  ret = cb_bst_lookup(thread_cb, table->root_b, &key_term, &value_term);
  if (ret == 0) goto done;
  ret = cb_bst_lookup(thread_cb, table->root_c, &key_term, &value_term);

done:
  if (ret != 0 || numToValue(cb_term_get_dbl(&value_term)).val == TOMBSTONE_VAL.val) {
    return false;
  }

  *value = numToValue(cb_term_get_dbl(&value_term));
  return true;
}

bool
tableSet(Table* table, Value key, Value value)
{
  struct cb_term key_term;
  struct cb_term value_term;
  Value temp_value;
  bool already_exists;
  int ret;

  cb_term_set_dbl(&key_term, valueToNum(key));
  cb_term_set_dbl(&value_term, valueToNum(value));

  //CBINT FIXME would be nice to avoid this lookup by leveraging
  // cb_bst_insert()'s lookup.
  already_exists = tableGet(table, key, &temp_value);

  ret = cb_bst_insert(&thread_cb,
                      &thread_region,
                      &(table->root_a),
                      a_write_cutoff,
                      &key_term,
                      &value_term);
  assert(ret == 0);

  //true if new key and we succeeded at inserting it.
  return (!already_exists && ret == 0);
}

bool
tableDelete(Table *table,
            Value  key)
{
  struct cb_term key_term;
  struct cb_term value_term;
  int ret;

  cb_term_set_dbl(&key_term, valueToNum(key));
  cb_term_set_dbl(&value_term, valueToNum(TOMBSTONE_VAL));

  ret = cb_bst_insert(&thread_cb,
                      &thread_region,
                      &(table->root_a),
                      a_write_cutoff,
                      &key_term,
                      &value_term);
  return (ret == 0);
}

struct TraversalAddClosure
{
  const Table *src;
  Table       *dest;
};

static int
traversalAdd(const struct cb_term *key_term,
             const struct cb_term *value_term,
             void                 *closure)
{
  TraversalAddClosure *taclosure = (TraversalAddClosure *)closure;

  Value key = numToValue(cb_term_get_dbl(key_term));
  Value value = numToValue(cb_term_get_dbl(value_term));
  Value tempValue;

  //CBINT FIXME: This need not check all BSTs in the tritable for all
  //  traversals, but I don't want to write the specializations now.
  //need to traverse C, inserting those not tombstone in A or B or C (C can't happen).
  //need to traverse B, inserting those not tombstone in A or B.
  //need to traverse A, inserting those not tombstone in A.
  if (tableGet(taclosure->src, key, &tempValue)) {
    tableSet(taclosure->dest, key, value);
  }

  return 0;
}

void
tableAddAll(const Table *from,
            Table       *to)
{
  TraversalAddClosure taclosure = { from, to };
  int ret;

  ret = cb_bst_traverse(thread_cb,
                        from->root_c,
                        &traversalAdd,
                        &taclosure);
  assert(ret == 0);

  ret = cb_bst_traverse(thread_cb,
                        from->root_b,
                        &traversalAdd,
                        &taclosure);
  assert(ret == 0);

  ret = cb_bst_traverse(thread_cb,
                        from->root_a,
                        &traversalAdd,
                        &taclosure);
  assert(ret == 0);
  (void)ret;
}

OID<ObjString>
tableFindString(Table      *table,
                cb_offset_t offset,
                const char *chars,
                int         length,
                uint32_t    hash)
{
#if KLOX_TRACE_ENABLED
  if (offset == CB_NULL) {
    KLOX_TRACE("table:%p chars:%.*s@RAW\n", table, length, chars);
  } else {
    KLOX_TRACE("table:%p chars:%.*s@%ju\n", table, length, chars, (uintmax_t)offset);
  }
#endif //KLOX_TRACE_ENABLED

  //NOTE: If lookup of the temporary string fails, a rewinding to undo the
  // useless allocations would entail:
  //   1) preserving the fields of pre-allocations region (in case the
  //      allocations caused a new region to be allocated),
  //   2) preserving the cb's cursor,
  //   3) restoring both of these to their pre-allocations state.
  // Ultimately, this seems like unnecessarily heavy work, so we just let the
  // normal garbage collection path reclaim it.
  OID<ObjString> lookupStringOID = rawAllocateString(chars, length);
  Value lookupStringValue = OBJ_VAL(lookupStringOID.id());
  Value internedStringValue;
  struct cb_term key_term;
  struct cb_term value_term;
  int ret;

  cb_term_set_dbl(&key_term, valueToNum(lookupStringValue));

  ret = cb_bst_lookup(thread_cb, table->root_a, &key_term, &value_term);
  if (ret == 0) goto done;
  ret = cb_bst_lookup(thread_cb, table->root_b, &key_term, &value_term);
  if (ret == 0) goto done;
  ret = cb_bst_lookup(thread_cb, table->root_c, &key_term, &value_term);

done:
  if (ret != 0 || numToValue(cb_term_get_dbl(&value_term)).val == TOMBSTONE_VAL.val) {
    KLOX_TRACE("table:%p tempLookupString:string#%ju@%ju\"%s\" -> NOT FOUND\n",
               table,
               (uintmax_t)lookupStringOID.id().id,
               (uintmax_t)lookupStringOID.clip().cp()->chars.co(),
               lookupStringOID.clip().cp()->chars.clp().cp());
    return CB_NULL_OID;
  }

  internedStringValue = numToValue(cb_term_get_dbl(&value_term));
  KLOX_TRACE("table:%p tempLookupString:string#%ju@%ju\"%s\" -> string#%ju@%ju\"%s\"\n",
             table,
             (uintmax_t)lookupStringOID.id().id,
             (uintmax_t)lookupStringOID.clip().cp()->chars.co(),
             lookupStringOID.clip().cp()->chars.clp().cp(),
             (uintmax_t)AS_OBJ_ID(internedStringValue).id,
             (uintmax_t)((ObjString*)AS_OBJ(internedStringValue))->chars.co(),
             ((ObjString*)AS_OBJ(internedStringValue))->chars.clp().cp());

  return AS_OBJ_ID(internedStringValue);
}

static int
gray_entry(const struct cb_term *key_term,
           const struct cb_term *value_term,
           void                 *closure)
{
  grayValue(numToValue(cb_term_get_dbl(key_term)));
  grayValue(numToValue(cb_term_get_dbl(value_term)));

  return CB_SUCCESS;
}

void
grayTable(Table* table)
{
  // Graying the table means that all keys and values of all entries of this
  // table are marked as "still in use". Everything recursively reachable from
  // the keys and values will also be marked.

  int ret;

  ret = cb_bst_traverse(thread_cb,
                        table->root_a,
                        &gray_entry,
                        NULL);
  assert(ret == 0);

  ret = cb_bst_traverse(thread_cb,
                        table->root_b,
                        &gray_entry,
                        NULL);
  assert(ret == 0);

  ret = cb_bst_traverse(thread_cb,
                        table->root_c,
                        &gray_entry,
                        NULL);
  assert(ret == 0);

  (void)ret;
}

struct printTableClosure
{
  const char *desc0;
  const char *desc1;
};

static int
printTableTraversal(const struct cb_term *key_term,
                    const struct cb_term *value_term,
                    void                 *closure)
{
  struct printTableClosure *clo = (struct printTableClosure *)closure;
  Value keyValue = numToValue(cb_term_get_dbl(key_term));
  Value valueValue = numToValue(cb_term_get_dbl(value_term));

  (void)clo, (void)keyValue, (void)valueValue;

  KLOX_TRACE("%s %s ", clo->desc0, clo->desc1);
  KLOX_TRACE_ONLY(printValue(keyValue, false));
  KLOX_TRACE_(" -> ");
  KLOX_TRACE_ONLY(printValue(valueValue, false));
  KLOX_TRACE_("\n");

  return 0;
}


void
printTable(Table* table, const char *desc)
{
  struct printTableClosure closure;
  int ret;

  closure.desc0 = desc;

  closure.desc1 = "A";
  ret = cb_bst_traverse(thread_cb,
                        table->root_a,
                        &printTableTraversal,
                        &closure);
  assert(ret == 0);

  closure.desc1 = "B";
  ret = cb_bst_traverse(thread_cb,
                        table->root_b,
                        &printTableTraversal,
                        &closure);
  assert(ret == 0);

  closure.desc1 = "C";
  ret = cb_bst_traverse(thread_cb,
                        table->root_c,
                        &printTableTraversal,
                        &closure);
  assert(ret == 0);

  (void)ret;
}

