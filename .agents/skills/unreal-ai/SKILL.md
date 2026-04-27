---
name: unreal-ai
description: UE AI systems via Monolith MCP. Triggers on navigation, navmesh, pathfinding, smart object, EQS, environment query, AIPerception, hearing, sight, team perception, AI controller. 100 AI actions.
---

# Unreal AI Workflows

You have access to **Monolith** with **100 AI actions** across 6 subsystems via `ai_query()`.

Smart Object actions (16) require `WITH_SMARTOBJECTS=1` — conditionally compiled. Runtime actions only work during Play-In-Editor sessions.

## Discovery

Always discover available actions first:
```
monolith_discover({ namespace: "ai" })
```

## Key Parameter Names

- `asset_path` — Blueprint, AI Controller, or data asset path (no `.uasset` extension)
- `blueprint_path` — Blueprint actor path (for component-add actions)
- `save_path` — destination path for newly created assets
- `actor` — actor label or path used by all `[RUNTIME]` actions (PIE-only)
- `sense_type` — sense class string: `Sight`, `Hearing`, `Damage`, `Touch`, `Team`, `Prediction`
- `dominant_sense` — same set as `sense_type`; controls which sense breaks ties
- `affiliation` — object with boolean fields `{ enemies, neutrals, friendlies }`
- `register_as_source_for` — array of sense class strings, e.g. `["Sight", "Hearing"]`
- `agent_index` — integer index into NavMesh supported agents list
- `option_index` — EQS generator slot index (0-based)
- `test_index` — EQS test slot index within a generator option
- `slot_index` — Smart Object slot index (0-based)
- `spec` — declarative JSON object for `build_eqs_query_from_spec`

## Action Reference

### AI Perception (12)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `add_perception_component` | `asset_path`, `dominant_sense`? | Add UAIPerceptionComponent to a Blueprint actor |
| `get_perception_config` | `asset_path` | Read all configured senses and their parameters |
| `configure_sight_sense` | `asset_path`, `radius` | Set sight radius, peripheral angle, affiliation, PoV offset |
| `configure_hearing_sense` | `asset_path`, `range` | Set hearing range, affiliation, max age |
| `configure_damage_sense` | `asset_path` | Set damage sense implementation and max age |
| `configure_touch_sense` | `asset_path` | Set touch sense affiliation and max age |
| `configure_team_sense` | `asset_path` | Set team sense max age and enabled state |
| `configure_prediction_sense` | `asset_path` | Set prediction sense max age and enabled state |
| `remove_sense` | `asset_path`, `sense_type` | Remove a sense from the perception component |
| `add_stimuli_source_component` | `asset_path`, `register_as_source_for[]` | Add UAIStimuliSourceComponent — required for actors to generate stimuli |
| `configure_stimuli_source` | `asset_path`, `sense_types[]` | Update which senses an existing stimuli source registers for |
| `validate_perception_setup` | `asset_path` | Lint: missing stimuli source, orphaned senses, invalid affiliation |

### Navigation (24)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `get_nav_system_config` | — | Read navigation system settings |
| `get_navmesh_config` | — | Read RecastNavMesh properties |
| `set_navmesh_config` | — | Set agent radius/height, cell/tile size, slope, step height |
| `get_navmesh_stats` | — | Tile counts, memory usage, build time |
| `add_nav_bounds_volume` | `location`, `extent` | Place a NavMeshBoundsVolume in the level |
| `list_nav_bounds_volumes` | — | List all NavMeshBoundsVolumes in the current level |
| `build_navigation` | — | Trigger a full navigation mesh build |
| `get_nav_build_status` | — | Check whether nav build is complete or in progress |
| `list_nav_areas` | — | List all registered NavArea classes |
| `create_nav_area` | `save_path`, `name` | Create a new NavArea Blueprint with cost and color |
| `add_nav_modifier_volume` | `location`, `extent`, `area_class` | Place a NavModifierVolume with specified area class |
| `add_nav_link_proxy` | `start_location`, `end_location` | Place a NavLinkProxy for jump/drop links |
| `configure_nav_link` | `actor_path` | Set nav link enabled state, area class, direction |
| `list_nav_links` | — | List all NavLinkProxy actors in level |
| `find_path` | `start`, `end` | Query a navigation path between two world points |
| `test_path` | `start`, `end` | Test whether a path exists between two points |
| `project_point_to_navigation` | `point` | Project a world point onto the navmesh |
| `get_random_navigable_point` | — | Get a random reachable point, optionally near an origin |
| `navigation_raycast` | `start`, `end` | Cast a nav ray and check for obstructions |
| `configure_nav_agent` | `agent_index` | Set radius, height, step height for a nav agent profile |
| `add_nav_invoker_component` | `blueprint_path` | Add NavInvoker for local nav tile generation |
| `get_crowd_manager_config` | — | Read DetourCrowdManager settings |
| `set_crowd_manager_config` | — | Set max agents, avoidance, separation, collision resolution |
| `analyze_navigation_coverage` | — | Sample navigability across level bounds |

### EQS — Environment Query System (21)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `create_eqs_query` | `save_path` | Create a new empty EQS query asset |
| `get_eqs_query` | `asset_path` | Read query structure: options, generators, tests |
| `list_eqs_queries` | — | List all EQS query assets, optionally by path |
| `delete_eqs_query` | `asset_path` | Delete an EQS query asset |
| `duplicate_eqs_query` | `source_path`, `dest_path` | Duplicate an EQS query to a new path |
| `add_eqs_generator` | `asset_path`, `generator_class` | Add a generator (Points, Actors, Pathing, etc.) |
| `remove_eqs_generator` | `asset_path`, `option_index` | Remove an option/generator by index |
| `configure_eqs_generator` | `asset_path`, `option_index`, `properties` | Set generator properties by option index |
| `add_eqs_test` | `asset_path`, `option_index`, `test_class` | Add a test to a generator option |
| `remove_eqs_test` | `asset_path`, `option_index`, `test_index` | Remove a test by index |
| `configure_eqs_test` | `asset_path`, `option_index`, `test_index`, `properties` | Set test properties |
| `configure_eqs_scoring` | `asset_path`, `option_index`, `test_index` | Set scoring equation, factor, normalization, clamping |
| `configure_eqs_filter` | `asset_path`, `option_index`, `test_index` | Set filter type (Minimum/Maximum/Range/Match) and thresholds |
| `reorder_eqs_tests` | `asset_path`, `option_index`, `new_order[]` | Reorder tests within a generator option |
| `list_eqs_generator_types` | — | List all available EQS generator classes |
| `list_eqs_test_types` | — | List all available EQS test classes |
| `list_eqs_contexts` | — | List all available EQS context classes |
| `build_eqs_query_from_spec` | `save_path`, `spec` | Create a complete EQS query from a declarative JSON spec |
| `validate_eqs_query` | `asset_path` | Lint: missing generators, invalid test params, scoring conflicts |
| `create_eqs_from_template` | `save_path`, `template` | Create from preset: `find_cover`, `find_flank`, `find_patrol_point`, `find_nearest_item` |
| `runtime_run_eqs_query` `[RUNTIME]` | `actor`, `asset_path` | Run an EQS query against a live actor during PIE |

### Smart Objects (16) — requires `WITH_SMARTOBJECTS`

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `create_smart_object_definition` | `save_path` | Create a new Smart Object Definition data asset |
| `get_smart_object_definition` | `asset_path` | Read slots, tags, behavior definitions |
| `list_smart_object_definitions` | — | List all Smart Object Definition assets |
| `delete_smart_object_definition` | `asset_path` | Delete a Smart Object Definition |
| `duplicate_smart_object_definition` | `source_path`, `dest_path` | Duplicate a definition to a new path |
| `add_so_slot` | `asset_path` | Add a slot with offset, rotation, and activity/user tags |
| `remove_so_slot` | `asset_path`, `slot_index` | Remove a slot by index |
| `configure_so_slot` | `asset_path`, `slot_index` | Set slot offset, rotation, tags, enabled state, name |
| `add_so_behavior_definition` | `asset_path`, `slot_index`, `behavior_class` | Attach a behavior definition to a slot |
| `remove_so_behavior_definition` | `asset_path`, `slot_index`, `behavior_index` | Remove a behavior definition from a slot |
| `set_so_tags` | `asset_path` | Set top-level activity and user tags on a definition |
| `add_smart_object_component` | `blueprint_path`, `definition_path` | Add USmartObjectComponent to a Blueprint actor |
| `place_smart_object_actor` | `definition_path`, `location` | Spawn a Smart Object actor in the level |
| `find_smart_objects_in_level` | — | List all Smart Object actors, optionally filtered |
| `validate_smart_object_definition` | `asset_path` | Lint: empty slot arrays, missing behavior defs, tag conflicts |
| `create_so_from_template` | `save_path`, `template` | Create from preset: `hide_spot`, `sit_chair`, `workstation`, `door_interaction`, `pickup_item` |

### AI Discovery & Diagnostics (10)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `get_ai_overview` | — | High-level summary of all AI assets in project |
| `get_ai_system_config` | — | Read AISystem global config (perception update rate, etc.) |
| `search_ai_assets` | `query` | Search by `asset_type`: `bt/bb/st/eqs/so/controller` |
| `validate_ai_data_flow` | `controller_path` | Trace data flow: Controller → Blackboard → BT → Perception |
| `find_eqs_references` | `eqs_path` | Find all BTs and nodes referencing an EQS query |
| `find_so_references` | `so_path` | Find all actors referencing a Smart Object definition |
| `lint_behavior_tree` | `asset_path` | Check BT for missing decorators, blackboard key mismatches |
| `detect_ai_circular_references` | — | Find circular dependencies in AI asset graph |
| `export_ai_manifest` | — | Export full AI asset catalog as JSON or Markdown |
| `get_ai_behavior_summary` | `asset_path` | Summarize an AI Controller or BT asset |

### Scaffolding & Bootstrap (7)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `scaffold_complete_ai_character` | `save_path`, `pawn_path` | Full AI stack: Controller + Blackboard + BT + Perception in one call |
| `scaffold_perception_to_blackboard` | `controller_path` | Wire perception callbacks to blackboard key updates |
| `scaffold_team_system` | `save_path` | Create Team ID DataAsset + Attitude Solver Blueprint |
| `scaffold_patrol_investigate_ai` | `save_path`, `pawn_path` | Patrol/Investigate BT with EQS patrol point query |
| `scaffold_enemy_ai` | `save_path`, `pawn_path` | Basic enemy AI: sight perception, chase, attack BT |
| `scaffold_eqs_move_sequence` | `bt_path`, `eqs_path` | Wire RunEQS → MoveTo sequence nodes into an existing BT |
| `scaffold_stealth_game_ai` | `save_path` | Stealth AI template: awareness, sound/sight, investigation |

### Runtime / PIE (10) — `[RUNTIME]` actions require an active PIE session

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `runtime_get_perceived_actors` `[RUNTIME]` | `actor` | List all actors currently perceived by an AI actor |
| `runtime_check_perception` `[RUNTIME]` | `actor`, `target` | Check whether `actor` currently perceives `target` |
| `runtime_report_noise` `[RUNTIME]` | `actor`, `location`, `loudness` | Inject a noise stimulus at a world location |
| `runtime_find_smart_objects` `[RUNTIME]` | — | Query Smart Object subsystem for available objects |
| `runtime_get_bb_value` `[RUNTIME]` | `actor`, `key_name` | Read a blackboard key value from a live AI actor |
| `runtime_set_bb_value` `[RUNTIME]` | `actor`, `key_name`, `value` | Write a blackboard key value on a live AI actor |
| `runtime_get_bt_state` `[RUNTIME]` | `actor` | Get the current Behavior Tree execution state |
| `runtime_start_bt` `[RUNTIME]` | `actor`, `behavior_tree_path` | Start a Behavior Tree on a live AI actor |
| `runtime_stop_bt` `[RUNTIME]` | `actor` | Stop the Behavior Tree running on a live AI actor |
| `runtime_get_bt_execution_path` `[RUNTIME]` | `actor` | Dump the active BT node execution path |

## Common Workflows

### Full AI Character Bootstrap

```
ai_query({ action: "scaffold_complete_ai_character", params: {
  save_path: "/Game/AI/Enemy",
  pawn_path: "/Game/Blueprints/BP_EnemyPawn"
}})
ai_query({ action: "validate_ai_data_flow", params: {
  controller_path: "/Game/AI/Enemy/AIC_Enemy"
}})
```

### Set Up Sight + Hearing Perception

```
// Perception actions target the AIController, not the Pawn:
ai_query({ action: "add_perception_component", params: {
  asset_path: "/Game/AI/AIC_Enemy",
  dominant_sense: "Sight"
}})
ai_query({ action: "configure_sight_sense", params: {
  asset_path: "/Game/AI/AIC_Enemy",
  radius: 1500.0,
  lose_radius: 2000.0,
  peripheral_angle: 60.0,
  affiliation: { enemies: true, neutrals: false, friendlies: false },
  max_age: 5.0
}})
ai_query({ action: "configure_hearing_sense", params: {
  asset_path: "/Game/AI/AIC_Enemy",
  range: 1000.0,
  affiliation: { enemies: true, neutrals: true, friendlies: false },
  max_age: 3.0
}})
// Stimuli source goes on the perceived actor (player pawn), not the AI:
ai_query({ action: "add_stimuli_source_component", params: {
  asset_path: "/Game/Blueprints/BP_PlayerPawn",
  register_as_source_for: ["Sight", "Hearing"]
}})
ai_query({ action: "validate_perception_setup", params: {
  asset_path: "/Game/AI/AIC_Enemy"
}})
```

### Build NavMesh and Verify Paths

```
ai_query({ action: "add_nav_bounds_volume", params: {
  location: { x: 0, y: 0, z: 0 },
  extent: { x: 5000, y: 5000, z: 500 }
}})
ai_query({ action: "build_navigation", params: {} })
ai_query({ action: "get_nav_build_status", params: {} })
ai_query({ action: "find_path", params: {
  start: { x: 0, y: 0, z: 0 },
  end: { x: 2000, y: 1500, z: 0 }
}})
```

### Create an EQS Cover Query

```
ai_query({ action: "create_eqs_from_template", params: {
  save_path: "/Game/AI/EQS/EQS_FindCover",
  template: "find_cover"
}})
ai_query({ action: "add_eqs_test", params: {
  asset_path: "/Game/AI/EQS/EQS_FindCover",
  option_index: 0,
  test_class: "EnvTestDistance"
}})
ai_query({ action: "configure_eqs_scoring", params: {
  asset_path: "/Game/AI/EQS/EQS_FindCover",
  option_index: 0,
  test_index: 1,
  purpose: "Score",
  equation: "InverseLinear",
  factor: 1.0
}})
ai_query({ action: "validate_eqs_query", params: {
  asset_path: "/Game/AI/EQS/EQS_FindCover"
}})
```

### Create a Smart Object

```
ai_query({ action: "create_so_from_template", params: {
  save_path: "/Game/AI/SmartObjects/SO_Workstation",
  template: "workstation"
}})
ai_query({ action: "configure_so_slot", params: {
  asset_path: "/Game/AI/SmartObjects/SO_Workstation",
  slot_index: 0,
  offset: { x: 0, y: -60, z: 0 },
  activity_tags: ["Activity.Work"],
  user_tags: ["User.Civilian"],
  enabled: true
}})
ai_query({ action: "place_smart_object_actor", params: {
  definition_path: "/Game/AI/SmartObjects/SO_Workstation",
  location: { x: 1200, y: 800, z: 0 }
}})
ai_query({ action: "validate_smart_object_definition", params: {
  asset_path: "/Game/AI/SmartObjects/SO_Workstation"
}})
```

### Build EQS Query from Spec (Fastest Path)

```
ai_query({ action: "build_eqs_query_from_spec", params: {
  save_path: "/Game/AI/EQS/EQS_FlankPosition",
  spec: {
    options: [
      {
        generator: {
          class: "EnvQueryGenerator_SimpleGrid",
          properties: { grid_size: 1000.0, space_between: 200.0 }
        },
        tests: [
          {
            class: "EnvQueryTest_Distance",
            properties: { distance_to: "EnvQueryContext_Querier" },
            scoring: { purpose: "Score", equation: "Linear", factor: 1.0 },
            filter: { filter_type: "Range", min: 300.0, max: 1200.0 }
          },
          {
            class: "EnvQueryTest_Trace",
            properties: { trace_to: "EnvQueryContext_Querier" },
            scoring: { purpose: "Filter" }
          }
        ]
      }
    ]
  }
}})
ai_query({ action: "validate_eqs_query", params: {
  asset_path: "/Game/AI/EQS/EQS_FlankPosition"
}})
```

### Wire Perception to Blackboard + EQS Move

```
ai_query({ action: "scaffold_perception_to_blackboard", params: {
  controller_path: "/Game/AI/AIC_Enemy"
}})
ai_query({ action: "scaffold_eqs_move_sequence", params: {
  bt_path: "/Game/AI/BT_Enemy",
  eqs_path: "/Game/AI/EQS/EQS_FindCover"
}})
```

### Runtime PIE Debugging

```
ai_query({ action: "runtime_get_perceived_actors", params: { actor: "BP_EnemyPawn_1" }})
ai_query({ action: "runtime_get_bt_execution_path", params: { actor: "BP_EnemyPawn_1" }})
ai_query({ action: "runtime_report_noise", params: {
  actor: "BP_EnemyPawn_1",
  location: { x: 500, y: 300, z: 0 },
  loudness: 1.0
}})
ai_query({ action: "runtime_get_bb_value", params: {
  actor: "BP_EnemyPawn_1",
  key_name: "TargetActor"
}})
```

## Key Technical Notes

1. **Smart Objects are conditionally compiled** — The 16 SO actions are only registered when `WITH_SMARTOBJECTS=1`. If SO actions return an unknown-action error, check plugin activation and rebuild. Use `get_ai_system_config` to surface active subsystems.

2. **All `[RUNTIME]` actions require PIE** — They operate on live UWorld actors by label. They fail outside Play-In-Editor. Start PIE before any `runtime_*` call.

3. **`affiliation` is an object, not a string** — `configure_sight_sense` and `configure_hearing_sense` require `affiliation: { enemies: true, neutrals: false, friendlies: false }`. Passing a string causes a validation error.

4. **`add_stimuli_source_component` targets the perceived actor** — The component goes on the pawn that needs to be seen/heard (e.g. the player), not on the AI doing the perceiving.

5. **NavMesh must be built before path queries** — `find_path`, `test_path`, `project_point_to_navigation`, and `navigation_raycast` require a valid built NavMesh. Call `build_navigation` and confirm `get_nav_build_status` is complete first.

6. **EQS filter direction is counterintuitive** — `filter_type: "Minimum"` keeps items **above** the threshold (discards low values); `"Maximum"` keeps items **below** (discards high values). Use `"Range"` with explicit `min`/`max` to avoid ambiguity.

7. **`validate_ai_data_flow` traces the full stack** — Follows the chain: AI Controller → Blackboard → BT → Perception → Stimuli Sources. Run it after scaffolding to surface missing links.

8. **Perception actions target the AIController, not the Pawn** — `add_perception_component`, `configure_sight_sense`, `configure_hearing_sense`, and all other `configure_*_sense` actions use the AIController Blueprint as `asset_path`. Setting them on the Pawn Blueprint silently creates a misplaced component that the AI system ignores.

9. **BT/BB asset CRUD is not in this skill** — `create_behavior_tree`, `add_bt_task`, `create_blackboard`, `add_bb_key`, and related graph-editing actions are handled by `blueprint_query()`. Use the `unreal-blueprints` skill for BT/BB asset creation and editing; use this skill for AI system wiring (perception, nav, EQS, scaffolding) and runtime inspection.

## Anti-Patterns to Validate

- **Forgetting `add_stimuli_source_component`** — An AI with sight sense will never perceive a pawn without a `UAIStimuliSourceComponent`. Always add it to perceived actors and call `validate_perception_setup`.

- **Running `find_path` on an unbuilt NavMesh** — Path queries against an empty NavMesh silently return no path. Call `build_navigation` + `get_nav_build_status` first.

- **Wrong EQS filter direction** — `filter_type: "Minimum"` does not keep the minimum; it discards values below the threshold. Use `"Maximum"` to keep closest points by distance.

- **Calling `runtime_*` outside PIE** — Runtime actions fail silently or with unhelpful errors when no PIE world is active.

- **Debugging perception before `validate_perception_setup`** — The validator catches missing stimuli sources, wrong affiliation flags, and zero-radius senses in one pass. Run it first.

- **Using Smart Object actions without checking `WITH_SMARTOBJECTS`** — Run `get_ai_system_config` first to confirm the subsystem is active.

## Tips

- **Start with `scaffold_complete_ai_character`** for new enemies — creates the full stack in one call with correct wiring.
- **Use `get_ai_overview`** at the start of an AI session to understand what AI assets already exist.
- **Always validate after creating** — `validate_perception_setup`, `validate_eqs_query`, `validate_smart_object_definition`, and `validate_ai_data_flow` catch wiring errors before PIE.
- **EQS templates are faster than scratch** — `create_eqs_from_template` with `find_cover`, `find_flank`, `find_patrol_point`, or `find_nearest_item` gives a correct baseline; layer additional tests with `add_eqs_test`.
- **NavInvoker over global NavMesh** for open worlds — use `add_nav_invoker_component` on AI pawns to generate local nav tiles, avoiding a massive single NavMesh build.
- **Team system first** — Scaffold with `scaffold_team_system` before configuring affiliation on senses; Attitude Solver classes must be registered for affiliation filtering to work.
- **`export_ai_manifest`** generates a full project AI report in JSON or Markdown — useful for auditing or documenting AI architecture.
