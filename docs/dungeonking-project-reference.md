# DungeonKing — External Game Project Reference

> The game code lives OUTSIDE this studio repo. This studio repo holds design
> docs, agents, and workflow; the actual Unreal project is a separate folder.

## Location
- **Path**: `C:\Users\guswn\Documents\Unreal Projects\DungeonKing`
- **Engine**: Unreal Engine 5.8 (`EngineAssociation: 5.8`)
- **Basis**: UE5 template renamed to DungeonKing, now expanded with two gameplay
  variants (Strategy, TwinStick) alongside the base Top Down code.

## Code (Source/DungeonKing/)

### Base (Top Down)
- `ADungeonKingCharacter` — ACharacter, top-down camera + spring arm, Blueprintable
- `ADungeonKingPlayerController` — click-to-move (NavMesh), Enhanced Input, Niagara cursor FX
- `ADungeonKingGameMode` — default pawn/controller BPs

### Variant_Strategy/ (RTS-style)
- `AStrategyPawn : APawn` (abstract) — player/camera pawn for RTS control
- `AStrategyUnit : ACharacter` (abstract) — controllable unit
- `AStrategyGameMode`, `AStrategyPlayerController`
- `UEnvQueryContext_MoveGoal` — EQS context for unit move goals
- **UI/**: `AStrategyHUD`, `UStrategyUI`, `UStrategyTouchControls`

### Variant_TwinStick/ (twin-stick shooter)
- `ATwinStickCharacter : ACharacter` (abstract) — player character
- `ATwinStickGameMode`, `ATwinStickPlayerController`
- **AI/**: `ATwinStickNPC : ACharacter` (abstract), `ATwinStickAIController`,
  `ATwinStickSpawner`, `TwinStickNPCDestruction`, `TwinStickStateTreeUtility`
  (StateTree-driven enemy AI)
- **Gameplay/**: `ATwinStickProjectile : AActor` (abstract), `TwinStickAoEAttack`,
  `TwinStickPickup`
- **UI/**: `UTwinStickUI`

## Module dependencies (DungeonKing.Build.cs)
`Core`, `CoreUObject`, `Engine`, `InputCore`, `EnhancedInput`, `AIModule`,
`NavigationSystem`, `StateTreeModule`, `GameplayStateTreeModule`, `Niagara`,
`UMG`, `Slate`
- New since 2026-07-14: **StateTreeModule, GameplayStateTreeModule, UMG, Slate**

## Plugins (DungeonKing.uproject)
- `ModelingToolsEditorMode` (editor-only)
- `StateTree`, `GameplayStateTree` — AI behavior via StateTree
- `ModelContextProtocol`, `Terminal`, `EditorToolset` — editor/tooling plugins
- New since 2026-07-14: **StateTree, GameplayStateTree, ModelContextProtocol,
  Terminal, EditorToolset**

## Config highlights
- Rendering: DX12 + SM6, Lumen (GI+reflections), Virtual Shadow Maps, Ray Tracing ON,
  static lighting OFF, Mesh Distance Fields ON — high-end desktop target
- Input: Enhanced Input is the default player input class
- Default map: /Game/TopDown/Maps/TopDownMap (World Partition)

## Content (names only — binary)
- UE5 Mannequins (Manny/Quinn), template BPs, LevelPrototyping grid meshes, cursor FX

## Blind spot
- Blueprint internals (BP_*, IMC_*, IA_*) and StateTree assets are binary — logic
  cannot be read directly. Needs screenshots or C++ exposure to inspect/modify precisely.

Last verified: 2026-07-17
