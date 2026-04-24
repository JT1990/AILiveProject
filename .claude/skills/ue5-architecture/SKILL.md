---
name: ue5-architecture
description: UE5.6/UE5.7 module architecture and boundary design. Use when requests involve module layout, Build.cs dependencies, reflection exposure, Public/Private API boundaries, naming conventions, or preventing circular dependencies.
---

# Reference Files

Read these when working on specific tasks:
- `references/module-layout.md` — module layout patterns and naming conventions
- `references/build-cs-patterns.md` — Build.cs dependency patterns and examples
- `references/project-adapter.md` — project-specific adapter for applying these patterns
- `references/ue5-module-routing-table-final.csv` — full engine module index for dependency lookup
- `scripts/generate_module_index_v2.py` — run to regenerate the module index if it's outdated

# Quick Start
- Collect current module list, `*.Build.cs`, and major gameplay/UI systems.
- Propose one target module graph before writing code.
- Output module responsibilities and ownership in a table.

# Workflow
- Identify runtime, editor, UI, networking, and data modules from current codebase.
- Define Public API for each module as minimal headers and Blueprint surface.
- Define Private implementation boundaries and include rules.
- Define `PublicDependencyModuleNames` and `PrivateDependencyModuleNames` per module.
- Report risks: circular includes, over-exposed reflection types, and cross-layer references.

# Constraints
- Keep `UCLASS/USTRUCT/UENUM` only where reflection is required.
- Prefer forward declarations in headers; include concrete headers in `.cpp`.
- Do not move types across modules without listing migration impact.
- Keep naming aligned with Unreal conventions (`U`, `A`, `F`, `E` prefixes).

# Failure Handling
- If ownership of a class is ambiguous, place it in runtime module first and log a TODO to split after usage mapping.
- If dependency graph becomes cyclic, extract shared contracts into a thin common module.
- If Build.cs dependencies are uncertain, choose minimal set and verify compile paths immediately.

# Escalation
- Escalate when a refactor requires asset redirectors, class renames, or package path migration.
- Escalate when module split changes public Blueprint class paths used by existing content.
