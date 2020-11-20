#ifndef klox_table_h
#define klox_table_h

#include "cb_integration.h"
#include "common.h"
#include "value.h"

typedef struct {
  cb_offset_t root_a;
  cb_offset_t root_b;
  cb_offset_t root_c;
} Table;

void initTable(Table                *table,
               cb_term_comparator_t  term_cmp,
               cb_term_render_t      term_render);

void freeTable(Table *table);

bool tableGet(const Table *table, Value key, Value *value);

bool tableSet(Table *table, Value key, Value value);

bool tableDelete(Table *table, Value key);

void tableAddAll(const Table *from, Table *to);

OID<ObjString> tableFindString(Table      *table,
                               cb_offset_t offset,
                               const char *chars,
                               int         length,
                               uint32_t    hash);

void grayTable(Table* table);

void printTable(Table* table, const char *desc);

#endif
