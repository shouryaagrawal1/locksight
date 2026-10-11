# LockSight JSON output (`--json FILE`)

```
locksight run --json events.jsonl ./my_program
```

LockSight writes **one JSON object per line** (JSON Lines) to the file, next to
the normal text report. `--log`, `--quiet`, colors and the exit status are not
affected: exit status 3 still means "a problem was reported". The file is
truncated when `locksight run` starts. If you set `LOCKSIGHT_JSON=FILE` yourself
instead of using the launcher, events are appended.

Every line has these fields:

| field | meaning |
|---|---|
| `schema` | always `"locksight/1"`; changes only if the format changes incompatibly |
| `event` | `potential_deadlock`, `self_deadlock`, `note`, `strict_stop` or `summary` |
| `pid` | process id (a script that starts several programs gives one stream per pid) |
| `time` | UTC time, ISO 8601 with milliseconds |

Events are written **at the moment they happen**, not at exit. If the program
freezes or is killed, the problems found so far are in the file; a missing
`summary` event means the program did not exit normally.

## `potential_deadlock`

```json
{"schema":"locksight/1","event":"potential_deadlock","pid":219,
 "time":"2026-10-10T14:14:27.302Z","number":1,"cycle_length":2,"truncated":false,
 "locks":[{"id":2,"kind":"mutex","address":"0x55c47ba21070",
           "first_used_at":{"pc":"0x55c47ba1e470","file":"bank_transfer.c","line":23,"function":"transfer"}}, ...],
 "steps":[{"thread":2,
           "holds":{"lock":2,"mode":"exclusive","acquired_at":{...}},
           "requests":{"lock":1,"mode":"exclusive","requested_at":{...},"called_from":[{...}]}}, ...],
 "fix":"make every thread take these locks in the same order"}
```

* `number`: the warning number within this process.
* `cycle_length`: real length of the cycle; `truncated` is true if more than 16
  steps exist (only the first 16 are listed).
* `locks[].id` / `steps[].holds.lock` / `steps[].requests.lock` use the same
  numbers as `lock#N` in the text report. `kind` is `mutex`, `rwlock` or `semaphore`.
* Each step reads: *thread T holds lock `holds.lock` and asks for lock
  `requests.lock`*. `mode` is `exclusive`, or `shared` for a read lock.
* Locations: `{"pc","file","line","function"}`. Without debug info (`-g`),
  `file`, `line` and `function` are `null` and a `text` field explains.
* If a very large event does not fit LockSight's buffer (128 KB), it is replaced by
  `{"schema":..,"event":"potential_deadlock","pid":..,"time":..,"number":N,"details_truncated":true}`.

## `self_deadlock`

`number`, `thread`, `lock` (same object as in `locks[]`), `held_at`,
`requested_at`, `called_from` (list of locations), `fix`.

## `note`

Currently only `"note":"semaphore_signaling"`: a semaphore created with
`sem_init(.., 1)` was posted by a thread that never waited on it, so LockSight
treats it as a signal and stops tracking it. Fields: `thread`, `semaphore`
(address), `posted_at`, `message`. A note is **not** a problem.

## `strict_stop`

Written when `--strict` stops the program: `exit_status` (3).

## `summary`

Written at normal exit: `threads`, `locks_seen`, `locks_tracked`,
`lock_order_edges`, `potential_deadlocks`, `self_deadlocks`, `semaphore_notes`,
`table_limit_locks_dropped`, `table_limit_edges_dropped`, `search_gave_up`
(the last three should be 0), and `result`: `problems_found` or `no_problems`.
A process that never used a lock writes no summary.

## Examples with `jq`

```
# number of problems found
jq -s '[.[] | select(.event=="potential_deadlock" or .event=="self_deadlock")] | length' events.jsonl

# where each deadlock cycle comes from: file:line of every request
jq -r 'select(.event=="potential_deadlock") | .steps[] | "\(.thread) \(.requests.requested_at.file):\(.requests.requested_at.line)"' events.jsonl

# one-line verdict per process
jq -r 'select(.event=="summary") | "\(.pid) \(.result)"' events.jsonl
```

## Limits worth knowing

* Lines are written with a single `write()` to a file opened for append, so
  concurrent processes writing the same file do not mix inside a line on local
  file systems; use one file per run anyway.
* The JSON is built without allocating memory and without a JSON library; text
  is escaped and invalid UTF-8 bytes in file names become `?`.
