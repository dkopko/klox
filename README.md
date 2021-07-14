## Summary (TL;DR)
* klox is a fork of the [clox interpreter](http://craftinginterpreters.com "Crafting Interpreters")
  with a proof-of-concept demonstration of O(1) garbage collection.
* The tradeoff:  Allocation and GC at O(1) cost to main thread, but dereference
  costs O(log32(n)) instead of O(1).
* The question:  Does this performance tradeoff make for an
  interesting/desirable language runtime?
* Bonus question:  Could the persistent and partially-persistent data structures
  of this work be leveraged for interesting language constructs (e.g. by-value
  semantics for large recursive structures like maps)?
* This took a lot of time to produce in the hope that it would be an interesting
  concept for a language runtime, so I would very much appreciate your feedback:
  dkopko at runbox.com.

## Using It
1. Build [CB](https://github.com/dkopko/cb "CB Project")
2. 
```sh
$ cd c
$ make -j CBROOT=/your/path/to/cb
$ cd ..
$ ./util/test.py                            # Runs the test suite
$ ./c/BUILD/RelWithDebInfo/klox [filename]  # Runs the REPL, or optionally provided script file
```

## Implementation
* Uses a power-of-2 sized ring for very fast sequential allocation.
  * "Pointers" become offsets into a ring, dereferenced via a bitmasked add to
    the ring's base memory address.
* Objects of the target language (those items directly traversed/marked via the
  garbage collector) use integer enumeration for their handles.
  * Object handles are dereferenced via an O(log32(n)) lookup of
    `handle -> offset`.
* Program memory is tri-partite, consisting of regions A, B, and C within a 
  ring.
  * At all times, regions B and C are read-only, and mutation of structures only
    occurs in region A.
  * Newly created structures are allocated in A.  Old structures in B or C must
    be copied to A before being mutated.  (Copies are of small records, not
    large nor recursive structures.)
  * Dereferencing an object handle checks regions A, B, C (in that order) until
    the desired target is resolved.
* O(1) Garbage Collection process:
  * Main thread executes with 2 active regions.  Regions A and B have useful
    data, region C is empty.
  * Main thread reaches point of execution where GC is invoked.
    * Regions shift:  C = B, B = A, A = new region 
    * Request is sent to GC thread to compact live objects of B and C into
      "newB"
  * Main thread continues execution, temporarily in a mode with 3 active
    regions.  GC thread concurrently performs live object compaction into newB.
  * GC thread completes compaction, responds to main thread with compacted newB
    region.
  * Main thread reaches an instruction which observes and integrates GC
    response.
    * Regions shift:  C = empty, B = newB.
  * Main thread continues execution, again with only 2 active regions.

## Cost
The shift in cost to instructions (measured in rdtsc ticks in my latop) is as follows (see ilat_compare.20210714-014511.out):
```
OP_INVOKE            lat:       75.5 ->      264.7 (+   250.5 %), Runtime%:  17.5 (+  9.7), AbsRuntime:  24593582980 (+   250.5 %)
OP_POP               lat:       25.8 ->       23.2 (-    10.1 %), Runtime%:  12.1 (-  8.9), AbsRuntime:  17053609960 (-    10.1 %)
OP_CONSTANT          lat:       26.7 ->       26.8 (+     0.2 %), Runtime%:  11.3 (-  6.3), AbsRuntime:  15920016520 (+     0.2 %)
OP_GET_PROPERTY      lat:       44.9 ->      101.2 (+   125.4 %), Runtime%:  10.0 (+  3.1), AbsRuntime:  14069303340 (+   125.4 %)
OP_GET_GLOBAL        lat:       30.9 ->       96.3 (+   211.8 %), Runtime%:   8.2 (+  4.1), AbsRuntime:  11485070980 (+   211.8 %)
OP_SET_GLOBAL        lat:       42.5 ->      348.2 (+   718.9 %), Runtime%:   6.1 (+  4.9), AbsRuntime:   8603748740 (+   718.9 %)
OP_EQUAL             lat:       26.7 ->       35.5 (+    33.0 %), Runtime%:   6.0 (-  1.1), AbsRuntime:   8486099980 (+    33.0 %)
OP_RETURN            lat:       27.2 ->       53.9 (+    98.3 %), Runtime%:   4.8 (+  1.0), AbsRuntime:   6733731096 (+    98.3 %)
OP_GET_LOCAL         lat:       25.1 ->       24.4 (-     2.7 %), Runtime%:   3.9 (-  2.4), AbsRuntime:   5490038904 (-     2.7 %)
OP_TRUE              lat:       28.5 ->       28.5 (+     0.1 %), Runtime%:   3.6 (-  2.0), AbsRuntime:   5132829560 (+     0.1 %)
OP_NIL               lat:       29.3 ->       28.5 (-     2.7 %), Runtime%:   3.5 (-  2.1), AbsRuntime:   4988193700 (-     2.7 %)
OP_CALL              lat:       51.5 ->      139.5 (+   171.0 %), Runtime%:   3.1 (+  1.3), AbsRuntime:   4414534240 (+   171.0 %)
OP_ADD               lat:       28.7 ->       43.7 (+    52.3 %), Runtime%:   3.0 (-  0.1), AbsRuntime:   4203289480 (+    52.3 %)
OP_JUMP_IF_FALSE     lat:       26.6 ->       34.1 (+    28.3 %), Runtime%:   2.5 (-  0.5), AbsRuntime:   3522163660 (+    28.3 %)
OP_LESS              lat:       27.3 ->       41.2 (+    51.0 %), Runtime%:   1.6 (-  0.1), AbsRuntime:   2216343500 (+    51.0 %)
OP_SUBTRACT          lat:       23.1 ->       38.8 (+    68.0 %), Runtime%:   0.8 (+  0.1), AbsRuntime:   1182540060 (+    68.0 %)
OP_SET_PROPERTY      lat:      222.1 ->      268.9 (+    21.1 %), Runtime%:   0.8 (-  0.2), AbsRuntime:   1087259920 (+    21.1 %)
OP_LOOP              lat:       31.0 ->       34.3 (+    10.5 %), Runtime%:   0.6 (-  0.2), AbsRuntime:    792237360 (+    10.5 %)
OP_FALSE             lat:       30.8 ->       31.7 (+     2.9 %), Runtime%:   0.4 (-  0.2), AbsRuntime:    633526160 (+     2.9 %)
OP_PRINT             lat:    21348.4 ->   177190.3 (+   730.0 %), Runtime%:   0.1 (+  0.1), AbsRuntime:     89835500 (+   730.0 %)
OP_NOT               lat:       31.7 ->       38.4 (+    21.3 %), Runtime%:   0.1 (-  0.0), AbsRuntime:     89677720 (+    21.3 %)
OP_SUPER_INVOKE      lat:       62.2 ->      158.0 (+   154.2 %), Runtime%:   0.0 (+  0.0), AbsRuntime:     52667300 (+   154.2 %)
OP_GREATER           lat:       26.2 ->       50.1 (+    91.4 %), Runtime%:   0.0 (+  0.0), AbsRuntime:     29524440 (+    91.4 %)
OP_GET_UPVALUE       lat:       41.8 ->       63.8 (+    52.5 %), Runtime%:   0.0 (-  0.0), AbsRuntime:     21273020 (+    52.5 %)
OP_JUMP              lat:       30.3 ->       31.0 (+     2.2 %), Runtime%:   0.0 (-  0.0), AbsRuntime:     21104460 (+     2.2 %)
OP_SET_LOCAL         lat:       29.4 ->       28.9 (-     1.6 %), Runtime%:   0.0 (-  0.0), AbsRuntime:      7251060 (-     1.6 %)
OP_CLOSURE           lat:      898.9 ->     2138.5 (+   137.9 %), Runtime%:   0.0 (+  0.0), AbsRuntime:       583820 (+   137.9 %)
OP_DEFINE_GLOBAL     lat:      367.0 ->     1502.3 (+   309.4 %), Runtime%:   0.0 (+  0.0), AbsRuntime:       408620 (+   309.4 %)
OP_METHOD            lat:      312.9 ->     1655.4 (+   429.0 %), Runtime%:   0.0 (+  0.0), AbsRuntime:       276460 (+   429.0 %)
OP_CLASS             lat:      457.4 ->      973.3 (+   112.8 %), Runtime%:   0.0 (+  0.0), AbsRuntime:        96360 (+   112.8 %)
OP_NEGATE            lat:       36.4 ->       78.9 (+   116.5 %), Runtime%:   0.0 (+  0.0), AbsRuntime:        54340 (+   116.5 %)
OP_INHERIT           lat:      644.8 ->     1521.6 (+   136.0 %), Runtime%:   0.0 (+  0.0), AbsRuntime:        38040 (+   136.0 %)
OP_CLOSE_UPVALUE     lat:      104.4 ->      504.4 (+   383.2 %), Runtime%:   0.0 (+  0.0), AbsRuntime:        16140 (+   383.2 %)
OP_MULTIPLY          lat:      167.8 ->      457.8 (+   172.8 %), Runtime%:   0.0 (+  0.0), AbsRuntime:         8240 (+   172.8 %)
OP_DIVIDE            lat:      293.3 ->      644.4 (+   119.7 %), Runtime%:   0.0 (+  0.0), AbsRuntime:         5800 (+   119.7 %)
OP_SET_UPVALUE       lat:      184.0 ->      408.0 (+   121.7 %), Runtime%:   0.0 (+  0.0), AbsRuntime:         2040 (+   121.7 %)
OP_GET_SUPER         lat:     1540.0 ->      940.0 (-    39.0 %), Runtime%:   0.0 (-  0.0), AbsRuntime:          940 (-    39.0 %)
```


## Understanding the Code
* `struct cb` - A "continuous buffer".  This is a power-of-2-sized ring
  implementation with methods paralleling typical memory allocator routines.
  Allocations return a `cb_offset_t` into the ring, which can be dereferenced
  to a raw pointer via `cb_at()`.  Such raw pointers into this ring can become
  invalid due to resizing:  if allocations within the `struct cb` exceed the
  available size, it will resize to the next larger power-of-2 size (and call
  a `cb_on_resize_t` callback, allowing a rewrite of raw pointers to be
  implemented).
* `cb_offset_t` - A location within the `struct cb` ring.  These follow
  [Serial Number Arithmetic](https://en.wikipedia.org/wiki/Serial_number_arithmetic),
  so must be compared with `cb_offset_cmp()` instead of standard comparison
  operations.
* `struct cb_region` - A subregion of a `struct cb` within which allocations can
  be made.  (NOTE: This is a separate concept to the garbage collector's
  A/B/C regions which are implicit ranges of `cb_offset_t`.)
* `struct cb_bst` - A partially-persistent red-black tree implementation.
  (The partial persistence feature is not leveraged in this POC.)
* `struct structmap` - An O(log32(n)) `uint64_t->uint64_t` map.  Used primarily
  to map the `ObjID` integers of `Obj` allocations to their `cb_offset_t`
  locations.  This structure was inspired by Phil Bagwell's
  [Ideal Hash Tries](http://lampwww.epfl.ch/papers/idealhashtrees.pdf), a.k.a.
  "Hash Array Mapped Trees (HAMT)", however it removes the hashing and the dense
  packing.
* `ObjID` a unique ID for an allocation of an `Obj`, which is a structure the
  GC will be responsible for collecting/compacting.  These are generated in
  `objtable_add()`, which is primarily invoked through `assignObjectToID()`
  upon the creation of `Obj`s.  These are resolved via `objtable_lookup()`,
  usually through an `OID<T>::clip()` or `OID<T>::mlip()`.
* A key concept is being able to adequately size the newB `cb_region` which the
  GC thread will use as a destination for its compaction of live objects.
  A confounding factor is the padding needed to fulfill alignment concerns of
  objects, and how the needed padding may fluctuate depending on order of
  allocations.  All objects therefore overestimate their size by including
  their maximal alignment needs, in patterns that look like the following:
  `sizoef(struct X) + alignof(struct X) - 1`.
* "Internal size" - The maximal allocation size (inclusive of alignment padding)
  of the internal nodes of a data structure.
* "External size" - The maximal allocation size (inclusive of alignment padding)
  of the referred-to entries a data structure.  (NOTE: This is used only by the
  ObjTable,  the mapping of ObjID integers to allocated objects.  This is
  because the ObjTable is considered to own the objects it refers to, and so
  these objects will need to be considered to be part of the ObjTable's size for
  compaction sizing concerns.)
* `CBO<T>` - A `cb_offset_t` reference to a `T`.  Dereferenced through an O(1)
  `cb_at()`.  Used to refer to raw allocations.
* `OID<T>` - An `ObjID` reference to a `T`.  Dereferenced through an O(log32(n))
  `objtable_lookup()`.  Used to refer to Obj allocations which are the purview
  of the garbage collector.
* `RCBP<T>` - A raw pointer into a `struct cb` ring which will be rewritten if
  that `struct cb` were to be resized due to a new allocation.  NOTE: At
  present, these may only exist *outside* of the `struct cb` ring, e.g. in a
  C-language stack frame.
* How to read some of the dereferencing methods:
  * cp() - "const pointer", O(1)
  * mp() - "mutable pointer", O(1)
  * clp() - "const local pointer", O(1)
  * mlp() - "mutable local pointer", O(1)
  * crp() - "const remote pointer" (pointing to a `struct cb` other than `thread_cb`), O(1)
  * mrp() - "mutable remote pointer" (pointing to a `struct cb` other than `thread_cb`), O(1)
  * clip() - "const local id-based pointer", O(log32(n))
  * mlip() - "mutable local id-based pointer", O(log32(n)) + possible `deriveMutableObjectLayer()` copy.
* `deriveMutableObjectLayer()` - Copies an object from the read-only B or C
  regions to the mutable A region and updates the ObjTable, such that this Obj
  for this ObjID can be modified until the next freeze of the A region due to
  a GC.  (NOTE: The reason it is considered a "mutable object layer" and not
  just a "mutable object" is because ObjClass methods and the ObjInstance
  fields maps will not be copied into the mutable region A.  Instead, these Obj
  types will be copied with empty maps into the mutable section A. New
  methods/fields can be added, but lookups will check the map "layers" at each
  of the the A, B, and C regions.  This preserves O(1) copying of ObjClass and
  ObjInstance objects.)
* GC is presently still initiated (as in `clox`) by the main thread invoking
  `reallocate()` and it choosing to call `collectGarbage()`.  This will cause a
  shift of regions on the main thread and emission (via `gc_submit_request()`)
  of a request to the GC thread that it should perform compaction.  The GC
  thread waits in `gc_main_loop()` for such requests to arrive, at which point
  it will perform its compaction to the request's destination region, and then
  respond back the main thread by posting a response via `gc_submit_response()`.
  The main thread will observe and integrate any ready compacted result of a GC
  cycle during `OP_LOOP/OP_CALL/OP_INVOKE/OP_SUPER_INVOKE/OP_RETURN`
  instructions through a call to `integrate_any_gc_response()`.
* `PIN_SCOPE` - Used to extend the lifetime of `struct cb`-allocated data within
  it scope, so that it may still be referred to after a potential GC due to an
  allocation.

## Miscellaneous Notes
* The cb ring will resize if an allocation calls for more memory than is
  available.  The expectation is that a program's running memory size can be
  stated in advance to a power-of-2 order of magnitude in order to avoid ring
  resizes, or else the program can pay the necessary penalty the resize.
* I intentionally avoided some optimization paths (e.g. labels as values for
  threaded interpretation) so that performance comparison of klox to clox would
  show only the impact of the new data structures and GC approach.  The purpose
  of this POC is to evaluate the costs and tradeoffs of this approach to an O(1)
  garbage collector.
* I am aware that there are probably a few cases where `PIN_SCOPE` as used is
  insufficient protection, but these bugs do not undermine the overall concept
  of this POC and the test suite is passing.  If this POC is considered worth
  expanding on, I expect it would be with translation of the concept to another
  language's runtime anyway, so I'm not convinced these gaps are worth fixing.
* Q: Why are verbose names used for things (e.g. clip()) that could be
  implemented as operator->() overloads?  A: These names are both greppable and
  make costs more apparent when read, facilitating optimization.
* Q: Why are there so many assert()s?  A: I follow a style called
  "Assertion-Oriented Programming" (AOP). The completion of this work would not
  have been possible without heavy reliance on assertions.  Even when one of my
  assert()-stated assumptions have been wrong, it has often taken hours to get
  to the bottom of such a bug.  If these assert()s did not exist, there is
  absolutely no way I could have fixed the same bugs because they would have
  only shown up at some unrelated later point of execution, and I would have
  long ago tired of this effort.
* The CB project's README contains old notes which elaborate on some of the
  concepts.  In those notes, the naming of the regions are inverted (the mutable
  region is called "C").
* The CB project also has a "structmap" class, which was an earlier
  implementation that was abandoned.  Aside from `struct cb`,
  `struct cb_region`, and `struct cb_bst`, most of the remaining code of that
  project is abandoned.





## Dedication
This work is dedicated to my family, with a special thank you to my parents, who
always supported me.
