# `reference/` -- the reading room

Upstream trees this project READS and never builds, ships or links against. Nothing here is
compiled into `main.dll`, nothing here is fetched by CI (`\.github/workflows/build-core.yml`
initialises `src/votv-coop/third_party/` only), and nothing here is required to build the mod.
A clone of this repository therefore carries this file and nothing else from the directory.

**Why the trees are not submodules.** They used to be, and the pointers cost every `git clone
--recursive` several gigabytes of material the build never touches. The only thing the pointers
bought that mattered was the PIN: a comment in our source that cites
`reference/mtasa-blue/Server/mods/deathmatch/logic/CGame.cpp:1234` means a specific line of a
specific commit, and an unpinned clone would drift off it as upstream moves. The pins live in the
table below instead, which is the same guarantee at none of the cost.

**Reading these is a project rule, not a nicety.** `CLAUDE.md` requires the MTA equivalent of a
problem to be grepped and READ before a coop feature is designed, and the RE-UE4SS equivalent
before an engine-level one. This directory is what those rules point at, so a machine doing that
work wants it populated.

## Rebuilding the room

Clone what you need; each tree is independent. The `--` commit is what this repository's citations
were read against.

```sh
# From the repository root.
git clone https://github.com/multitheftauto/mtasa-blue.git  reference/mtasa-blue
git -C reference/mtasa-blue checkout c07ccb00e30d973cebc4b907a9da15fab01ee6c1

git clone https://github.com/UE4SS-RE/RE-UE4SS.git          reference/RE-UE4SS
git -C reference/RE-UE4SS checkout 7f7cc36f8cdc082566cd676acc26975a22a41aaa

git clone https://github.com/lsalzman/enet.git              reference/enet
git -C reference/enet checkout 5a9c537fd464b3c6d3c55e1d3bd47588faf71b42
```

## The trees

| Tree | Upstream | Licence | Commit our citations were read at | What it is here for |
|--|--|--|--|--|
| `mtasa-blue/` | https://github.com/multitheftauto/mtasa-blue | GPLv3 | `c07ccb00e30d973cebc4b907a9da15fab01ee6c1` | The architectural precedent: the parallel class hierarchy, the keysync packet, sessions, host-authoritative AI, the latent send queue, the server browser. **GPLv3, so SHAPES port and code does not.** |
| `RE-UE4SS/` | https://github.com/UE4SS-RE/RE-UE4SS | MIT | `7f7cc36f8cdc082566cd676acc26975a22a41aaa` | How the engine is reached: AOB-resolved reflection, the `FUObjectArray` listener layout, the script-VM loop. **MIT, so code PORTS with per-site attribution** -- see `THIRD-PARTY-NOTICES.md`. |
| `enet/` | https://github.com/lsalzman/enet | MIT | `5a9c537fd464b3c6d3c55e1d3bd47588faf71b42` | The congestion control neither MTA nor GameNetworkingSockets gives us: `enet_peer_throttle` (`peer.c`) and the RTT estimator that feeds it (`protocol.c`), ported as the send-rate control law. **MIT, code PORTS with attribution.** |
| `baritone/` | https://github.com/cabaletta/baritone | LGPLv3 | -- | Pathfinding precedent for the bot director. Read only; no line is cited in our source. |
| `VoiceChatMC/` | https://github.com/henkelmax/simple-voice-chat | MIT | -- | Voice-chat RE reference (a saved page plus a clone). Read only. |
| `voidmod-extracted/` | -- | -- | -- | An extracted VOTV mod, read for its shape. Not redistributable. |
| `unreal-shimloader/` | https://github.com/Dei-Vias/unreal-shimloader | -- | -- | How other mod loaders enter an Unreal process. |
| `psk-psa-v9.1.2/` | -- | -- | -- | Blender PSK/PSA addon, a model-format RE aid. |
| `agency-agents/` | -- | -- | -- | Our own audit-agent prompts. Local by the maintainer's decision, like `docs/QUESTION_FORM_*`. |

A tree with no commit in the table carries no `file:line` citation in our source, so it needs no
pin; clone whatever version you like. **When you add a citation to a tree, pin it here in the same
change** -- an uncited tree that gains a citation is how a `file:line` silently stops meaning
anything.

## Licences

Every tree keeps its own. Where this project actually PORTS code -- RE-UE4SS and ENet, both MIT --
the obligation is discharged by `THIRD-PARTY-NOTICES.md`, which reproduces each licence verbatim
and is tracked, public and independent of whether this directory was ever populated. MTA:SA is
GPLv3 and is read for its shapes only; no MTA line is copied into this codebase.
