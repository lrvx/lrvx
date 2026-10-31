"""Simulate venue downtime: a scheduled outage and Poisson random outages."""
import lrvx

va = lrvx.VenueAvailability()

t0 = 1_700_000_000_000_000_000  # arbitrary epoch ns
ONE_HOUR_NS = 3600 * 10**9
TWO_MIN_NS = 120 * 10**9
THIRTY_SEC_NS = 30 * 10**9

# Scheduled maintenance: 2-minute window 1 hour from now, kill all open orders.
va.schedule_outage(start_ns=t0 + ONE_HOUR_NS,
                   duration_ns=TWO_MIN_NS,
                   on_open_orders="cancel_all")

# Random outages: 0.5 per day, 30-second mean duration, HOLD policy.
va.auto_random_outages(per_day=0.5,
                       mean_duration_ns=THIRTY_SEC_NS,
                       on_open_orders="hold",
                       seed=42)

print("up at t0:                          ", va.is_up(t0))
print("up at t0 + 30min:                  ", va.is_up(t0 + ONE_HOUR_NS // 2))
print("up at t0 + 1h + 60s (mid outage):  ", va.is_up(t0 + ONE_HOUR_NS + 60 * 10**9))
print("up at t0 + 1h + 5min (post outage):", va.is_up(t0 + ONE_HOUR_NS + 5 * 60 * 10**9))

# Attach to a SimulatedExecutor; submits during outages buffer + flush at recovery.
exec = lrvx.SimulatedExecutor()
exec.set_venue_availability(va)
print("attached venue availability")
exec.set_venue_availability(None)
print("detached venue availability")
