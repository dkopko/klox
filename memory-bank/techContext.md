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
