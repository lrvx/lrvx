"""__PROJECT_NAME__ — lrvx live trading scaffold.

Edit ``__PROJECT_SLUG___strategy`` below. The scaffold runs in
**dry-run** mode by default — order placements are logged, the
exchange call is skipped. Set ``LRVX_LIVE=1`` and supply api keys via
``__PROJECT_PREFIX___API_KEY`` / ``__PROJECT_PREFIX___SECRET`` env vars to
route orders through the exchange (sandbox by default — see ``config.toml``).

Run::

    pip install -r requirements.txt
    python main.py
"""
from __future__ import annotations

import asyncio
import logging
import os
import signal
import sys

import lrvx
from lrvx.ccxt import CcxtBroker

from config import LiveConfig, load_config


logger = logging.getLogger(__name__)


class __PROJECT_SLUG___strategy(lrvx.Strategy):
    """SMA(fast/slow) crossover. Replace with your own logic."""

    def __init__(self, symbols, *, fast: int, slow: int, qty: float,
                 dry_run: bool):
        super().__init__(symbols)
        self.fast = lrvx.SMA(fast)
        self.slow = lrvx.SMA(slow)
        self.qty = qty
        self.dry_run = dry_run
        self.tick_count = 0

    def on_start(self):
        mode = "dry-run" if self.dry_run else "LIVE"
        logger.info("__PROJECT_NAME__ started in %s mode", mode)

    def on_stop(self):
        logger.info("__PROJECT_NAME__ stopped after %d ticks",
                    self.tick_count)

    def on_trade(self, ctx, trade):
        self.tick_count += 1
        if self.tick_count % 100 == 0:
            logger.info("processed %d ticks, last price %.2f",
                        self.tick_count, trade.price)
        f = self.fast.update(trade.price)
        s = self.slow.update(trade.price)
        if f is None or s is None or not self.slow.ready:
            return
        if self.dry_run:
            if f > s and ctx.is_flat():
                logger.info("[dry] would buy %.4f @ ~%.2f", self.qty, trade.price)
            elif f < s and ctx.is_long():
                logger.info("[dry] would sell %.4f @ ~%.2f", self.qty, trade.price)
            return
        if f > s and ctx.is_flat():
            self.market_buy(self.qty)
        elif f < s and ctx.is_long():
            self.market_sell(self.qty)

    def on_fill(self, ctx, event):
        logger.info("order %s %s %s @ %s position=%s", event.order_id, event.status,
                    event.fill_qty, event.fill_price, ctx.position)

    def on_order_update(self, ctx, event):
        logger.info("order %s %s", event.order_id, event.status)

    def on_initial_position(self, sym, qty, avg):
        logger.warning("starting with non-zero position on %s: qty=%s avg=%s",
                       sym, qty, avg)


async def run(config: LiveConfig) -> None:
    broker = CcxtBroker(
        exchange_id=config.exchange,
        api_key=os.environ.get(f"{config.env_prefix}_API_KEY"),
        secret=os.environ.get(f"{config.env_prefix}_SECRET"),
        password=os.environ.get(f"{config.env_prefix}_PASSPHRASE"),
        sandbox=config.sandbox,
        retry_initial_delay=config.retry_initial_delay,
        retry_max_delay=config.retry_max_delay,
        retry_multiplier=config.retry_multiplier,
    )
    async with broker:
        lrvx_symbols = []
        for sym in config.symbols:
            lrvx_symbols.append(await broker.add_symbol(sym))

        strat = __PROJECT_SLUG___strategy(
            lrvx_symbols,
            fast=config.fast_period,
            slow=config.slow_period,
            qty=config.order_qty,
            dry_run=config.dry_run,
        )
        broker.add_strategy(strat)

        # Graceful shutdown on SIGINT / SIGTERM. asyncio.run installs
        # its own SIGINT handler on POSIX; we override to also stop
        # the broker cleanly.
        loop = asyncio.get_running_loop()
        stop_event = asyncio.Event()

        def _shutdown():
            logger.info("shutdown signal received")
            stop_event.set()

        for sig in (signal.SIGINT, signal.SIGTERM):
            try:
                loop.add_signal_handler(sig, _shutdown)
            except (NotImplementedError, RuntimeError):
                # Windows / non-main thread.
                pass

        run_task = asyncio.create_task(
            broker.run(streams=tuple(config.streams),
                       book_depth=config.book_depth,
                       reconcile=config.reconcile_positions))

        try:
            await stop_event.wait()
        finally:
            run_task.cancel()
            try:
                await run_task
            except asyncio.CancelledError:
                pass


def _setup_logging(level: str) -> None:
    logging.basicConfig(
        level=getattr(logging, level.upper(), logging.INFO),
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
        stream=sys.stdout,
    )


def main() -> None:
    config = load_config()
    _setup_logging(config.log_level)
    if config.dry_run:
        logger.info("dry-run mode — orders will be logged, not placed")
    else:
        logger.warning("LIVE mode — orders will be sent to %s (%s)",
                       config.exchange,
                       "sandbox" if config.sandbox else "PRODUCTION")
    try:
        asyncio.run(run(config))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
