# Transcribing a routine: what the listing says and the C must too

Read with section 3 of `skill/SKILL.md`.

## A result that lives only in the flags

A routine that ends `moveq #0,d0; rts` (or `moveq #1,d0`) leaves its answer in the condition codes as much as in d0, and the caller branches on the flags (`bsr routine; beq else`). Transcribe the caller's branch from the disassembly: which flag, which way, and what the branch skips. Do not take the sense from the routine's name or from a comment on its return value. A continue screen returned 0 for "the countdown ran out" and 1 for "fire"; the caller's `beq` went to GAME OVER on 0, and a C `if (routine())` written the other way round restarted the game when the countdown ran out. The oracle cannot see this (it compares the routine, not its caller's branch); the lockstep finds it where the player reaches it.

Check every call of such a routine in the listing, not only the first.

## Code that follows a call

What a routine does after a call returns is part of that routine: the text shown after a failed continue, the clearing of a flag, the jump to the exit of the loop. When the lockstep shows the port one sequence behind or ahead of the original, look for a call the C left out (the original showed GAME OVER before clearing the player's flag).

## Interrupt handlers

Routines that end in `rte` are `void irq_...(void)` functions. A handler is installed by assigning the C function to the `irq_vectors` array (levels 1 to 7 at `$64..$7C`); code that stores a handler address into a vector assigns the function. The oracle calls routines that end in `rts` and cannot call these: translate them with extra care and list them as unchecked in the report; the lockstep covers them through the work they hand to the main code.

## Self-modifying code

A word written over a routine's first instruction (`move.w #$4E75,addr`) or a branch patched into a slot becomes a function pointer or a flag in C. Say so in a comment and compare the memory dumps before and after the patch. The unexecuted end of a routine that was never reached is `/* goes on at $ADDR */`.

## Registers a routine leaves for the next

A test of a point whose coordinates the caller left in d0/d1, a call with an address register that is overwritten right after: write them with the values the original had at that instruction, not the ones the name suggests.

## Stale addresses

A free object, or a table row no one reads, can hold the address of code of an earlier level. Resolving it to a C function when the memory is loaded reports `no C` for code that never runs. Skip free objects when converting, so that a real missing function stands out.
