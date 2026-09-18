# AI Coding log availability statement

This file is a disclosure note. It is not an AI Coding transcript and is not
intended to replace the JSONL session logs requested by the competition.

The official contest-format AI Coding export was not available when the
submission snapshot was prepared. The contest scaffold contained only an
example account and example JSONL, so those template files were removed rather
than submitted as development evidence.

The original Codex Desktop rollout records were later recovered from the local
Codex session store. A selected, explicitly labeled partial conversion is now
kept under `logs/Q1KO-Official/`, with a manifest warning that it is not a
complete session export. The preview and provenance files remain under
`docs/supporting-evidence/codex/`.

No conversation, tool output, timestamp, or hidden encrypted reasoning has
been invented. The supplementary conversion preserves source turn IDs,
timestamps, visible messages, tool calls and tool outputs, with only known
credential patterns and the local user-home path redacted.

If a complete authentic export is later recovered and accepted for official
submission, it should replace or supplement the partial entry in the official
structure below and this statement should be updated:

~~~text
logs/
  <github-login>/
    manifest.json
    YYYY-MM-DD/
      codex__<session-id>.jsonl
~~~

Until then, this repository should be evaluated with the understanding that
valid AI Coding transcript evidence is unavailable.
