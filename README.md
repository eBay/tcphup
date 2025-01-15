# tcphup

Hang up on TCP connections.

tcphup is particularily useful for dropping stale TCP keep alive connections during service failovers.

tcphup targets the destinations to hang up on based on L3/L4 address info.

# Why tcphup

tcphup is an alternative to existing approaches which attempt to
man-in-the-middle TCP RST packets on existing flows.

The primary disadvantages of existing approaches are:

1. The next sequence number has to be guessed and front run in order to insert
the RST packet. This approach also incurs a performance penalty.
2. For keep alive connections, idleness causes delays in trying to insert the
RST packet.
3. Numerous application I/O frameworks do not handle a flood of RST's on the wire as gracefully as they would a proper close(2) call.

# tcphup is different

tcphup does a proper shutdown(2) on the socket, resulting in a proper FIN, as if a client had
called shutdown(2)/close(2) to a socket without any modifications to the running applications.

tcphup is more efficient (libnetlink to traverse connections) and provides better heuristics for closing stale TCP connections.

tcphup works with multi-path TCP and only requires glibc.

## Example use case

An application opens keep alive connections to a service which is then failed over (region exit, network split, etc.), 
however, the application does not connect to the new
service IP in a timely fashion due to long keepalive_cnt and/or keepalive_interval options on the socket(s).

tcphup is then executed to kill the existing connections for the stale IP (or the same IP in the case of anycast or a VIP).

tcphup issues a close(2) on behalf of the application, hangs up the keep alive
connection, which allows the application to handle the service fail over.

# Dependencies
- linux > 5.10.0
- glibc

# Build
```bash
$ make
```

# Usage

Basic usage:

```bash
tcphup <IP> <port>
```

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

Reduced values would create more network chatter with TCP keepalive packets between multiple hosts.

TCP keep alive packets are, at a minimum, 64 bytes.

A "run of the mill" server serving frontend web traffic may have persistent connections to redis, postgresql, and object store. Let's model this out with the default system TCP keep alive values.

Default values in Linux 5.x follow:

|param|value|
|--|--|
|tcp_keepalive_intvl|75 seconds|
|tcp_keepalive_probes|9 times|
|tcp_keepalive_time|7200 seconds|

Each web frontend server 3 services sends 64 bytes every 75 seconds. For the purpose of this example, presume there is no packet loss (tcp_keepalive_probes > 1 is ignored), in 1 hour that is 9216 bytes of keepalive traffic from each web frontend server.

In this setup, there are 9 probes sent before a connection is finally hung up (without data packets in between), 75 * 9, that is up to 11 minutes to hang up a stale connection.

In order to address this, let's presume tcp_keep_alive_intvl is reduced, let's see what effect what halving the keepalive_intvl has:

| tcp_keepalive_intvl | keepalive traffic (1 hour) | max TTH (time to hangup) |
|--|--|--|
|75|9216 bytes|~11.25 minutes|
|38|18189 bytes|~5.5 minutes|
|19|36378 bytes|~2.5 minutes|
|10|69210 bytes|~1.5 minutes|
|5|138240 bytes|45 seconds|
|3|230400 bytes|27 seconds|
|1|691200 bytes|9 seconds|

One could further tune tcp_keepalive_probes to be more aggressive - reducing TTH in exchange for possibly more frequent false positives during network events.

tcp_keepalive_time is ignored, it has no effect on reducing the costs related to a service failure scenario.

Simply reducing the keepalive parameters in an effort to reduce TTH and network chatter has costs which increase appreciably with the number of keepalive-enabled services and clients.

Explanation of the tcp_keepalive_* parameters from https://tldp.org/HOWTO/TCP-Keepalive-HOWTO/usingkeepalive.html follows:

tcp_keepalive_time
>    the interval between the last data packet sent (simple ACKs are not considered data) and the first keepalive probe; after the connection is marked to need keepalive, this counter is not used any further

tcp_keepalive_intvl
>    the interval between subsequential keepalive probes, regardless of what the connection has exchanged in the meantime 

tcp_keepalive_probes 
>    the number of unacknowledged probes to send before considering the connection dead and notifying the application layer

# License
[MIT License](./LICENSE.txt)
