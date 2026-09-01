# Register pressure — the axis our optimizer does not model

Status: SCOPED, not started. Board task: `t_9156140b`.
Gated behind correctness — see "Sequencing" at the end.

## 1. The finding

**Our compiler emits byte-identical output at `-O0`, `-O1`, `-O2` and `-O3`.**
Measured across every tracked shader: nothing in our pipeline gates on the
optimization level, so the level is a declaration and not a behaviour.

The other side of the byte-exact diff harness does not behave that way, and the
axis it varies is not the one you would guess. **It spends instructions to save
registers.** Its levels differ in how aggressively — and how successfully —
they make that trade.

So the optimization gap is not a list of transformations to copy. It is one
bounded design problem: **we do not model register pressure at all.**

## 2. Why that is the right trade on this hardware

An NV40 fragment program declares how many temp registers it uses, and that
count governs how many fragment threads the hardware keeps in flight. Fewer
registers means more parallelism, so a shader with more instructions and fewer
registers can be *faster* than a shorter one that needs an extra register.

This is a property of the hardware, not of any particular compiler, and it has
one immediate consequence for us:

> **Instruction count cannot carry a convergence or quality metric.**
> Ranking output by instruction count optimises the wrong axis. Register count
> and measured pixels are the metrics; instruction count is a size statistic.

## 3. The mechanism, as observed

The behaviour is a scheduler that relocates contiguous blocks of ALU work to
shorten value **live ranges** — defining values close to where they are used so
that fewer are simultaneously live. Three consequences, each measured, and each
counter-intuitive enough to be worth writing down:

1. **Instruction count is not monotonic in the optimization level.** Higher
   levels sometimes emit *more* instructions. If the count going up looks like
   a bug, it is not.
2. **A tighter schedule creates read-after-write hazards on temp registers**,
   which must then be covered by a pipeline-stall instruction. That stall is an
   extra instruction — it is the *price* of the register saving, not a separate
   phenomenon. Hoist, hazard, stall and register drop are one behaviour.
3. **The trade is not always won.** There are shaders where one level pays the
   stall and saves no register — a bad trade — and the next level up finds an
   allocation achieving the saving without the hazard, and drops the stall
   again. Levels differ in how *well* they trade, not only how hard they try.

Per-shader measurements supporting all three are recorded in the local-only
study that accompanies the diff harness; they are not reproduced here because
they are derived from another implementation's output.

## 4. What the work is

In this order, because each step is meaningless without the one before it:

1. **Make register pressure observable.** We do not compute live ranges today.
   Nothing else on this list can be evaluated until we can measure what we are
   trying to reduce.
2. **Give the `-O` levels behaviour.** The default level is already aligned
   with the harness's other side; what sits behind it is not implemented. Gate
   real passes on `CompileOptions::opt` so the levels stop being a declaration.
3. **Implement live-range-shortening scheduling**, and only then the
   hazard-stall insertion it requires — the stall exists to serve the
   reordering, so building it first would be building a cost with no benefit.
4. **Judge on register count and pixels.** Never on instruction count; see §2.

## 5. Sequencing

**Gated behind correctness.** A compiler that is fast and wrong is worse than
one that is slow and right, and today a large majority of the shaders our
general path compiles have not been adjudicated as correct by pixel readback.
Do not start this before that coverage exists.

## 6. Caveat on the mechanism

§3's instruction-level account rests on our own reverse engineering of two
opcodes whose meaning appears in no hardware header we have; our assembler
records one of them as verified by observation rather than documentation. **If
that inference is ever corrected, the measurements survive and the story in §3
does not.**

The conclusion in §1 and §2 is unaffected either way: it rests on register
counts read directly from container headers, and on our own output being
level-invariant, both of which are measured directly.
