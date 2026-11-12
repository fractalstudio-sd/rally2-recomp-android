// Online leaderboard for the Android port: Cloudflare Worker + D1. GPLv3.
//   POST /api/submit   {player, app, results: [{kind, course, courseName, car, carName, gear, mode, name, time, ts}]}
//   GET  /api/boards   boards that have results, with their display names
//   GET  /api/top?board=championship|course:N[&car=K][&gear=AT|MT][&limit=50]   best time per player
//   GET  /             the leaderboard page
// Times are game units (1/6000 s). Results are checked for plausibility only: see README for what that does not stop.

const JSON_HDR = { "content-type": "application/json; charset=utf-8", "access-control-allow-origin": "*" };
const json = (o, status = 200) => new Response(JSON.stringify(o), { status, headers: JSON_HDR });
const clean = (s, max) => String(s ?? "").replace(/[^ -~]/g, "").replace(/[<>&"'\\]/g, "").trim().slice(0, max);

export default {
  async fetch(req, env) {
    const url = new URL(req.url);
    try {
      if (req.method === "OPTIONS") return new Response(null, { headers: { ...JSON_HDR, "access-control-allow-headers": "content-type", "access-control-allow-methods": "GET,POST" } });
      if (url.pathname === "/api/submit" && req.method === "POST") return await submit(req, env);
      if (url.pathname === "/api/top") return await top(url, env);
      if (url.pathname === "/api/boards") return await boards(env);
      if (url.pathname === "/" || url.pathname === "/index.html") return new Response(PAGE, { headers: { "content-type": "text/html; charset=utf-8" } });
      return json({ error: "not found" }, 404);
    } catch (e) {
      return json({ error: "server error" }, 500);
    }
  },
};

async function submit(req, env) {
  const body = await req.json().catch(() => null);
  if (!body || !Array.isArray(body.results)) return json({ error: "bad request" }, 400);
  const player = clean(body.player, 40);
  if (!/^[0-9a-f-]{16,40}$/i.test(player)) return json({ error: "bad player id" }, 400);
  const now = Math.floor(Date.now() / 1000);
  const recent = await env.DB.prepare("SELECT COUNT(*) AS n FROM results WHERE player = ? AND created > ?").bind(player, now - 3600).first();
  const budget = Math.max(0, Number(env.MAX_PER_HOUR || 60) - (recent?.n || 0));
  const minCourse = Number(env.MIN_COURSE_TIME || 180000), minChamp = Number(env.MIN_CHAMPIONSHIP_TIME || 1440000);
  const stmts = []; let rejected = 0;
  for (const r of body.results.slice(0, 50)) {
    const kind = String(r.kind);
    const course = Number.isInteger(r.course) ? r.course : -2;
    const car = Number.isInteger(r.car) ? r.car : -1;
    const time = Number.isInteger(r.time) ? r.time : -1;
    const gear = r.gear === "MT" ? "MT" : r.gear === "AT" ? "AT" : null;
    const name = clean(r.name, 3).toUpperCase();
    let ok = gear && car >= 0 && car <= 7 && /^[A-Z0-9.]{1,3}$/.test(name) && time > 0 && time < 6000 * 3600;
    let board;
    if (kind === "championship") { ok = ok && course === -1 && time >= minChamp; board = "championship"; }
    else if (kind === "stage" || kind === "practice") { ok = ok && course >= 0 && course <= 11 && time >= minCourse; board = "course:" + course; }
    else ok = false;
    if (!ok || stmts.length >= budget) { rejected++; continue; }
    stmts.push(env.DB.prepare("INSERT INTO results (player, name, board, kind, course, course_name, car, car_name, gear, time, ts, created, app) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)")
      .bind(player, name, board, kind, course, clean(r.courseName, 16), car, clean(r.carName, 16), gear, time, Number.isInteger(r.ts) ? r.ts : null, now, clean(body.app, 16)));
  }
  if (stmts.length) await env.DB.batch(stmts);
  return json({ accepted: stmts.length, rejected });
}

async function boards(env) {
  const { results } = await env.DB.prepare(
    "SELECT board, course, MAX(course_name) AS course_name, COUNT(DISTINCT player) AS players FROM results GROUP BY board ORDER BY course").all();
  const { results: cars } = await env.DB.prepare("SELECT car, MAX(car_name) AS car_name FROM results GROUP BY car ORDER BY car").all();
  return json({ boards: results, cars });
}

async function top(url, env) {
  const board = url.searchParams.get("board") || "championship";
  if (!/^(championship|course:\d{1,2})$/.test(board)) return json({ error: "bad board" }, 400);
  const limit = Math.min(200, Math.max(1, Number(url.searchParams.get("limit")) || 50));
  const where = ["board = ?"], args = [board];
  const car = url.searchParams.get("car");
  if (car !== null && car !== "" && /^[0-7]$/.test(car)) { where.push("car = ?"); args.push(Number(car)); }
  const gear = url.searchParams.get("gear");
  if (gear === "AT" || gear === "MT") { where.push("gear = ?"); args.push(gear); }
  // best time of every player (SQLite returns the other columns of the row holding the MIN)
  const sql = `SELECT player, name, MIN(time) AS time, car, car_name, gear, kind, course_name, created FROM results
               WHERE ${where.join(" AND ")} GROUP BY player ORDER BY time ASC LIMIT ?`;
  const { results } = await env.DB.prepare(sql).bind(...args, limit).all();
  const me = url.searchParams.get("player");
  return json({ board, results: results.map((r, i) => ({ rank: i + 1, name: r.name, time: r.time, car: r.car, carName: r.car_name, gear: r.gear,
    kind: r.kind, courseName: r.course_name, date: r.created, you: me ? r.player === me : false })) });
}

const PAGE = `<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Rally Recomp - Leaderboard</title>
<style>
:root{--bg:#0d1117;--fg:#e6edf3;--mut:#8b949e;--acc:#f0a830;--row:#161b22;--you:#2d3b1f}
body{margin:0;background:var(--bg);color:var(--fg);font:16px/1.4 system-ui,sans-serif}
main{max-width:760px;margin:0 auto;padding:16px}
h1{font-size:22px;margin:8px 0 12px}
.bar{display:flex;flex-wrap:wrap;gap:8px;margin-bottom:12px}
select,button{background:var(--row);color:var(--fg);border:1px solid #30363d;border-radius:8px;padding:10px 12px;font-size:16px}
table{width:100%;border-collapse:collapse}
td,th{padding:10px 6px;text-align:left;white-space:nowrap}
th{color:var(--mut);font-weight:500;font-size:13px;text-transform:uppercase}
tr:nth-child(even) td{background:var(--row)}
tr.you td{background:var(--you)}
td.t{font-variant-numeric:tabular-nums;color:var(--acc);font-weight:600}
td.r{width:2.5em;color:var(--mut)}
.mut{color:var(--mut);font-size:13px}
</style></head><body><main>
<h1>Leaderboard</h1>
<div class="bar"><select id="board"></select><select id="car"><option value="">All cars</option></select>
<select id="gear"><option value="">AT + MT</option><option>AT</option><option>MT</option></select></div>
<table><thead><tr><th>#</th><th>Name</th><th>Time</th><th>Car</th></tr></thead><tbody id="rows"></tbody></table>
<p class="mut" id="msg"></p>
</main><script>
const q=new URLSearchParams(location.search), me=q.get("player")||"";
const fmt=t=>{const ms=Math.round(t/6),m=Math.floor(ms/60000),s=Math.floor(ms/1000)%60;return m+":"+String(s).padStart(2,"0")+"."+String(ms%1000).padStart(3,"0")};
const nice=s=>s?s.replace(/_/g," ").toLowerCase().replace(/\\b\\w/g,c=>c.toUpperCase()):"";
const $=id=>document.getElementById(id);
async function init(){
  const b=await (await fetch("/api/boards")).json();
  const sel=$("board"); sel.innerHTML='<option value="championship">Championship</option>';
  for(const x of b.boards) if(x.board!=="championship"){const n=x.course>=6?nice(x.course_name)+" (alt)":nice(x.course_name)||("Course "+x.course);
    sel.insertAdjacentHTML("beforeend",'<option value="'+x.board+'">'+n+'</option>')}
  for(const c of b.cars) $("car").insertAdjacentHTML("beforeend",'<option value="'+c.car+'">'+nice(c.car_name||("Car "+c.car))+'</option>');
  for(const id of ["board","car","gear"]) $(id).onchange=load;
  load();
}
async function load(){
  const p=new URLSearchParams({board:$("board").value,car:$("car").value,gear:$("gear").value,player:me,limit:100});
  const r=await (await fetch("/api/top?"+p)).json(); const rows=$("rows"); rows.innerHTML="";
  for(const x of r.results||[]) rows.insertAdjacentHTML("beforeend",'<tr class="'+(x.you?"you":"")+'"><td class="r">'+x.rank+'</td><td>'+x.name+(x.you?' <span class="mut">(you)</span>':'')+'</td><td class="t">'+fmt(x.time)+'</td><td>'+nice(x.carName)+' <span class="mut">'+x.gear+'</span></td></tr>');
  $("msg").textContent=(r.results||[]).length?"Best time per player.":"No times yet.";
}
init();
</script></body></html>`;
