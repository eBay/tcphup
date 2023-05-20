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

An application opens keep alive connections to a service which is then failed over (region exit, network split, etc.), however due to large
keep alive intervals or counts, the application does not connect to the new
service IP in a timely fashion due to long keepalive_cnt and keepalive_interval options on the socket.

tcphup to kill the existing connections for the stale IP (or the same IP if anycast or a VIP).

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

# License
[MIT License](./LICENSE.txt)
