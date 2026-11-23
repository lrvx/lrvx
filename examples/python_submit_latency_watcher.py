"""Detect submit-side late-cross rejections from a strategy."""
import lrvx


class Watcher(lrvx.Strategy):
    def __init__(self, symbols):
        super().__init__(symbols)
        self.post_only_failures = 0

    def on_order_update(self, ctx, ev):
        if (ev.status == "REJECTED" and
                ev.reject_reason == "late_post_only_crossed"):
            self.post_only_failures += 1
