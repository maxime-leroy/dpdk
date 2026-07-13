Testing l3fwd-power Rx Queue Interrupts on dpaa2
================================================

This note describes how to validate that l3fwd-power really runs in Rx
interrupt mode on a dpaa2 platform (for example LX2160A), and why an earlier
"it works" result was not conclusive.

Two independent problems can make such a test misleading.

1. The example may not be built at all
--------------------------------------

l3fwd-power depends on the ``power``, ``lpm`` and ``metrics`` libraries (in
addition to ``timer``, ``hash`` and ``telemetry``). When DPDK is configured
with ``-Dexamples=all``, an example whose dependencies are missing is skipped
silently: no error, no binary. If ``enable_libs`` does not list ``power``,
``lpm`` and ``metrics``, l3fwd-power is dropped and there is nothing to test.

Make sure the build enables them, for example::

    enable_libs = ...,timer,hash,telemetry,lpm,power,metrics

To turn a missing dependency into a hard error instead of a silent skip,
request the example explicitly::

    -Dexamples=l3fwd-power

and check that ``dpdk-l3fwd-power`` is produced for the target.

2. Forwarding can work without interrupts
-----------------------------------------

When idle, a worker arms the Rx queue interrupts and sleeps in
``rte_epoll_wait()``. That call uses a short timeout (10 ms) so the loop can
still run periodic housekeeping (Tx drain, statistics) and honour shutdown. As
a side effect, the worker keeps forwarding even if the interrupt is never
delivered: it simply wakes on the timeout and polls again.

Observing that traffic is forwarded therefore does NOT prove that the interrupt
path works; it may only prove that the 10 ms timeout fires.

How to test properly
--------------------

Run at a low traffic rate, with idle gaps long enough for the workers to sleep
between packets. At line rate the workers never sleep, so nothing exercises the
interrupt path.

Then confirm real interrupt wake-ups by one of:

*   Log message: a wake-up caused by an interrupt prints::

        lcore N is waked up from rx interrupt on port X queue Y

    This line is emitted only when ``rte_epoll_wait()`` returns an event, that
    is on a real interrupt, and never on a timeout. If traffic flows but this
    line never appears, the workers are woken by the timeout, not by the Rx
    interrupt.

*   Decisive check: temporarily make ``rte_epoll_wait()`` block instead of
    timing out, by changing the timeout argument in
    ``sleep_until_rx_interrupt()`` from ``10`` to ``-1`` and rebuilding. Only a
    real interrupt can then wake the worker. If forwarding still works at a low
    rate, the interrupt path is confirmed; if it stalls, the wake-ups were
    coming from the timeout. Restore ``10`` afterwards; note that ``-1``
    disables the periodic shutdown check, so the application must be stopped
    with a signal.
