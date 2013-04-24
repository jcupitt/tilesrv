# tilesrv - simple fast-cgi tile server for deepzoom

Dependencies:

```bash
apt-get install fastcgi uriparser vips
```

Compile with:

```bash
gcc tilesrv.c \
	`pkg-config vips --cflags --libs` \
	-lfcgi -luriparser \
	-o tilesrv.fcgi
```

Then copy to:

/usr/local/httpd/fcgi-bin

You may need to create the logfile, eg.

```bash
touch /tmp/tilesrv.log
chmod ugo+rw /tmp/tilesrv.log
```

Sample fcgi.conf:

```xml
<IfModule mod_fastcgi.c>
  FastCgiIpcDir /var/lib/apache2/fastcgi

  # Create a directory for the tilesrv binary
  ScriptAlias /fcgi-bin/ "/usr/local/httpd/fcgi-bin/"

  # Set the options on that directory
  <Directory "/usr/local/httpd/fcgi-bin">
     AllowOverride None
     Options None
     Order allow,deny
     Allow from all
  </Directory>

  # Set the module handler
  AddHandler fastcgi-script fcg fcgi fpl

  # Initialise some variables for the FCGI server
  FastCgiServer /usr/local/httpd/fcgi-bin/tilesrv.fcgi \
    -initial-env LOGFILE=/tmp/tilesrv.log 
  
</IfModule>
```
