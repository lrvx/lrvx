"""HTML backtest report.

Renders a self-contained HTML file from a backtest's stats, equity
curve, and trades. No external assets — every chart is inline SVG —
so the file works offline and can be checked into a repo as-is.

Use it directly from a strategy script after a backtest::

    from lrvx.report import write_html

    bt = lrvx.BacktestRunner(registry, fee_rate=0.0004,
                             initial_capital=10_000)
    bt.set_strategy(MyStrategy([btc]))
    stats = bt.run_csv("data.csv", symbol="BTCUSDT")
    write_html(
        "report.html",
        stats=stats,
        equity_curve=bt.equity_curve(),
        trades=bt.trades(),
    )

Or via the CLI on a JSON dump of the stats dict::

    lrvx report stats.json -o report.html

The CLI path renders a summary-only report (no charts) because it
only sees the stats dict — the rich version needs in-process access
to the runner.
"""
from __future__ import annotations

import html
import json
from pathlib import Path
from typing import Any, Mapping, Optional, Sequence


_PCT_KEYS = {"return_pct", "max_drawdown_pct", "win_rate"}


def _fmt_value(key: str, val: Any) -> str:
    if val is None:
        return "—"
    if isinstance(val, float):
        if key == "win_rate":
            return f"{val * 100:.2f}%"
        if key in _PCT_KEYS:
            return f"{val:+.4f}%" if "return" in key else f"{val:.4f}%"
        return f"{val:.4f}"
    return str(val)


_SUMMARY_LAYOUT = [
    ("Return", "return_pct"),
    ("Sharpe", "sharpe_ratio"),
    ("Max DD", "max_drawdown_pct"),
    ("Trades", "total_trades"),
    ("Win rate", "win_rate"),
    ("Profit factor", "profit_factor"),
    ("Net PnL", "net_pnl"),
    ("Total fees", "total_fees"),
    ("Initial capital", "initial_capital"),
    ("Final capital", "final_capital"),
    ("Unrealized PnL", "unrealized_pnl"),
    ("Final equity", "final_equity"),
]


def _summary_cards(stats: Mapping[str, Any]) -> str:
    cards = []
    for label, key in _SUMMARY_LAYOUT:
        if key not in stats:
            continue
        val_str = _fmt_value(key, stats.get(key))
        klass = "card"
        # Highlight return/sharpe in green/red so the eye lands there.
        if key == "return_pct":
            v = stats.get(key)
            if isinstance(v, (int, float)):
                klass += " good" if v > 0 else " bad"
        if key == "sharpe_ratio":
            v = stats.get(key)
            if isinstance(v, (int, float)):
                klass += " good" if v > 1 else ("bad" if v < 0 else "")
        cards.append(
            f'<div class="{klass}"><div class="label">{html.escape(label)}'
            f'</div><div class="val">{html.escape(val_str)}</div></div>'
        )
    return "\n      ".join(cards)


def _svg_line_chart(
    *,
    title: str,
    x_values: Sequence[float],
    y_values: Sequence[float],
    y_baseline: Optional[float] = None,
    width: int = 900,
    height: int = 240,
    padding_left: int = 60,
    padding_right: int = 20,
    padding_top: int = 20,
    padding_bottom: int = 30,
    color: str = "#2f81f7",
) -> str:
    if not x_values or not y_values:
        return f'<div class="chart-empty">{html.escape(title)}: no data</div>'
    n = len(y_values)
    xmin, xmax = min(x_values), max(x_values)
    ymin, ymax = min(y_values), max(y_values)
    if y_baseline is not None:
        ymin = min(ymin, y_baseline)
        ymax = max(ymax, y_baseline)
    if xmax == xmin:
        xmax = xmin + 1
    if ymax == ymin:
        ymax = ymin + 1
    iw = width - padding_left - padding_right
    ih = height - padding_top - padding_bottom

    def sx(x: float) -> float:
        return padding_left + (x - xmin) / (xmax - xmin) * iw

    def sy(y: float) -> float:
        return padding_top + (1 - (y - ymin) / (ymax - ymin)) * ih

    pts = " ".join(f"{sx(x_values[i]):.2f},{sy(y_values[i]):.2f}"
                   for i in range(n))

    # Y-axis ticks: 4 evenly spaced.
    y_ticks = []
    for i in range(5):
        yv = ymin + (ymax - ymin) * i / 4
        yp = sy(yv)
        y_ticks.append(
            f'<line x1="{padding_left}" y1="{yp:.1f}" '
            f'x2="{width - padding_right}" y2="{yp:.1f}" '
            f'stroke="#30363d" stroke-dasharray="2,3"/>'
            f'<text x="{padding_left - 6}" y="{yp + 3:.1f}" '
            f'class="ax">{yv:.2f}</text>'
        )
    baseline_svg = ""
    if y_baseline is not None and ymin <= y_baseline <= ymax:
        bp = sy(y_baseline)
        baseline_svg = (
            f'<line x1="{padding_left}" y1="{bp:.1f}" '
            f'x2="{width - padding_right}" y2="{bp:.1f}" '
            f'stroke="#7d8590" stroke-width="1"/>'
        )

    return f'''<div class="chart">
  <div class="chart-title">{html.escape(title)}</div>
  <svg viewBox="0 0 {width} {height}" xmlns="http://www.w3.org/2000/svg">
    {''.join(y_ticks)}
    {baseline_svg}
    <polyline points="{pts}" fill="none" stroke="{color}" stroke-width="1.5"/>
  </svg>
</div>'''


def _trades_table(trades: Mapping[str, Sequence[Any]],
                  limit: int = 50) -> str:
    def _to_list(key: str) -> list:
        v = trades.get(key)
        return list(v) if v is not None else []
    pnl = _to_list("pnl")
    if not pnl:
        return '<div class="empty">no trades recorded</div>'
    n = len(pnl)
    sym = _to_list("symbol")
    side = _to_list("side")
    ep = _to_list("entry_price")
    xp = _to_list("exit_price")
    qty = _to_list("quantity")
    fee = _to_list("fee")
    show = min(n, limit)
    rows = []
    for i in range(show):
        side_str = "long" if int(side[i]) == 0 else "short"
        klass = "win" if pnl[i] > 0 else ("lose" if pnl[i] < 0 else "")
        rows.append(
            f'<tr class="{klass}">'
            f'<td>{i + 1}</td>'
            f'<td>{html.escape(side_str)}</td>'
            f'<td>{ep[i]:.4f}</td>'
            f'<td>{xp[i]:.4f}</td>'
            f'<td>{qty[i]:.4f}</td>'
            f'<td>{pnl[i]:+.4f}</td>'
            f'<td>{fee[i]:.4f}</td>'
            f'</tr>'
        )
    rows_html = "\n        ".join(rows)
    note = ""
    if n > show:
        note = (f'<p class="muted">showing first {show} of {n} trades.</p>')
    return f'''<table class="trades">
  <thead>
    <tr><th>#</th><th>side</th><th>entry</th><th>exit</th>
        <th>qty</th><th>pnl</th><th>fee</th></tr>
  </thead>
  <tbody>
        {rows_html}
  </tbody>
</table>
{note}'''


_HTML_TEMPLATE = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>lrvx backtest report</title>
<style>
  :root {{
    --bg: #0e1116; --fg: #e6edf3; --muted: #7d8590; --border: #30363d;
    --card-bg: #161b22; --good: #3fb950; --bad: #f85149;
  }}
  * {{ box-sizing: border-box; }}
  body {{
    margin: 0; font: 14px/1.5 -apple-system, BlinkMacSystemFont, "Segoe UI",
      Inter, sans-serif; background: var(--bg); color: var(--fg);
  }}
  header {{
    padding: 24px 32px; border-bottom: 1px solid var(--border);
  }}
  header h1 {{ margin: 0 0 4px 0; font-size: 20px; }}
  header .sub {{ color: var(--muted); font-size: 12px; }}
  main {{ padding: 24px 32px; max-width: 1100px; margin: 0 auto; }}
  .cards {{
    display: grid; grid-template-columns: repeat(5, 1fr); gap: 12px;
    margin-bottom: 24px;
  }}
  .card {{
    background: var(--card-bg); border: 1px solid var(--border);
    border-radius: 6px; padding: 12px;
  }}
  .card .label {{ color: var(--muted); font-size: 11px;
    text-transform: uppercase; letter-spacing: 0.5px; }}
  .card .val {{ font-size: 18px; margin-top: 4px; font-weight: 500; }}
  .card.good .val {{ color: var(--good); }}
  .card.bad .val {{ color: var(--bad); }}
  section {{ margin-bottom: 32px; }}
  section h2 {{ font-size: 14px; color: var(--muted);
    text-transform: uppercase; letter-spacing: 0.5px;
    border-bottom: 1px solid var(--border); padding-bottom: 6px; }}
  .chart {{
    background: var(--card-bg); border: 1px solid var(--border);
    border-radius: 6px; padding: 12px; margin-bottom: 12px;
  }}
  .chart-title {{ color: var(--muted); font-size: 12px; margin-bottom: 6px; }}
  .chart svg {{ width: 100%; height: auto; }}
  .chart-empty {{ color: var(--muted); padding: 12px;
    background: var(--card-bg); border: 1px solid var(--border);
    border-radius: 6px; }}
  text.ax {{ fill: var(--muted); font-size: 10px; text-anchor: end; }}
  table.trades {{
    width: 100%; border-collapse: collapse;
    background: var(--card-bg); border: 1px solid var(--border);
    border-radius: 6px; overflow: hidden;
  }}
  table.trades th, table.trades td {{
    padding: 6px 10px; text-align: right; font-variant-numeric: tabular-nums;
    font-size: 12px;
  }}
  table.trades th {{
    background: rgba(255,255,255,0.03); color: var(--muted);
    text-transform: uppercase; font-size: 11px;
  }}
  table.trades th:nth-child(1), table.trades td:nth-child(1) {{
    text-align: left; color: var(--muted); }}
  table.trades th:nth-child(2), table.trades td:nth-child(2) {{
    text-align: left; }}
  table.trades tr.win td:nth-child(6) {{ color: var(--good); }}
  table.trades tr.lose td:nth-child(6) {{ color: var(--bad); }}
  .muted {{ color: var(--muted); font-size: 12px; }}
  .empty {{ color: var(--muted); font-style: italic; padding: 12px;
    background: var(--card-bg); border: 1px solid var(--border);
    border-radius: 6px; }}
</style>
</head>
<body>
<header>
  <h1>{title}</h1>
  <div class="sub">{subtitle}</div>
</header>
<main>
  <section>
    <div class="cards">
      {cards}
    </div>
  </section>
  <section>
    <h2>Equity curve</h2>
    {equity_chart}
  </section>
  <section>
    <h2>Drawdown</h2>
    {drawdown_chart}
  </section>
  <section>
    <h2>Trades</h2>
    {trades_table}
  </section>
</main>
</body>
</html>
"""


def render_html(
    stats: Mapping[str, Any],
    *,
    equity_curve: Optional[Mapping[str, Sequence[float]]] = None,
    trades: Optional[Mapping[str, Sequence[Any]]] = None,
    title: str = "lrvx backtest report",
    subtitle: str = "",
) -> str:
    """Build an HTML string from stats + optional equity / trades arrays."""
    cards = _summary_cards(stats)
    eq_arr = (equity_curve or {}).get("equity")
    if eq_arr is not None and len(eq_arr) > 0:
        ts_arr = (equity_curve or {}).get("timestamp_ns")
        ts = list(ts_arr) if ts_arr is not None else []
        eq = list(eq_arr)
        x = ts if len(ts) > 0 else list(range(len(eq)))
        equity_chart = _svg_line_chart(
            title="Equity over time", x_values=x, y_values=eq,
            y_baseline=stats.get("initial_capital"),
        )
        dd_arr = (equity_curve or {}).get("drawdown_pct")
        if dd_arr is not None and len(dd_arr) > 0:
            drawdown_chart = _svg_line_chart(
                title="Drawdown %", x_values=x, y_values=list(dd_arr),
                y_baseline=0.0, color="#f85149",
            )
        else:
            drawdown_chart = '<div class="empty">no drawdown data</div>'
    else:
        equity_chart = ('<div class="empty">no equity data — pass '
                        'equity_curve=runner.equity_curve() to write_html</div>')
        drawdown_chart = '<div class="empty">no drawdown data</div>'
    pnl_arr = (trades or {}).get("pnl")
    if pnl_arr is not None and len(pnl_arr) > 0:
        trades_table = _trades_table(trades)
    else:
        trades_table = ('<div class="empty">no trades data — pass '
                        'trades=runner.trades() to write_html</div>')
    return _HTML_TEMPLATE.format(
        title=html.escape(title),
        subtitle=html.escape(subtitle),
        cards=cards,
        equity_chart=equity_chart,
        drawdown_chart=drawdown_chart,
        trades_table=trades_table,
    )


def write_html(
    output_path: str | Path,
    *,
    stats: Mapping[str, Any],
    equity_curve: Optional[Mapping[str, Sequence[float]]] = None,
    trades: Optional[Mapping[str, Sequence[Any]]] = None,
    title: str = "lrvx backtest report",
    subtitle: str = "",
) -> Path:
    """Write the HTML report to ``output_path``. Returns the resolved path."""
    out = Path(output_path)
    out.write_text(render_html(
        stats,
        equity_curve=equity_curve,
        trades=trades,
        title=title,
        subtitle=subtitle,
    ))
    return out


def heatmap_html(
    z,
    *,
    row_labels=None,
    col_labels=None,
    title: str = "",
    x_axis_name: str = "",
    y_axis_name: str = "",
    metric_name: str = "",
) -> str:
    """Render a heatmap of a 2D array as a self-contained HTML string.

    The render lives in C++ (``lrvx::report::renderHeatmapHtml``) and is
    shared with the Node and Codon bindings, so all three languages
    produce byte-identical output for the same input. Inline SVG only —
    no external assets, no scripts beyond ``<title>`` tooltips.

    ``z`` is row-major (list of lists, or any 2D iterable). Row and
    column labels are optional; when omitted, numeric indices are used.
    """
    from lrvx._lrvx import _heatmap  # type: ignore[attr-defined]

    rows = [list(map(float, r)) for r in z]
    return _heatmap.heatmap_html(
        rows,
        list(row_labels) if row_labels is not None else [],
        list(col_labels) if col_labels is not None else [],
        title, x_axis_name, y_axis_name, metric_name,
    )


def write_heatmap(
    output_path: str | Path,
    z,
    *,
    row_labels=None,
    col_labels=None,
    title: str = "",
    x_axis_name: str = "",
    y_axis_name: str = "",
    metric_name: str = "",
) -> Path:
    """Render :func:`heatmap_html` and write it to ``output_path``."""
    out = Path(output_path)
    out.write_text(heatmap_html(
        z,
        row_labels=row_labels, col_labels=col_labels,
        title=title, x_axis_name=x_axis_name,
        y_axis_name=y_axis_name, metric_name=metric_name,
    ))
    return out


def _load_json(path: str | Path) -> Any:
    with open(path) as f:
        return json.load(f)


def _arrays_to_lists(d: Mapping[str, Any]) -> dict:
    """Coerce numpy arrays / typed arrays to plain Python lists.

    Reports work fine with either, but JSON dumps want lists."""
    out = {}
    for k, v in d.items():
        try:
            out[k] = [float(x) for x in v] if isinstance(v[0], float) else list(v)
        except (TypeError, IndexError):
            out[k] = list(v) if v is not None else None
    return out
