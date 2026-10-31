"""React to a resting limit order moving across market-position states."""
import lrvx


class MarketPositionWatcher(lrvx.Strategy):
    def __init__(self, symbols):
        super().__init__(symbols)
        self.last = {}

    def on_market_position_change(self, ctx, ev):
        prev = self.last.get(ev.order_id)
        self.last[ev.order_id] = ev.market_position
        print(f"order {ev.order_id}: {prev} -> {ev.market_position} "
              f"distance={ev.distance_to_best_ticks}")
        # Example: cancel and reprice when we slip from best to behind_best.
        if ev.market_position == "behind_best":
            self.cancel(ev.order_id)
