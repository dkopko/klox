# Project Brief: klox

## Project Overview

klox is a fork of the [clox interpreter](http://craftinginterpreters.com "Crafting Interpreters") that demonstrates an O(1) garbage collection approach. The project serves as a proof-of-concept for a novel memory management system in language runtimes.

## Core Objective

To implement and evaluate a garbage collection mechanism that offers constant-time allocation and collection while accepting a performance tradeoff where dereferencing becomes O(log32(n)) instead of O(1).

## Key Research Questions

1. Does this performance tradeoff provide an interesting/desirable alternative for language runtime design?
2. Could the persistent and partially-persistent data structures used in this implementation be leveraged for interesting language constructs (e.g., by-value semantics for large recursive structures like maps)?

## Technical Approach

- Implement a power-of-2 sized ring for fast sequential allocation
- Use integer enumeration for object handles
- Divide program memory into three regions (A, B, C) within a ring with specific roles:
  - Regions B and C are read-only
  - Region A is for mutation and new allocations
  - Older structures must be copied to A before mutation

## Performance Goals

Achieve O(1) garbage collection by:
- Main thread executing with 1 mutable region and 1 or 2 read-only regions
- GC thread concurrently performing live object compaction
- Using efficient data structures optimized for this memory model

## Project Scope

- This is a proof-of-concept to demonstrate and evaluate the memory management approach
- The implementation is based on the clox codebase but with significant modifications to the memory management system
- The project is intended to stimulate discussion on alternative approaches to garbage collection in language runtimes

## Success Criteria

- Functioning interpreter that passes the test suite
- Demonstrated O(1) garbage collection performance
- Comprehensive performance evaluation comparing to original implementation
- Clear documentation of the approach and its tradeoffs
