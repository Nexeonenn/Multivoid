# The send path

## Purpose

How fast the mod sends on each connection, who may put reliable bytes into a connection's send
buffer, and what happens to a send those rules hold back. The transport offers no bandwidth
estimate of its own, so every link a peer owns is measured and paced by the mod. This is the detail
under [architecture.md](architecture.md)'s transport section; the join's own download is in
[join.md](join.md).

## How it works

### The transport contributes no estimate

GameNetworkingSockets writes a connection's send rate once at connect, from the ping, and
thereafter only clamps it into the configured minimum and maximum. A link past a few milliseconds
of ping therefore runs at the configured floor for its whole life, whatever it can actually carry.

Nothing writes a rate globally. `coop/net/send_rate_control` is the sole writer, one decision per
connection, and `coop/net/connection_tuning` opens each link at the start rate. The global floor
that used to stand under every connection is gone, on two measurements: it was the rate rather than
a floor on every link a player has, and a host uplink thinner than it was overdriven into pure
loss, costing 71% of a 256 KB/s link. Its second premise -- that a raised floor shields the
unreliable pose stream from a reliable burst -- was wrong in the same runs. Nothing drops a queued
unreliable message; what starves one is the shared send buffer, and a reliable retransmission is
gathered ahead of the lane-priority loop, so overdrive is precisely what puts a bulk retry in front
of the pose datagram.

### Two byte counters, and no reference that can drift

Each slot is measured on two quantities, both byte counters:

- **served** -- bytes the peer acknowledges, plus unreliable bytes handed over, per second. A queue
  cannot inflate it: no amount of buffering makes a link deliver more than it can.
- **inflight** -- unacknowledged reliable bytes divided by the rate those bytes are being
  acknowledged at. Little's law read backwards: the milliseconds of delivery standing on the wire.
  Both terms are reliable-only, which is the only way the division means anything.

A round trip is timed as well, on the high lane the pose stream rides, and is kept as a diagnostic
rather than as a control input. A busy link reports once a second; an idle one is probed once a
second with a 32-byte datagram, so a baseline is never taken only under load.

Neither term is compared against a reference derived from its own past, and that is the whole
design. Two laws built before this one were, and both were refuted on the same property: a
delay-versus-baseline rule of the LEDBAT family, and a port of ENet's peer throttle, which judges
against the previous interval's lowest smoothed round trip
(`reference/enet/protocol.c:908-911`). A standing queue moves that reference,
so "better than baseline" stops meaning anything exactly when it starts to matter. The ENet port
was measured doing it: the baseline rose from 18 ms to a median of 1,545 ms, its accelerate branch
turned true of nearly every reading, and the rate ratcheted to the ceiling for 180 of 210 seconds.
Read against the same archived run, the byte terms separate with no overlap -- healthy inflight ran
a median of 60 ms (p90 163) and the collapsed regime a median of 13,055 (p10 1,458), while the rate
the delay law applied stood a median of 442 times above the delivery printed on the same line.

### The law

One decision per sample, ten times a second, so a join can climb inside its own download:

| Branch | When | What it does |
|---|---|---|
| hold | nothing queued, or delivery not measured yet | leaves the rate alone |
| brake | inflight above 400 ms | drops straight to the measured delivery |
| climb | inflight at or under 150 ms | 5/4 of the rate plus 8 KB/s |
| dead band | between the two | leaves the rate alone, because a controller with no dead band hunts |
| anchor | always, after the above | a rate may not stand above 5/4 of the largest recent delivery reading |

The thresholds are the regimes above: the climb stops where health stopped (p90 163 ms) and the
brake fires an order of magnitude under the collapse (p10 1,458 ms), so a threshold between them
flags 93.4% of the collapse and none of the health. The brake goes to the measurement rather than
to a fraction of the current rate, because a multiplicative back-off from a rate hundreds of times
too high takes hundreds of decisions to arrive.

The anchor is the guard that would have stopped both refuted laws on its own, and its margin above
1.0 is paid for in loss: pacing at k times what a link carries offers a share of every packet to a
bottleneck that cannot take it. At k = 2 on the drop-policed rig the goodput held at 95.3% of
nominal while the peer reported about half of what we sent as needing retransmission -- and a
reliable retry outranks the pose datagram, so that is not free. 5/4 is the probe margin BBR uses
and the step the climb takes.

A link opens at 145 KB/s, about 1% of the ceiling: measured reaching the ceiling in three seconds
where the headroom exists, and inside a thin link's capacity rather than far outside it. The
envelope runs from 32 KB/s -- an honest minimum, where a join is slow rather than broken -- to the
save pump's own output, which is the largest producer in the tree; a rate above that paces nothing
that exists.

### Who may fill the buffer

The transport admits a message only while a connection's pending bytes plus that message fit the
send buffer, and that sum is connection-wide: a bulk stream at the brim refuses the pose and voice
datagrams beside it, and the probe that times the path they ride. So `coop/net/send_admission`
bounds the mod's own contribution to that brim -- every reliable send but the probe pair stops
short of a 64 KB reserve, or a quarter of the buffer where that is smaller. It is not a guarantee
of room: the transport re-queues a NACKed segment with no admission check, so on a lossy link
pending passes the buffer size from outside while the mod adds nothing.

A send the rule holds back is never lost. It enters the per-lane backlog (`coop/net/send_backlog`),
or it is the save pump's pacing signal, which is what a full buffer already meant to it.

### The second bound, in time

The byte rule alone bounds a queue whose drain rate varies by more than an order of magnitude: the
same 4 MiB brim measured 5.6 seconds of queue on one policed joiner and 125.96 seconds on a slot
losing a three-way race for a single uplink. Bytes inside the transport's buffer can be neither
retracted nor reordered, so that is how long a cancelled transfer keeps spending a host's uplink,
and how far behind everything queued after it starts.

The send path with no queue behind it -- where a refusal is the producer's pacing signal -- is
therefore admitted only while this slot's queue is under two seconds of the link's own delivery.
Measured across a bulk transfer, the queue's median fell from 7.97 to 1.98 seconds and its maximum
from 28.12 to 3.23, at no cost in throughput (96.7% against 96.9% of nominal, with the delivery
window unchanged); three joiners contending on one machine fell from 57.81 to 2.21.

The divisor is the delivery the mod measures, never the rate the transport is paced at. With the
law on, the transport returns the mod's own last written rate through its equal-bounds branch,
which on a policed link stood 1.29 times above what that link carried -- a budget built on it
admitted 2.57 seconds while its name said two, and the metric checking it divided by the same
inflated figure.

The backlog path keeps the byte rule alone, and deliberately: its producers are one-shot bursts
against a sustained stream. The whole join burst measures about 740 KB, while the world blob is
17 MB offered at 13.1 MB/s, and only the blob was ever measured holding a buffer for two minutes.

MTA's precedent for the time bound is `CLatentSendQueue::DoPulse`
(`reference/mtasa-blue/Shared/mods/deathmatch/logic/CLatentSendQueue.cpp:46-69`), where a bulk
transfer hands the transport a rate times an elapsed time per pulse and keeps the remainder in its
own buffer. Deliberate divergence: MTA has to integrate because RakNet publishes no queue depth,
while GameNetworkingSockets does, so the mod bounds the depth directly and has nothing to drift.

## Who owns what

| State | Owner |
|---|---|
| The rate a connection is paced at | the peer that sends on it. Nothing is negotiated, and a peer never paces another peer's link |
| The delivery and round trip of a link | the sending end, which measures both; the receiving end counts the same reliable bytes independently, so the estimate is checkable against a second measurement |
| Admission into a send buffer | the sender, per slot, re-anchored on the transport's own pending total at each link sample |
| The save pump's pacing | the send path's refusals, which the pump reads as backpressure rather than as an error |

## Wire messages

One pair, both on the high lane: a link probe carrying a token and a send time, and its echo.
The round trip is timed against the sender's own record of when that token went out, never against
the time the reply carries, so a wrong or hostile echo cannot invent one. A responder that cannot
put an echo on the wire -- the send buffer at the brim -- says so, because to the prober a refused
echo and a lost probe otherwise look identical.

## Late join

A joining peer's link opens at the start rate and is measured from zero. A slot's byte counters are
armed for reset at teardown and cleared by the net thread at its next touch of the slot, so the
next occupant of a slot inherits no byte debt from the last one; the transport's own pending
counters restart with the connection in the same way.

## Known limits

| Limit | Evidence |
|---|---|
| A rate is a ceiling, not a reservation: two peers sharing one uplink are paced by what each measures, and neither knows about the other | `[V]` `coop/net/send_rate_control`, measured on three contending joiners |
| The time bound prices bytes against the delivery measured at the moment they are committed, and cannot retract them; a rate that collapses inside a second leaves a tail | `[V]` `coop/net/send_admission` |
| The occupancy compared is an estimate. It reads high when a drain went unseen, which is the safe direction, and low by a bounded amount -- the stream header the transport adds after its own check -- until the next anchor | `[V]` `coop/net/send_admission` |
| A link that genuinely is slow is still a long download; the mod can only decline to make it worse | `[V]` `coop/net/send_rate_control` |
| The round trip is a diagnostic, not an input. It rides the high lane, which the scheduler serves ahead of the bulk queue a transfer stands in, so it under-reports what that transfer is waiting behind | `[V]` `coop/net/send_rate_control` |

## Code map

| File | What it owns |
|---|---|
| `coop/net/send_rate_control` | the measurement of each link, the law above, and the rate the session writes to a connection |
| `coop/net/send_admission` | both bounds on entering a send buffer: the byte reserve, and the time budget on the unqueued path |
| `coop/net/send_backlog` | where a reliable send the byte rule refuses waits, by lane |
| `coop/net/connection_tuning` | the connect-time write: lanes, buffer size, and the rate a link opens at |
| `coop/net/session_start` | transport init, and the drill's global link policer |

Knobs, all off or default in a shipped build: `net.ratecontrol` turns the law off and leaves every
link at the transport's own stock rate, which is a control arm rather than a fallback;
`net.sendrate_kbs` pins a fixed rate; `net.sendbuf_kb` sets the buffer the bounds above are
measured against; `net.fakelink_kbs` simulates a thin uplink with the transport's own packet
policer; and the developer row `bulk_queue_cap_ms` sets the time budget, where 0 disables it.
Their shapes are in [testing.md](testing.md).
