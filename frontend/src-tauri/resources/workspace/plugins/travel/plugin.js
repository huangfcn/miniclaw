// miniclaw travel tool — JS plugin.
//
// Search flight or train tickets, compare prices and times, return a
// ranked top-N table.
//    search  — Amadeus live offers (flights: v2/shopping/flight-offers,
//              trains: v1/shopping/train-offers), ranked by
//              price/duration/stops, with a Brave web fallback when the
//              Amadeus keys are not configured.
//    rerank  — re-sort the last saved search (sort: price|duration|balance).
//
// Host capabilities (api):
//    api.fetch(url, {method, headers, body}) -> {status, body}
//    api.config(section, key) -> string
//    api.readFile(path) / api.writeFile(path, content)  (workspace-sandboxed)
//    api.log(msg), api.now(), api.workspace()

// ─── config / helpers ─────────────────────────────────────────────────────
function cfg(key) { return api.config("travel", key) || ""; }

function base64(str) {
     // Amadeus Basic auth = base64(clientId:clientSecret). QuickJS has no
     // btoa, so build it manually (ASCII input only).
     const CH = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
     let out = "";
     for (let i = 0; i < str.length; i += 3) {
          const a = str.charCodeAt(i);
          const b = i + 1 < str.length ? str.charCodeAt(i + 1) : -1;
          const c = i + 2 < str.length ? str.charCodeAt(i + 2) : -1;
          out += CH.charAt(a >> 2);
          out += CH.charAt(((a & 3) << 4) | (b < 0 ? 0 : b >> 4));
          out += b < 0 ? "=" : CH.charAt(((b & 15) << 2) | (c < 0 ? 0 : c >> 6));
          out += c < 0 ? "=" : CH.charAt(c & 63);
     }
     return out;
}

function http_get(url, headers) {
     const h = headers ? [].concat(headers) : ["Accept: application/json"];
     return api.fetch(url, { method: "GET", headers: h });
}

function http_post(url, json, headers) {
     const h = ["Content-Type: application/json",
                "Accept: application/json"].concat(headers || []);
     return api.fetch(url, { method: "POST", headers: h, body: json });
}

function jparse(s) {
     if (!s) throw new Error("empty response");
     return JSON.parse(s);
}

function to_num(v, d) {
     const n = typeof v === "number" ? v : parseFloat(v);
     return isNaN(n) ? d : n;
}

// "PT2H35M" / "2H 35MIN" style durations -> minutes (best effort).
function dur_minutes(v) {
     if (typeof v === "number") return v;
     if (typeof v !== "string") return 0;
     const s = v.toUpperCase();
     if (s.indexOf("P") !== -1) {
          let m = 0, r;
          if ((r = s.match(/(\d+)H/))) m += parseInt(r[1], 10) * 60;
          if ((r = s.match(/(\d+)M/))) m += parseInt(r[1], 10);
          if (m > 0) return m;
     }
     if ((r0 = s.match(/(\d+)\s*(?:H|HR|HOURS?)/i))) m0 = parseInt(r0[1], 10) * 60;
     if ((r1 = s.match(/(\d+)\s*(?:MIN|MINS?)/i))) m1 = parseInt(r1[1], 10);
     if (m0 || m1) return m0 + m1;
     return 0;
     var r0, r1, m0, m1;
}

function fmt_min(min) {
     min = Math.round(min);
     if (min <= 0) return "?";
     const h = Math.floor(min / 60), m = min % 60;
     return h > 0 ? (m > 0 ? h + "h" + m + "m" : h + "h") : m + "m";
}

// ─── Amadeus OAuth ────────────────────────────────────────────────────────
let _amadeus_token = "";
let _amadeus_expires = 0;

function amadeus_token() {
     const id = cfg("amadeus_client_id");
     const secret = cfg("amadeus_client_secret");
     if (!id || !secret)
          throw new Error("Amadeus keys not configured (travel.amadeus_client_id / travel.amadeus_client_secret)");
     const now = Date.now() / 1000;
     if (_amadeus_token && now < _amadeus_expires - 60) return _amadeus_token;
     const base = (cfg("amadeus_base_url") || "https://test.api.amadeus.com")
                 .replace(/\/+$/, "");
     const r = http_post(base + "/v1/security/oauth2/token", "",
                          ["Authorization: Basic " + base64(id + ":" + secret)]);
     if (r.status < 200 || r.status >= 300)
          throw new Error("Amadeus auth failed (HTTP " + r.status + ")");
     const j = jparse(r.body);
     if (!j.access_token) throw new Error("Amadeus auth: no access_token");
     _amadeus_token = j.access_token;
     _amadeus_expires = now + to_num(j.expires_in, 1800);
     return _amadeus_token;
}

// ─── Amadeus flight offers ────────────────────────────────────────────────
function search_flights(origin, dest, date, pax, maxn) {
     const base = (cfg("amadeus_base_url") || "https://test.api.amadeus.com")
                  .replace(/\/+$/, "");
     const url = base + "/v2/shopping/flight-offers?originLocationCode=" +
                 encodeURIComponent(origin) +
                 "&destinationLocationCode=" + encodeURIComponent(dest) +
                 "&departureDate=" + date + "&adults=" + pax +
                 "&max=" + maxn;
     const r = http_get(url, ["Authorization: Bearer " + amadeus_token()]);
     if (r.status === 401) throw new Error("Amadeus auth failed (401)");
     if (r.status < 200 || r.status >= 300)
          throw new Error("Amadeus flights failed (HTTP " + r.status + ")");
     const j = jparse(r.body);
     const out = [];
     if (!j.data) return out;
     for (let i = 0; i < j.data.length; i++) {
          const o = j.data[i];
          const offer = {
             price: to_num(o.price && o.price.total, 0),
             currency: (o.price && o.price.currency) || "",
             duration_min: 0,
             stops: 0,
             origin: "", dest: "",
             depart: "", arrive: "",
             carrier: "",
          };
          if (o.duration && typeof o.duration.total === "number")
              offer.duration_min = Math.round(o.duration.total);
          // itinerary: first traveler, first itinerary
          let segs = null;
          if (o.travelers && o.travelers[0] &&
              o.travelers[0].itineraries && o.travelers[0].itineraries[0] &&
              o.travelers[0].itineraries[0].segments)
              segs = o.travelers[0].itineraries[0].segments;
          if (!segs || segs.length === 0) continue;
          const first = segs[0], last = segs[segs.length - 1];
          if (first.departure) {
              offer.origin = first.departure.airportCode || "";
              offer.depart = (first.departure.dateTime || "").replace("T", " ");
          }
          if (last.arrival) {
              offer.dest = last.arrival.airportCode || "";
              offer.arrive = (last.arrival.dateTime || "").replace("T", " ");
          }
          if (segs[0].carriers && segs[0].carriers[0])
              offer.carrier = segs[0].carriers[0].carrierCode || "";
          offer.stops = Math.max(0, segs.length - 1);
          // fallback duration from segment timestamps
          if (offer.duration_min <= 0 && first.departure && last.arrival &&
              first.departure.dateTime && last.arrival.dateTime) {
              const t0 = Date.parse(first.departure.dateTime);
              const t1 = Date.parse(last.arrival.dateTime);
              if (!isNaN(t0) && !isNaN(t1) && t1 > t0)
                  offer.duration_min = Math.round((t1 - t0) / 60000);
          }
          out.push(offer);
     }
     return out;
}

// ─── Amadeus train offers ─────────────────────────────────────────────────
function search_trains(origin, dest, date, pax, maxn) {
     const base = (cfg("amadeus_base_url") || "https://test.api.amadeus.com")
                  .replace(/\/+$/, "");
     const body = JSON.stringify({
          departure: { departureDate: date },
          arrival: { arrivalDate: date },
          adults: pax,
          max: maxn,
     });
     const r = http_post(base + "/v1/shopping/train-offers", body,
                          ["Authorization: Bearer " + amadeus_token()]);
     if (r.status === 401) throw new Error("Amadeus auth failed (401)");
     if (r.status < 200 || r.status >= 300)
          throw new Error("Amadeus trains failed (HTTP " + r.status + ")");
     const j = jparse(r.body);
     const out = [];
     if (!j.data) return out;
     for (let i = 0; i < j.data.length; i++) {
          const o = j.data[i];
          const offer = {
             price: to_num(o.price && o.price.total, 0),
             currency: (o.price && o.price.currency) || "",
             duration_min: 0,
             stops: 0,
             origin: "", dest: "",
             depart: "", arrive: "",
             carrier: "",
          };
          if (o.duration) {
              if (typeof o.duration.total === "number")
                  offer.duration_min = Math.round(o.duration.total);
              else offer.duration_min = dur_minutes(o.duration.text);
          }
          let segs = null, jt = null;
          if (o.travelers && o.travelers[0] &&
              o.travelers[0].itineraries && o.travelers[0].itineraries[0]) {
              jt = o.travelers[0].itineraries[0];
              if (jt.segments) segs = jt.segments;
          }
          if (jt) {
              if (jt.departureDateTime)
                  offer.depart = String(jt.departureDateTime).replace("T", " ");
              if (jt.arrivalDateTime)
                  offer.arrive = String(jt.arrivalDateTime).replace("T", " ");
          }
          if (segs && segs.length > 0) {
              const first = segs[0], last = segs[segs.length - 1];
              if (first.departure && first.departure.city)
                  offer.origin = first.departure.city.cityName || "";
              if (last.arrival && last.arrival.city)
                  offer.dest = last.arrival.city.cityName || "";
              if (segs[0].carriers && segs[0].carriers[0])
                  offer.carrier = segs[0].carriers[0].carrierCode || "";
              offer.stops = Math.max(0, segs.length - 1);
          }
          out.push(offer);
     }
     return out;
}

// ─── Brave web fallback (no Amadeus keys) ─────────────────────────────────
function brave_fallback(origin, dest, date, transport, maxn) {
     const key = api.config("web", "brave_api_key") ||
                 api.config("search", "brave_api_key");
     if (!key)
          return {
              note: "Amadeus keys not configured and no Brave key — no live offers available. Set travel.amadeus_client_id / travel.amadeus_client_secret (free Amadeus test keys) in config.",
          };
     const q = transport === "trains"
          ? "train tickets " + origin + " to " + dest + " " + date
          : "flights " + origin + " to " + dest + " " + date;
     const url = "https://api.search.brave.com/res/v1/web/search?q=" +
                 encodeURIComponent(q) + "&count=" + maxn;
     const r = http_get(url, [
          "Accept: application/json",
          "X-Subscription-Token: " + key,
     ]);
     if (r.status < 200 || r.status >= 300)
          return { note: "web fallback failed (HTTP " + r.status + ")" };
     let j;
     try { j = jparse(r.body); } catch (e) {
          return { note: "web fallback: unparseable response" };
     }
     const results = (j && j.web && j.web.results) || [];
     const rows = [];
     for (let i = 0; i < results.length && i < maxn; i++) {
          const w = results[i];
          rows.push({
             title: w.title || "",
             url: w.url || "",
             desc: (w.description || "").slice(0, 200),
          });
     }
     return { note: "Amadeus keys not configured — showing indicative web results (set travel.amadeus_* for live prices).",
              web: rows };
}

// ─── ranking + rendering ──────────────────────────────────────────────────
function rank(offers, sort) {
     if (!offers.length) return [];
     let pmin = Infinity, pmax = -Infinity, dmin = 0, dmax = 0;
     for (const o of offers) {
          if (o.price > 0) { pmin = Math.min(pmin, o.price); pmax = Math.max(pmax, o.price); }
          dmin = Math.min(dmin, o.duration_min || 9999);
          dmax = Math.max(dmax, o.duration_min || 0);
     }
     const scored = offers.map(function (o) {
          const ps = o.price > 0 && pmax > pmin ? (o.price - pmin) / (pmax - pmin) : 0;
          const ds = dmax > dmin ? (o.duration_min - dmin) / (dmax - dmin) : 0;
          const ss = Math.min(o.stops || 0, 3) / 3;
          let score;
          if (sort === "price") score = ps;
          else if (sort === "duration") score = ds;
          else score = 0.45 * ps + 0.35 * ds + 0.2 * ss;
          return { offer: o, score: score };
     });
     scored.sort(function (a, b) {
          if (sort === "duration")
              return (a.offer.duration_min || 0) - (b.offer.duration_min || 0);
          if (sort === "price")
              return (a.offer.price || 1e9) - (b.offer.price || 1e9);
          return a.score - b.score;
     });
     return scored;
}

function render(offers, sort, topN, origin, dest, date, transport) {
     if (!offers.length)
          return "No offers found for " + origin + " → " + dest + " on " + date + ".";
     const scored = rank(offers, sort);
     const top = scored.slice(0, topN);
     const cur = offers[0].currency || cfg("currency") || "";
     const o = [];
     o.push("## " + (transport === "trains" ? "Train" : "Flight") +
             " options: " + origin + " → " + dest + " on " + date);
     o.push("");
     o.push("| # | Price | Duration | Stops | Route | Times |");
     o.push("|---|-------|----------|-------|-------|-------|");
     for (let i = 0; i < top.length; i++) {
          const r = top[i];
          const price = r.offer.price > 0
              ? r.offer.price.toFixed(0) + " " + (r.offer.currency || cur)
              : "?";
          const route = (r.offer.origin || origin) + " → " +
                        (r.offer.dest || dest) +
                        (r.offer.carrier ? " (" + r.offer.carrier + ")" : "");
          const times = (r.offer.depart || "") + " → " + (r.offer.arrive || "");
          o.push("| " + (i + 1) + " | " + price + " | " +
                 fmt_min(r.offer.duration_min) + " | " +
                 (r.offer.stops || 0) + " | " + route + " | " +
                 (times || "-") + " |");
     }
     // highlights
     let cheapest = top[0], fastest = top[0];
     for (const r of scored) {
          if (r.offer.price > 0 && (cheapest.offer.price <= 0 ||
              r.offer.price < cheapest.offer.price)) cheapest = r;
          if (r.offer.duration_min > 0 &&
              (fastest.offer.duration_min <= 0 ||
               r.offer.duration_min < fastest.offer.duration_min)) fastest = r;
     }
     o.push("");
     o.push("**Cheapest:** " + cheapest.offer.price.toFixed(0) + " " +
             (cheapest.offer.currency || cur) + " — " +
             fmt_min(cheapest.offer.duration_min) + ", " +
             (cheapest.offer.stops || 0) + " stop(s)");
     if (fastest !== cheapest)
          o.push("**Fastest:** " + fmt_min(fastest.offer.duration_min) + ", " +
                 fastest.offer.price.toFixed(0) + " " +
                 (fastest.offer.currency || cur));
     o.push("");
     o.push("Best value (price + duration + stops): #1 above.");
     return o.join("\n");
}

// ─── last-search persistence (for rerank) ─────────────────────────────────
function save_last(payload) {
     try { api.writeFile("travel/last.json", JSON.stringify(payload)); }
     catch (e) { api.log("could not save last travel search: " + e); }
}

function load_last() {
     const s = api.readFile("travel/last.json");
     if (!s) return null;
     try { return JSON.parse(s); } catch (e) { return null; }
}

// ─── tool handler ─────────────────────────────────────────────────────────
function travel(args) {
     const action = args.action || "search";
     const sort = args.sort || "balance";
     const topN = Math.min(Math.max(parseInt(args.top, 10) || 5, 1), 10);

     if (action === "rerank") {
          const last = load_last();
          if (!last || !last.offers)
              return "Error: no previous travel search to rerank (run action=search first).";
          const rendered = render(last.offers, sort, topN, last.origin,
                                   last.dest, last.date, last.transport);
          return "Reranked last search (" + sort + "):\n\n" + rendered;
     }

     const origin = args.origin || "";
     const dest = args.destination || "";
     const date = args.date || "";
     if (!origin || !dest || !date)
          return "Error: origin, destination and date (YYYY-MM-DD) are required.";
     const transport = args.transport === "trains" ? "trains" : "flights";
     const pax = Math.max(1, parseInt(args.pax, 10) || 1);
     const maxn = Math.min(Math.max(parseInt(args.max, 10) || 20, 1), 50);

     let offers = null;
     try {
          offers = transport === "trains"
              ? search_trains(origin, dest, date, pax, maxn)
              : search_flights(origin, dest, date, pax, maxn);
     } catch (e) {
          api.log("Amadeus search failed: " + e);
          return travel_fallback_result(origin, dest, date, transport, maxn,
                                         sort, topN);
     }

     // optional outbound + return
     let out_text;
     if (args.return_date) {
          let back = null;
          try {
              back = transport === "trains"
                  ? search_trains(dest, origin, args.return_date, pax, maxn)
                  : search_flights(dest, origin, args.return_date, pax, maxn);
          } catch (e) {
              api.log("return search failed: " + e);
          }
          out_text = render(offers, sort, topN, origin, dest, date, transport) +
                     "\n\n" +
                     render(back || [], sort, topN, dest, origin,
                             args.return_date, transport);
          save_last({
              origin: origin, dest: dest, date: date, transport: transport,
              sort: sort, offers: offers.concat(back || []),
          });
     } else {
          out_text = render(offers, sort, topN, origin, dest, date, transport);
          save_last({
              origin: origin, dest: dest, date: date, transport: transport,
              sort: sort, offers: offers,
          });
     }
     return out_text;
}

function travel_fallback_result(origin, dest, date, transport, maxn,
                                  sort, topN) {
     const fb = brave_fallback(origin, dest, date, transport, Math.min(maxn, 5));
     const o = [];
     if (fb.web && fb.web.length) {
          o.push("## " + (transport === "trains" ? "Train" : "Flight") +
                   " search: " + origin + " → " + dest + " on " + date);
          o.push("");
          o.push("> " + fb.note);
          o.push("");
          for (let i = 0; i < fb.web.length; i++) {
              const w = fb.web[i];
              o.push((i + 1) + ". **" + w.title + "**\n   " + w.url +
                      (w.desc ? "\n   " + w.desc : ""));
          }
          o.push("");
          o.push("(Indicative results — configure Amadeus keys in the travel " +
                   "config section for exact prices and times.)");
          save_last({
              origin: origin, dest: dest, date: date, transport: transport,
              sort: sort, offers: [],
          });
     } else {
          o.push("## Travel search: " + origin + " → " + dest + " on " + date);
          o.push("");
          o.push("No live offers available: " + fb.note);
     }
     return o.join("\n");
}
