# Co-op architecture

## Instances and puppets
Each player runs a full native instance where they are player 1, with their own
camera, lock-on, menus, equipment and abilities. Other players appear as **puppets**
in party slots: their actors exist, but AI is disabled and they are driven by
network state.

## Authority
| State | Owner | Mechanism |
|---|---|---|
| Player transform, animation, HP/MP | That player | Snapshots at 20–30 Hz, interpolated |
| Abilities, spells, limits | That player | Events; effects played locally on the puppet |
| Enemy AI, positions, HP, deaths, drops | Host | Snapshots; enemy AI disabled on clients |
| Hits on enemies | Attacker detects, host confirms | Hit event → host applies damage → broadcast |
| Hits on players | Defender detects | So dodges seen locally work |
| Story flags, rooms, cutscenes, chests | Host | Host's save is canonical; clients follow room changes |

## Functions to find and override
- Actor/object system: spawn, transform, animation state
- Party AI update
- Damage application, enemy HP and death
- Enemy AI update
- Room loading and event triggers

## Open design questions
- Drive Forms consume party members — restrict to AI slots, or solo only?
- Limits are Sora + party member — two-player moves?
- Reaction commands, scripted boss phases, no pausing in co-op.
- Playing **as** Donald/Goofy needs a player-controllable moveset; a second Sora is
  only a puppet with Sora's model on other screens, so it is the easier option.
