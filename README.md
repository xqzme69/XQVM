# XQVM

XQVM is a C++20 research VM built to get rid of the convenient boundary most
small VMs leave behind:

```text
opcode + complete operand tuple + handler + next VPC
```

MachineIR still has normal typed actions such as `add`, `load`, and `branch`.
The serialized module does not. The compiler lowers every block and edge into a
physical island made of a local primitive DAG. Logical values are split into
three affine-mapped shares, block state and origin are mixed into the plan, and
all carrier and memory writes stay delayed until the whole island has passed
its checks.

There is no persistent public VPC either. Control moves through a continuation
lane and its proof, both split across three carriers. Physical handlers read
their own operands from the byte stream, so the runtime never keeps one
long-lived `DecodedInstruction` with an opcode and a complete operand tuple.

Nothing here is supposed to be unbreakable. The runtime has to decode the
module, the external profile contains the seed needed to derive its codec, and
the implementation is public. The repo exists so the compiler, verifier,
runtime, formats, tests, and fuzz target can be pulled apart without guessing
what a closed prototype might be doing.

## Build

You need CMake 3.24 or newer, Ninja, and a C++20 compiler.

```powershell
cmake --preset debug
cmake --build --preset debug --parallel
ctest --preset debug --output-on-failure
```

For an optimized build:

```powershell
cmake --preset release
cmake --build --preset release --parallel
ctest --preset release --output-on-failure
```

The repo also has pinned `gcc`, `asan`, and `fuzz` presets. The sanitizer preset
uses Clang. On Windows, CMake copies the required ASan runtime beside the built
executables.

## Run it

The shortest path is the in-memory factorial demo:

```powershell
.\build\debug\xqvm.exe demo 10
```

To produce files that can be inspected or broken independently:

```powershell
.\build\debug\xqvm.exe build-demo .\artifacts\factorial 0xC4A7D93E21B6580F
```

That command writes three files:

- `factorial.xqvm` contains the public physical islands and initial state.
- `factorial.xqprofile` contains the external opcode, mask, and continuation
  profile.
- `factorial.xqmap` contains compiler-only origins, labels, grouped physical
  shares, affine maps, and continuations. It is ground truth for experiments,
  not something to ship with a protected module.

The public module and profile are enough for the normal tools:

```powershell
.\build\debug\xqvm.exe verify .\artifacts\factorial.xqvm .\artifacts\factorial.xqprofile
.\build\debug\xqvm.exe disasm .\artifacts\factorial.xqvm .\artifacts\factorial.xqprofile
.\build\debug\xqvm.exe run    .\artifacts\factorial.xqvm .\artifacts\factorial.xqprofile 10
.\build\debug\xqvm.exe trace  .\artifacts\factorial.xqvm .\artifacts\factorial.xqprofile 10
```

## Design

### MachineIR can refuse bad input

The source IR is typed and SSA-like. Blocks own their values, cross-block data
has to travel through edge arguments and block parameters, and every value has
a domain and origin. The verifier rejects duplicate IDs and origins, unknown
targets, future operands, domain mismatches, bad edge argument lists, and
cross-block uses that skipped the parameter boundary.

### Physical values are shared values

Each MachineIR result becomes a `PhysicalValue`:

```text
domain + family + origin + 3 x (carrier, affine map)
```

The family is XOR or additive. Every share is stored through an invertible
affine map over `uint64_t`. Reconstructing the logical value needs the right
carrier set, family algebra, and inverse maps. The public stream contains the
constants required to do that work; the private map only keeps the original
grouping and labels.

### A physical island is a local DAG

The physical layer has exactly 20 primitive schemas. A node may reference only
earlier nodes from the same island. Compiler-generated plans use those schemas
to reconstruct inputs, perform the useful operation, mix semantic and history
state, create new shares, and seal the next continuation.

MachineIR edges get their own islands. An edge reconstructs committed source
state, moves block arguments into the target carrier layout, and produces new
path-dependent state for the target. Phi-like transport is physical work here,
not free metadata attached to a jump.

### Writes commit once

Carrier and VM-memory writes are staged. The executor runs the complete DAG,
checks targets, conflicts, resource bounds, and the exact island end, then
commits. A failure before that point leaves the old state untouched.

One consequence matters for compiler work: a load sees memory from before the
current island's commit. Store-to-load forwarding needs another island or an
explicit forwarding model.

### Continuation uses six carriers

Carriers `c0..c2` hold the continuation lane. `c3..c5` hold its proof. Every
island has to replace all six. The runtime reconstructs the pair only long
enough to verify it and obtain the next island offset, then drops it again.

The proof catches corruption and control-state desynchronization inside this
model. It is not a signature. A valid proof also does not make an arbitrary
offset legal: the physical verifier records island boundaries and the opened
target must match one of them.

## Tests and fuzzing

The test suite is mostly about refusal paths and invariants rather than one
factorial happy path. It covers all 20 schemas, integer operators and compare
conditions, both share families, affine inversion and composition, malformed
modules and profiles, continuation tampering, delayed-write atomicity, resource
limits, reproducible output, and cross-block history transport.

```powershell
ctest --preset debug --output-on-failure
```

The libFuzzer target feeds the strict profile and module parsers, verifier,
disassembler, machine constructor, runtime, and the bounded inner-island path.

```powershell
cmake --preset fuzz
cmake --build --preset fuzz --parallel
.\build\fuzz\xqvm_module_fuzz.exe .\artifacts\corpus -runs=200000 -seed=13371337
```

## Repository layout

- `include/xqvm/` contains the public contracts.
- `src/machine_ir.cpp` verifies typed input IR.
- `src/compiler.cpp` lowers blocks and edges into physical islands.
- `src/physical.cpp` implements shares, families, domains, and affine maps.
- `src/primitive.cpp` owns the 20-schema primitive layer.
- `src/runtime.cpp` contains the streaming fused executor and atomic commit.
- `src/disassembler.cpp` independently scans and decodes physical islands.
- `src/module.cpp` and `src/profile.cpp` implement the bounded file formats.
- `tests/xqvm_tests.cpp` holds contract, tamper, algebra, and property tests.
- `fuzz/module_loader_fuzz.cpp` is the parser-to-runtime fuzz boundary.

## Current limits

- one function per module;
- scalar 64-bit values and at most 16 external arguments;
- byte and 64-bit VM-memory operations;
- default limits of 4,096 primitive nodes, 1,024 carrier writes, and 1,024
  memory writes per island;
- a default runtime limit of 1,000,000 executed islands;
- no native-code JIT, exception model, host syscalls, or concurrent VM state;
- CRC32 checks file consistency, not authenticity;
- the external profile is reproducible from its seed and is not a secret-key
  container.

The missing parts are missing on purpose. The current tree is small enough to
read end to end, while the lowering, physical state, edge transport, verifier,
and failure atomicity are already real code instead of README promises.
