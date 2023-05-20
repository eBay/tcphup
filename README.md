# tcphup

Hang up on TCP connections.

tcphup is particularily useful for dropping stale TCP keep alive connections during service failovers.

# Why tcphup

tcphup is an alternative to existing approaches which attempt to
man-in-the-middle TCP RST packets on existing flows.

The primary disadvantages of existing approaches are:

1. The next sequence number has to be guessed and front run in order to insert
the RST packet.
2. For keep alive connections, idleness causes delays in trying to insert the
RST packet.
3. Numerous application I/O frameworks do not handle a flood of RST's on the wire as gracefully as they would a proper close(2) call.

# tcphup is different

tcphup sends a proper shutdown to the socket, resulting in a proper FIN, as if a client had
called close(2) to a specific socket without any modifications to the running applications.

tcphup is more efficient (libnetlink to traverse connections) and provides better heuristics for closing stale TCP connections.

tcphup works with multi-path TCP.

## Example use case

An application opens keep alive connections to a service which is then failed over (region exit, network split, etc.), 
however, the application does not connect to the new
service IP in a timely fashion due to long keepalive_cnt and/or keepalive_interval options on the socket(s).

tcphup is then executed to kill the existing connections for the stale IP (or the same IP in the case of anycast or a VIP).

tcphup issues a close(2) on behalf of the application, hangs up the keep alive
connection, which allows the application to handle the service fail over.

# Dependencies
- linux > 5.10.0

# Build
```bash
$ make
```

# Usage

Kill all port 80/tcp connections to httpstat.us:

```bash
$ curl -v httpstat.us/200?sleep=500000
# in another tty
$ tcphup $(getent hosts httpstat.us | awk '{ print $1 }') 80
```

Kill all connections to httpstat.us (set port to 0):

```bash
$ curl -v httpstat.us/200?sleep=500000
# in another tty
$ tcphup $(getent hosts httpstat.us | awk '{ print $1 }') 0
```

# Why not not just restart the application during failover?

Cold starts are a potential issue.

Larger deployments pose larger thundering herd effects and risks.

An application is most likely connected to many services at once, having to reconnect to all services causes un-necessary churn on the platform (like kubernetes) and thundering herd loads on services which were working perfectly fine.

# Why not just reduce keepalive_intvl / keepalive_probes (count) / keepalive_time?

The default values in Linux 5.x follow:

|param|value|
|--|--|
|tcp_keepalive_intvl|75 seconds|
|tcp_keepalive_probes|9 times|
|tcp_keepalive_time|7200 seconds|

An explanation of these from https://tldp.org/HOWTO/TCP-Keepalive-HOWTO/usingkeepalive.html follows:

tcp_keepalive_time
>    the interval between the last data packet sent (simple ACKs are not considered data) and the first keepalive probe; after the connection is marked to need keepalive, this counter is not used any further

tcp_keepalive_intvl
>    the interval between subsequential keepalive probes, regardless of what the connection has exchanged in the meantime 

tcp_keepalive_probes 
>    the number of unacknowledged probes to send before considering the connection dead and notifying the application layer
    


# License
[MIT License](./LICENSE.txt)
