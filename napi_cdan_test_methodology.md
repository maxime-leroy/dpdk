# net/dpaa2 CDAN Rx queue interrupts - test methodology

How to test the v3 series (dpaa2 `.rx_queue_intr_enable/disable`, CDAN +
volatile pull). The consumer is grout `--napi`: idle workers arm their rxq
CDAN and block on the portal eventfd via `rte_eth_dev_rx_intr_*`, wake on the
first frame, disarm and resume polling.

## Branches under test

- DPDK: https://github.com/maxime-leroy/dpdk/tree/cdan-napi-v3-v25.11
- grout: https://github.com/maxime-leroy/grout/tree/napi

## Setup

Platform: LX2160A, DPDK v25.11 base (x86 disables fslmc/dpaa2, so build and
run on the target).

Per-queue observability comes from the last patch, "net/dpaa2: add napi Rx
debug xstats (drop before merge)": `napi_cdan_count`, `napi_poll_count`,
`napi_deq_count`, `napi_eagain`, `napi_arm_fqne`, and per-queue
arm/disarm/CDAN-enable counts. It is the primary functional probe; drop it
before merge. Read them at runtime with `grcli stats hardware`.

Harness: an L2 cross-connect between two ports, no L3. Launch grout in napi
mode (rxqs = number of datapath cores, one DPCON/portal per worker):

    export DPRC=dprc.2   # bound to vfio-fsl-mc; the fslmc bus auto-probes its dpni ports
    grout -n -v --metrics :0 --syslog

    grcli interface add port p0 devargs "fslmc:dpni.X" rxqs "${NB_RXQS}"
    grcli interface add port p1 devargs "fslmc:dpni.Y" rxqs "${NB_RXQS}"
    grcli interface set port p0 promisc on
    grcli interface set port p1 promisc on
    grcli interface set port p0 domain p1
    grcli interface set port p1 domain p0   # domain is per-port; set both

## Test procedure

1. Wake, no-loss and latency (ping, idle link). With the xconnect up and grout
   `--napi`, `ping` through it at a low rate from the peer. Pass: no loss, i.e.
   the sleeping worker wakes on each CDAN; a lost-wake wedge shows as ping loss
   with the FQ frame_count climbing. RTT also measures the wake latency:
   compare `--napi` against poll-mode (`-p`); the extra is one CDAN wake plus a
   channel pull, bounded by the ITPR holdoff (0 here, so the first CDAN raises
   the DQRI immediately).

2. Real NAPI sleep vs silent busy-poll. `grcli stats hardware`: `rx_burst_1_pkts`
   near 100% means the worker genuinely sleeps and wakes per single packet
   (busy-poll instead returns large bursts). This distinguishes a real sleep
   from a fallback to polling.

3. Wedge watch. `grcli stats hardware zero` (plain hides zero-valued xstats).
   Healthy: `cdan_count` / `poll_count` / `deq_count` keep advancing and the FQ
   `schedstate` drains back to 2. Wedge: those counts frozen with frame_count
   climbing and `schedstate` stuck at 3 (scheduled, undrained), live `isr=4`
   (DQRI asserted) + `iir=0` (unmasked) = a lost CDAN wake on the sleeping
   worker; a continuously loaded sibling that keeps the core awake hides it.

4. Throughput (trex, line rate). Drive line rate with trex (or pktgen) and
   compare `--napi` against poll-mode (`-p` without `-n`); under load the
   worker busy-polls, so they should match. Note the known channel-dequeue tax
   of the NAPI path (single-core I/O ~5.9 Mpps vs l2fwd ~10.8 Mpps).

5. Re-home. Queues are runtime re-homable to another core without a port
   restart; forwarding follows the worker.

## Reference commands

Wake / no-loss and latency from the peer (loss = lost-wake wedge; RTT vs
poll-mode = the wake latency added by --napi):

    ping -i 0.2 <peer>

Per-queue CDAN counters, including zero-valued ones (plain output hides them);
cdan/poll/deq must keep advancing, frozen with frames piling = wedge:

    grcli stats hardware zero pattern "*napi*"

Real NAPI sleep vs busy-poll (rx_burst_1_pkts near 100% = per-packet wake):

    grcli stats hardware pattern "*burst*"

Per-thread CPU: an idle worker's core drops to ~0, a busy one sits near 100:

    top -H -p "$(pidof grout)"

Busy vs total cycles (the interrupt sleep is counted in total, not busy):

    grcli stats software

Throughput and drops, to compare --napi against poll-mode (`-p` without `-n`):

    grcli stats hardware pattern "*packets*"

Drive line rate from a generator on the link partner (testpmd, trex or
pktgen) and read the rx/tx deltas; under load --napi should match poll-mode.
Reset counters between runs:

    grcli stats reset

## Alternative harness: l3fwd-power (independent of grout)

`examples/l3fwd-power` is the DPDK sample app for Rx interrupt mode: idle
lcores block on `rte_epoll_wait()` and wake on the queue's interrupt eventfd.
It validates the CDAN path outside grout, isolated from the graph/RCU/reconfig
machinery.

This is offered as an alternative for reviewers who would rather not run grout.
I have not run this path myself yet, but it should work: l3fwd-power is the
standard DPDK sample for Rx interrupt mode and this branch implements the
`rte_eth_dev_rx_intr_*` API it relies on.

Prerequisites:
- Run on this branch (commit 8289559b31, "net/dpaa2: support Rx queue
  interrupts"). Upstream dpaa2 has only LSC, no per-Rx-queue interrupts, so
  stock DPDK fails at `rte_eth_dev_rx_intr_ctl_q()`.
- Includes fixup ed489c7f4a ("examples/l3fwd-power: handle shared Rx interrupt
  eventfd") so multi-queue-per-lcore enters interrupt mode instead of silently
  polling.

Use `--interrupt-only`: it forces APP_MODE_INTERRUPT and skips cpufreq
entirely (pure sleep/wake on the Rx interrupt). The autodetect default
(LEGACY) is CPU frequency scaling, unrelated to dpaa2, and only engages if the
kernel exposes CPPC cpufreq.

dpaa2 gotcha (why the fixup exists): every Rx queue serviced by the same
portal (same core) shares one portal eventfd (dpaa2_ethdev.c:3193), so the
2nd+ queue on a lcore returns -EEXIST from `rte_eth_dev_rx_intr_ctl_q(ADD)`.
Stock `event_register()` treated that as fatal and silently fell back to
polling (a false pass); the fixup accepts -EEXIST. Mapping one queue per lcore
avoids the shared-fd path entirely.

Run (one queue per lcore is safest):

    export DPRC=dprc.2   # DPRC bound to vfio-fsl-mc
    l3fwd-power -l 0-2 -a fslmc:dpni.X -a fslmc:dpni.Y -- \
        -p 0x3 -P --interrupt-only \
        --config "(0,0,1),(1,0,2)"   # (port,queue,lcore), distinct cores

Verify it is really in interrupt mode, from the logs:
- PASS: "lcore N is waked up from rx interrupt on port P queue Q" - CDAN
  firing, sleeping/waking for real.
- FALSE PASS: "RX interrupt won't enable" - interrupts disabled, app is
  polling, nothing is being tested.

Scope vs grout: l3fwd-power blocks with a 10 ms bounded `epoll_wait` and has no
arm-race recheck, no stop/start/RCU/reconfig. It is a clean positive smoke
test ("does a CDAN wake epoll, arm->sleep->wake->disarm, repeatedly?"), but the
bounded wait structurally masks the lost-edge / re-arm / stop-start wedge that
grout's indefinite wait exposes. Green l3fwd-power means "CDAN path works", not
"grout won't wedge". Conversely, if l3fwd-power itself hangs >10 ms with
traffic waiting, that isolates a driver bug with no grout involvement.
