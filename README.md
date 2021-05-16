# netcat to POST
Shim that receives raw data via TCP (e.g. sent using netcat) and POSTs it to an HTTP(S) URL.  
Useful for adding [termbin.com](http://termbin.com/)-like behaviour on top of existing HTTP-based file hosting sites.  

# Usage:
```
./nc2p
Usage: ./build/nc2p [-l listen_ip] [-p listen_port] [-t connection_timeout] [-f form_field] [-n filename] upstream_url
```

# Building:
```bash
meson build
ninja -C build
```
