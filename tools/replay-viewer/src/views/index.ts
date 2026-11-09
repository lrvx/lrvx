// View renderers. Each takes a slice of state and a container element.
// Cheap re-render on every cursor tick is acceptable for the MVP scope
// (50K events, M-class macbook target). Optimize later if needed.

import type { ViewerState } from '../state.ts';
import { tapeSlice, signalsSlice, ordersSlice, fillsSlice } from '../state.ts';
import type { Trade } from '../parsers/lrvx.ts';

const fmtTs = (ns: bigint) => {
  const ms = Number(ns / 1_000_000n);
  const d = new Date(ms);
  return d.toISOString().slice(11, 23);
};
const fmtNum = (n: number, dp = 2) => n.toFixed(dp);

// Cache the most recent render fingerprint per view so we skip the
// expensive part (innerHTML / canvas redraw) when the visible slice
// has not changed since the previous frame. The price chart cursor
// line is the one element that does need to redraw every frame, so
// renderPriceChart bypasses this cache.
let lastTradesSig = '';
let lastSignalsSig = '';
let lastOrdersSig = '';
let lastOrderbookSig = '';
let lastEquitySig = '';

// Stable per-symbol colour. Cycles through a curated palette by
// global symbol id; symbol_id=1 → first colour, =2 → second, etc.
// Palette tuned for white background + colour-vision-deficiency friendly.
const SYMBOL_PALETTE = [
  '#138caf',  // teal
  '#d6464b',  // red
  '#7c4dff',  // violet
  '#f59e0b',  // amber
  '#21a165',  // green
  '#e879f9',  // pink
  '#3b82f6',  // blue
  '#84cc16',  // lime
];
function symbolColor(sid: number): string {
  return SYMBOL_PALETTE[(sid - 1) % SYMBOL_PALETTE.length] ?? '#138caf';
}

function symbolLabel(state: ViewerState, sid: number): string {
  const s = state.symbols.get(sid);
  if (!s) return `#${sid}`;
  if (s.name.includes('/') || !s.exchange) return s.name || `#${sid}`;
  return `${s.exchange}/${s.name}`;
}

// Compact column-friendly version for trade-row rendering. Strips the
// "/" segment so the label fits in the narrow trades pane, while the
// full name lives in the `title=` tooltip for hover-reveal.
function compactSymbolLabel(state: ViewerState, sid: number): string {
  const full = symbolLabel(state, sid);
  const slash = full.indexOf('/');
  if (slash > 0) return full.slice(0, slash);  // venue prefix only
  return full;
}

function renderTradeRows(trades: Array<any>, state: ViewerState, compact: boolean = false): string {
  if (compact) {
    // Split-view: drop the timestamp + side word; row colour conveys
    // side (bid/ask classes). Only price + qty remain, both wide.
    return trades.map((t) => {
      if (t.type !== 'trade') return '';
      return `<div class="row ${t.side === 'buy' ? 'bid' : 'ask'}" title="${fmtTs(t.ts_ns)} · ${t.side}">
        <span class="price">${fmtNum(t.price)}</span>
        <span class="qty">${fmtNum(t.qty, 4)}</span>
      </div>`;
    }).join('');
  }
  return trades
    .map((t) => {
      if (t.type !== 'trade') return '';
      const fullLabel = symbolLabel(state, t.symbol_id);
      const compactLabel = compactSymbolLabel(state, t.symbol_id);
      return `<div class="row ${t.side === 'buy' ? 'bid' : 'ask'}">
        <span class="ts">${fmtTs(t.ts_ns)}</span>
        <span class="symbol-label" data-sid="${t.symbol_id}" title="${escapeHtml(fullLabel)}">${escapeHtml(compactLabel)}</span>
        <span>${t.side}</span>
        <span>${fmtNum(t.price)}</span>
        <span>${fmtNum(t.qty, 4)}</span>
      </div>`;
    })
    .join('');
}

export function renderTrades(state: ViewerState) {
  const el = document.getElementById('view-trades')!;
  const empty = document.querySelector<HTMLElement>('.emptystate[data-for="trades"]')!;
  const allTrades = tapeSlice(state).filter((e): e is Trade => e.type === 'trade');
  // Group by symbol; render side-by-side when ≤2 venues, single column
  // otherwise. Matches the orderbook pane's split policy so the
  // multi-venue user reads both panels with the same mental model.
  const bySym = new Map<number, Trade[]>();
  for (const t of allTrades) {
    let arr = bySym.get(t.symbol_id);
    if (!arr) { arr = []; bySym.set(t.symbol_id, arr); }
    arr.push(t);
  }
  const venues = [...bySym.entries()]
                   .sort((a, b) => b[1].length - a[1].length)
                   .map(([sid]) => sid);
  const sig = `${state.tape.length}|${allTrades.length}|${allTrades[allTrades.length - 1]?.ts_ns ?? ''}|${state.symbols.size}|${venues.join(',')}`;
  if (sig === lastTradesSig) return;
  lastTradesSig = sig;
  if (allTrades.length === 0 && state.tape.length === 0) {
    empty.hidden = false;
    empty.textContent = 'No `.lrvx` loaded. Drop one to see trades.';
    el.innerHTML = '';
    return;
  }
  empty.hidden = true;
  if (venues.length <= 1) {
    el.classList.remove('split');
    el.innerHTML = renderTradeRows(allTrades.slice(-100).reverse(), state);
    return;
  }
  // ≤2 venues: split. >2: pick busiest two, footer note for the rest.
  const cols = venues.slice(0, 2);
  el.classList.add('split');
  const colHTML = cols.map((sid) => {
    const recent = (bySym.get(sid) ?? []).slice(-100).reverse();
    const label = symbolLabel(state, sid);
    const colour = symbolColor(sid);
    return `<div class="trades-col" data-sid="${sid}">
      <div class="trades-col-header" style="border-top:2px solid ${colour}">${escapeHtml(label)}</div>
      ${renderTradeRows(recent, state, /*compact*/ true)}
    </div>`;
  }).join('');
  const footer = venues.length > 2
    ? `<div class="trades-overflow">+ ${venues.length - 2} more venue(s) hidden</div>`
    : '';
  el.innerHTML = colHTML + footer;
}

export function renderSymbolLegend(state: ViewerState) {
  // Optional UI: a small panel listing every symbol present in the
  // loaded tape with its (exchange, name) label. The element is
  // created on first render if not present so legacy HTML stays valid.
  let el = document.getElementById('view-symbol-legend') as HTMLElement | null;
  if (!el) {
    const container = document.querySelector('main')
                       ?? document.body;
    el = document.createElement('div');
    el.id = 'view-symbol-legend';
    el.style.cssText = 'padding:4px 8px; font-size:11px; color:#555; '
                     + 'border-top:1px solid #eee;';
    container.appendChild(el);
  }
  // Distinct symbol ids actually seen in the loaded tape.
  const seen = new Set<number>();
  for (const ev of state.tape) {
    if (ev.type === 'trade') seen.add(ev.symbol_id);
    else seen.add(ev.symbol_id);
  }
  if (seen.size === 0) {
    el.innerHTML = '';
    return;
  }
  const items: string[] = [];
  for (const sid of [...seen].sort((a, b) => a - b)) {
    const label = symbolLabel(state, sid);
    const colour = symbolColor(sid);
    items.push(
      `<span class="symbol-legend-item" data-sid="${sid}" `
      + `style="margin-right:14px; display:inline-flex; align-items:center; gap:5px">`
      + `<span style="display:inline-block; width:10px; height:10px; background:${colour}; border-radius:2px"></span>`
      + `${escapeHtml(label)}`
      + `</span>`);
  }
  el.innerHTML = `<strong>symbols:</strong> ${items.join('')}`;
}

// Reconstruct {bids, asks} for one symbol from a tape slice, finding
// the most recent snapshot before cursor and applying deltas after it.
// Tops are not depth and stay out of it: applying one as a delta would
// leave a stale better level behind when the touch moves away.
function reconstructBook(bookEvents: Array<{type: string, bids: any[], asks: any[]}>,
                          sid: number): { bids: Map<number, number>, asks: Map<number, number>, hasSnapshot: boolean } {
  const filtered = bookEvents.filter(
    (ev: any) => (ev.type === 'book_snapshot' || ev.type === 'book_delta')
              && ev.symbol_id === sid,
  );
  let lastSnapIdx = -1;
  for (let i = filtered.length - 1; i >= 0; i--) {
    if (filtered[i].type === 'book_snapshot') { lastSnapIdx = i; break; }
  }
  const bids = new Map<number, number>();
  const asks = new Map<number, number>();
  if (lastSnapIdx === -1) return { bids, asks, hasSnapshot: false };
  for (let i = lastSnapIdx; i < filtered.length; i++) {
    const ev = filtered[i];
    if (ev.type === 'book_snapshot') {
      bids.clear(); asks.clear();
    }
    for (const lvl of ev.bids as Array<{price: number, qty: number}>) {
      if (lvl.qty === 0) bids.delete(lvl.price);
      else bids.set(lvl.price, lvl.qty);
    }
    for (const lvl of ev.asks as Array<{price: number, qty: number}>) {
      if (lvl.qty === 0) asks.delete(lvl.price);
      else asks.set(lvl.price, lvl.qty);
    }
  }
  return { bids, asks, hasSnapshot: true };
}

type Touch = { bid: { price: number, qty: number } | null,
               ask: { price: number, qty: number } | null };

// The latest top-of-book frame for one symbol, if one arrived after the
// last depth frame: then it is the venue's freshest word on the touch and
// the depth ladder below it is older. A depth frame after it wins.
function touchAfterDepth(bookEvents: Array<any>, sid: number): Touch | null {
  for (let i = bookEvents.length - 1; i >= 0; i--) {
    const ev = bookEvents[i];
    if (ev.symbol_id !== sid) continue;
    if (ev.type !== 'book_top') return null;
    return { bid: ev.bids[0] ?? null, ask: ev.asks[0] ?? null };
  }
  return null;
}

// Paint one venue's order book into a canvas rectangle. Layout:
//
//   [PRICE QTY ▮▮▮▮ ]  ← bids (left of centre, text starts hard-left)
//   [▮▮▮▮ PRICE QTY ]  ← asks (right of centre)
//
// Switched away from the centred "price text inside the bar" layout
// because at narrow widths (≤200px per pane in the split view) the
// text and bars overlap; left/right-anchored text + bars on the spread
// side reads cleanly down to ~120px per pane.
function paintOneBook(ctx: CanvasRenderingContext2D, state: ViewerState,
                       sid: number,
                       bids: Map<number, number>, asks: Map<number, number>,
                       touch: Touch | null,
                       rectX: number, rectY: number, rectW: number, rectH: number) {
  const narrow = rectW < 200;
  const font = narrow ? '10px ui-monospace, monospace' : '11px ui-monospace, monospace';
  ctx.font = font;
  ctx.textBaseline = 'middle';
  ctx.fillStyle = symbolColor(sid);
  ctx.fillRect(rectX, rectY, 3, rectH);  // venue-colour spine on the left
  ctx.fillStyle = '#475569';
  ctx.fillText(symbolLabel(state, sid), rectX + 8, rectY + 12);

  const headerH = 24;
  const usableH = rectH - headerH;
  const minRowH = narrow ? 12 : 14;
  // A fresher touch takes one row between the asks and the bids.
  const touchRows = touch ? 1 : 0;
  const N = Math.max(4, Math.min(15, Math.floor((usableH - 4 - touchRows * minRowH) / (2 * minRowH))));
  const bidList = [...bids.entries()].sort((a, b) => b[0] - a[0]).slice(0, N);
  const askList = [...asks.entries()].sort((a, b) => a[0] - b[0]).slice(0, N);
  const maxQty = Math.max(
    1, ...bidList.map(([, q]) => q), ...askList.map(([, q]) => q),
  );
  const rowH = Math.max(minRowH, Math.min(20, usableH / (N * 2 + touchRows)));

  // Split each row: left half = "price qty" text, right half = bar.
  // For asks the layout flips (bar left, text right) so the spread —
  // best ask at top of bids half, best bid at top of asks half — sits
  // visually centred.
  const padX = 6;
  const textW = Math.max(70, rectW * 0.55);  // text column width
  const barAreaX = rectX + textW;
  const barAreaW = Math.max(20, rectW - textW - padX);

  askList.slice().reverse().forEach(([price, qty], idx) => {
    const y = rectY + headerH + idx * rowH;
    const barW = (qty / maxQty) * barAreaW;
    ctx.fillStyle = 'rgba(214, 70, 75, 0.18)';
    ctx.fillRect(barAreaX, y, barW, rowH - 2);
    ctx.fillStyle = '#d6464b';
    ctx.fillText(`${price.toFixed(2)}  ${qty.toFixed(4)}`,
                  rectX + padX + 4, y + rowH / 2);
  });
  if (touch) {
    // The touch line: the latest top, newer than the depth around it.
    const y = rectY + headerH + askList.length * rowH;
    const side = (l: { price: number, qty: number } | null) =>
      l ? `${l.price.toFixed(2)} ${l.qty.toFixed(4)}` : 'empty';
    ctx.fillStyle = '#2563eb';
    ctx.fillRect(rectX + padX, y + rowH / 2, rectW - 2 * padX, 1);
    ctx.fillStyle = '#ffffff';
    const text = narrow ? `${side(touch.bid)} | ${side(touch.ask)}`
                        : `touch  ${side(touch.bid)}  |  ${side(touch.ask)}`;
    const tw = ctx.measureText(text).width;
    ctx.fillRect(rectX + padX, y + 1, tw + 10, rowH - 2);
    ctx.fillStyle = '#2563eb';
    ctx.fillText(text, rectX + padX + 5, y + rowH / 2);
  }
  bidList.forEach(([price, qty], idx) => {
    const y = rectY + headerH + (askList.length + touchRows + idx) * rowH;
    const barW = (qty / maxQty) * barAreaW;
    ctx.fillStyle = 'rgba(33, 161, 101, 0.18)';
    ctx.fillRect(barAreaX, y, barW, rowH - 2);
    ctx.fillStyle = '#21a165';
    ctx.fillText(`${price.toFixed(2)}  ${qty.toFixed(4)}`,
                  rectX + padX + 4, y + rowH / 2);
  });
}

export function renderOrderbook(state: ViewerState) {
  const canvas = document.getElementById('view-orderbook') as HTMLCanvasElement;
  const empty = document.querySelector<HTMLElement>('.emptystate[data-for="orderbook"]')!;
  const ctx = canvas.getContext('2d');
  if (!ctx) return;
  const dpr = window.devicePixelRatio || 1;
  const w = canvas.clientWidth, h = canvas.clientHeight;
  canvas.width = w * dpr;
  canvas.height = h * dpr;
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, w, h);

  const tape = tapeSlice(state);
  const allBookEvents = tape.filter((e) => e.type === 'book_snapshot' || e.type === 'book_delta'
                                          || e.type === 'book_top');
  const eventsBySym = new Map<number, number>();
  for (const ev of allBookEvents) {
    if (ev.type === 'book_snapshot' || ev.type === 'book_delta' || ev.type === 'book_top') {
      eventsBySym.set(ev.symbol_id, (eventsBySym.get(ev.symbol_id) ?? 0) + 1);
    }
  }
  const sig = `${state.tape.length}|${allBookEvents.length}|${allBookEvents[allBookEvents.length - 1]?.ts_ns ?? ''}|${w}x${h}|${[...eventsBySym.keys()].sort().join(',')}`;
  if (sig === lastOrderbookSig) return;
  lastOrderbookSig = sig;
  if (allBookEvents.length === 0 || eventsBySym.size === 0) {
    empty.hidden = false;
    empty.textContent = state.tape.length === 0
      ? 'No `.lrvx` loaded.'
      : 'Tape has no book events at this cursor.';
    return;
  }
  empty.hidden = true;

  // Pick venues to render: ≤2 venues → side-by-side split. >2 → busiest
  // single venue + footer hint. Single venue → full-pane.
  const venues = [...eventsBySym.entries()]
                   .sort((a, b) => b[1] - a[1])  // busiest first
                   .map(([sid]) => sid);
  const showVenues = venues.length <= 2 ? venues : venues.slice(0, 1);
  const paneW = w / showVenues.length;
  showVenues.forEach((sid, paneIdx) => {
    const { bids, asks, hasSnapshot } = reconstructBook(allBookEvents as any, sid);
    // With tops only so far, the touch is all there is to show.
    const touch = touchAfterDepth(allBookEvents, sid);
    if (!hasSnapshot && !touch) {
      ctx.fillStyle = '#9ca3af';
      ctx.font = '11px sans-serif';
      ctx.fillText('waiting for first snapshot…',
                    paneIdx * paneW + 16, 30);
      return;
    }
    paintOneBook(ctx, state, sid, bids, asks, touch, paneIdx * paneW, 0, paneW, h);
    if (paneIdx > 0) {
      // Visual divider between panes.
      ctx.strokeStyle = '#e5e7eb';
      ctx.beginPath();
      ctx.moveTo(paneIdx * paneW, 6);
      ctx.lineTo(paneIdx * paneW, h - 6);
      ctx.stroke();
    }
  });
  if (venues.length > 2) {
    ctx.fillStyle = '#94a3b8';
    ctx.font = '10px sans-serif';
    ctx.fillText(
      `(showing busiest of ${venues.length} venues; side-by-side mode kicks in at ≤2)`,
      8, h - 6);
  }
}

export function renderSignals(state: ViewerState) {
  const el = document.getElementById('view-signals')!;
  const empty = document.querySelector<HTMLElement>('.emptystate[data-for="signals"]')!;
  if (!state.run) {
    empty.hidden = false;
    empty.textContent = 'No `.lrvxrun` loaded. Drop one to see strategy decisions.';
    el.innerHTML = '';
    return;
  }
  const sigsAll = signalsSlice(state);
  const sig = `${state.run.signals.length}|${sigsAll.length}|${sigsAll[sigsAll.length - 1]?.run_ts_ns ?? ''}`;
  if (sig === lastSignalsSig) return;
  lastSignalsSig = sig;
  const sigs = sigsAll.slice(-50).reverse();
  empty.hidden = true;
  el.innerHTML = sigs.map((s) => `
    <div class="signal-card">
      <div><span class="ts">${fmtTs(s.run_ts_ns)}</span> <span class="name">${escapeHtml(s.name)}</span> id=${s.signal_id}</div>
      <div>symbols=[${s.symbol_ids.join(', ')}] flags=${s.flags} strength=${s.strength.toFixed(4)}</div>
      ${s.payload ? `<div style="color: var(--muted)">${escapeHtml(s.payload)}</div>` : ''}
    </div>
  `).join('');
}

const ORDER_KIND_NAMES = ['?', 'submit', 'cancel', 'modify', 'ack', 'reject', 'expire'];

export function renderOrders(state: ViewerState) {
  const el = document.getElementById('view-orders')!;
  const empty = document.querySelector<HTMLElement>('.emptystate[data-for="orders"]')!;
  if (!state.run) {
    empty.hidden = false;
    el.innerHTML = '';
    return;
  }
  const ordersAll = ordersSlice(state);
  const fillsAll = fillsSlice(state);
  const sig = `${state.run.orders.length}|${ordersAll.length}|${fillsAll.length}|${ordersAll[ordersAll.length - 1]?.run_ts_ns ?? ''}|${fillsAll[fillsAll.length - 1]?.run_ts_ns ?? ''}`;
  if (sig === lastOrdersSig) return;
  lastOrdersSig = sig;
  const orders = ordersAll.slice(-50).reverse();
  const fills = fillsAll;
  const fillsByOrder = new Map<string, number>();
  for (const f of fills) {
    const k = f.order_id.toString();
    fillsByOrder.set(k, (fillsByOrder.get(k) ?? 0) + f.qty);
  }
  empty.hidden = true;
  el.innerHTML = orders.map((o) => {
    const filled = fillsByOrder.get(o.order_id.toString()) ?? 0;
    const kind = ORDER_KIND_NAMES[o.event_kind] ?? `?${o.event_kind}`;
    return `
      <div class="order-card">
        <div><span class="ts">${fmtTs(o.run_ts_ns)}</span>
             <span class="id">order=${o.order_id}</span>
             ${kind} ${o.side === 0 ? 'buy' : 'sell'} ${o.qty.toFixed(4)} @ ${o.price.toFixed(2)}</div>
        ${filled > 0 ? `<div>filled: ${filled.toFixed(4)}</div>` : ''}
        ${o.reason ? `<div style="color: var(--muted)">${escapeHtml(o.reason)}</div>` : ''}
      </div>
    `;
  }).join('');
}

export function renderEquity(state: ViewerState) {
  const canvas = document.getElementById('view-equity') as HTMLCanvasElement;
  const empty = document.querySelector<HTMLElement>('.emptystate[data-for="equity"]')!;
  const ctx = canvas.getContext('2d');
  if (!ctx) return;
  const dpr = window.devicePixelRatio || 1;
  const w = canvas.clientWidth, h = canvas.clientHeight;
  canvas.width = w * dpr;
  canvas.height = h * dpr;
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, w, h);
  if (!state.run) {
    empty.hidden = false;
    return;
  }
  const fills = fillsSlice(state);
  const sig = `${state.run.fills.length}|${fills.length}|${fills[fills.length - 1]?.run_ts_ns ?? ''}|${w}x${h}`;
  if (sig === lastEquitySig) return;
  lastEquitySig = sig;
  if (fills.length === 0) {
    empty.hidden = false;
    empty.textContent = 'No fills yet at this cursor.';
    return;
  }
  empty.hidden = true;
  // Naive PnL: position * mark, mark = last fill price per symbol.
  // Realized = signed fill * price. Good enough for MVP.
  type SymState = { pos: number; cash: number };
  const states = new Map<number, SymState>();
  const points: Array<{ ts: bigint; equity: number }> = [];
  for (const f of fills) {
    let st = states.get(f.symbol_id);
    if (!st) { st = { pos: 0, cash: 0 }; states.set(f.symbol_id, st); }
    const signedQty = f.side === 0 ? f.qty : -f.qty;
    st.pos += signedQty;
    st.cash -= signedQty * f.price + f.fee;
    let total = 0;
    for (const s of states.values()) total += s.cash + s.pos * f.price; // mark with last seen price
    points.push({ ts: f.run_ts_ns, equity: total });
  }
  const minEq = Math.min(...points.map((p) => p.equity), 0);
  const maxEq = Math.max(...points.map((p) => p.equity), 0);
  const span = Math.max(1e-9, maxEq - minEq);
  const t0 = points[0].ts;
  const tN = points[points.length - 1].ts;
  const tSpan = Math.max(1, Number(tN - t0));
  ctx.strokeStyle = '#138caf';
  ctx.lineWidth = 1.5;
  ctx.beginPath();
  points.forEach((p, i) => {
    const x = ((Number(p.ts - t0) / tSpan) * (w - 16)) + 8;
    const y = h - 8 - ((p.equity - minEq) / span) * (h - 16);
    if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
  });
  ctx.stroke();
  ctx.fillStyle = '#9ca3af';
  ctx.font = '10px ui-monospace, monospace';
  ctx.fillText(`min ${minEq.toFixed(2)}`, 8, h - 4);
  ctx.fillText(`max ${maxEq.toFixed(2)}`, 8, 12);
}

// Cached chart geometry from the most recent render. Mouse handlers
// read this to convert pointer position back into a tape timestamp.
interface ChartContext {
  trades: Trade[];
  t0: bigint;
  tN: bigint;
  padL: number;
  padT: number;
  plotW: number;
  plotH: number;
  minP: number;
  maxP: number;
  normalizeMode: boolean;
  baseline: Map<number, number>;
}
let priceChartCtx: ChartContext | null = null;
let priceChartWired = false;

// Offscreen canvas holding everything that does not move when the
// cursor moves: grid, line, trade dots, signal bands, fill triangles.
// We blit it onto the visible canvas every frame and only paint the
// cursor on top in real time.
let priceChartStaticCanvas: HTMLCanvasElement | null = null;
let lastPriceStaticSig = '';

function wirePriceChart(canvas: HTMLCanvasElement, tooltip: HTMLElement) {
  if (priceChartWired) return;
  priceChartWired = true;

  const xToTs = (x: number): bigint | null => {
    const c = priceChartCtx;
    if (!c) return null;
    const tSpan = Number(c.tN - c.t0);
    const frac = (x - c.padL) / c.plotW;
    if (!Number.isFinite(frac) || frac < 0 || frac > 1) return null;
    return c.t0 + BigInt(Math.round(frac * tSpan));
  };

  const nearestTrade = (ts: bigint): Trade | null => {
    const c = priceChartCtx;
    if (!c || c.trades.length === 0) return null;
    let lo = 0, hi = c.trades.length - 1;
    while (lo < hi) {
      const mid = (lo + hi) >>> 1;
      if (c.trades[mid].ts_ns < ts) lo = mid + 1; else hi = mid;
    }
    const candidates = [c.trades[lo - 1], c.trades[lo]].filter(Boolean) as Trade[];
    let best = candidates[0];
    for (const t of candidates) {
      if (t.ts_ns > ts ? t.ts_ns - ts : ts - t.ts_ns) {
        const da = t.ts_ns > ts ? t.ts_ns - ts : ts - t.ts_ns;
        const db = best.ts_ns > ts ? best.ts_ns - ts : ts - best.ts_ns;
        if (da < db) best = t;
      }
    }
    return best;
  };

  const fmtTs = (ns: bigint) =>
    new Date(Number(ns / 1_000_000n)).toISOString().slice(11, 23);

  canvas.addEventListener('mousemove', (ev) => {
    const c = priceChartCtx;
    if (!c) return;
    const rect = canvas.getBoundingClientRect();
    const x = ev.clientX - rect.left;
    const y = ev.clientY - rect.top;
    if (x < c.padL || x > c.padL + c.plotW || y < c.padT || y > c.padT + c.plotH) {
      tooltip.hidden = true;
      return;
    }
    const ts = xToTs(x);
    if (ts === null) {
      tooltip.hidden = true;
      return;
    }
    const t = nearestTrade(ts);
    if (!t) {
      tooltip.hidden = true;
      return;
    }
    const sideClass = t.side === 'buy' ? 'side-buy' : 'side-sell';
    tooltip.innerHTML =
      `<span class="ts">${fmtTs(t.ts_ns)}</span>` +
      `<span class="${sideClass}">${t.side}</span> ` +
      `${t.price.toFixed(2)} × ${t.qty.toFixed(4)}`;
    tooltip.hidden = false;
    // Position above-right of the cursor; clamp to canvas.
    const tipX = Math.min(x + 12, canvas.clientWidth - tooltip.offsetWidth - 4);
    const tipY = Math.max(y - tooltip.offsetHeight - 8, 4);
    tooltip.style.left = `${tipX}px`;
    tooltip.style.top = `${tipY}px`;
  });
  canvas.addEventListener('mouseleave', () => { tooltip.hidden = true; });
  canvas.addEventListener('click', (ev) => {
    const rect = canvas.getBoundingClientRect();
    const ts = xToTs(ev.clientX - rect.left);
    if (ts !== null) {
      canvas.dispatchEvent(new CustomEvent('viewer-seek', {
        detail: { ts }, bubbles: true,
      }));
    }
  });
}

export function renderPriceChart(state: ViewerState) {
  const canvas = document.getElementById('view-price') as HTMLCanvasElement;
  const tooltip = document.getElementById('price-tooltip') as HTMLElement;
  const empty = document.querySelector<HTMLElement>('.emptystate[data-for="price"]')!;
  const ctx = canvas.getContext('2d');
  if (!ctx) return;
  wirePriceChart(canvas, tooltip);
  const dpr = window.devicePixelRatio || 1;
  const w = canvas.clientWidth, h = canvas.clientHeight;
  canvas.width = w * dpr;
  canvas.height = h * dpr;
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, w, h);

  const trades: Trade[] = state.tape.filter((e): e is Trade => e.type === 'trade');
  if (trades.length === 0) {
    empty.hidden = false;
    empty.textContent = state.tape.length === 0
      ? 'No `.lrvx` loaded. Drop one to see the price chart.'
      : 'Tape has no trades.';
    priceChartCtx = null;
    return;
  }
  empty.hidden = true;

  // For multi-venue tapes, clip the X-axis to the time range where
  // ALL venues have data. Otherwise the slow-to-connect WebSocket
  // produces a leading gap where one line is missing — confusing
  // because the gap looks like a state change.
  const firstPerSym = new Map<number, bigint>();
  const lastPerSym = new Map<number, bigint>();
  for (const t of trades) {
    if (!firstPerSym.has(t.symbol_id)) firstPerSym.set(t.symbol_id, t.ts_ns);
    lastPerSym.set(t.symbol_id, t.ts_ns);
  }
  let t0 = state.ts_min;
  let tN = state.ts_max;
  if (firstPerSym.size >= 2) {
    const latestFirst = [...firstPerSym.values()].reduce((a, b) => a > b ? a : b);
    const earliestLast = [...lastPerSym.values()].reduce((a, b) => a < b ? a : b);
    if (earliestLast > latestFirst) {
      t0 = latestFirst;
      tN = earliestLast;
    }
  }

  // Per-symbol price ranges + baseline. Used to decide whether the
  // Y-axis should be raw price (when all symbols share a comparable
  // scale, e.g. two BTC perp tapes at ~80k each — the basis is the
  // story and we want to see it as a visual offset) or % change from
  // each symbol's baseline (when scales diverge wildly, e.g. BTC at
  // 100k vs SHIB at 0.0000001 — absolute is unviewable).
  const baseline = new Map<number, number>();
  const symRange = new Map<number, { lo: number, hi: number }>();
  for (const t of trades) {
    if (!baseline.has(t.symbol_id)) baseline.set(t.symbol_id, t.price);
    const r = symRange.get(t.symbol_id);
    if (!r) symRange.set(t.symbol_id, { lo: t.price, hi: t.price });
    else {
      if (t.price < r.lo) r.lo = t.price;
      if (t.price > r.hi) r.hi = t.price;
    }
  }
  const sids = [...symRange.keys()];
  // Use % mode only when ranges genuinely don't overlap on the same
  // scale. Heuristic: pairwise ratio (max / min) of midpoints > 1.5.
  // Two BTC perps differ by < 1%, ratio ≈ 1.001 → absolute.
  let normalizeMode = false;
  if (sids.length >= 2) {
    const mids = sids.map((s) => {
      const r = symRange.get(s)!;
      return (r.lo + r.hi) / 2;
    });
    const minMid = Math.min(...mids);
    const maxMid = Math.max(...mids);
    if (minMid > 0 && maxMid / minMid > 1.5) {
      normalizeMode = true;
    }
  }

  let minP = Infinity, maxP = -Infinity;
  if (normalizeMode) {
    for (const t of trades) {
      const b = baseline.get(t.symbol_id)!;
      const pct = (t.price - b) / b * 100;
      if (pct < minP) minP = pct;
      if (pct > maxP) maxP = pct;
    }
  } else {
    for (const t of trades) {
      if (t.price < minP) minP = t.price;
      if (t.price > maxP) maxP = t.price;
    }
  }
  const padP = Math.max(1e-9, (maxP - minP) * 0.08);
  minP -= padP; maxP += padP;

  const padL = 56, padR = 12, padT = 10, padB = 22;
  const plotW = w - padL - padR;
  const plotH = h - padT - padB;
  priceChartCtx = { trades, t0, tN, padL, padT, plotW, plotH, minP, maxP,
                    normalizeMode, baseline };

  const sigCount = state.run ? state.run.signals.length : 0;
  const fillCount = state.run ? state.run.fills.length : 0;
  const sigKey = `${trades.length}|${sigCount}|${fillCount}|${w}x${h}|${minP}|${maxP}|${normalizeMode}|${state.symbols.size}`;
  if (sigKey !== lastPriceStaticSig) {
    lastPriceStaticSig = sigKey;
    if (!priceChartStaticCanvas) priceChartStaticCanvas = document.createElement('canvas');
    priceChartStaticCanvas.width = w * dpr;
    priceChartStaticCanvas.height = h * dpr;
    const sctx = priceChartStaticCanvas.getContext('2d')!;
    sctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    sctx.clearRect(0, 0, w, h);
    paintPriceStatic(sctx, w, h, trades, state, t0, tN, padL, padT, plotW, plotH, minP, maxP,
                     normalizeMode, baseline);
  }

  if (priceChartStaticCanvas) ctx.drawImage(priceChartStaticCanvas, 0, 0, w, h);
  if (state.cursor_ns >= t0 && state.cursor_ns <= tN) {
    const tSpan = Math.max(1, Number(tN - t0));
    const cx = padL + (Number(state.cursor_ns - t0) / tSpan) * plotW;
    ctx.strokeStyle = 'rgba(26, 35, 50, 0.4)';
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.moveTo(cx, padT);
    ctx.lineTo(cx, h - padB);
    ctx.stroke();
  }
}

function paintPriceStatic(target: CanvasRenderingContext2D, w: number, h: number,
                           trades: Trade[], state: ViewerState,
                           t0: bigint, tN: bigint,
                           padL: number, padT: number, plotW: number, plotH: number,
                           minP: number, maxP: number,
                           normalizeMode: boolean = false,
                           baseline: Map<number, number> = new Map()) {
  const tSpan = Math.max(1, Number(tN - t0));
  const pSpan = Math.max(1e-9, maxP - minP);
  const xOf = (ts: bigint) => padL + (Number(ts - t0) / tSpan) * plotW;
  // In multi-symbol normalize mode the Y-axis is "% change from each
  // symbol's first observation"; in single-symbol mode it's the raw
  // price. Same plotting math, different scale meaning.
  const yOfPrice = (price: number, sid: number) => {
    if (!normalizeMode) return padT + (1 - (price - minP) / pSpan) * plotH;
    const b = baseline.get(sid);
    if (b === undefined || b === 0) return padT + plotH;
    const pct = (price - b) / b * 100;
    return padT + (1 - (pct - minP) / pSpan) * plotH;
  };
  target.strokeStyle = '#eef0f4';
  target.lineWidth = 1;
  target.fillStyle = '#9ca3af';
  target.font = '10px ui-monospace, monospace';
  target.textBaseline = 'middle';
  for (let i = 0; i <= 4; i++) {
    const y = padT + (i / 4) * plotH;
    target.beginPath();
    target.moveTo(padL, y);
    target.lineTo(w - 12, y);
    target.stroke();
    const p = maxP - (i / 4) * pSpan;
    target.fillText(normalizeMode ? `${p.toFixed(2)}%` : p.toFixed(2), 6, y);
  }
  if (normalizeMode) {
    // Faint zero-line: where each symbol's series enters the view.
    const yZero = padT + (1 - (0 - minP) / pSpan) * plotH;
    target.strokeStyle = '#cbd5e1';
    target.setLineDash([4, 3]);
    target.beginPath();
    target.moveTo(padL, yZero);
    target.lineTo(w - 12, yZero);
    target.stroke();
    target.setLineDash([]);
  }
  // Group trades by symbol_id, then drop events outside the visible
  // time window. Two reasons:
  //   1. Multi-venue tapes get a distinct polyline per group.
  //   2. Without the window filter, canvas keeps off-canvas line
  //      segments visible up to the moment they cross the left edge —
  //      so a venue whose first trade falls 13s before the clip start
  //      "starts" mid-chart instead of at padL. Filtering to the
  //      in-window slice makes every polyline begin exactly at the
  //      first event inside the clip, which after `latestFirst` clip
  //      means both lines start at the same X coordinate.
  const bySymbol = new Map<number, Trade[]>();
  for (const t of trades) {
    if (t.ts_ns < t0 || t.ts_ns > tN) continue;
    let arr = bySymbol.get(t.symbol_id);
    if (!arr) { arr = []; bySymbol.set(t.symbol_id, arr); }
    arr.push(t);
  }
  for (const [sid, group] of bySymbol) {
    if (group.length === 0) continue;
    target.strokeStyle = symbolColor(sid);
    target.lineWidth = 1.2;
    target.beginPath();
    group.forEach((t, i) => {
      const x = xOf(t.ts_ns), y = yOfPrice(t.price, t.symbol_id);
      if (i === 0) target.moveTo(x, y); else target.lineTo(x, y);
    });
    target.stroke();
  }
  for (const t of trades) {
    if (t.ts_ns < t0 || t.ts_ns > tN) continue;
    target.fillStyle = t.side === 'buy' ? 'rgba(33, 161, 101, 0.55)' : 'rgba(214, 70, 75, 0.55)';
    target.beginPath();
    target.arc(xOf(t.ts_ns), yOfPrice(t.price, t.symbol_id), 1.6, 0, Math.PI * 2);
    target.fill();
  }
  const sigs = state.run ? state.run.signals : [];
  target.font = '10px ui-monospace, monospace';
  for (const s of sigs) {
    if (s.run_ts_ns < t0 || s.run_ts_ns > tN) continue;
    const x = xOf(s.run_ts_ns);
    const isExit = (s.flags & 0x02) !== 0;
    target.strokeStyle = isExit ? 'rgba(214, 70, 75, 0.5)' : 'rgba(33, 161, 101, 0.5)';
    target.lineWidth = 1;
    target.setLineDash([3, 3]);
    target.beginPath();
    target.moveTo(x, padT);
    target.lineTo(x, h - 22);
    target.stroke();
    target.setLineDash([]);
    target.fillStyle = isExit ? '#d6464b' : '#21a165';
    target.fillText(s.name, x + 3, padT + 8);
  }
  const fills = state.run ? state.run.fills : [];
  for (const f of fills) {
    if (f.run_ts_ns < t0 || f.run_ts_ns > tN) continue;
    const x = xOf(f.run_ts_ns);
    const y = yOfPrice(f.price, f.symbol_id);
    target.fillStyle = f.side === 0 ? '#21a165' : '#d6464b';
    target.beginPath();
    if (f.side === 0) {
      target.moveTo(x, y - 6);
      target.lineTo(x - 4, y);
      target.lineTo(x + 4, y);
    } else {
      target.moveTo(x, y + 6);
      target.lineTo(x - 4, y);
      target.lineTo(x + 4, y);
    }
    target.closePath();
    target.fill();
  }
}

function escapeHtml(s: string): string {
  return s.replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]!));
}
