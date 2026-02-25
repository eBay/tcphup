# tcphup

Hang up on TCP connections and manipulate TCP socket options.

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

tcphup can operate in three modes:
1. **Shutdown mode** (no flags): Closes matching connections
2. **Manipulation mode** (`-m` flag): Sets TCP socket options on matching connections without shutting them down
3. **Info mode** (`-i` flag): Displays TCP socket options (can be combined with `-m` to verify changes)

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
tcphup [-i] [-m OPTION:value] ... <IP> <port>
```

**Modes:**
- No flags: Shutdown connections
- `-m`: Manipulate socket options (no shutdown)
- `-i`: Display socket options
- `-i -m`: Manipulate and display (for verification)

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

## Inspecting TCP Options

Use the `-i` flag to display all TCP and socket options for matching connections without shutting them down:

```bash
# Display TCP options for all connections to 192.168.1.1:80
$ tcphup -i 192.168.1.1 80

=== Connection: pid=1234, fd=5 ===
  TCP_KEEPIDLE:      7200
  TCP_KEEPINTVL:     75
  TCP_KEEPCNT:       9
  TCP_SYNCNT:        6
  TCP_LINGER2:       60
  TCP_DEFER_ACCEPT:  0
  TCP_WINDOW_CLAMP:  0
  TCP_USER_TIMEOUT:  0
  TCP_NODELAY:       1
  TCP_MAXSEG:        1448
  TCP_CORK:          0
  TCP_QUICKACK:      1
  TCP State:         1
  TCP RTT:           1234 us
  TCP RTT Variance:  567 us
  TCP RTO:           204000 us
  TCP SND_MSS:       1448
  TCP RCV_MSS:       536
  TCP Retransmits:   0
  TCP Total Retrans: 0
  SO_KEEPALIVE:      1
  SO_SNDBUF:         16384
  SO_RCVBUF:         87380
  SO_REUSEADDR:      0
  SO_REUSEPORT:      0
  SO_ERROR:          0
  SO_TYPE:           1
  SO_SNDTIMEO:       0.000000 s
  SO_RCVTIMEO:       0.000000 s
  SO_LINGER:         l_onoff=0, l_linger=0
```

## Manipulating Socket Options

You can manipulate TCP and socket options on existing connections using the `-m` flag. Multiple `-m` options can be specified.

**Important**: When `-m` options are specified, connections are **NOT** shut down. Socket manipulation and shutdown are separate modes:

- **No flags**: Shutdown connections (no manipulation)
- **`-m` only**: Manipulate socket options WITHOUT shutdown
- **`-i` only**: Display current socket options
- **`-i -m`**: Manipulate options and display the results (for verification)

```bash
# Manipulation mode: Set TCP option WITHOUT closing connection
$ tcphup -m TCP_KEEPINTVL:3 192.168.1.1 80

# Manipulation mode: Set multiple TCP options WITHOUT closing
$ tcphup -m TCP_KEEPINTVL:3 -m TCP_SYNCNT:1 -m TCP_LINGER2:1 192.168.1.1 80

# Manipulation mode: Set socket timeouts WITHOUT closing (decimals supported)
$ tcphup -m SO_SNDTIMEO:1.5 -m SO_RCVTIMEO:2.0 192.168.1.1 80

# Manipulation mode: Adjust socket buffers WITHOUT closing
$ tcphup -m SO_SNDBUF:65536 -m SO_RCVBUF:131072 192.168.1.1 80

# Manipulation mode: Set linger WITHOUT closing (onoff,seconds or just seconds)
$ tcphup -m SO_LINGER:1,30 192.168.1.1 80
$ tcphup -m SO_LINGER:30 192.168.1.1 80  # Assumes onoff=1

# Manipulation mode: Combine TCP and socket options WITHOUT closing
$ tcphup -m TCP_KEEPINTVL:3 -m SO_SNDTIMEO:1.0 -m SO_KEEPALIVE:1 192.168.1.1 80

# Info mode: Set options and display to verify
$ tcphup -i -m TCP_KEEPINTVL:3 192.168.1.1 80
$ tcphup -i -m TCP_KEEPINTVL:3 -m SO_KEEPALIVE:1 192.168.1.1 80
```

### Supported TCP Options (SOL_TCP)

- `TCP_KEEPIDLE` - Time before keepalive probes start (seconds)
- `TCP_KEEPINTVL` - Interval between keepalive probes (seconds)
- `TCP_KEEPCNT` - Number of keepalive probes
- `TCP_SYNCNT` - Number of SYN retransmits
- `TCP_LINGER2` - Time to wait in FIN-WAIT-2 state (seconds)
- `TCP_DEFER_ACCEPT` - Wait for data before accepting (seconds)
- `TCP_WINDOW_CLAMP` - Bound advertised window (bytes)
- `TCP_USER_TIMEOUT` - Maximum timeout for unacknowledged data (milliseconds)
- `TCP_NODELAY` - Disable Nagle's algorithm (0 or 1)
- `TCP_MAXSEG` - Maximum segment size (bytes)
- `TCP_CORK` - Cork the socket (0 or 1)
- `TCP_QUICKACK` - Enable quick ACKs (0 or 1)

### Supported Socket Options (SOL_SOCKET)

- `SO_KEEPALIVE` - Enable/disable keepalive (0 or 1)
- `SO_SNDBUF` - Send buffer size (bytes)
- `SO_RCVBUF` - Receive buffer size (bytes)
- `SO_REUSEADDR` - Reuse local addresses (0 or 1)
- `SO_REUSEPORT` - Reuse port (0 or 1)
- `SO_SNDTIMEO` - Send timeout (decimal seconds, e.g., 1.5)
- `SO_RCVTIMEO` - Receive timeout (decimal seconds, e.g., 2.0)
- `SO_LINGER` - Linger on close (format: `onoff,seconds` or just `seconds`)

### Value Formats

- **Integer options**: Plain integer (e.g., `TCP_KEEPCNT:5`)
- **Timeout options**: Decimal seconds (e.g., `SO_SNDTIMEO:1.5` for 1.5 seconds)
- **Linger option**: Two formats supported:
  - `SO_LINGER:1,30` - onoff=1, linger=30 seconds
  - `SO_LINGER:30` - onoff=1 (implied), linger=30 seconds

Note: Some options cannot be changed after a connection is established (e.g., `TCP_SYNCNT`, `TCP_MAXSEG`). Options like keepalive settings, timeouts, and buffers work well on established connections.

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
