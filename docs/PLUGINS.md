# Writing a JS Tool Plugin

MiniClaw tools can be written in plain JavaScript. Plugins run inside
**QuickJS** (interpreted, no JIT — App Store / Play compliant), are loaded
automatically at startup, and expose their tools to the agent exactly like
built-in C++ tools.

A plugin has **no access to the host** except through the `api` object
documented below: no filesystem, no network, no process, no `require`.

## Layout

```
<workspace>/plugins/<name>/
├── plugin.json    # manifest (required)
└── plugin.js      # JS source (default; override with "file" in manifest)
```

`<workspace>` is the MiniClaw workspace directory
(`~/.miniclaw` by default). Drop a folder in, restart `miniclaw`, and the
tool appears. No compilation, no registration code.

## Minimal example

`plugins/hello/plugin.json`:

```json
{
  "name": "hello",
  "version": "1.0.0",
  "description": "Greets people.",
  "tools": [
    {
      "name": "greet",
      "description": "Return a friendly greeting for the given name.",
      "handler": "greet",
      "parameters": {
        "type": "object",
        "properties": {
          "name": { "type": "string", "description": "Who to greet" }
        },
        "required": ["name"]
      }
    }
  ]
}
```

`plugins/hello/plugin.js`:

```js
function greet(args) {
    return "Hello, " + args.name + "!";
}
```

That's it. The agent can now call `greet({name: "Ada"})`.

## Manifest reference (`plugin.json`)

| Field | Required | Meaning |
|---|---|---|
| `name` | yes | Plugin name (used in log lines `[plugin:<name>]`) |
| `version` | no | Free-form version string |
| `description` | no | Human-readable plugin description |
| `file` | no | JS source file name (default `plugin.js`) |
| `tools` | yes | Array of tool specs; at least one valid entry |

Per tool:

| Field | Required | Meaning |
|---|---|---|
| `name` | yes | Tool name as seen by the agent |
| `description` | no | What the tool does — **write this for the LLM**: when to use it, what it returns |
| `handler` | no | Global JS function to call (default: same as tool name) |
| `parameters` | no | OpenAI function-calling schema (`{type:"object", properties:{...}, required:[...]}`). Default: empty object |

Invalid manifests are skipped with a warning in the log — they never crash
the host.

## Handler contract

Your handler is a **global function** called as `handler(args)`:

- **`args` is a plain object.** Every value is a **string**, even numbers
  (`{pax: "2"}`) — parse what you need: `const n = parseInt(args.pax, 10)`.
  Missing optional args are simply absent.
- **Return a string.** That string is handed to the agent as the tool
  result. Markdown works well (tables, bullets). Non-string return values
  are rejected — if you want to return structured data, `JSON.stringify` it.
- **Fail by throwing.** `throw new Error("date must be YYYY-MM-DD")` — the
  message is reported to the agent so it can fix its arguments and retry.
  Never return error text as a normal result unless you mean "this is the
  answer".

```js
function mytool(args) {
    if (!args.date) throw new Error("date is required");
    // ... work ...
    return "| A | B |\n|---|---|\n| 1 | 2 |";   // string, markdown OK
}
```

## The `api` object

The only host capabilities visible to plugin code:

### `api.fetch(url, opts) → {status, body}`

HTTP request. Blocks until complete (30 s timeout, follows redirects).

- `opts.method` — `"GET"` (default), `"POST"`, `"PUT"`, `"PATCH"`, or any
  custom verb.
- `opts.headers` — array of `"Name: value"` strings.
- `opts.body` — request body string (POST/PUT/PATCH).
- Returns `{status, body}`: HTTP status code (`0` = transport error, e.g.
  DNS failure) and the response body as a string.

```js
const r = api.fetch("https://api.example.com/v1/items", {
    method: "GET",
    headers: ["Accept: application/json", "Authorization: Bearer " + token],
});
if (r.status !== 200) throw new Error("API error " + r.status);
const data = JSON.parse(r.body);
```

### `api.config(section, key) → string`

Read a value from `config.yaml`, e.g. `api.config("travel", "amadeus_client_id")`.
Returns `""` when unset — treat empty as "not configured" and degrade
gracefully (or throw a helpful error telling the user what to configure).

### `api.readFile(path) → string | undefined` / `api.writeFile(path, content) → bool`

File I/O **sandboxed to the workspace**:

- Relative paths resolve against the workspace root.
- Absolute paths outside the workspace are rejected (read returns
  `undefined`, write returns `false`).
- `writeFile` creates parent directories automatically.
- Both treat empty as failure — check truthiness: `const s = api.readFile(p); if (!s) ...`

### `api.log(msg)` — log a line as `[plugin:<name>] msg` (info level)

### `api.now() → string` — local time, ISO 8601 (`2026-10-07T07:37:30`)

### `api.workspace() → string` — absolute workspace path

## State between calls: use files

**Each tool call runs in a fresh JS context.** The plugin source is
re-evaluated on every invocation, so top-level `let`/`var` state does **not**
persist between calls. If a tool needs memory across calls (e.g. "re-rank
the last search"), persist it to the workspace with `writeFile`/`readFile`:

```js
const STATE = "mytool/state.json";   // relative → <workspace>/mytool/state.json

function save(state) { api.writeFile(STATE, JSON.stringify(state)); }

function load() {
    const s = api.readFile(STATE);
    return s ? JSON.parse(s) : null;
}
```

This is exactly what the `travel` plugin does (`travel/last.json` holds the
last search so `action=rerank` works). Keep state files small and namespaced
under a folder with your plugin's name.

## What QuickJS gives you (and doesn't)

**Available:** standard ECMAScript builtins — `JSON`, `Math`, `Date`,
`RegExp`, `Array`, `Map`, `Set`, `Promise` (but see note), template
literals, arrow functions, destructuring, spread, `String.prototype.*`,
etc.

**Not available:**

- No `require` / `import` / modules — one flat script per plugin.
- No `btoa` / `atob` — implement base64 yourself if needed (the travel
  plugin has a compact ASCII implementation you can copy).
- No DOM, no `fetch`, no timers — use `api.fetch` and `api.now()`.
- `Promise` exists but nothing is async: write **synchronous** code. The
  handler must return its string before it finishes; there is no event loop
  to resolve later promises.

**Loop budget:** execution is interrupted after ~50 M interpreter
operations. Infinite loops don't hang the host — they abort the call with an
error. Still, keep hot loops tight.

## Conventions that make good tools

1. **One plugin folder per capability**, one or more tools inside. Tool
   names are global across all plugins — don't collide with built-ins
   (`web_search`, `terminal`, `travel`, ...).
2. **Description is the prompt.** The LLM chooses your tool based on
   `description` + `parameters`. State required args, defaults, and what
   the output looks like.
3. **Fail loudly with actionable errors** (`throw new Error("set
   mytool.api_key in config")`) — the agent can relay that to the user or
   adjust arguments.
4. **Return markdown**, not raw JSON dumps — it's read by an LLM and shown
   to the user.
5. **Degrade gracefully** when optional integrations (API keys) are missing:
   fall back to a weaker source or explain what's missing.
6. **Namespaced state files** (`<plugin>/state.json`) as shown above.

## Testing

- `backend/tests/test_plugins.cpp` → builds `test_plugins.exe`; it loads all
  plugins from the workspace and exercises the travel tool end-to-end:
  ```sh
  cd backend/build && ninja test_plugins
  PATH="/c/msys64/ucrt64/bin:$PATH" ./test_plugins.exe   # MSYS2: needs ucrt64 bin on PATH
  ```
- For fast iteration on pure logic, copy your functions into a Node.js
  script and stub `api` (`global.api = { fetch: () => ({status:200, body:"{}"}), ... }`) —
  the JS itself is portable; only the `api` calls are host-specific.
- Watch the log for `[plugin:<name>]` lines and `Loaded JS plugin tool:` on
  startup to confirm your manifest parsed.

## Reference implementation

`~/.miniclaw/plugins/travel/` (448 lines) is the reference plugin:
multi-action tool (`search` / `rerank`), live API with Basic auth, config
keys, web fallback, ranked markdown tables, and file-backed state.
