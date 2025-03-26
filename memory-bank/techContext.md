# Technical Context: klox

## Technology Stack

klox is implemented primarily in C/C++, building upon the foundation provided by the clox interpreter. The technical stack includes:

- **C/C++**: Core implementation language
- **CB**: The Continuous Buffer library that provides the fundamental memory structures
- **CMake/Make**: Build system for compiling and linking
- **Python**: Used for testing and benchmarking utilities

## Dependencies

### CB (Continuous Buffer)

CB is a critical dependency that provides the power-of-2 sized ring implementation and related data structures. It contains:

- `struct cb`: The ring buffer implementation
- `struct cb_region`: Sub-regions within a buffer for allocations
- `struct cb_bst`: A partially-persistent red-black tree implementation

### External Headers

The project incorporates various header files in the `external/` directory including:
- `cycle.h`: Cycle detection utilities
- Tree implementations from BSD variants
- Queue implementations
- `xxhash.h/c`: Fast hashing functions

## Build and Development Environment

### Building klox

1. First, build the CB dependency:
   ```sh
   # Clone and build CB
   git clone https://github.com/dkopko/cb
   cd cb
   make -j
   ```

2. Then build klox:
   ```sh
   cd klox/c
   make -j CBROOT=/path/to/cb
   ```

3. Run tests:
   ```sh
   cd ..
   ./util/test.py
   ```

4. Run the interpreter:
   ```sh
   ./c/BUILD/RelWithDebInfo/klox [filename]
   ```

### Development Tools

- **GCC/Clang**: Preferred compilers
- **GDB/LLDB**: Debuggers useful for working with the code
- **Valgrind**: Useful for memory analysis
- **Python**: Required for running the test suite

## Technical Constraints

### Memory Management

- **Ring Buffer Constraints**: The continuous buffer is a power-of-2 sized ring, which means memory allocation follows specific patterns
- **Serial Number Arithmetic**: Locations within the buffer use serial number arithmetic and must be compared with specialized functions
- **Alignment Requirements**: Object allocations must account for alignment padding, affecting size calculations

### Concurrency

- **Thread Safety**: The GC implementation requires careful coordination between the main thread and GC thread
- **Synchronization Points**: GC response integration occurs only at specific points in VM execution
- **Region Ownership**: The main thread and GC thread must respect region ownership rules to prevent race conditions

### Performance Considerations

- **Dereferencing Cost**: The O(log32(n)) cost of dereferencing objects is a fundamental constraint
- **Buffer Resizing**: Resizing the continuous buffer is expensive and should be minimized
- **Memory Estimation**: Ideally, program memory size should be estimated in advance to avoid need to resize the continuous buffer (cb)

## Optimization Techniques

The project intentionally avoids certain optimizations (like using labels as values for threaded interpretation) to enable fair performance comparison with clox, focusing purely on the impact of the memory management approach.

## Assertion-Oriented Programming

The codebase follows an "Assertion-Oriented Programming" (AOP) style:
- Heavy use of assertions to catch invariant violations early
- Critical for debugging and maintaining correctness in complex memory management
- Essential for development of the proof-of-concept

## Technical Documentation

### Code Naming Conventions

Various naming conventions explain object dereferencing patterns:
- `cp()`: "const pointer", O(1)
- `mp()`: "mutable pointer", O(1)
- `clip()`: "const local id-based pointer", O(log32(n))
- `mlip()`: "mutable local id-based pointer", O(log32(n))

These verbose names are intentionally chosen for:
- Improved code searchability
- Making performance costs apparent in the code
- Facilitating optimization by highlighting operation costs

## Testing Infrastructure

- `test.py`: Main test runner
- Test cases are organized in directories by functionality
- Benchmark tests evaluate performance characteristics

## Known Technical Limitations

- Some edge cases in PIN_SCOPE usage may be insufficient for protection
- The implementation prioritizes demonstrating the concept over production readiness

## NaN-Boxing Technique

klox uses NaN-boxing (also called NaN-tagging) as a memory optimization technique for representing values in the language runtime. This approach leverages IEEE 754 double-precision floating point bit patterns to store different value types efficiently within a single 64-bit word.

### Value Representation

```c
typedef struct { uint64_t val; } Value;
```

All values in klox (numbers, booleans, nil, objects) are unified under this single 64-bit representation.

### IEEE 754 Double Precision and NaN

In IEEE 754, a double-precision floating point value uses 64 bits:
- 1 bit for sign (S)
- 11 bits for exponent (E)
- 52 bits for fraction/mantissa (F)

A NaN value is represented when all exponent bits are 1s and the fraction is non-zero.

### Bit Patterns

```c
// Sign bit (highest bit)
#define SIGN_BIT ((uint64_t)0x8000000000000000)

// Quiet NaN pattern
#define QNAN ((uint64_t)0x7ffc000000000000)

// Tag values for singletons
#define TAG_NIL       1 // 001
#define TAG_FALSE     2 // 010
#define TAG_TRUE      3 // 011
#define TAG_TOMBSTONE 4 // 100
```

### Value Type Encoding

1. **Numbers**: Regular IEEE 754 doubles
   - Identified by: `(val & QNAN) != QNAN` (NaN bits not set)
   - Unlimited precision within double range

2. **Objects**: `SIGN_BIT | QNAN | object_id`
   - Identified by: `(val & (QNAN | SIGN_BIT)) == (QNAN | SIGN_BIT)`
   - ~51 bits available for object IDs (potential for 2^51 unique objects)

3. **Booleans**:
   - `TRUE_VAL`: `QNAN | TAG_TRUE`
   - `FALSE_VAL`: `QNAN | TAG_FALSE`

4. **Nil**: `QNAN | TAG_NIL`

5. **Tombstone**: `QNAN | TAG_TOMBSTONE` (used internally in hash tables)

### Type Detection Macros

```c
#define IS_NUMBER(v)  ((((v).val) & QNAN) != QNAN)
#define IS_OBJ(v)     ((((v).val) & (QNAN | SIGN_BIT)) == (QNAN | SIGN_BIT))
#define IS_BOOL(v)    ((((v).val) & (SIGN_BIT | QNAN | TAG_FALSE)) == (QNAN | TAG_FALSE))
#define IS_NIL(v)     (((v).val) == NIL_VAL.val)
```

### Value Creation and Conversion

```c
// Creation
#define NUMBER_VAL(num)   numToValue(num)
#define OBJ_VAL(objid)    ((Value) { (SIGN_BIT | QNAN | (uint64_t)((objid).id)) })
#define BOOL_VAL(b)       ((Value) { ((b) ? TRUE_VAL : FALSE_VAL) })
#define NIL_VAL           ((Value) { (uint64_t)(QNAN | TAG_NIL) })

// Conversion
#define AS_NUMBER(v)  valueToNum(v)
#define AS_OBJ_ID(v)  ((ObjID) { ((v).val) & ~(SIGN_BIT | QNAN) })
#define AS_BOOL(v)    (((v).val) == TRUE_VAL.val)
```

### Integration with Memory Model

The NaN-boxing system is fully integrated with klox's O(1) garbage collection:

1. For objects, the boxed value contains an ObjID, not a direct pointer
2. The ObjID is used for lookup in the objtable to get the actual memory location
3. This indirection supports the tripartite memory model (regions A, B, C)
4. Object value dereferencing occurs via `AS_OBJ(v)` which converts through ObjID

### Extensibility

The current implementation uses 4 tag values (NIL, FALSE, TRUE, TOMBSTONE) but the pattern allows for additional singleton types or special values if needed. With each tag requiring only a few bits, there is ample room for extension within the NaN space.

### Performance Implications

- Type checking and singleton value operations are extremely fast (single bit operations)
- Number operations require no conversion for arithmetic (direct IEEE 754)
- Object operations require objtable lookup, which follows the O(log32(n)) cost model
