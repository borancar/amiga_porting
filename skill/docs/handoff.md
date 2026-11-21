# The files a project keeps beside the port

`skill/SKILL.md` names `README.md`, `DECOMPILING.md`, `PLAN.md` and a handoff file. What each is for, so that they do not repeat one another:

- **`DECOMPILING.md`**: the method as applied to this game (conventions for the hardware, how a function is checked, what the tools are). Changes rarely.
- **`PLAN.md`**: the goal, the work packages and the status by layer. A package is an entry-address range from the trace analysis (`show.py --todo`) with its function count, instruction count and a guess at what is there; each range becomes a module of reasonable size and gives the order of the work. The status lists what is done (setup, build, oracle, decompiled, port, shop and levels) and how each was checked. Rewrite it when the goal moves; a goal that says "the first level" after five levels run is a stale plan.
- **`HANDOFF.md`**: where things stand today, for the next person or session. Sections: what exists (one line per folder and what it holds); what is not in git and how to bring it over (disks, ROMs, saves, captures); the commands; the state of the checks (what was compared, how long it ran clean, what was not played); the open items, each with what is known and what was not looked at; things that bit (the mistakes that cost time, as one line each); the user's standing rules. Dated; update it with each lesson, and put the lesson in the skill when it applies to any game.
- **`AGENTS.md`**: what an agent starting in the project must know before any command.
