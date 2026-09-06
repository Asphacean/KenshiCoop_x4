# Build setup

This document has moved to keep a single canonical copy (Phase 1 doc hygiene).

See [`resources/BUILD_SETUP.md`](../resources/BUILD_SETUP.md).

## Fresh-machine setup notes (2026-09, verified working)

`resources/` is gitignored, so the canonical doc is unavailable in a fresh clone.
The following reproduces a working build environment from scratch:

1. **VC++ 2010 (v100) x64 + Windows SDK 7.1** — the SDK 7.1 web installer hangs/fails
   with modern VC redists installed. Instead, download `GRMSDKX_EN_DVD.iso` from
   Microsoft, mount it, and install these MSIs directly with `msiexec /i <msi> /qn`:
   `WinSDK_amd64`, `WinSDKBuild_amd64`, `WinSDKTools_amd64`, `WinSDKWin32Tools_amd64`,
   `WinSDKInterop_amd64`, `vc_stdx86`, `vc_stdamd64`. Then apply
   `VC-Compiler-KB2519277.exe /q /norestart` (SP1 compiler update → cl 16.00.40219.01).
2. **MSBuild** — VS 2019 or 2022 Build Tools both work (`build_plugin.cmd` locates
   either via `vswhere -products *`).
3. **ENet** — clone `lsalzman/enet` at tag `v1.3.18` into `third_party/enet/enet/`
   and apply both patches in `third_party/enet/patches/` (see that README).
4. **KenshiLib_deps** — `BFrizzleFoShizzle/KenshiLib_Examples_deps` stores its
   binaries in Git LFS, whose budget may be exhausted (`git lfs pull` fails). The
   same binaries are available as GitHub release assets, hash-identical to the LFS
   pointers: `KenshiLib.lib` from `BFrizzleFoShizzle/KenshiLib` release **v0.4.0**;
   `OgreMain_x64.lib`, `MyGUIEngine_x64.lib`, `boost.zip` from the deps repo's
   **v0.1** release zip. Extract `boost.zip` in place (`boost_1_60_0/boost/...`).
5. **Local header fixes in the deps clone** (the published snapshot differs from the
   tree KenshiCoop was developed against; these edits live only in the gitignored
   deps directory and must be re-applied after a re-clone):
   - copy `KenshiLib/Include/kenshi/combat/*.h` up into `KenshiLib/Include/kenshi/`
     (the plugin includes `<kenshi/CombatClass.h>`, and the quoted includes inside
     it only resolve from the `kenshi/` root);
   - wrap the identical `enum BuildingDesignation` definitions in
     `kenshi/Building/Building.h` and `kenshi/Platoon.h` in a shared include guard
     (`KENSHICOOP_BUILDING_DESIGNATION_GUARD`) — they redefine each other;
   - in `kenshi/Building/CraftingBuilding.h`, give `CraftingItem` a minimal complete
     definition (`class CraftingItem { public: char _kcoop_opaque; };`) — VC10's
     `std::deque` member requires a complete type; the deque object layout is
     T-independent, and no plugin code touches the elements.

After this, `scripts/build_prototest.cmd`, `build_tunneltest.cmd`, `build_nettest.cmd`,
and `scripts/build_plugin.cmd` (Harness) all build and their test exes pass.
