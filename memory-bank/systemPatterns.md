# System Patterns: klox

## Memory Architecture

The core innovation in klox is its tri-partite memory model that enables O(1) garbage collection. The system divides memory into three regions (A, B, C) within a power-of-2 sized ring buffer.

### Memory Regions

1. **Region A**: The mutable region
   - Only region where mutation of structures occurs
   - Where new objects are allocated
   - Structures from B or C must be copied here before modification

2. **Regions B and C**: Read-only regions
   - Cannot be modified directly
   - Contain objects from previous GC cycles
   - Accessed during dereference operations

### Object Resolution Hierarchy

When dereferencing an object handle, the system searches:
1. First in region A (newest objects/mutations)
2. Then in region B (previous cycle)
3. Finally in region C (oldest active objects)

This layered approach allows for efficient concurrent garbage collection while maintaining program correctness.

## Garbage Collection Process

The GC implementation achieves essentially O(1) performance through a careful coordination between the main and GC threads:

### Main Thread GC Workflow

1. **Normal Execution**: Main thread runs with 2 active regions (A and B contain data, C is empty)
2. **GC Initiation**:
   - Regions shift: C = B, B = A, A = new region (now empty)
   - Request sent to GC thread to compact live objects of B and C into "newB"
   - This request transmission is brief and constant time
3. **Continued Execution**: Main thread continues running with 3 active regions while GC thread works
4. **GC Integration**:
   - GC thread completes compaction and sends response
   - Main thread integrates response: C = empty, B = newB
   - This integration step is brief and constant time
5. **Resume Normal Execution**: Main thread continues with 2 active regions

### Region Management

- **Region Shifting**: A critical operation that changes the role of each region during GC cycles
- **Read-Only Enforcement**: Ensures B and C regions cannot be modified after freezing
- **Copy-on-Write**: Objects in B or C are copied to region A when modifications are needed

## Key Data Structures

### Continuous Buffer (`struct cb`)

- Power-of-2-sized ring implementation
- Acts as the foundation for all memory allocations
- Allocations return offsets into this ring instead of pointers
- Manages resizing when needed using power-of-2 sizes

### Object IDs and References

- **ObjID**: A unique identifier for GC-managed objects
- **cb_offset_t**: A location within the ring buffer
- **CBO\<T>**: A ring buffer offset reference (O(1) dereference)
- **OID\<T>**: An object ID reference (O(log32(n)) dereference)

### Structure Map (`struct structmap_amt`)

- An O(log32(n)) uint64_t→uint64_t map
- Primary use: mapping ObjID to cb_offset_t locations
- Inspired by Hash Array Mapped Tries (HAMT)
- Designed for efficiency in the context of the memory model

### Cached Raw Pointers

- **RCBP Template**:
  - RCBP<> (Rewritable Continuous Buffer Pointer) is exclusively used within the runtime's own stack implementation
  - Not used in Obj objects that implement the target language's features (which use CBO/OID instead)
  - Purpose: When a Mutator thread resizes the continuous buffer, runtime stack pointers become invalid and must be updated
  - Only Mutator threads can trigger buffer resizes via:
    * cb_resize
    * cb_grow
    * cb_shrink
  - RCBP instances on the runtime stack form a linked list for efficient traversal during resize
  - GC thread can work with these stack pointers safely (even when held in RCBP<> instances due to common code paths) because:
    * GC only works in pre-allocated regions and therefore never causes buffer resizes
    * RCBP<> instances link into a thread-local linked list, preventing GC RCBP<> instances' operations from interfering with those of the Mutator thread.
  - Rewriting is necessary to maintain pointer validity since underlying memory addresses change
- **Naming Patterns**:
  - Fields ending in "P" indicate individual cached raw pointers from earlier lookups
  - Fields ending in "direct" indicate cached array pointers into specific memory regions (e.g., adirect, bdirect, cdirect)
- **Invalidation Mechanisms**:
  - Epoch-based validation: Pointers checked against gc_integration_epoch
  - Explicit recaching: Through tristack_recache() and triframes_recache() functions
  - Automatic recaching on buffer changes or GC cycles
- **Performance Benefits**: Avoids repeated lookups for frequently accessed pointers
- **Safety**: System ensures cached pointers remain valid through memory layout changes

### Red-Black Tree (`struct cb_bst`)

- Partially-persistent red-black tree implementation
- Supports the underlying memory management system

## Object Lifecycle

1. **Creation**: Objects are allocated in region A with a unique ObjID
2. **Access**: Objects are dereferenced via OID\<T>::clip() (to obtain a pointer to a const object) or OID\<T>::mlip() (to obtain a pointer to a non-const object)
3. **Modification**:
   - If in region A: Direct modification
   - If in regions B or C: Copied to A via deriveMutableObjectLayer() before modification
4. **Collection**: When GC runs, live objects are compacted into a new region

## Concurrency Model

- **Main Thread**: Handles program execution, freezes regions, and initiates GC
- **GC Thread**: Performs compaction of live objects concurrently
- **Synchronization**: Uses request/response mechanism between threads
- **Integration Points**: GC responses are integrated during specific VM operations

### CB Resize During GC

When a CB resize occurs while GC is in progress, a sophisticated handoff ensures consistency:

1. **Dual Buffer State**:
   - Mutator creates new CB at double size and copies all contents
   - Mutator continues execution in new CB
   - GC continues working in old CB with its original contents

2. **Thread Isolation**:
   - GC thread maintains thread-local pointers to old CB
   - GC operates only in pre-allocated regions (never causes resizes)
   - Thread-local RCBP lists prevent pointer interference

3. **Integration Protocol**:
   - When GC completes, its target region in old CB must be copied to new CB
   - This happens during the integration step when Mutator receives GC response
   - Integration handles this by checking if orig_cb != thread_cb (meaning the orig_cb written-to by the GC is no longer equal to the thread_cb held by the Mutator)
   - If CB was resized, it explicitly copies the consolidated newB region from old to new CB
   - This ensures no GC work is lost during resize transitions

4. **Safety Mechanisms**:
   - GC regions are pre-sized to prevent resize during collection
   - Pointer updates are managed through thread-local lists
   - Integration step handles critical pointer adjustments:
     * Frame pointers (functionP, constantsValuesP, ip_root, ip) are recalculated from function references
     * Stack and frame caches are updated via tristack_recache and triframes_recache
     * VM's currentFrame pointer is reestablished to point into new buffer
     * Object table layers are recached to point to correct buffer locations
     * Current frame is ensured to be mutable through triframes_ensureCurrentFrameIsMutable

## Design Decisions and Tradeoffs

### Performance Shift

The design explicitly shifts costs:
- GC and allocation become O(1) operations
- Object dereference becomes O(log32(n)) rather than O(1)

### Memory Constraints

- Uses power-of-2 sized ring buffer
- Resize operations are possible but expensive
- To avoid ring buffer resize costs, program memory size (to a power-of-2) should be well-estimated in advance

### Implementation Complexity

- Complex reference handling with multiple pointer types
- Careful synchronization between main and GC threads
- Memory safety ensured through assertion-oriented programming

## Pin Mechanism

### Pin Mechanism Details

The pin mechanism consists of two key components:

1. **pinned_lower_bound**:
   - Thread-local variable tracking the lower memory bound
   - Prevents garbage collection below this bound
   - Can be nested through multiple PIN_SCOPE blocks
   - Critical for preserving temporary objects during GC

2. **scoped_pin Class**:
   ```cpp
   class scoped_pin {
     cb_offset_t prev_pin_offset_;  // Previous pin location
     cb_offset_t curr_pin_offset_;  // Current pin location
     
     // Sets new pin at current region cursor
     scoped_pin(): prev_pin_offset_(pinned_lower_bound) {
       curr_pin_offset_ = cb_region_cursor(&thread_region);
       if (pinned_lower_bound == CB_NULL)
         pinned_lower_bound = curr_pin_offset_;
     }
     
     // Restores previous pin on destruction
     ~scoped_pin() {
       pinned_lower_bound = prev_pin_offset_;
     }
   };
   ```

3. **Usage Patterns**:
   - Entire compilation phase is pinned
   - Object creation and manipulation operations
   - String concatenation and similar operations
   - Any operation creating temporary objects that must survive GC

4. **Safety Mechanisms**:
   - RAII pattern ensures proper pin cleanup
   - Assertions verify pin ordering
   - Debug tracing for pin operations
- Critical for operations that might trigger GC

## Error Handling and Safety

- Heavy use of assertions to catch invariant violations
- Serial number arithmetic for offset comparisons
- Careful management of object copying to ensure consistency
