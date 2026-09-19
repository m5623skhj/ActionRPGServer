# ActionRPGServer

GameRoomServer references the MultiSocketRUDP server core and Logger as C++ projects.
TownServer is a separate application; its implementation is still empty.

## Dependencies

MultiSocketRUDP is pinned as a Git submodule under `External/MultiSocketRUDP`.
For a new checkout:

```powershell
git submodule update --init --recursive
```

Only CommonCode is required for server builds. To skip upstream test dependencies:

```powershell
git submodule update --init External/MultiSocketRUDP
git -C External/MultiSocketRUDP submodule update --init external/CommonCode
```

## Build

Use Visual Studio with the v145 C++ toolset and Windows SDK. Open
`ActionRPGServer/ActionRPGServer.slnx`, select x64, and build GameRoomServer.
The RUDP server and Logger are built and linked automatically.

```powershell
msbuild ActionRPGServer/GameRoomServer/GameRoomServer.vcxproj /m /p:Configuration=Debug /p:Platform=x64
msbuild ActionRPGServer/GameRoomServer/GameRoomServer.vcxproj /m /p:Configuration=Release /p:Platform=x64
```

Outputs are written to `artifacts/bin/x64/<Configuration>/` and intermediate
files to `artifacts/obj/`. `Directory.Build.targets` adapts upstream include
paths to this repository without modifying the submodule. The integration
supports x64 only.

The initial `main.cpp` constructs and destroys the core to verify compilation,
linkage and basic execution. It does not listen for connections yet.
Game sessions, packet handlers, option files and a TLS certificate are needed
when implementing `StartServer()` usage. Upstream `ContentsServer` provides
an example, but is not a dependency of this project.

## Updating the library

Check out the desired tested commit in `External/MultiSocketRUDP`, update its
CommonCode submodule, rebuild GameRoomServer, and commit the changed submodule
pointer in this repository. Do not copy library sources into game content.
