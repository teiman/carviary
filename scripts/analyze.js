// analyze.js -- reads profile.json produced by the `profile <demo>` command
// and emits report.html: a human-readable dashboard AND an AI-ingestible
// dense summary (machine-readable JSON + plain-text digest + automated
// heuristics). Run via `npm run analyze`.

const fs = require("fs");
const path = require("path");

const CARVIARY = path.resolve(__dirname, "..");
const ROOT_INPUT = path.join(CARVIARY, "profile.json");
const DATA_DIR = path.join(CARVIARY, "data");
const OUTPUT = path.join(CARVIARY, "report.html");

// ---------------------------------------------------------------------------
// load: prefer ./profile.json, else fall back to the newest
// data/profile_*.json produced by scripts/profile.js
// ---------------------------------------------------------------------------
function resolveInput() {
  if (fs.existsSync(ROOT_INPUT)) return ROOT_INPUT;
  if (!fs.existsSync(DATA_DIR)) return null;
  const files = fs
    .readdirSync(DATA_DIR)
    .filter((n) => n.startsWith("profile_") && n.endsWith(".json"))
    .map((n) => path.join(DATA_DIR, n));
  if (!files.length) return null;
  files.sort(
    (a, b) => fs.statSync(b).mtimeMs - fs.statSync(a).mtimeMs,
  );
  return files[0];
}

const INPUT = resolveInput();
if (!INPUT) {
  console.error(
    `analyze: no profile.json in ${CARVIARY} and no data/profile_*.json. ` +
      `Run \`npm run profile\` first.`,
  );
  process.exit(1);
}
console.log(`analyze: reading ${path.relative(CARVIARY, INPUT)}`);
const data = JSON.parse(fs.readFileSync(INPUT, "utf8"));

// ---------------------------------------------------------------------------
// heuristics: each takes `data` and returns { severity, title, detail } or null
// severity: "info" | "warn" | "issue"
// ---------------------------------------------------------------------------
const H = {
  us: (s, k, m) => data.sections[s]?.[k]?.[m] ?? null,
  hasSection: (s) => !!data.sections[s],
};

function h_vsync_dominates() {
  const swap_mean = H.us("swap", "gpu_us", "mean");
  const total_mean = H.us("frame_total", "gpu_us", "mean");
  if (swap_mean == null || total_mean == null || total_mean === 0) return null;
  const ratio = swap_mean / total_mean;
  if (ratio < 0.5) return null;
  return {
    severity: "info",
    title: `GPU frame time is ${(ratio * 100).toFixed(0)}% swap / vsync wait (expected)`,
    detail:
      `frame_total gpu_us.mean=${total_mean} vs swap gpu_us.mean=${swap_mean}. ` +
      `Vsync is hard-enabled in this build (SDL_GL_SetSwapInterval(1) in ` +
      `gl_vid_sdl.cpp) and there is no flag to disable it. This is not a bug ` +
      `to fix, it is a measurement constraint: frame_total and swap numbers ` +
      `mean nothing for build-vs-build comparison here. Compare per-section ` +
      `gpu_us (clear / world_opaque / viewmodel / polyblend / hud_2d / scene) ` +
      `instead. Those are unaffected by vsync.`,
  };
}

function h_cpu_gt_gpu(section) {
  const c = H.us(section, "cpu_us", "mean");
  const g = H.us(section, "gpu_us", "mean");
  if (c == null || g == null || g === 0) return null;
  if (c < g * 2) return null;
  return {
    severity: "info",
    title: `${section}: CPU-bound (cpu ${c}us vs gpu ${g}us, ${(c / g).toFixed(1)}x)`,
    detail:
      `Cost lives on the CPU side — command buffer prep, state changes, ` +
      `BSP walks, alias model blending. GPU work itself is small. ` +
      `Optimization target: reduce CPU work per draw or batch more.`,
  };
}

function h_gpu_gt_cpu(section) {
  const c = H.us(section, "cpu_us", "mean");
  const g = H.us(section, "gpu_us", "mean");
  if (c == null || g == null || c === 0) return null;
  if (g < c * 2) return null;
  return {
    severity: "info",
    title: `${section}: GPU-bound (gpu ${g}us vs cpu ${c}us, ${(g / c).toFixed(1)}x)`,
    detail:
      `The GPU is the bottleneck here — likely fill rate (lots of overdraw/ ` +
      `large surfaces) or shader cost. Optimization target: reduce ` +
      `fragments (culling, smaller triangles, early-Z) or shader ops.`,
  };
}

function h_high_variability(section, metric) {
  const p50 = H.us(section, metric, "p50");
  const p95 = H.us(section, metric, "p95");
  if (p50 == null || p95 == null || p50 < 10) return null;
  const ratio = p95 / p50;
  if (ratio < 3) return null;
  return {
    severity: "warn",
    title: `${section}.${metric} p95/p50 = ${ratio.toFixed(1)}x (spiky)`,
    detail:
      `p50=${p50}, p95=${p95}. A few sampled frames cost several times more ` +
      `than the median. Common causes: texture upload, shader compile, GC/ ` +
      `hunk alloc, variable geometry (alias model swaps). Investigate max-frame samples.`,
  };
}

function h_polyblend_overdraw() {
  const frags = H.us("polyblend", "frags_visible", "max");
  const res = data.resolution;
  if (!frags || !res) return null;
  const fullscreen_frags = res[0] * res[1];
  if (frags < fullscreen_frags * 0.9) return null;
  return {
    severity: "info",
    title: `polyblend: full-screen blend reaches ~${Math.round((frags / fullscreen_frags) * 100)}% of the screen`,
    detail:
      `frags_visible.max=${frags}, screen=${fullscreen_frags}. polyblend draws ` +
      `a fullscreen quad for damage/brightness. This is expected when the player ` +
      `takes damage or brightness<1. If mean is close to max, it's being drawn ` +
      `every frame — check that v_blend[3] is not always non-zero.`,
  };
}

function h_big_gpu_consumer() {
  const rank = [];
  for (const [name, s] of Object.entries(data.sections)) {
    if (name === "frame_total" || name === "scene" || name === "swap") continue;
    rank.push({ name, gpu: s.gpu_us?.p50 ?? 0 });
  }
  rank.sort((a, b) => b.gpu - a.gpu);
  const top = rank[0];
  if (!top || top.gpu === 0) return null;
  const sum = rank.reduce((a, b) => a + b.gpu, 0);
  return {
    severity: "info",
    title: `Top GPU section (p50): ${top.name} (${top.gpu}us, ${((top.gpu / sum) * 100).toFixed(0)}% of non-swap GPU)`,
    detail:
      `Ranking by gpu_us.p50 (excluding frame_total/scene/swap): ` +
      rank
        .slice(0, 5)
        .map((r) => `${r.name}=${r.gpu}us`)
        .join(", "),
  };
}

function h_big_cpu_consumer() {
  const rank = [];
  for (const [name, s] of Object.entries(data.sections)) {
    if (name === "frame_total" || name === "scene") continue;
    rank.push({ name, cpu: s.cpu_us?.p50 ?? 0 });
  }
  rank.sort((a, b) => b.cpu - a.cpu);
  const top = rank[0];
  if (!top || top.cpu === 0) return null;
  return {
    severity: "info",
    title: `Top CPU section (p50): ${top.name} (${top.cpu}us)`,
    detail:
      `Ranking by cpu_us.p50 (excluding frame_total/scene): ` +
      rank
        .slice(0, 5)
        .map((r) => `${r.name}=${r.cpu}us`)
        .join(", "),
  };
}

function h_low_sample_count() {
  if (data.samples >= 50) return null;
  return {
    severity: "warn",
    title: `Only ${data.samples} samples collected`,
    detail:
      `Percentile statistics are noisy below ~50 samples. The demo may be ` +
      `shorter than the target-frames list. Either record a longer demo or ` +
      `shorten the target-frames list in gl_profiler.cpp.`,
  };
}

// scene is a container that wraps R_DrawWorld + water + particles + alias + sprite + ...
// We instrument world_opaque (LEAF, has GL queries) plus a set of CPU-only
// subsections (cpu_mark_leaves, cpu_water, etc). "Unaccounted" is whatever
// scene.cpu - sum(world_opaque + cpu_*) is: code paths we still haven't named.
const SCENE_CHILDREN_CPU = [
  "world_opaque",
  "cpu_mark_leaves",
  "cpu_trans_setup",
  "cpu_water",
  "cpu_particles",
  "cpu_alias",
  "cpu_sprite",
  "cpu_brush_trans",
];

function h_scene_blind_spot() {
  const scene_cpu = H.us("scene", "cpu_us", "p50");
  if (scene_cpu == null) return null;
  const parts = [];
  let accounted = 0;
  for (const n of SCENE_CHILDREN_CPU) {
    const v = H.us(n, "cpu_us", "p50");
    if (v == null) continue;
    parts.push(`${n}=${v}`);
    accounted += v;
  }
  const unaccounted = scene_cpu - accounted;

  // If scene_cpu is close to a vsync period (~16ms @ 60Hz or ~17ms) the
  // wall-clock measurement is contaminated -- something inside scene blocks
  // on vsync (S_ExtraUpdate, for instance, or a GL sync). The "unaccounted"
  // time is then the *stall*, not real work.
  const vsync_us = 16000;
  const looks_pinned_to_vsync = scene_cpu > vsync_us * 0.9 && scene_cpu < vsync_us * 1.2;

  if (looks_pinned_to_vsync) {
    return {
      severity: "warn",
      title: `scene.cpu_us.p50 (${scene_cpu}us) is pinned to ~vsync — the number is the stall, not the work`,
      detail:
        `Measured CPU time for scene sits inside a vsync period (~16-17ms). ` +
        `The instrumented children sum to only ${accounted}us ` +
        `(${parts.join(", ")}). The "missing" ${unaccounted}us is almost ` +
        `certainly wall-clock time spent waiting somewhere -- either ` +
        `S_ExtraUpdate(), a driver sync, or the GL pipeline flushing. ` +
        `Trust the per-child numbers, not scene.cpu_us itself. The real ` +
        `CPU cost of the 3D render is closer to ${accounted}us than to ${scene_cpu}us.`,
    };
  }

  if (unaccounted < 500 || unaccounted < scene_cpu * 0.1) {
    return {
      severity: "info",
      title: `scene CPU is ${((accounted / scene_cpu) * 100).toFixed(0)}% attributed`,
      detail:
        `scene.cpu_us.p50=${scene_cpu}, accounted=${accounted} ` +
        `(${parts.join(", ")}). Unaccounted: ${unaccounted}us. ` +
        `Good coverage; the remaining delta is noise or trivial glue code ` +
        `(R_SetupFrame / R_PushDlights / R_SetFrustum / R_SetupGL).`,
    };
  }
  return {
    severity: "warn",
    title: `scene has ~${unaccounted}us of CPU time still unaccounted (${((unaccounted / scene_cpu) * 100).toFixed(0)}% of scene)`,
    detail:
      `scene.cpu_us.p50=${scene_cpu}, accounted=${accounted} ` +
      `(${parts.join(", ")}). The missing ~${unaccounted}us lives in code we ` +
      `haven't split out yet -- probably R_SetupFrame, R_PushDlights, ` +
      `R_SetFrustum, R_SetupGL, or R_MoveParticles. Add more CPU-only ` +
      `sections if this becomes the next bottleneck.`,
  };
}

function h_polyblend_cpu_anomaly() {
  const p50 = H.us("polyblend", "cpu_us", "p50");
  const p99 = H.us("polyblend", "cpu_us", "p99");
  const gpu_p99 = H.us("polyblend", "gpu_us", "p99");
  if (p50 == null || p99 == null || gpu_p99 == null) return null;
  // polyblend is 2-3 fullscreen quads; real CPU cost is sub-microsecond.
  // If p99 is in the milliseconds range, we are measuring something else.
  if (p99 < 1000) return null;
  return {
    severity: "warn",
    title: `polyblend cpu_us p99=${p99}us but gpu_us p99=${gpu_p99}us — measurement is contaminated`,
    detail:
      `R_PolyBlend issues at most 3 fullscreen quad draws. Real CPU cost is ` +
      `<10us. A p99 of ${p99}us (>1ms) cannot be actual polyblend work — it is ` +
      `almost certainly vsync stall or a context switch captured by ` +
      `Sys_FloatTime() inside the section. Known limitation of wall-clock ` +
      `CPU timing in a vsync-bound pipeline; trust GPU timings for this section.`,
  };
}

function h_gpu_headroom() {
  const scene_gpu = H.us("scene", "gpu_us", "p95");
  const frame_gpu = H.us("frame_total", "gpu_us", "p50");
  if (scene_gpu == null || frame_gpu == null || frame_gpu === 0) return null;
  const pct = (scene_gpu / frame_gpu) * 100;
  return {
    severity: "info",
    title: `GPU headroom: scene p95 uses only ${pct.toFixed(1)}% of the frame budget`,
    detail:
      `scene.gpu_us.p95=${scene_gpu}us, frame_total.gpu_us.p50=${frame_gpu}us. ` +
      `The actual render work fits ${(frame_gpu / scene_gpu).toFixed(0)}x over ` +
      `in the frame budget -- plenty of GPU headroom. The large frame_total ` +
      `is vsync padding, not engine work.`,
  };
}

function h_world_opaque_scales_with_draws() {
  const gpu_p50 = H.us("world_opaque", "gpu_us", "p50");
  const gpu_p95 = H.us("world_opaque", "gpu_us", "p95");
  const dc_p50 = H.us("world_opaque", "draw_calls", "p50");
  const dc_p95 = H.us("world_opaque", "draw_calls", "p95");
  if (gpu_p50 == null || dc_p50 == null || dc_p50 === 0) return null;
  const per_draw_p50 = gpu_p50 / dc_p50;
  const per_draw_p95 = gpu_p95 / dc_p95;
  const ratio = per_draw_p95 / per_draw_p50;
  // If us-per-draw is roughly constant across percentiles, the "spikiness" is
  // just more draws, not a per-draw stall.
  if (ratio > 1.4 || ratio < 0.7) return null;
  return {
    severity: "info",
    title: `world_opaque cost scales linearly with draw count (~${per_draw_p50.toFixed(2)}us/draw)`,
    detail:
      `us/draw is ${per_draw_p50.toFixed(2)} at p50 and ${per_draw_p95.toFixed(2)} at p95 ` +
      `(ratio ${ratio.toFixed(2)}). The p95/p50 "spike" in gpu_us is not a ` +
      `per-draw stall -- it is the PVS exposing more surfaces in some frames. ` +
      `Optimization target: reduce number of draws (batching, larger surface ` +
      `fans) rather than cost per draw.`,
  };
}

function run_heuristics() {
  const results = [];
  const push = (h) => h && results.push(h);

  push(h_vsync_dominates());
  push(h_gpu_headroom());
  push(h_scene_blind_spot());
  push(h_low_sample_count());
  push(h_big_gpu_consumer());
  push(h_big_cpu_consumer());
  push(h_polyblend_overdraw());
  push(h_polyblend_cpu_anomaly());
  push(h_world_opaque_scales_with_draws());

  for (const name of Object.keys(data.sections)) {
    if (name === "frame_total" || name === "scene") continue;
    push(h_cpu_gt_gpu(name));
    push(h_gpu_gt_cpu(name));
    push(h_high_variability(name, "gpu_us"));
    push(h_high_variability(name, "cpu_us"));
  }

  const order = { issue: 0, warn: 1, info: 2 };
  results.sort((a, b) => order[a.severity] - order[b.severity]);
  return results;
}

// ---------------------------------------------------------------------------
// plain-text digest for LLM ingestion
// ---------------------------------------------------------------------------
function text_digest(analysis) {
  const L = [];
  L.push(`# carviary profile digest`);
  L.push(``);
  L.push(`build: ${data.build}`);
  L.push(`gl_version: ${data.gl_version}`);
  L.push(`demo: ${data.demo}`);
  L.push(`map: ${data.map}`);
  L.push(`resolution: ${data.resolution.join("x")}`);
  L.push(`samples: ${data.samples}`);
  L.push(``);
  L.push(
    `Each section below reports gpu_us / cpu_us / draw_calls / tris / frags_visible`,
  );
  L.push(`as mean p50 p95 p99 min max. Times in microseconds; counters absolute.`);
  L.push(``);
  L.push(`## per-section stats`);
  L.push(``);
  for (const [name, s] of Object.entries(data.sections)) {
    L.push(`### ${name}`);
    for (const metric of ["gpu_us", "cpu_us", "draw_calls", "tris", "frags_visible"]) {
      const m = s[metric];
      if (!m) continue;
      L.push(
        `  ${metric.padEnd(14)}` +
          ` mean=${m.mean}` +
          ` p50=${m.p50}` +
          ` p95=${m.p95}` +
          ` p99=${m.p99}` +
          ` min=${m.min}` +
          ` max=${m.max}`,
      );
    }
    L.push(``);
  }
  const plain = plain_summary();
  if (plain) {
    L.push(`## plain-language summary`);
    L.push(``);
    for (const p of plain) {
      L.push(`* ${p.tldr}`);
      L.push(`  ${p.plain}`);
      L.push(``);
    }
  }
  L.push(`## automated analysis`);
  L.push(``);
  for (const a of analysis) {
    L.push(`[${a.severity}] ${a.title}`);
    L.push(`  ${a.detail}`);
    L.push(``);
  }
  const review = manual_review();
  if (review) {
    L.push(`## manual review`);
    L.push(``);
    for (const r of review) {
      L.push(`### ${r.heading}`);
      L.push(r.body);
      L.push(``);
    }
  }
  return L.join("\n");
}

// ---------------------------------------------------------------------------
// html building
// ---------------------------------------------------------------------------
function esc(s) {
  return String(s)
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;");
}

// sections in a canonical order for tables
function ordered_sections() {
  const desired = [
    "clear",
    "world_opaque",
    "viewmodel",
    "polyblend",
    "hud_2d",
    "swap",
    "scene",
    "frame_total",
    // CPU-only subsections inside scene
    "cpu_mark_leaves",
    "cpu_trans_setup",
    "cpu_water",
    "cpu_particles",
    "cpu_alias",
    "cpu_sprite",
    "cpu_brush_trans",
  ];
  const present = Object.keys(data.sections);
  const out = desired.filter((n) => present.includes(n));
  for (const n of present) if (!out.includes(n)) out.push(n);
  return out;
}

// Sections we omit from the default chart views because they're either
// pinned by vsync (swap, frame_total) or are containers that double-count
// their children (scene).
const CHART_EXCLUDE = new Set(["swap", "frame_total", "scene"]);

// Plain-language summary. No jargon, no percentiles, no microseconds if we
// can avoid it. The goal is: someone who doesn't know what p95 means should
// still walk away understanding what the profile says.
function plain_summary() {
  const S = data.sections;
  if (!S.scene?.gpu_us?.p50 || !S.world_opaque?.gpu_us?.p50) return null;

  const scene_ms = (S.scene.gpu_us.p95 / 1000).toFixed(1);
  const frame_ms = (S.frame_total.gpu_us.p50 / 1000).toFixed(1);
  const world_share = Math.round(
    (S.world_opaque.gpu_us.p50 /
      (S.world_opaque.gpu_us.p50 +
        S.viewmodel.gpu_us.p50 +
        S.polyblend.gpu_us.p50 +
        S.hud_2d.gpu_us.p50 +
        S.clear.gpu_us.p50)) *
      100,
  );
  // Sum everything we can attribute inside scene's CPU budget.
  let scene_accounted_us = 0;
  for (const n of SCENE_CHILDREN_CPU) {
    scene_accounted_us += S[n]?.cpu_us?.p50 ?? 0;
  }
  const unaccounted_ms = (
    Math.max(0, S.scene.cpu_us.p50 - scene_accounted_us) /
    1000
  ).toFixed(1);

  // Find the CPU-only subsection that dominates inside scene.
  let top_cpu_child = null;
  for (const n of SCENE_CHILDREN_CPU) {
    if (n === "world_opaque") continue;
    const v = S[n]?.cpu_us?.p50 ?? 0;
    if (!top_cpu_child || v > top_cpu_child.v) top_cpu_child = { n, v };
  }

  const items = [];

  items.push({
    tldr: "El motor va sobrado de GPU.",
    plain:
      `La GPU solo trabaja unos ${scene_ms} ms por frame. El monitor dibuja ` +
      `un frame cada ${frame_ms} ms (60 veces por segundo). Sobra muchísimo ` +
      `tiempo. La GPU pasa más del 90% del frame esperando al monitor.`,
  });

  items.push({
    tldr: "Los números de frame_total y swap no sirven para comparar.",
    plain:
      `Son fijos: siempre valen lo mismo porque están atados al monitor. Si ` +
      `cambias código y el número no se mueve, no significa que no hayas ` +
      `mejorado. Mira las otras filas (world_opaque, viewmodel, etc.) — esas ` +
      `sí reaccionan a cambios de código.`,
  });

  items.push({
    tldr: "Lo que más cuesta en GPU es el mundo (paredes y suelo).",
    plain:
      `world_opaque se lleva alrededor del ${world_share}% del trabajo de la ` +
      `GPU entre todas las fases medidas. Si algún día el motor va lento, ` +
      `ese es el primer sitio donde mirar.`,
  });

  if (top_cpu_child) {
    const top_ms = (top_cpu_child.v / 1000).toFixed(1);
    const pretty = {
      cpu_mark_leaves: "marcar superficies visibles (PVS)",
      cpu_trans_setup: "preparar entidades transparentes",
      cpu_water: "superficies de agua",
      cpu_particles: "partículas",
      cpu_alias: "modelos de enemigos",
      cpu_sprite: "sprites",
      cpu_brush_trans: "geometría transparente",
    }[top_cpu_child.n] ?? top_cpu_child.n;
    items.push({
      tldr: `Dentro del 3D, lo que más CPU come es ${pretty} (~${top_ms} ms).`,
      plain:
        `Hemos desglosado el render 3D en partes. La que más tarda en CPU ` +
        `ahora mismo es "${top_cpu_child.n}" (${pretty}): ${top_ms} ms. Si ` +
        `algún día quieres que el motor vaya más rápido en CPU, es el primer ` +
        `sitio donde hay que mirar.`,
    });
  }

  // If scene_cpu is stuck near a vsync period the "unaccounted" time is the
  // stall, not real work. Say so honestly.
  const scene_cpu_us = S.scene.cpu_us.p50;
  const pinned = scene_cpu_us > 14000 && scene_cpu_us < 19000;
  items.push({
    tldr: pinned
      ? "Los ~" + unaccounted_ms + " ms 'sin explicar' no son trabajo, son espera."
      : parseFloat(unaccounted_ms) < 0.5
        ? "Ya sabemos en qué se va casi todo el trabajo."
        : `Queda ~${unaccounted_ms} ms sin identificar dentro del 3D.`,
    plain: pinned
      ? `La fase "scene" marca ~${(scene_cpu_us / 1000).toFixed(1)} ms de ` +
        `CPU, pero las subfases que medimos dentro suman mucho menos. La ` +
        `diferencia es tiempo *esperando*, no trabajando: el hilo se bloquea ` +
        `(en audio, en sincronización con la GPU, en vsync). Son milisegundos ` +
        `fantasma. La suma de las subfases (world_opaque + cpu_water + ` +
        `cpu_particles + ...) es el coste real de CPU del render 3D.`
      : parseFloat(unaccounted_ms) < 0.5
        ? `La parte 3D (scene) está prácticamente toda medida. Lo que queda ` +
          `sin clasificar son microsegundos de código de pegamento que no ` +
          `merece la pena desglosar.`
        : `Dentro de la fase "scene" quedan ${unaccounted_ms} milisegundos ` +
          `de CPU sin asignar a ninguna fase. Si hace falta optimizar más, ` +
          `añadir más fases en una próxima versión del profiler.`,
  });

  items.push({
    tldr: "Para comparar builds usa la columna us/draw (verde, al final de la tabla grande).",
    plain:
      `Más abajo en esta página hay una tabla enorme con todas las fases y ` +
      `sus números. Las dos últimas columnas están resaltadas en verde: ` +
      `"us/draw gpu" y "us/draw cpu". Es microsegundos por draw call — el ` +
      `coste de dibujar una cosa cualquiera. Cuando cambies código y quieras ` +
      `saber si mejoraste, compara esa columna entre builds. Si baja, ` +
      `optimizaste de verdad. Si solo baja el tiempo total pero esa columna ` +
      `sube, no optimizaste: simplemente había menos cosas en pantalla.`,
  });

  return items;
}

// A hand-written narrative that interprets the current numbers. Returns null
// if some required field is missing. Unlike automated heuristics, this is an
// opinionated reading of the profile that explains what the shape of the data
// actually means in context.
function manual_review() {
  const S = data.sections;
  const need = [
    S.frame_total?.gpu_us?.p50,
    S.swap?.gpu_us?.p50,
    S.scene?.gpu_us?.p95,
    S.scene?.cpu_us?.p50,
    S.world_opaque?.cpu_us?.p50,
    S.world_opaque?.gpu_us?.p50,
    S.world_opaque?.gpu_us?.p95,
    S.world_opaque?.draw_calls?.p50,
    S.world_opaque?.draw_calls?.p95,
    S.polyblend?.cpu_us?.p99,
  ];
  if (need.some((v) => v == null)) return null;

  const frame_gpu_p50 = S.frame_total.gpu_us.p50;
  const swap_gpu_p50 = S.swap.gpu_us.p50;
  const scene_gpu_p95 = S.scene.gpu_us.p95;
  const scene_cpu_p50 = S.scene.cpu_us.p50;
  const world_cpu_p50 = S.world_opaque.cpu_us.p50;
  const world_gpu_p50 = S.world_opaque.gpu_us.p50;
  const world_gpu_p95 = S.world_opaque.gpu_us.p95;
  const world_dc_p50 = S.world_opaque.draw_calls.p50;
  const world_dc_p95 = S.world_opaque.draw_calls.p95;
  const poly_cpu_p99 = S.polyblend.cpu_us.p99;

  const unaccounted_cpu = scene_cpu_p50 - world_cpu_p50;
  const per_draw_p50 = (world_gpu_p50 / world_dc_p50).toFixed(2);
  const per_draw_p95 = (world_gpu_p95 / world_dc_p95).toFixed(2);
  const headroom = (frame_gpu_p50 / scene_gpu_p95).toFixed(0);
  const swap_pct = ((swap_gpu_p50 / frame_gpu_p50) * 100).toFixed(0);

  return [
    {
      heading: "The frame budget is not the real metric",
      body:
        `frame_total gpu_us.p50 is ${frame_gpu_p50}us — that is just 1/60s ` +
        `(vsync). Of that, swap alone takes ${swap_gpu_p50}us (${swap_pct}%). ` +
        `The actual render work (scene) fits in ${scene_gpu_p95}us at p95, ` +
        `about ${headroom}x under budget. Vsync is hard-wired on in this build ` +
        `(SDL_GL_SetSwapInterval(1) in gl_vid_sdl.cpp has no off switch), so ` +
        `frame_total and swap will always look this way. That is fine — just ` +
        `ignore them. Compare per-section gpu_us instead; those are unaffected ` +
        `by vsync and show real engine work.`,
    },
    {
      heading: "scene has a blind spot we should eliminate",
      body:
        `scene.cpu_us.p50 = ${scene_cpu_p50}us, world_opaque.cpu_us.p50 = ` +
        `${world_cpu_p50}us. That leaves ~${unaccounted_cpu}us inside scene ` +
        `that we cannot attribute to any section. It is spread across ` +
        `R_DrawParticles, R_DrawWaterSurfaces, R_DrawAliasModel, ` +
        `R_DrawSpriteModel, R_DrawBrushMTexTrans, R_MarkLeaves, R_MoveParticles, ` +
        `R_SetupTransEntities — none of them instrumented. Until we split ` +
        `these, "optimize scene" is a dartboard. Next profiler iteration ` +
        `should add CPU-only sections (just Sys_FloatTime) for each of those ` +
        `— GL query nesting isn't an issue if we only measure CPU.`,
    },
    {
      heading: "world_opaque 'spike' is not a stall, it is more work",
      body:
        `world_opaque.gpu_us jumps from p50=${world_gpu_p50} to p95=` +
        `${world_gpu_p95}us, which the automated heuristics flag as spiky. ` +
        `But draw_calls also jump from p50=${world_dc_p50} to p95=` +
        `${world_dc_p95}. Cost per draw is ${per_draw_p50}us at p50 and ` +
        `${per_draw_p95}us at p95 — essentially identical. That means the ` +
        `"spike" is just the PVS exposing more surfaces when the player looks ` +
        `into a bigger room. There is no per-draw regression to fix. ` +
        `Optimization target: reduce number of draws (batch surfaces, merge ` +
        `fans), not cost per draw.`,
    },
    {
      heading: "polyblend CPU numbers are measurement noise",
      body:
        `polyblend.cpu_us.p99 = ${poly_cpu_p99}us, but R_PolyBlend issues at ` +
        `most three fullscreen quad draws — real CPU cost is under 10us. ` +
        `The large p99 is almost certainly vsync stall or a context switch ` +
        `captured by Sys_FloatTime() inside the section, not real work. ` +
        `Don't chase it. It also exposes a limitation of the current CPU ` +
        `timing: it is wall-clock, so anything that happens to block the ` +
        `thread during a section contaminates the sample. If this becomes ` +
        `a problem, replace Sys_FloatTime with QueryThreadCycleTime or ` +
        `a thread-time API.`,
    },
    {
      heading: "us/draw is the metric to watch between builds",
      body:
        `Absolute section times move with content (PVS, entity count), so ` +
        `p50 alone can regress without any code change. us/draw (p50 gpu_us ` +
        `divided by p50 draw_calls, shown in the green column of the table) ` +
        `normalizes out content load: if a code change makes per-draw cost go ` +
        `from 0.56us to 0.71us while draw count stays the same, you just ` +
        `regressed. This is a better comparison knob for build-vs-build diffs ` +
        `than gpu_us.p50 in isolation.`,
    },
  ];
}

function build_html(analysis, digest) {
  const sections = ordered_sections();

  // GPU chart: only sections with gpu_us data, excluding vsync-pinned and containers.
  const gpu_sections = sections.filter(
    (n) =>
      !CHART_EXCLUDE.has(n) &&
      data.sections[n].gpu_us != null,
  );
  // CPU chart: every non-excluded section (CPU-only subsections included).
  const cpu_sections = sections.filter((n) => !CHART_EXCLUDE.has(n));

  const gpu_labels = JSON.stringify(gpu_sections);
  const gpu_p50 = JSON.stringify(gpu_sections.map((n) => data.sections[n].gpu_us.p50));
  const gpu_p95 = JSON.stringify(gpu_sections.map((n) => data.sections[n].gpu_us.p95));
  const cpu_labels = JSON.stringify(cpu_sections);
  const cpu_p50 = JSON.stringify(cpu_sections.map((n) => data.sections[n].cpu_us.p50));
  const cpu_p95 = JSON.stringify(cpu_sections.map((n) => data.sections[n].cpu_us.p95));

  const sevClass = { issue: "sev-issue", warn: "sev-warn", info: "sev-info" };

  const rows = sections
    .map((name) => {
      const s = data.sections[name];
      const cell = (m) =>
        m
          ? `<td class="num">${m.mean}</td><td class="num">${m.p50}</td><td class="num">${m.p95}</td><td class="num">${m.p99}</td><td class="num">${m.min}</td><td class="num">${m.max}</td>`
          : `<td colspan="6" class="na">-</td>`;

      // Derived: us per draw at p50. Useful to see if a section's cost
      // scales with draw count (stable us/draw) vs a per-draw regression.
      let perDraw = `<td class="na" colspan="2">-</td>`;
      const dc50 = s.draw_calls?.p50;
      if (dc50 && dc50 > 0 && s.gpu_us?.p50 != null) {
        const gpuPerDraw = (s.gpu_us.p50 / dc50).toFixed(2);
        const cpuPerDraw = s.cpu_us?.p50 != null ? (s.cpu_us.p50 / dc50).toFixed(2) : "-";
        perDraw = `<td class="num derived">${gpuPerDraw}</td><td class="num derived">${cpuPerDraw}</td>`;
      }

      return `<tr>
        <td class="sec">${esc(name)}</td>
        ${cell(s.gpu_us)}
        ${cell(s.cpu_us)}
        ${cell(s.draw_calls)}
        ${cell(s.tris)}
        ${cell(s.frags_visible)}
        ${perDraw}
      </tr>`;
    })
    .join("\n");

  const analysisHtml = analysis
    .map(
      (a) =>
        `<div class="finding ${sevClass[a.severity]}">
      <div class="finding-title"><span class="sev">${a.severity}</span> ${esc(a.title)}</div>
      <div class="finding-detail">${esc(a.detail)}</div>
    </div>`,
    )
    .join("\n");

  const review = manual_review();
  const reviewHtml = review
    ? review
        .map(
          (r) => `<div class="review-item">
      <div class="review-heading">${esc(r.heading)}</div>
      <div class="review-body">${esc(r.body)}</div>
    </div>`,
        )
        .join("\n")
    : "";

  const plain = plain_summary();
  const plainHtml = plain
    ? plain
        .map(
          (p) => `<div class="plain-item">
      <div class="plain-tldr">${esc(p.tldr)}</div>
      <div class="plain-body">${esc(p.plain)}</div>
    </div>`,
        )
        .join("\n")
    : "";

  return `<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>carviary profile report</title>
<style>
  body { font-family: system-ui, sans-serif; margin: 0; padding: 24px 32px; background: #0e1116; color: #e6edf3; }
  h1, h2 { margin: 0 0 8px 0; }
  h1 { font-size: 22px; }
  h2 { font-size: 16px; margin-top: 28px; border-bottom: 1px solid #30363d; padding-bottom: 4px; }
  .meta { color: #8b949e; font-size: 13px; margin-bottom: 12px; }
  .meta span { margin-right: 18px; }
  .grid2 { display: grid; grid-template-columns: 1fr 1fr; gap: 20px; }
  .chart-box { background: #161b22; border: 1px solid #30363d; border-radius: 6px; padding: 12px; height: 340px; }
  table { border-collapse: collapse; width: 100%; font-size: 12px; font-family: ui-monospace, Consolas, monospace; }
  th, td { padding: 4px 8px; border-bottom: 1px solid #21262d; text-align: right; }
  th { background: #161b22; position: sticky; top: 0; }
  td.sec { text-align: left; color: #79c0ff; font-weight: 600; }
  td.na { text-align: center; color: #6e7681; }
  .num { color: #c9d1d9; }
  .derived { color: #7ee787; font-weight: 600; background: #0d1f14; }
  .derived-header { background: #0d1f14 !important; color: #7ee787 !important; }
  .group { border-left: 1px solid #30363d; }
  .finding { background: #161b22; border: 1px solid #30363d; border-left: 4px solid #30363d; border-radius: 4px; padding: 10px 14px; margin: 8px 0; }
  .sev-issue { border-left-color: #f85149; }
  .sev-warn  { border-left-color: #d29922; }
  .sev-info  { border-left-color: #58a6ff; }
  .finding-title { font-weight: 600; }
  .finding-detail { color: #8b949e; font-size: 13px; margin-top: 4px; }
  .sev { display: inline-block; text-transform: uppercase; font-size: 10px; padding: 1px 6px; border-radius: 3px; margin-right: 8px; background: #30363d; color: #c9d1d9; letter-spacing: 0.5px; }
  .review-item { background: #0b1a2b; border: 1px solid #1f6feb; border-left-width: 4px; border-radius: 4px; padding: 12px 16px; margin: 10px 0; }
  .review-heading { font-weight: 600; color: #79c0ff; margin-bottom: 6px; }
  .review-body { color: #c9d1d9; font-size: 13px; line-height: 1.5; }
  .plain-item { background: #0f2415; border: 1px solid #238636; border-left-width: 4px; border-radius: 4px; padding: 14px 18px; margin: 12px 0; }
  .plain-tldr { font-weight: 700; color: #7ee787; margin-bottom: 8px; font-size: 15px; }
  .plain-body { color: #e6edf3; font-size: 14px; line-height: 1.6; }
  pre.digest { background: #010409; border: 1px solid #30363d; padding: 12px; overflow: auto; max-height: 420px; font-size: 12px; border-radius: 6px; }
  details summary { cursor: pointer; color: #8b949e; margin: 12px 0 6px; }
  .hint { color: #8b949e; font-size: 12px; margin: 6px 0 14px; }
</style>
</head>
<body>
  <h1>carviary profile report</h1>
  <div class="meta">
    <span>build: <b>${esc(data.build)}</b></span>
    <span>gl: <b>${esc(data.gl_version)}</b></span>
    <span>demo: <b>${esc(data.demo)}</b></span>
    <span>map: <b>${esc(data.map)}</b></span>
    <span>res: <b>${data.resolution.join("x")}</b></span>
    <span>samples: <b>${data.samples}</b></span>
  </div>

  ${
    plain
      ? `<h2>En cristiano (sin jerga)</h2>
  <div class="hint">Las conclusiones principales, explicadas como se las contarias a alguien que no sabe de rendering.</div>
  ${plainHtml}`
      : ""
  }

  <h2>Automated analysis</h2>
  <div class="hint">Heuristics computed from percentile stats. Not a substitute for judgement.</div>
  ${analysisHtml}

  ${
    review
      ? `<h2>Manual review</h2>
  <div class="hint">Opinionated reading of the current numbers. The patterns here
    are not captured by the automated heuristics because they require context
    (what is being measured, how, and why a number looks wrong).</div>
  ${reviewHtml}`
      : ""
  }

  <h2>Per-section charts (p50 &amp; p95, microseconds)</h2>
  <div class="grid2">
    <div class="chart-box"><canvas id="gpu-chart"></canvas></div>
    <div class="chart-box"><canvas id="cpu-chart"></canvas></div>
  </div>

  <h2>Full stats table</h2>
  <div class="hint">Columns repeat: mean / p50 / p95 / p99 / min / max. Times in microseconds; counters absolute.</div>
  <div style="overflow-x:auto">
  <table>
    <thead>
      <tr>
        <th rowspan="2">section</th>
        <th colspan="6">gpu_us</th>
        <th colspan="6" class="group">cpu_us</th>
        <th colspan="6" class="group">draw_calls</th>
        <th colspan="6" class="group">tris</th>
        <th colspan="6" class="group">frags_visible</th>
        <th colspan="2" class="group derived-header">us/draw p50</th>
      </tr>
      <tr>
        <th>mean</th><th>p50</th><th>p95</th><th>p99</th><th>min</th><th>max</th>
        <th class="group">mean</th><th>p50</th><th>p95</th><th>p99</th><th>min</th><th>max</th>
        <th class="group">mean</th><th>p50</th><th>p95</th><th>p99</th><th>min</th><th>max</th>
        <th class="group">mean</th><th>p50</th><th>p95</th><th>p99</th><th>min</th><th>max</th>
        <th class="group">mean</th><th>p50</th><th>p95</th><th>p99</th><th>min</th><th>max</th>
        <th class="group derived-header">gpu</th><th class="derived-header">cpu</th>
      </tr>
    </thead>
    <tbody>
      ${rows}
    </tbody>
  </table>
  </div>

  <h2>Machine-readable</h2>
  <div class="hint">
    The block below is intended for ingestion by AI tools: plain-text digest followed by the raw JSON.
    Select all and paste.
  </div>
  <details open>
    <summary>plain-text digest</summary>
    <pre class="digest" id="digest-text">${esc(digest)}</pre>
  </details>
  <details>
    <summary>raw profile.json</summary>
    <pre class="digest" id="raw-json">${esc(JSON.stringify(data, null, 2))}</pre>
  </details>

  <script type="application/json" id="profile-data">
${JSON.stringify(data)}
  </script>

  <script src="https://cdn.jsdelivr.net/npm/chart.js@4.4.1/dist/chart.umd.min.js"></script>
  <script>
    const GPU_LABELS = ${gpu_labels};
    const GPU_P50    = ${gpu_p50};
    const GPU_P95    = ${gpu_p95};
    const CPU_LABELS = ${cpu_labels};
    const CPU_P50    = ${cpu_p50};
    const CPU_P95    = ${cpu_p95};

    function makeChart(id, title, labels, p50, p95) {
      const ctx = document.getElementById(id);
      return new Chart(ctx, {
        type: "bar",
        data: {
          labels,
          datasets: [
            { label: "p50", data: p50, backgroundColor: "#58a6ff" },
            { label: "p95", data: p95, backgroundColor: "#d29922" },
          ],
        },
        options: {
          indexAxis: "y",
          responsive: true,
          maintainAspectRatio: false,
          plugins: {
            title: { display: true, text: title, color: "#e6edf3" },
            legend: { labels: { color: "#c9d1d9" } },
            tooltip: { callbacks: { label: (c) => c.dataset.label + ": " + c.parsed.x + " us" } },
          },
          scales: {
            x: { ticks: { color: "#8b949e" }, grid: { color: "#21262d" }, title: { display: true, text: "microseconds", color: "#8b949e" } },
            y: { ticks: { color: "#c9d1d9" }, grid: { color: "#21262d" } },
          },
        },
      });
    }

    makeChart("gpu-chart", "GPU time per section (swap/frame_total excluded)", GPU_LABELS, GPU_P50, GPU_P95);
    makeChart("cpu-chart", "CPU time per section (swap/frame_total excluded)", CPU_LABELS, CPU_P50, CPU_P95);
  </script>
</body>
</html>
`;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
const analysis = run_heuristics();
const digest = text_digest(analysis);
const html = build_html(analysis, digest);
fs.writeFileSync(OUTPUT, html, "utf8");

console.log(`analyze: wrote ${OUTPUT}`);
console.log(`analyze: ${analysis.length} findings, ${data.samples} samples`);
for (const a of analysis.slice(0, 5)) {
  console.log(`  [${a.severity}] ${a.title}`);
}
