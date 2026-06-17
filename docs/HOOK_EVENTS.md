# Hook event coverage

Per Claude Code's official docs there are 29 hook events. This daemon routes
19 of them to a light effect and explicitly ignores the rest. Codex emits a
10-event subset; all 10 are handled.

## Claude Code events

| Event | Effect | Notes |
| --- | --- | --- |
| `SessionStart` | `idle` | steady green |
| `UserPromptSubmit` | `thinking` | green breathe |
| `UserPromptExpansion` | `thinking` | slash-command expansion |
| `PreToolUse` | `working` | tri-color cycle |
| `PreToolUse:AskUserQuestion` | `attention` | **tool-level override** — when Claude asks you something, light goes yellow instead of green |
| `PostToolUse` (success) | `working` | continues the work cycle |
| `PostToolUse` (`tool_response.error` or `tool_error` set) | `blocked` | **payload-driven** — failed `Bash` command turns the light red even though the event name is `PostToolUse` |
| `PostToolUse:AskUserQuestion` | `working` | back to work once you answered |
| `PostToolUseFailure` | `blocked` | explicit failure event |
| `PostToolBatch` | `working` | parallel tool batch resolved |
| `PreCompact` / `PostCompact` | `working` | context compaction |
| `SubagentStart` / `SubagentStop` | `working` | spawned agent threads |
| `PermissionRequest` | `permission` | red flash, urgent |
| `PermissionDenied` | `blocked` | auto-mode classifier rejected the tool |
| `Elicitation` | `permission` | MCP server requesting form input |
| `ElicitationResult` | `working` | user submitted the form |
| `Notification` (`permission_prompt` / `elicitation_dialog`) | `permission` | |
| `Notification` (`idle_prompt`) | `attention` | |
| `Notification` (`auth_success` / `elicitation_complete` / `elicitation_response`) | *no change* | informational only |
| `Stop` (normal) | `done` (turn_end) | 1.5 s green flash, drops session, then aggregate |
| `Stop` (`stop_reason: error` or `max_tokens`) | `blocked` | |
| `StopFailure` (`rate_limit` / `overloaded` / `server_error` / `max_output_tokens`) | `attention` | transient — soft yellow |
| `StopFailure` (other error types) | `blocked` | hard failures (auth, billing, invalid request, etc.) |
| `SessionEnd` | (drops session) | aggregate recomputed |

## Codex events

Codex emits a subset of Claude's events. All handled, no extra code needed.

| Event | Effect |
| --- | --- |
| `SessionStart` | `idle` |
| `UserPromptSubmit` | `thinking` |
| `PreToolUse` | `working` |
| `PostToolUse` | `working` (or `blocked` on payload error) |
| `PreCompact` / `PostCompact` | `working` |
| `SubagentStart` / `SubagentStop` | `working` |
| `PermissionRequest` | `permission` |
| `Stop` | `done` |

## Events we deliberately ignore

These fire frequently or carry no visual meaning. Routing them would either
thrash the LED (`MessageDisplay` streams every token) or waste hook latency on
events that don't change "is the AI busy / blocked / asking me?".

| Event | Reason |
| --- | --- |
| `MessageDisplay` | fires per streamed token — would strobe the light |
| `InstructionsLoaded` | boot-time CLAUDE.md / rules load, not a state change |
| `ConfigChange` | `settings.json` edit, not relevant to agent activity |
| `CwdChanged` | `cd` happened |
| `FileChanged` | filesystem watch noise |
| `WorktreeCreate` / `WorktreeRemove` | git worktree plumbing |
| `Setup` | only fires for `--init` / `--maintenance` CLI flags |
| `TaskCreated` / `TaskCompleted` | internal task tracking |
| `TeammateIdle` | multi-agent teammate coordination, internal |

To add or remove ignores, edit `ignored_events` in
`~/.signal_light/config.json` and save (daemon hot-reloads).

## Priority aggregation

When multiple sessions are active, the daemon picks the highest-priority event
across all of them:

```
blocked > permission > attention > working > idle
```

This means a permission prompt in one window keeps the light red even if
another window starts working in parallel. A normal `Stop` only clears the
emitting session's working state; it does not erase another session's red.

The Web UI's `WINNING` chip shows which session is currently driving the
light.
