# Online leaderboard (Cloudflare Worker + D1)

The Android app queues every finished race (championship total, each championship stage, practice runs) and uploads it
here; the page at `/` shows the best time per player for the championship and for each course, filterable by car and
gearbox. Free Cloudflare plan is enough.

## Deploy

```sh
cd server/leaderboard
npx wrangler login
npx wrangler d1 create rally-leaderboard          # copy the printed database_id into wrangler.toml
npx wrangler d1 execute rally-leaderboard --remote --file=schema.sql
npx wrangler deploy                               # prints https://rally-leaderboard.<you>.workers.dev
```

Then either put that address in `LB_URL` (LauncherActivity.java) and rebuild, or write it into
`Android/data/org.srally2/files/leaderboard_url.txt` on the device. The game shows "Online leaderboard" in its settings
menu once an address is set.

## What is checked

Only plausibility: field formats, minimum times (`MIN_COURSE_TIME`, `MIN_CHAMPIONSHIP_TIME`, game units of 1/6000 s)
and a per-player hourly limit. A modified client can still send invented times. The game is deterministic, so a later
step can upload the inputs of a run and have other clients replay and confirm it.

## API

* `POST /api/submit` `{player, app, results: [{kind, course, courseName, car, carName, gear, mode, name, time, ts}]}`
* `GET /api/boards`
* `GET /api/top?board=championship|course:N[&car=0..7][&gear=AT|MT][&limit=N][&player=ID]`
