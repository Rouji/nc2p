# netcat to POST
Receives raw data via TCP (e.g. sent using netcat) and POSTs it to an HTTP(S) URL.  
Useful for adding [termbin](http://termbin.com/)-like behaviour on top of existing HTTP-based file hosting sites.  

# Building:
```bash
meson build
ninja -C build
```

# Usage:
```
./nc2p
Usage: ./build/nc2p [-l listen_ip] [-p listen_port] [-t connection_timeout] [-f form_field] [-n filename] upstream_url
```

# Note about timeouts
[termbin](http://termbin.com/) accepts files as valid, even if clients don't explicitly close the socket after EOF and run into the timeout instead. That has the benefit of your netcat implementation not needing to do that (some do, some don't, some only optionally). Downside is that those that don't will make you wait for the timeout, and if your connection is bad you're potentially left with a truncated file on the server.  

That's mostly fine for smaller files and especially text, but kind of bad for anything else.  

`nc2p` will by default (compile-time switch, see `ALLOW_TIMEOUT`) cancel the HTTP POST to upstream, if a client times out. That does mean you might have to add `-N` to your `netcat` command. `nc2p` will (try to) tell clients about that when they time out.

