---
name: unreal-cpp
description: Unreal C++ via Monolith MCP. Triggers on C++, UCLASS, UPROPERTY, UFUNCTION, include path, class hierarchy, engine API, source file. Engine API lookup, signature verification, source reading, config.
---

# Unreal C++ Development Workflows

You have access to **Monolith** with 11 source actions via `source_query()` and 6 config actions via `config_query()`.

## Discovery

```
monolith_discover({ namespace: "source" })
monolith_discover({ namespace: "config" })
```

## Source Actions (11)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `search_source` | `query` | Find symbols (classes, functions, structs) across engine source |
| `read_source` | `symbol` | Read actual engine source code for a symbol |
| `get_class_hierarchy` | `symbol` | Class inheritance tree |
| `find_callers` | `symbol` | Who calls this function in engine code |
| `find_callees` | `symbol` | What does this function call |
| `find_references` | `symbol` | All references to a symbol |
| `get_module_info` | `symbol` | Module dependencies, build type |
| `get_symbol_context` | `symbol` | Get a symbol's definition and surrounding context |
| `read_file` | `file_path` | Read raw engine source file by path |
| `trigger_reindex` | — | Trigger full C++ engine source re-index |
| `trigger_project_reindex` | — | Trigger incremental project-only C++ source re-index (faster) |

## Key Parameter Names

- `symbol` — the symbol to look up (e.g., `"ACharacter"`, `"UGameplayStatics::ApplyDamage"`)
- `query` — search query string (for `search_source`)
- `file_path` — file path for `read_file`

## Common Workflows

### Find and read an API
```
source_query({ action: "search_source", params: { query: "ApplyDamage" } })
source_query({ action: "read_source", params: { symbol: "UGameplayStatics::ApplyDamage" } })
```

### Get symbol context (definition + surrounding code)
```
source_query({ action: "get_symbol_context", params: { symbol: "UCharacterMovementComponent::PhysWalking" } })
```

### Understand how Epic uses an API
```
source_query({ action: "find_callers", params: { symbol: "UPrimitiveComponent::SetCollisionEnabled" } })
```

### Explore a class hierarchy
```
source_query({ action: "get_class_hierarchy", params: { symbol: "ACharacter" } })
```

### Read engine implementation details
```
source_query({ action: "read_source", params: { symbol: "UCharacterMovementComponent::PhysWalking" } })
```

### Resolve config/CVar values
```
config_query({ action: "resolve_setting", params: { file: "DefaultEngine", section: "/Script/Engine.RendererSettings", key: "r.Lumen.TraceMeshSDFs" } })
config_query({ action: "explain_setting", params: { setting: "r.DefaultFeature.AntiAliasing" } })
```

## Build.cs Gotchas

Common linker errors and their fixes:

| Error | Fix |
|-------|-----|
| `LNK2019` unresolved external for `UDeveloperSettings` | Add `"DeveloperSettings"` to Build.cs — it's a separate module from `Engine` |
| `LNK2019` for any UE type | Check module with `source_query("get_module_info", ...)` and add to Build.cs |
| Missing `#include` | Use `source_query("search_source", ...)` to find the correct header — never guess include paths |
| Template instantiation errors | Check if the type needs explicit export (`_API` macro) |

## UE 5.7 API Notes

- `FSkinWeightInfo` uses `uint16` for `InfluenceWeights` (not `uint8`) and `FBoneIndexType` for bones
- `CreatePackage` with same path returns existing in-memory package — use unique names
- Live Coding only handles `.cpp` body changes — header changes require editor restart + full UBT build

## Tips

- **Never guess** `#include` paths or function signatures — always verify with `source_query`
- The search action is `search_source` (not `search`)
- The source index covers engine Runtime, Editor, Developer modules + plugins + shaders (1M+ symbols)
- Use `find_callers` to learn idiomatic usage patterns from Epic's own code
- Use `get_symbol_context` for quick definition lookup without reading the full source
- Combine `source_query` (engine) with project-level search for full picture
- Use `config_query("explain_setting")` before changing any unfamiliar CVar
- Non-existent actions: `get_include_path`, `get_function_signature`, `get_deprecation_warnings` — these do NOT exist
