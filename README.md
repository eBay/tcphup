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

tcphup sends a proper shutdown to the socket, a proper FIN, as if the client had
called close(2) on the socket without any modifications to running applications.

tcphup is more efficient and provides better reliability in closing the connection.

## Example use case

An application opens keep alive connections to a service, however due to large
keep alive intervals or counts, the application cannot fail over to a new
service IP in the event of fail over(s) in a timely fashion.

tcphup issuing a close(2) on behalf of the application hangs up the keep alive
connection, which would allow the application to handle service fail overs more
gracefully.

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
