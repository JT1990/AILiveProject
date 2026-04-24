---
name: unreal-mesh
description: Meshes and levels via Monolith MCP. Triggers on mesh, actor, spawn, blockout, scatter, procedural geometry, spatial query, lighting, acoustics, encounter design, town gen. 242 mesh actions.
---

# Unreal Mesh & Spatial Workflows

You have access to **Monolith** with **242 Mesh actions** (197 core + 45 experimental town gen) via `mesh_query()`.

Town gen actions require `bEnableProceduralTownGen = true` in Editor Preferences (disabled by default — work-in-progress with known geometry issues).

For full action signatures and parameter details, read `references/mesh-action-reference.md`.

## World Outliner Organization

Every actor spawned by Monolith should be placed in an Outliner folder. Leaving actors loose at root level makes complex scenes unmanageable — batch deletion, feature isolation, and scene readability all depend on folder structure. Use the `folder` param on any spawn/place action, or the action auto-assigns one.

| Action | Default Folder |
|--------|---------------|
| `create_parametric_mesh` | `/Procedural/{Type}` |
| `create_structure` | `/Procedural/Structure` |
| `create_horror_prop` | `/Procedural/Horror` |
| `create_maze` | `/Procedural/Maze` |
| `create_building_shell` | `/Procedural/Building` |
| `create_pipe_network` | `/Procedural/Pipes` |
| `create_terrain_patch` | `/Procedural/Terrain` |
| `create_building_from_grid` | `/Procedural/Town/{BuildingName}` |
| `create_city_block` | `/Procedural/Town/{BlockName}` |
| `create_street` | `/Procedural/Town/Streets` |
| `place_street_furniture` | `/Procedural/Town/StreetFurniture` |
| `create_foundation` | `/Procedural/Town/Foundations` |
| `create_balcony` | `/Procedural/Town/Features` |
| `create_porch` | `/Procedural/Town/Features` |
| `create_fire_escape` | `/Procedural/Town/Features` |
| `furnish_room` | `/Procedural/Town/Furniture` |
| `furnish_building` | `/Procedural/Town/Furniture` |
| `scatter_props` | `/Scatter/{VolumeName}` |
| `scatter_on_surface` | `/Surface/{SurfaceActorName}` |
| `scatter_on_walls` | `/Scatter/{VolumeName}/Walls` |
| `scatter_on_ceiling` | `/Scatter/{VolumeName}/Ceiling` |
| `place_light` | `/Lights` |
| `spawn_actor` | `/Spawned` |
| `place_blueprint_actor` | `/Prefabs` |
| `place_prop_kit` | `/Props/Kits/{KitName}` |
| `place_storytelling_scene` | `/Storytelling/{Pattern}` |
| `place_along_path` | `/PathProps` |
| `place_decals` | `/Decals` |

When orchestrating multiple related spawns, group them under a shared descriptive folder.

## Discovery

Always discover available actions first:
```
monolith_discover({ namespace: "mesh" })
```

## Key Parameter Names

- `asset_path` -- mesh asset path for inspection actions
- `actor_name` -- placed level actor name (label or internal name)
- `volume_name` -- name of a BlockingVolume with Monolith.Blockout tag
- `handle` -- named mesh handle (GeometryScript only)
- `building_id` -- registered building in spatial registry (town gen)
- `room_id` -- registered room in spatial registry (town gen)
- `block_id` -- city block identifier (town gen)

## Action Categories

Read `references/mesh-action-reference.md` for full tables. Category summary:

| Category | Count | Purpose |
|----------|-------|---------|
| Mesh Inspection | 12 | Read-only queries on any StaticMesh/SkeletalMesh |
| Scene Manipulation | 8 | Actor CRUD on placed level actors |
| Scene Spatial Queries | 11 | Physics-based world queries |
| Level Blockout | 15 | Tag-based volumes, asset matching, replacement |
| Mesh Operations | 12 | GeometryScript edits — requires handles |
| Horror Spatial Analysis | 8 | Sightlines, hiding spots, tension classification |
| Accessibility Analysis | 6 | Wheelchair clearance, cognitive scoring, reach |
| Lighting Analysis | 5 | Luminance sampling, dark corner detection |
| Audio & Acoustics | 14 | Material-aware reverb, stealth maps, sound paths |
| Performance Analysis | 5 | Budget-aware placement, draw calls, overdraw |
| Decal & Detail Placement | 4 | Environmental storytelling, scatter |
| Level Design | 9 | Lights, materials, mesh swap, instancing |
| Volumes & Properties | 7 | Trigger/kill/navmesh volumes, selection |
| Horror Intelligence | 4 | Spawn scoring, patrol routes, encounter pacing |
| Tech Art Pipeline | 7 | Import, LOD gen, texel density |
| Advanced Level Design | 8 | Sublevels, prefabs, splines |
| Context-Aware Props | 8 | Surface scatter, disturbance, physics settle |
| Procedural Geometry | 8 | GeometryScript furniture, horror props, structures |
| Procedural Town Generation | 46 | Full building-to-block pipeline |
| Genre Presets | 8 | Horror/fantasy/sci-fi storytelling presets |
| Encounter Design | 8 | Patrol, ambush, safe rooms, pacing |
| Quality & Polish | 9 | Naming, HLOD, texture budget, framing |

## Typical Workflows

### Blockout a Room
```
get_blockout_volumes -> scan_volume -> create_blockout_primitives_batch -> match_all_in_volume -> apply_replacement
```

### Inspect a Mesh
```
get_mesh_info -> analyze_mesh_quality -> compare_meshes
```

### Edit a Mesh (GeometryScript)
```
create_handle -> [mesh_simplify | mesh_boolean | compute_uvs | ...] -> save_handle -> release_handle
```

### Horror Level Design
```
analyze_sightlines -> find_hiding_spots -> analyze_escape_routes -> classify_zone_tension -> analyze_pacing_curve
```

### Accessibility Check
```
generate_accessibility_report (or individual validate_ actions)
```

### Horror Audio Design
```
create_surface_datatable (first time) -> get_surface_materials -> analyze_room_acoustics -> get_stealth_map -> can_ai_hear_from -> suggest_audio_volumes
```

### Lighting Audit
```
get_light_coverage -> find_dark_corners -> analyze_light_transitions -> suggest_light_placement
```

### Performance Budget
```
get_region_performance -> get_triangle_budget -> analyze_shadow_cost -> find_overdraw_hotspots
```

### Environmental Storytelling
```
place_storytelling_scene -> place_along_path -> scatter_props -> analyze_prop_density
```

### Understand a Scene
```
get_scene_statistics -> query_radial_sweep -> get_spatial_relationships
```

### Generate a Single Building (Town Gen)
```
list_building_archetypes -> generate_floor_plan -> create_building_from_grid -> generate_facade -> generate_roof -> register_building -> auto_volumes_for_building -> furnish_building -> validate_building
```

### Generate a City Block (Town Gen)
```
create_city_block (full pipeline in one call) OR step-by-step:
create_lot_layout -> [generate_floor_plan -> create_building_from_grid -> generate_facade -> generate_roof -> validate_building per lot] -> create_street -> place_street_furniture -> auto_volumes_for_block -> save_block_descriptor
```

### Place Building on Terrain (Town Gen)
```
sample_terrain_grid -> analyze_building_site -> create_foundation -> place_building_on_terrain
```

### Debug/Inspect a Building (Town Gen)
```
toggle_section_view -> toggle_ceiling_visibility -> capture_floor_plan -> highlight_room -> query_room_at -> query_adjacent_rooms
```

### Add Architectural Details (Town Gen)
```
create_balcony / create_porch / create_fire_escape / create_ramp_connector -> apply_horror_damage
```

## Gotchas

- `spawn_actor` does NOT spawn `ABlockingVolume` -- use the editor for volumes
- `delete_actors` deletes placed actors, NOT asset files (use `editor_query("delete_assets")` for that)
- `batch_execute` rejects nested `batch_execute` and caps at 200 actions
- `set_actor_properties`: Mobility must be "Movable" BEFORE enabling SimulatePhysics
- `query_radial_sweep` hard cap: `ray_count * vertical_angles <= 512`
- `search_meshes_by_size` requires `monolith_reindex()` to have populated the mesh catalog first
- All spatial queries work in editor WITHOUT a play session
- `query_` prefix = active physics queries. `get_` prefix = reads stored data
- `create_city_block` is a capstone action -- calls the full pipeline internally. Use step-by-step actions for fine control
- `register_building` / `register_room` are required before spatial registry queries will return results
- `save_block_descriptor` / `load_block_descriptor` persist to JSON, not uasset
- `create_ramp_connector` follows ADA slope guidelines by default -- override with `max_slope` for non-accessible ramps
- `furnish_room` uses the spatial registry -- the room must be registered first
- Stairwells need minimum 4x6 cells (24 grid cells) for switchback stairs at 270cm floor height
- Use `omit_exterior_walls: true` when calling `generate_facade` to avoid double-wall issue
- Pass `building_context` to architectural features for auto-orientation from Building Descriptor's `exterior_faces`
- Run `validate_building` after generation to verify playability (capsule sweep, BFS connectivity, stair angles)
