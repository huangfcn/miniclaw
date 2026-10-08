# Writing a JS Tool Plugin

MiniClaw tools can be written in plain JavaScript. Plugins run inside
**QuickJS-ng v0.17.0** (vendored in `backend/third-party/quickjs`;
interpreted, no JIT — App Store / Play compliant), are loaded automatically
at startup, and expose their tools to the agent exactly like built-in C++
tools.

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

## Step by step: building a weather plugin

A realistic walkthrough, one step at a time. We'll build a `weather` tool
that answers "how is the weather in Paris?" using the free
[Open-Meteo](https://open-meteo.com/) API (no key needed). Every step ends
with a way to verify it before moving on.

### The environment: what a tool call actually does

Before writing code, know exactly what the host does when the agent calls
your tool — every step below builds on this:

1. **A brand-new QuickJS context is created.** Nothing survives from a
   previous call: no variables, no cached HTTP responses, no module state.
   Your `plugin.js` is re-evaluated from disk on *every* invocation.
2. **Your whole `plugin.js` is evaluated** in that context. Top-level
   `const`/`function` declarations are fine — they exist for the duration of
   this one call and are discarded when it ends.
3. **Exactly one host global is injected: `api`** (fetch, config,
   readFile, writeFile, log, now, workspace). There is no `console`, no
   `require`, no `process`, no timers — anything not on `api` simply does
   not exist in the environment.
4. **The handler is looked up as a global function** by the name from your
   manifest (default: the tool name) and called as `handler(args)` with
   `this === undefined`.
5. **`args` is a plain object whose values are all strings** — the host
   builds it from the LLM's JSON arguments, stringifying every value
   (`{pax: "2"}`). Missing optional keys are simply absent.
6. **The return value is checked:** an exception becomes the tool error the
   agent sees; a returned object/array is rejected (use
   `JSON.stringify`); anything else is coerced to a string and handed to
   the agent as the observation.
7. **The context is freed.** Memory, timers (none anyway), and top-level
   state are gone. If you need state across calls, it must be in a file
   (step 5).

So a plugin is: *a function of (string args, `api`) → string*, re-created
from scratch on every call, with files as its only memory.

### Step 1 — Folder + manifest, stub handler

**What we're doing:** register the tool with the host and prove the whole
pipeline works before writing any real logic. The manifest is what the host
reads at startup to learn that a tool exists and what its arguments look
like; the stub handler proves the host can find and call your function.

Create `~/.miniclaw/plugins/weather/` with two files.

`plugin.json`:

```json
{
  "name": "weather",
  "version": "0.1.0",
  "description": "Current weather for a city.",
  "tools": [
    {
      "name": "weather",
      "description": "Get the current weather for a city.",
      "parameters": {
        "type": "object",
        "properties": {
          "city": { "type": "string", "description": "City name, e.g. 'Paris'" }
        },
        "required": ["city"]
      }
    }
  ]
}
```

`plugin.js` — a stub that proves the pipeline works before any real logic:

```js
function weather(args) {
    return "weather for " + args.city + " (stub)";
}
```

**Verify:** restart `miniclaw` (or run `test_plugins.exe`) and look for
`Loaded JS plugin tool: weather (plugin weather v0.1.0)` in the log — that
line means the manifest parsed and the handler was found at load time. Then
ask the agent "what's the weather in Paris?" — it should call the tool and
show you the stub string. If the tool never appears, the manifest didn't
parse: check the log for a `[plugin:weather]` warning and validate your JSON.

### Step 2 — Validate arguments, fail by throwing

**What we're doing:** harden the handler's input boundary. The LLM chooses
your argument values freely, so treat them as untrusted: wrong types, empty
strings, nonsense city names. Because every value arrives as a string (the
host stringifies the LLM's JSON), you own all parsing and validation.
Throwing is the *designed* failure path — the host catches the exception,
and the message becomes the tool error the agent reads, so it can fix its
arguments and retry. A thrown error is not a crash; it's part of the
conversation.

```js
function weather(args) {
    const city = (args.city || "").trim();
    if (!city) throw new Error("city is required, e.g. 'Paris'");
    return "weather for " + city + " (stub)";
}
```

### Step 3 — Call a real API with `api.fetch`

**What we're doing:** replace the stub with real work. `api.fetch` is the
only network door out of the sandbox: it blocks the current call until the
response arrives (30 s timeout, redirects followed) and hands you back
`{status, body}` — a number and a string, nothing else. `status === 0`
means the request never got an HTTP response at all (DNS failure, timeout,
connection refused); any other number is the real HTTP status code. We
encode both cases as thrown errors so the agent gets an actionable message
either way.

Open-Meteo has two endpoints: geocoding (city → coordinates) and forecast
(coordinates → weather). Both are plain JSON over HTTPS:

```js
// WMO weather code -> phrase (Open-Meteo uses WMO codes)
const WMO = {
    0: "clear sky", 1: "mainly clear", 2: "partly cloudy", 3: "overcast",
    45: "fog", 48: "rime fog",
    51: "light drizzle", 53: "drizzle", 55: "dense drizzle",
    61: "light rain", 63: "rain", 65: "heavy rain",
    71: "light snow", 73: "snow", 75: "heavy snow",
    80: "light showers", 81: "showers", 82: "violent showers",
    95: "thunderstorm", 96: "thunderstorm with hail", 99: "severe thunderstorm"
};
function describe(code) { return WMO[code] || "conditions " + code; }

function getJson(url) {
    const r = api.fetch(url);
    if (r.status === 0) throw new Error("network error contacting " + url);
    if (r.status !== 200) throw new Error("HTTP " + r.status + " from " + url);
    return JSON.parse(r.body);
}

function weather(args) {
    const city = (args.city || "").trim();
    if (!city) throw new Error("city is required, e.g. 'Paris'");

    // 1. city -> coordinates
    const geo = getJson("https://geocoding-api.open-meteo.com/v1/search?count=1&name="
                        + encodeURIComponent(city));
    if (!geo.results || !geo.results.length)
        throw new Error("city not found: " + city);
    const loc = geo.results[0];

    // 2. coordinates -> current conditions
    const f = getJson("https://api.open-meteo.com/v1/forecast?latitude=" + loc.latitude
        + "&longitude=" + loc.longitude
        + "&current=temperature_2m,apparent_temperature,weather_code,wind_speed_10m");
    const c = f.current;

    return "Weather in " + loc.name + ": " + c.temperature_2m + "\u00b0C, " +
           describe(c.weather_code) + ", wind " + c.wind_speed_10m + " km/h";
}
```

**Verify:** ask the agent again. You should get a real temperature. Try a
nonsense city ("Xyzzyville") — the agent should see `city not found:` and
tell you so, instead of crashing or hanging.

### Step 4 — Optional settings via `api.config`

**What we're doing:** make the plugin configurable without code changes.
`api.config(section, key)` reads your host `config.yaml` — the same file
the engine itself uses — so a user can tune behavior by editing one YAML
file. The contract to remember: it returns `""` (empty string) when the
key is unset, never an error. So the *default* behavior must be the empty
string's branch, and a missing optional setting must never throw — only a
setting that is present but invalid should.

Let users pick units in `config.yaml` without touching the plugin:

```yaml
# ~/.miniclaw/config.yaml
weather:
  units: imperial     # default: metric
```

```js
function weather(args) {
    // ... after fetching f.current as c: ...
    const imperial = api.config("weather", "units") === "imperial";
    const t = imperial ? Math.round(c.temperature_2m * 9 / 5 + 32) + "\u00b0F"
                       : Math.round(c.temperature_2m) + "\u00b0C";
    const w = imperial ? Math.round(c.wind_speed_10m * 0.621) + " mph"
                       : Math.round(c.wind_speed_10m) + " km/h";
    return "Weather in " + loc.name + ": " + t + ", " +
           describe(c.weather_code) + ", wind " + w;
}
```

`api.config` returns `""` when the key is unset — code the *default* as the
empty case, and never throw for a missing optional setting.

### Step 5 — Remember the last city (file-backed state)

**What we're doing:** give the tool memory. Recall the environment rule:
every call gets a fresh context and re-evaluates `plugin.js`, so top-level
variables cannot persist — the only thing that survives across calls is
what you write to disk. `api.writeFile`/`api.readFile` are sandboxed to the
workspace (relative paths resolve under it; outside paths are rejected),
and `writeFile` creates parent directories for you. The convention: one
small JSON file namespaced under a folder with your plugin's name, so
plugins never clobber each other.

To make a bare "weather?" repeat the last query:

```js
const STATE = "weather/last.json";   // relative -> <workspace>/weather/last.json

function weather(args) {
    let city = (args.city || "").trim();
    if (!city) {
        const s = api.readFile(STATE);
        if (!s) throw new Error("no city given and no previous query — pass a city");
        city = JSON.parse(s).city;
    }
    api.writeFile(STATE, JSON.stringify({ city: city }));  // created dirs OK
    // ... rest as before ...
}
```

Keep state files small and under a folder named after your plugin.

### Step 6 — Write the description *for the LLM*

**What we're doing:** finish the contract the agent actually reads. The
model never sees your code — it picks tools and fills arguments based solely
on `description` + `parameters` from the manifest, so that text is a prompt,
not documentation for humans. State what the tool does, what it returns,
and any optional-argument behavior. Note that step 5 changed the schema:
`city` is no longer `required`, and the description must say so — if schema
and description disagree, the agent will use the tool wrong (e.g. always
inventing a city, or never passing one).

Final manifest entry:

```json
{
  "name": "weather",
  "description": "Current weather for a city (temperature, conditions, wind). Omit city to repeat the last query. Returns one markdown line.",
  "parameters": {
    "type": "object",
    "properties": {
      "city": { "type": "string", "description": "City name, e.g. 'Paris'. Optional if a previous query exists." }
    }
  }
}
```

### Step 7 — Ship it

**What we're doing:** put the plugin where the host looks. The loader scans
`<workspace>/plugins/` at startup, so "deploying" is just placing the folder
and restarting:

- **Desktop:** the folder lives in `~/.miniclaw/plugins/`; restart
  `miniclaw` to load it. (The Tauri app seeds its first workspace from
  `frontend/src-tauri/resources/workspace/` — that's where the bundled
  `travel` plugin comes from.)
- **Mobile:** the workspace is the app's private directory; place the folder
  at `<workspace>/plugins/weather/` (the same location the file tools see).
- Iterate on pure logic in Node with a stubbed `api` (see Testing below);
  the JS is portable, only the `api.*` calls are host-specific.

The finished plugin is ~70 lines. When yours outgrows that, look at the
reference implementation.

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
  result. Markdown works well (tables, bullets). Objects and arrays are
  **rejected** with an error telling you to `JSON.stringify` them. Numbers
  and booleans are coerced to strings (works, but be explicit), and
  returning `undefined`/`null` silently produces the literal text
  `"undefined"`/`"null"` — a trap, so always return a real string.
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
etc. v0.17.0 tracks modern ES, so recent features work too: iterator
helpers (`toSorted`, `toReversed`, `flatMapToSorted`, `Iterator.zip`),
`Object.groupBy` / `Map.groupBy`, `Array.fromAsync`, and friends.

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

The bundled `travel` plugin (448 lines, source at
`frontend/src-tauri/resources/workspace/plugins/travel/`) is the reference:
multi-action tool (`search` / `rerank`), live API with Basic auth, config
keys, web fallback, ranked markdown tables, and file-backed state.
