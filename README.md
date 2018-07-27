# tcpfwd
Tcp connection forwarding utility

Usage: `./tcpfwd <local_port> <remote_host> <remote_port>`

Proxies incoming tcp connections to `remote_host:remote_port`.
Listens on `local_port` on all local network interfaces.
`remote_host` can be either an IP or a hostname and will be resolved on every new connection.
